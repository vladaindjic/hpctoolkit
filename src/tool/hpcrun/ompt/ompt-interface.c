// -*-Mode: C++;-*- // technically C99

// * BeginRiceCopyright *****************************************************
//
// $HeadURL$ 
// $Id$
//
// --------------------------------------------------------------------------
// Part of HPCToolkit (hpctoolkit.org)
//
// Information about sources of support for research and development of
// HPCToolkit is at 'hpctoolkit.org' and in 'README.Acknowledgments'.
// --------------------------------------------------------------------------
//
// Copyright ((c)) 2002-2020, Rice University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of Rice University (RICE) nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// This software is provided by RICE and contributors "as is" and any
// express or implied warranties, including, but not limited to, the
// implied warranties of merchantability and fitness for a particular
// purpose are disclaimed. In no event shall RICE or contributors be
// liable for any direct, indirect, incidental, special, exemplary, or
// consequential damages (including, but not limited to, procurement of
// substitute goods or services; loss of use, data, or profits; or
// business interruption) however caused and on any theory of liability,
// whether in contract, strict liability, or tort (including negligence
// or otherwise) arising in any way out of the use of this software, even
// if advised of the possibility of such damage.
//
// ******************************************************* EndRiceCopyright *

/******************************************************************************
 * global include files
 *****************************************************************************/

#include <sys/param.h>



//*****************************************************************************
// local includes
//*****************************************************************************

#include <hpcrun/cct/cct.h>
#include <hpcrun/cct/cct-node-vector.h>
#include <hpcrun/cct2metrics.h>
#include <hpcrun/device-finalizers.h>
#include <hpcrun/hpcrun-initializers.h>
#include <hpcrun/main.h>
#include <hpcrun/memory/hpcrun-malloc.h>
#include <hpcrun/safe-sampling.h>
#include <hpcrun/sample-sources/blame-shift/blame-shift.h>
#include <hpcrun/sample-sources/blame-shift/directed.h>
#include <hpcrun/sample-sources/blame-shift/undirected.h>
#include <hpcrun/sample-sources/sample-filters.h>
#include <hpcrun/thread_data.h>

#include "ompt-callstack.h"
#include "ompt-defer.h"
#include "ompt-interface.h"
//#include "ompt-queues.h"
#include "ompt-region.h"
#include "ompt-placeholders.h"
#include "ompt-task.h"
#include "ompt-thread.h"
#include "ompt-device.h"



//*****************************************************************************
// macros
//*****************************************************************************

#define ompt_event_may_occur(r) \
  ((r ==  ompt_set_sometimes) | (r ==  ompt_set_always))

#define OMPT_DEBUG_STARTUP 0
#define OMPT_DEBUG_TASK 0



//*****************************************************************************
// static variables
//*****************************************************************************

// struct that contains pointers for initializing and finalizing
static ompt_start_tool_result_t init;
static const char* ompt_runtime_version;

volatile int ompt_debug_wait = 1;

static int ompt_elide = 0;
static int ompt_initialized = 0;

static int ompt_task_full_context = 0;

static int ompt_mutex_blame_requested = 0;
static int ompt_idle_blame_requested = 0;

static int ompt_idle_blame_enabled = 0;

static bs_fn_entry_t mutex_bs_entry;
static bs_fn_entry_t idle_bs_entry;
static sf_fn_entry_t serial_only_sf_entry;

// state for directed blame shifting away from spinning on a mutex
static directed_blame_info_t omp_mutex_blame_info;

// state for undirected blame shifting away from spinning waiting for work
static undirected_blame_info_t omp_idle_blame_info;

static __thread int thread_is_idle = 0;

//-----------------------------------------
// declare ompt interface function pointers
//-----------------------------------------

#define ompt_interface_fn(f) \
  static f ## _t f ## _fn;

FOREACH_OMPT_INQUIRY_FN(ompt_interface_fn)

#undef ompt_interface_fn



/******************************************************************************
 * thread-local variables
 *****************************************************************************/

//-----------------------------------------
// variable ompt_idle_count:
//    this variable holds a count of how 
//    many times the current thread has
//    been marked as idle. a count is used 
//    rather than a flag to support
//    nested marking.
//-----------------------------------------
static __thread int ompt_idle_count;


/******************************************************************************
 * private operations 
 *****************************************************************************/

//----------------------------------------------------------------------------
// support for directed blame shifting for mutex objects
//----------------------------------------------------------------------------

//-------------------------------------------------
// return a mutex that should be blamed for 
// current waiting (if any)
//-------------------------------------------------

static uint64_t
ompt_mutex_blame_target
(
 void
)
{
  if (ompt_initialized) {
    ompt_wait_id_t wait_id;
    ompt_state_t state = hpcrun_ompt_get_state(&wait_id);

    switch (state) {
    case ompt_state_wait_critical:
    case ompt_state_wait_lock:
    case ompt_state_wait_atomic:
    case ompt_state_wait_ordered:
      return wait_id;
    default: break;
    }
  }
  return 0;
}


static int
ompt_serial_only
(
 void *arg
)
{
  if (ompt_initialized) {
    ompt_wait_id_t wait_id;
    ompt_state_t state = hpcrun_ompt_get_state(&wait_id);

    ompt_thread_t ttype = ompt_thread_type_get();
    if (ttype != ompt_thread_initial) return 1;

    if (state == ompt_state_work_serial) return 0;
    return 1;
  }
  return 0;
}


static int *
ompt_get_idle_count_ptr
(
 void
)
{
  return &ompt_idle_count;
}


//----------------------------------------------------------------------------
// identify whether a thread is an OpenMP thread or not
//----------------------------------------------------------------------------

static _Bool
ompt_thread_participates
(
 void
)
{
  switch(ompt_thread_type_get()) {
  case ompt_thread_initial:
  case ompt_thread_worker:
    return true;
  case ompt_thread_other:
  case ompt_thread_unknown:
  default:
    break;
  }
  return false;
}


static _Bool
ompt_thread_needs_blame
(
 void
)
{
  if (ompt_initialized) {
    ompt_wait_id_t wait_id;
    ompt_state_t state = hpcrun_ompt_get_state(&wait_id);
    switch(state) {
      case ompt_state_wait_barrier:
      case ompt_state_wait_barrier_implicit:
      case ompt_state_wait_barrier_explicit:
      case ompt_state_wait_barrier_implicit_parallel:
      case ompt_state_wait_barrier_implicit_workshare:
	
	// johnmc ompt-blame: check this
	// it seems like having thread 0 accept idle samples while charging itself one 
	// for barriers will get the costs in the right place. otherwise, they don't 
	// score as idleness 
#if 0
	return (hpcrun_ompt_get_thread_num(0) == 0);
#endif

      case ompt_state_idle:
      case ompt_state_wait_taskwait:
      case ompt_state_wait_taskgroup:
      default:
	return false;

      case ompt_state_work_serial:
      case ompt_state_work_parallel:
        return true;
    }
  }
  return true;
}


//----------------------------------------------------------------------------
// interface function to register support for directed blame shifting for 
// OpenMP operations on mutexes if event OMP_MUTEX is present
//----------------------------------------------------------------------------

static void
ompt_mutex_blame_shift_register
(
 void
)
{
  mutex_bs_entry.fn = directed_blame_sample;
  mutex_bs_entry.next = NULL;
  mutex_bs_entry.arg = &omp_mutex_blame_info;

  omp_mutex_blame_info.get_blame_target = ompt_mutex_blame_target;

  omp_mutex_blame_info.blame_table = blame_map_new();
  omp_mutex_blame_info.levels_to_skip = 1;

  blame_shift_register(&mutex_bs_entry);
}


static void
ompt_register_mutex_metrics
(
 void
)
{
  kind_info_t *mut_kind = hpcrun_metrics_new_kind();
  omp_mutex_blame_info.wait_metric_id = 
    hpcrun_set_new_metric_info_and_period(mut_kind, "OMP_MUTEX_WAIT", 
				    MetricFlags_ValFmt_Int, 1, metric_property_none);

  omp_mutex_blame_info.blame_metric_id = 
    hpcrun_set_new_metric_info_and_period(mut_kind, "OMP_MUTEX_BLAME",
				    MetricFlags_ValFmt_Int, 1, metric_property_none);
  hpcrun_close_kind(mut_kind);
}


static void
ompt_register_idle_metrics
(
 void
)
{
  kind_info_t *idl_kind = hpcrun_metrics_new_kind();
  omp_idle_blame_info.idle_metric_id = 
    hpcrun_set_new_metric_info_and_period(idl_kind, "OMP_IDLE",
				    MetricFlags_ValFmt_Real, 1, metric_property_none);

  omp_idle_blame_info.work_metric_id = 
    hpcrun_set_new_metric_info_and_period(idl_kind, "OMP_WORK",
				    MetricFlags_ValFmt_Real, 1, metric_property_none);
  hpcrun_close_kind(idl_kind);
}


static void
ompt_idle_blame_shift_register
(
 void
)
{
  ompt_idle_blame_enabled = 1;

  idle_bs_entry.fn = undirected_blame_sample;
  idle_bs_entry.next = NULL;
  idle_bs_entry.arg = &omp_idle_blame_info;

  omp_idle_blame_info.get_idle_count_ptr = ompt_get_idle_count_ptr;
  omp_idle_blame_info.participates = ompt_thread_participates;
  omp_idle_blame_info.working = ompt_thread_needs_blame;

  blame_shift_register(&idle_bs_entry);
}


//----------------------------------------------------------------------------
// initialize pointers to callback functions supported by the OMPT interface
//----------------------------------------------------------------------------

static void 
ompt_init_inquiry_fn_ptrs
(
 ompt_function_lookup_t ompt_fn_lookup
)
{
#define ompt_interface_fn(f) \
  f ## _fn = (f ## _t) ompt_fn_lookup(#f);

FOREACH_OMPT_INQUIRY_FN(ompt_interface_fn)

#undef ompt_interface_fn
}


//----------------------------------------------------------------------------
// support for undirected blame shifting to attribute idleness
//----------------------------------------------------------------------------

//-------------------------------------------------
// note birth and death of threads to support 
// undirected blame shifting using the IDLE metric
//-------------------------------------------------


static void
ompt_thread_begin
(
 ompt_thread_t thread_type,
 ompt_data_t *thread_data
)
{
  ompt_thread_type_set(thread_type);
  undirected_blame_thread_start(&omp_idle_blame_info);

  // initialize freelist
  notification_freelist_head = NULL;
  typed_channel_init(notification)(&thread_notification_channel);
  typed_channel_init(region)(&region_freelist_channel);
  // FIXME vi3: get the max active levels of nesting from omp
  // initialize random access stack of active parallel regions
  region_stack = typed_random_access_stack_init(region)(MAX_NESTING_LEVELS);
  unresolved_cnt = 0;
#if FREELISTS_DEBUG
  atomic_exchange(&region_freelist_channel.region_used, 0);
#endif

#if ENDING_REGION_MULTIPLE_TIMES_BUG_FIX
  runtime_master_region_stack =
      typed_random_access_stack_init(runtime_region)(MAX_NESTING_LEVELS);
#endif
}


static void
ompt_thread_end
(
 ompt_data_t *thread_data
)
{
  // TODO(keren): test if it is called
  undirected_blame_thread_end(&omp_idle_blame_info);
//  printf("DEBUG: number of unresolved regions: %d\n", unresolved_cnt);
}


//-------------------------------------------------
// note the beginning and end of idleness to 
// support undirected blame shifting
//-------------------------------------------------

void
ompt_idle_begin
(
 void
)
{
  if (!thread_is_idle) {
    undirected_blame_idle_begin(&omp_idle_blame_info);
    thread_is_idle = 1;
  }
}


void
ompt_idle_end
(
 void
)
{
  if (thread_is_idle) {
    undirected_blame_idle_end(&omp_idle_blame_info);
    thread_is_idle = 0;
  }
}


static void 
__attribute__ ((unused))
ompt_idle
(
 ompt_scope_endpoint_t endpoint
)
{
  if (endpoint == ompt_scope_begin) ompt_idle_begin();
  else if (endpoint == ompt_scope_end) ompt_idle_end();
  else assert(0);

  //printf("Thread id = %d, \tIdle %s\n", omp_get_thread_num(), endpoint==1?"begin":"end");
}

#if 0
static void
ompt_sync
(
 ompt_sync_region_t kind,
 ompt_scope_endpoint_t endpoint,
 ompt_data_t *parallel_data,
 ompt_data_t *task_data,
 const void *codeptr_ra
)
{
#if 0
  printf("Thread id = %d, \tBarrier %s\n", omp_get_thread_num(), 
	 endpoint==1 ? "begin" : "end"
	);
#endif
  switch(kind) {
    case ompt_sync_region_barrier:
    case ompt_sync_region_barrier_implicit:
    case ompt_sync_region_barrier_explicit:
    case ompt_sync_region_barrier_implementation:
    case ompt_sync_region_taskwait:
    case ompt_sync_region_taskgroup:
      if (endpoint == ompt_scope_begin) ompt_idle_begin();
      else if (endpoint == ompt_scope_end) ompt_idle_end();
      else assert(0);
  default:
    break;
  }
}
#endif


//-------------------------------------------------
// accept any blame accumulated for mutex while 
// this thread held it
//-------------------------------------------------

static void 
ompt_mutex_blame_accept
(
 uint64_t mutex
)
{
  //printf("Lock has been realesed. \n");
  directed_blame_accept(&omp_mutex_blame_info, mutex);
}


//----------------------------------------------------------------------------
// initialization of OMPT interface by setting up callbacks
//----------------------------------------------------------------------------

static void 
init_threads
(
 void
)
{
  int retval;
  retval = ompt_set_callback_fn(ompt_callback_thread_begin,
		    (ompt_callback_t)ompt_thread_begin);
  assert(ompt_event_may_occur(retval));

  retval = ompt_set_callback_fn(ompt_callback_thread_end,
		    (ompt_callback_t)ompt_thread_end);
  assert(ompt_event_may_occur(retval));
}


static void 
init_parallel_regions
(
 void
)
{
  ompt_parallel_region_register_callbacks(ompt_set_callback_fn);
  ompt_regions_init();
}


static void 
init_tasks
(
 void
)
{
  ompt_task_register_callbacks(ompt_set_callback_fn);
}


static void
__attribute__ ((unused))
init_mutex_blame_shift
(
 const char *version
)
{
  int mutex_blame_shift_avail = 0;
  int retval = 0;
  // vi3: ompt_idle_blame_shift_request has been removed from
  // init_idle_blame_shift.
  // If this remains here and if hpcrun is called without -e, then
  // merge_metrics (ompt-defer.c). It seems that in this case some metrics
  // remained uninitialized.
  // ompt_mutex_blame_shift_request();

  if (!ompt_mutex_blame_requested) return;

  retval = ompt_set_callback_fn(ompt_callback_mutex_released,
                                (ompt_callback_t) ompt_mutex_blame_accept);
  mutex_blame_shift_avail |= ompt_event_may_occur(retval);


  if (mutex_blame_shift_avail) {
    ompt_mutex_blame_shift_register();
  } else {
    printf("hpcrun warning: OMP_MUTEX event requested, however the\n"
               "OpenMP runtime present (%s) doesn't support the\n"
               "events needed for MUTEX blame shifting. As a result\n"
               "OMP_MUTEX blame will not be monitored or reported.\n", version);
  }
}


//-------------------------------------------------
// register callbacks to support undirected blame
// shifting, namely attributing thread idleness to
// the work happening on other threads when the
// idleness occurs.
//-------------------------------------------------
static void
__attribute__ ((unused))
init_idle_blame_shift
(
 const char *version
)
{
  int idle_blame_shift_avail = 0;
#if 0
  int retval = 0;
#endif

  if (!ompt_idle_blame_requested) return;

#if 0
  ompt_idle_blame_shift_request();

  retval = ompt_set_callback_fn(ompt_callback_idle,
                                (ompt_callback_t)ompt_idle);
  idle_blame_shift_avail |= ompt_event_may_occur(retval);
#endif

#if 0
  retval = ompt_set_callback_fn(ompt_callback_sync_region_wait,
                                (ompt_callback_t)ompt_sync);
  idle_blame_shift_avail |= ompt_event_may_occur(retval);
#endif
  idle_blame_shift_avail = 1;

  if (idle_blame_shift_avail) {
    ompt_idle_blame_shift_register();
  } else {
    printf("hpcrun warning: OMP_IDLE event requested, however the\n"
             "OpenMP runtime present (%s) doesn't support the\n"
             "events needed for blame shifting idleness. As a result\n"
             "OMP_IDLE blame will not be monitored or reported.\n", version);
  }
}


bool
ompt_idle_blame_shift_enabled
(
  void
)
{
  return ompt_idle_blame_enabled;
}

//*****************************************************************************
// interface operations
//*****************************************************************************


//-------------------------------------------------
// ompt_initialize will be called by an OpenMP
// runtime system immediately after it initializes
// itself.
//-------------------------------------------------

static int
ompt_initialize
(
 ompt_function_lookup_t lookup,
 int initial_device_num,
 ompt_data_t *tool_data
)
{
  hpcrun_safe_enter();

#if OMPT_DEBUG_STARTUP
  printf("Initializing OMPT interface\n");
#endif

  ompt_initialized = 1;

  ompt_init_inquiry_fn_ptrs(lookup);
  ompt_init_placeholders();

  init_threads();
  init_parallel_regions();

  init_mutex_blame_shift(ompt_runtime_version);
  init_idle_blame_shift(ompt_runtime_version);

  char* ompt_task_full_ctxt_str = getenv("OMPT_TASK_FULL_CTXT");
  if (ompt_task_full_ctxt_str) {
    ompt_task_full_context = 
      strcmp("ENABLED", getenv("OMPT_TASK_FULL_CTXT")) == 0;
  } else{
    ompt_task_full_context = 0;
  }

  prepare_device();
  init_tasks();

#if DEBUG_TASK
  printf("Task full context: %s\n", ompt_task_full_context ? "yes" : "no");
#endif

  if (!ENABLED(OMPT_KEEP_ALL_FRAMES)) {
    ompt_elide = 1;
    ompt_callstack_init();
  }
  if (getenv("HPCRUN_OMP_SERIAL_ONLY")) {
    serial_only_sf_entry.fn = ompt_serial_only;
    serial_only_sf_entry.arg = 0;
    sample_filters_register(&serial_only_sf_entry);
  }

  hpcrun_safe_exit();

  return 1;
}


void
ompt_finalize
(
 ompt_data_t *tool_data
)
{
  hpcrun_safe_enter();

#if OMPT_DEBUG_STARTUP
  printf("Finalizing OMPT interface\n");
#endif

  hpcrun_safe_exit();
}


ompt_start_tool_result_t *
ompt_start_tool
(
 unsigned int omp_version,
 const char *runtime_version
)
{

 if (getenv("OMPT_DEBUG_WAIT")) {
    while (ompt_debug_wait);
 }
 
#if OMPT_DEBUG_STARTUP
  printf("Starting tool...\n");
#endif
  ompt_runtime_version = runtime_version;

  init.initialize = &ompt_initialize;
  init.finalize = &ompt_finalize;

  return &init;
}


int 
hpcrun_omp_state_is_overhead
(
 void
)
{
  if (ompt_initialized) {
    ompt_wait_id_t wait_id;
    ompt_state_t state = hpcrun_ompt_get_state(&wait_id);

    switch (state) {
    case ompt_state_overhead:
    case ompt_state_wait_critical:
    case ompt_state_wait_lock:
    case ompt_state_wait_atomic:
    case ompt_state_wait_ordered:
      return 1;

    default: 
      break;
    }
  }
  return 0;
}


//-------------------------------------------------
// returns true if OpenMP runtime frames should
// be elided out of thread callstacks
//-------------------------------------------------

int
hpcrun_ompt_elide_frames
(
 void
)
{
  return ompt_elide; 
}


//----------------------------------------------------------------------------
// safe (wrapped) versions of API functions that can be called from the rest
// of hpcrun, even if no OMPT support is available
//----------------------------------------------------------------------------


ompt_state_t
hpcrun_ompt_get_state
(
 uint64_t *wait_id
)
{
  if (ompt_initialized) return ompt_get_state_fn(wait_id);
  return ompt_state_undefined;
}

ompt_state_t
hpcrun_ompt_get_state_only
(
 void
)
{
  uint64_t wait_id;
  return hpcrun_ompt_get_state(&wait_id);
}


ompt_frame_t *
hpcrun_ompt_get_task_frame
(
 int level
)
{
  if (ompt_initialized) {
    int task_type_flags;
    ompt_data_t *task_data = NULL;
    ompt_data_t *parallel_data = NULL;
    ompt_frame_t *task_frame = NULL;
    int thread_num = 0;

    ompt_get_task_info_fn(level, &task_type_flags, &task_data, &task_frame, &parallel_data, &thread_num);
    //printf("Task frame pointer = %p\n", task_frame);
    return task_frame;
  }
  return NULL;
}


ompt_data_t*
hpcrun_ompt_get_task_data
(
 int level
)
{
  if (ompt_initialized){
    int task_type_flags;
    ompt_data_t *task_data = NULL;
    ompt_data_t *parallel_data = NULL;
    ompt_frame_t *task_frame = NULL;
    int thread_num = 0;

    ompt_get_task_info_fn(level, &task_type_flags, &task_data, &task_frame, &parallel_data, &thread_num);
    return task_data;
  }
  return (ompt_data_t*) ompt_data_none;
}


int
hpcrun_ompt_get_task_info
(
  int ancestor_level,
  int *flags,
  ompt_data_t **task_data,
  ompt_frame_t **task_frame,
  ompt_data_t **parallel_data,
  int *thread_num
) {

  int ret = -1;

  if (ompt_initialized) {
    // call runtime entry point
    ret = ompt_get_task_info_fn(ancestor_level, flags,
        task_data, task_frame, parallel_data, thread_num);
  }

  // Is there need to invalidate arguments passed by reference
  if (ret != 2) {
    // Standard:
    // "If no task region exists at the
    //  specified ancestor level or the information is
    //  unavailable then the values of variables passed by
    //  reference to the entry point are undefined"
    // I guess it is ok to invalidate them in order to detect some
    // of the edge cases.
    *flags = 0;  // invalid flag
    *task_data = NULL;
    *task_frame = NULL;
    *parallel_data = NULL;
    *thread_num = -1;  // invalid thread num
  }

  return ret;
}


typed_stack_elem(region) *
hpcrun_ompt_get_region_data_from_task_info
(
  int ancestor_level
)
{
  if (ompt_initialized){
    int task_type_flags;
    ompt_data_t *task_data = NULL;
    ompt_data_t *parallel_data = NULL;
    ompt_frame_t *task_frame = NULL;
    int thread_num = 0;

    int retVal = ompt_get_task_info_fn(ancestor_level, &task_type_flags, &task_data,
                          &task_frame, &parallel_data, &thread_num);
    if (retVal != 2) {
      return NULL;
    }

    if (task_type_flags & ompt_task_initial) {
      // TODO: think about this
      // initial task should not have corresponding region_data
      return NULL;
    }

    return ATOMIC_LOAD_RD(parallel_data) ? ATOMIC_LOAD_RD(parallel_data) : NULL;
  }
  return NULL;
}


void *
hpcrun_ompt_get_idle_frame
(
 void
)
{
  return NULL;
}


// new version

int hpcrun_ompt_get_parallel_info
(
 int ancestor_level,
 ompt_data_t **parallel_data,
 int *team_size
)
{
  if (ompt_initialized) {
    return ompt_get_parallel_info_fn(ancestor_level, parallel_data, team_size);
  }
  return 0;
}

uint64_t hpcrun_ompt_get_unique_id()
{
  if (ompt_initialized) return ompt_get_unique_id_fn();
  return 0;
}


uint64_t 
hpcrun_ompt_get_parallel_info_id
(
 int ancestor_level
)
{
  ompt_data_t *parallel_info = NULL;
  int team_size = 0;
  hpcrun_ompt_get_parallel_info(ancestor_level, &parallel_info, &team_size);
  if (parallel_info == NULL) return 0;
  return parallel_info->value;
//  typed_queue_elem(region)* region_data = (typed_queue_elem(region)*)parallel_info->ptr;
//  return region_data->region_id;
}


void 
hpcrun_ompt_get_parallel_info_id_pointer
(
 int ancestor_level, 
 uint64_t *region_id
)
{
  ompt_data_t *parallel_info = NULL;
  int team_size = 0;
  hpcrun_ompt_get_parallel_info(ancestor_level, &parallel_info, &team_size);
  if (parallel_info == NULL){
    *region_id = 0L;
  };
  *region_id =  parallel_info->value;
}


//-------------------------------------------------
// a special safe function that determines the
// outermost parallel region enclosing the current
// context.
//-------------------------------------------------

uint64_t
hpcrun_ompt_outermost_parallel_id
(
 void
)
{ 
  ompt_id_t outer_id = 0; 
  if (ompt_initialized) { 
    int i = 0;
    for (;;) {
      ompt_id_t next_id = hpcrun_ompt_get_parallel_info_id(i++);
      if (next_id == 0) break;
      outer_id = next_id;
    }
  }
  return outer_id;
}


void
ompt_mutex_blame_shift_request
(
  void
)
{
  ompt_mutex_blame_requested = 1;
  ompt_register_mutex_metrics();
}


void
ompt_idle_blame_shift_request
(
  void
)
{
  ompt_idle_blame_requested = 1;
  ompt_register_idle_metrics();
}


int 
ompt_task_full_context_p
(
  void
)
{
  return ompt_task_full_context;
}


// allocating and free notifications
typed_stack_elem_ptr(notification)
hpcrun_ompt_notification_alloc
(
 void
)
{
#if FREELISTS_ENABLED
#if FREELISTS_DEBUG
  // vi3 debug
  notification_used++;
#endif
  // only the current thread uses notification_freelist_head (thread_safe)
  // try to pop notification from the freelist
  typed_stack_elem_ptr(notification) first =
    typed_stack_pop(notification, sstack)(&notification_freelist_head);
  // allocate new notification if there's no any to reuse
  first = first ? first :
      (typed_stack_elem_ptr(notification))
          hpcrun_malloc(sizeof(typed_stack_elem(notification)));
  // invalidate values of notification's fields
  memset(first, 0, sizeof(typed_stack_elem(notification)));
  return first;
#else
  return (typed_stack_elem_ptr(notification))
      hpcrun_malloc(sizeof(typed_stack_elem(notification)));
#endif
}


void
hpcrun_ompt_notification_free
(
 typed_stack_elem_ptr(notification) notification
)
{
#if FREELISTS_ENABLED
  typed_stack_push(notification, cstack)(&notification_freelist_head,
                                         notification);
#if FREELISTS_DEBUG
  // vi3 debug
  notification_used--;
#endif
#endif
}


void
hpcrun_ompt_region_free
(
  typed_stack_elem_ptr(region) region_data
)
{
#if FREELISTS_ENABLED
#if FREELISTS_DEBUG
#if DEBUG_BARRIER_CNT
  // just debug
  int old = atomic_fetch_add(&region_data->barrier_cnt, 0);
  if (old >= 0) {
    printf("hpcrun_ompt_region_free >>> Region should be inactive: %d.\n", old);
  }
#endif
  atomic_fetch_sub(&region_data->owner_free_region_channel->region_used, 1);
#endif
#if KEEP_PARENT_REGION_RELATIONSHIP
  // disconnect from parent, otherwise the whole parent-child chain
  // will be added to freelist
  typed_stack_next_set(region, sstack)(region_data, 0);
#endif
  region_data->region_id = 0xdeadbead;
  typed_channel_shared_push(region)(region_data->owner_free_region_channel,
                                    region_data);
#endif
}


// vi3: Helper function to get region_data
typed_stack_elem_ptr(region)
hpcrun_ompt_get_region_data
(
 int ancestor_level
)
{
  ompt_data_t* parallel_data = NULL;
  int team_size;
  int ret_val = hpcrun_ompt_get_parallel_info(ancestor_level, &parallel_data, &team_size);
  // FIXME: potential problem if parallel info is unavailable and runtime returns 1
  if (ret_val < 2)
    return NULL;
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
  return parallel_data ? (typed_stack_elem_ptr(region))parallel_data->ptr : NULL;
#else
  return parallel_data ? ATOMIC_LOAD_RD(parallel_data) : NULL;
#endif
}


typed_stack_elem_ptr(region)
hpcrun_ompt_get_current_region_data
(
 void
)
{
  return hpcrun_ompt_get_region_data(0);
}


typed_stack_elem_ptr(region)
hpcrun_ompt_get_parent_region_data
(
 void
)
{
  return hpcrun_ompt_get_region_data(1);
}

bool
hpcrun_ompt_is_thread_region_owner
(
  typed_stack_elem(region) *region_data
)
{
  return region_data->owner_free_region_channel == &region_freelist_channel;
}


typed_stack_elem_ptr(region)
hpcrun_ompt_get_top_region_on_stack
(
  void
)
{
  typed_random_access_stack_elem(region) *top =
      typed_random_access_stack_top(region)(region_stack);
  return top ? top->region_data : NULL;
}


cct_node_t *
hpcrun_ompt_get_top_unresolved_cct_on_stack
(
  void
)
{
  // TODO FIXME vi3 >> put this boiler plate code in separate function
  typed_random_access_stack_elem(region) *top =
      typed_random_access_stack_top(region)(region_stack);
  return top ? top->unresolved_cct : NULL;
}


typed_random_access_stack_elem(region) *
get_corresponding_stack_element_if_any
(
  typed_stack_elem_ptr(region) region_data
)
{
  // corresponding stack element must be at index equal to depth of the region
  typed_random_access_stack_elem(region) *el =
      typed_random_access_stack_get(region)(region_stack, region_data->depth);
  // If thread took sample in region_data, then region_data should be contained
  // in el. Otherwise, return NULL as indication that thread didn't take sample
  // in region_data, so there's no stack element which corresponds to
  // region_data.
  return el->region_data == region_data ? el : NULL;
}


// FIXME vi3 >>> It seems that thread_num cam be 0 even though
//   thread wasn't part of the team (outer regions).
int
hpcrun_ompt_get_thread_num(int level)
{
  if (ompt_initialized) {
    int task_type_flags;
    ompt_data_t *task_data = NULL;
    ompt_data_t *parallel_data = NULL;
    ompt_frame_t *task_frame = NULL;
    int thread_num = 0;

    ompt_get_task_info_fn(level, &task_type_flags, &task_data, &task_frame, &parallel_data, &thread_num);
    //printf("Task frame pointer = %p\n", task_frame);
    return thread_num;
  }
  return -1;
}


ompt_set_result_t 
ompt_set_callback_internal
(
  ompt_callbacks_t event,
  ompt_callback_t callback
)
{
  return ompt_set_callback_fn(event, callback);
}


static ompt_state_t
check_state
(
  void
)
{
  uint64_t wait_id;
  return hpcrun_ompt_get_state(&wait_id);
}


// FIXME vi3: this doesn't belong to the OMPT interface
int
try_to_detect_the_case
(
  void
)
{
  int ancestor_level = 0;
  int flags;
  ompt_frame_t *frame;
  ompt_data_t *task_data = NULL;
  ompt_data_t *parallel_data = NULL;
  int thread_num = -1;
  int ret = hpcrun_ompt_get_task_info(ancestor_level, &flags, &task_data, &frame,
                                      &parallel_data, &thread_num);

  if (ret != 2 || !parallel_data) {
    // Check one level above
    ancestor_level++;
    ret = hpcrun_ompt_get_task_info(ancestor_level, &flags, &task_data, &frame,
                                    &parallel_data, &thread_num);
    if (ret != 2 || !parallel_data) {
      // FIXME vi3: Check if this assumption is valid.
      //   It is possible that information about the innermost task
      //   is not set properly. If at the same time the information
      //   about outer task is not available too, then don't try
      //   to handle this case.
      //   Consult the elider.
      return -1;
    }
  }

  if (!parallel_data) {
    printf("No region at this level? Out of bounds\n");
    return -2;
  }
  ompt_state_t current_state = check_state();
  if (current_state == ompt_state_wait_barrier_implicit_parallel) {
    // Waiting on the last implicit barrier.

    // FIXME vi3: This may be the barrier of the region on the level 0.
    //   I'm not sure whether to attribute this waiting to the region
    //   at level 1.
    if (thread_num != 0) {
      // Thread is the worker and is waiting on the barrier.
      // TODO vi3: Accumulate idleness.
      return -3;
    }
  } else if (current_state == ompt_state_idle) {
    if (thread_num != 0) {
      // FIXME VI3: I guesS it is more safe to omit registration process
      //  in this case?
      return -4;
    }
  }




  return ancestor_level;

}


// implement sequential stack of regions
typed_stack_impl(region, sstack);
// implement concurrent stack of regions
typed_stack_impl(region, cstack);
// channel of regions
typed_channel_impl(region);

// implement sequential stack of notifications
typed_stack_impl(notification, sstack);
// implement concurrent stack of notifications
typed_stack_impl(notification, cstack);
// implement channel of notifications
typed_channel_impl(notification);
// implement region stack
typed_random_access_stack_impl(region);

#if ENDING_REGION_MULTIPLE_TIMES_BUG_FIX
typed_random_access_stack_impl(runtime_region);
#endif
