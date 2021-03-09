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


//*****************************************************************************
// system includes
//*****************************************************************************

#include <assert.h>



//*****************************************************************************
// libmonitor
//*****************************************************************************

#include <monitor.h>



//*****************************************************************************
// local includes
//*****************************************************************************

#include <hpcrun/safe-sampling.h>
#include <hpcrun/thread_data.h>

#include "ompt-callback.h"
#include "ompt-callstack.h"
#include "ompt-defer.h"
#include "ompt-interface.h"
//#include "ompt-queues.h"
#include "ompt-region.h"
#include "ompt-region-debug.h"
#include "ompt-thread.h"
#include "ompt-task.h"


//*****************************************************************************
// forward declarations
//*****************************************************************************

static typed_stack_elem_ptr(region)
ompt_region_acquire
(
 void
);




//*****************************************************************************
// private operations
//*****************************************************************************


typed_stack_elem_ptr(region)
ompt_region_data_new
(
 uint64_t region_id, 
 cct_node_t *call_path
)
{
  typed_stack_elem_ptr(region) e = ompt_region_acquire();

  e->region_id = region_id;
  e->call_path = call_path;
  e->notification_stack = NULL;
  typed_stack_next_set(region, cstack)(e, 0);
  e->owner_free_region_channel = &region_freelist_channel;
  e->depth = 0;
#if VI3_PARALLEL_DATA_DEBUG == 1
  atomic_store(&e->process, 0);
  atomic_store(&e->registered, 0);
  atomic_store(&e->resolved, 0);
  e->master_channel = NULL;
#endif
  // debug
  ompt_region_debug_region_create(e);

#if DEBUG_BARRIER_CNT
  // FIXME vi3 >>> Check if this is right.
  atomic_exchange(&e->barrier_cnt, 0);
  int old_value = atomic_fetch_add(&e->barrier_cnt, 0);
  if (old_value != 0) {
    printf("************ barrier_cnt initialization error... The old value was: %d\n", old_value);
  }
#endif
  return e;
}


static void 
ompt_parallel_begin_internal
(
 ompt_data_t *parallel_data,
 int flags
)
{
  // region_id is used to log information about stack unwinding.
  // One way is to generate it, other way is to pass 0 as a dummy value instead.
  // uint64_t region_id = hpcrun_ompt_get_unique_id();
  uint64_t region_id = 0;
  // Collect and store region calling context.
  parallel_data->ptr = ompt_parallel_begin_context(region_id,
                              flags & ompt_parallel_invoker_program);
}


static void
ompt_parallel_end_internal
(
 ompt_data_t *parallel_data,    // data of parallel region
 int flags
)
{
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
  typed_stack_elem_ptr(region) region_data =
    (typed_stack_elem_ptr(region))parallel_data->ptr;
#else
  typed_stack_elem_ptr(region) region_data =
    (typed_stack_elem_ptr(region)) ATOMIC_LOAD_RD(parallel_data);
#endif

#if ENDING_REGION_MULTIPLE_TIMES_BUG_FIX == 1
  // Pop the innermost region in which thread is the master.
  typed_random_access_stack_elem(runtime_region) *runtime_top_el =
      typed_random_access_stack_pop(runtime_region)(runtime_master_region_stack);
  typed_stack_elem(region) *runtime_master_region = runtime_top_el->region_data;
  if (runtime_master_region != region_data) {
    // FIXME vi3 >>> runtime tries to end region_data another time
    //  Use the value we provided for now.
    region_data = runtime_master_region;
  }
#endif
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
  if (!ompt_eager_context_p()){
#else
  // if (!ompt_eager_context_p() && region_data){
  if (region_data){
#if VI3_PARALLEL_DATA_DEBUG == 1
    region_data->master_channel = &region_freelist_channel;
    atomic_fetch_add(&region_data->process, 1);
#endif
    // It is possible that region_data is not initialized, if none thread took
    // sample while region was active.
#endif
#if DEBUG_BARRIER_CNT
    // Debug only
    // Mark that this region is finished
    int old_value = atomic_fetch_add(&region_data->barrier_cnt, MAX_THREAD_IN_TEAM);
    if (old_value < 0) {
      msg_deferred_resolution_breakpoint(
          "ompt_parallel_end_internal: barrier_cnt value was negative before spin waiting.");
    }
    // Spin waiting until all worker finished with registering.
    for(;;) {

      old_value = atomic_fetch_add(&region_data->barrier_cnt, 0);
      if (old_value < MAX_THREAD_IN_TEAM) {
        msg_deferred_resolution_breakpoint(
            "ompt_parallel_end_internal: barrier_cnt value is under the minimum value of "
            "the barrier in the middle of sping waitingbefore spin waiting was negative");
      }
      // FIXME vi3 >>> the condition should be old_value == MAX_THREAD_IN_TEAM
      if (old_value <= MAX_THREAD_IN_TEAM) {
        if (old_value == MAX_THREAD_IN_TEAM) {
          break;
        } else {
          printf("UNDER THE LIMIT: %d\n", old_value);
        }
      }
    }
#endif
    // check if there is any thread registered that should be notified
    // that region call path is available
    typed_stack_elem_ptr(notification) to_notify =
      typed_stack_pop(notification, cstack)(&region_data->notification_stack);

    // mark that current region is ending
    ending_region = region_data;

    // FIXME vi3: need better way to decide whether you took sample or not
    //  compare region_id e.g
    typed_random_access_stack_elem(region) *stack_el =
        get_corresponding_stack_element_if_any(region_data);

    // NOTE: These two conditions should be equal:
    // 1. notification->unresolved_cct != NULL
    // 2. stack_el->took_sample

    // Region call path is missing.
    // Some thread from the team took a sample in the region.
    if (!region_data->call_path &&
          (stack_el || to_notify)) {
      // the region has not been provided before, so we will do that now
      region_data->call_path = ompt_region_context_eager(region_data->region_id, ompt_scope_end,
                               flags & ompt_parallel_invoker_program);
    }

    cct_node_t *region_prefix = region_data->call_path;
    if (to_notify){
      // store region prefix separately from region_data
      to_notify->region_prefix = region_prefix;
      // notify first thread in chain
      typed_channel_shared_push(notification)(to_notify->notification_channel, to_notify);
    } else {
      // No one registered, so free region_data by returning it to
      // the originator freelist.
      // Since none thread registered for the region, the master is the one
      // who created the region_data when had been processing the sample.
      // It should be safe to push region_data to the private stack of
      // region free channel.
      ompt_region_release(region_data);
    }

    // If master took a sample in this region, it needs to resolve its call path.
    if (stack_el) {
#if 1
      // CASE: thread took sample in an explicit task,
      // so we need to resolve everything under pseudo node
      resolve_one_region_context(region_prefix, stack_el->unresolved_cct);
      // mark that master resolved this region
      unresolved_cnt--;
#else
      //ompt_resolve_region_contexts_poll();
#endif
      // Since we never do real push and pop operations, it is possible that
      // this region_data will be reused by new region at the same depth.
      // In that case, thread could think that it already register for
      // the new region.
      // Invalidating value of stack_el->region_data will prevent this.
      stack_el->region_data = NULL;
    }

#if 1
    // Instead of popping in ompt_implicit_task_end, master of the region
    // will pop region here.
    typed_random_access_stack_pop(region)(region_stack);
#endif
    // mark that no region is ending
    ending_region = NULL;
  }

}



static void
ompt_parallel_begin
(
 ompt_data_t *parent_task_data,
 const ompt_frame_t *parent_frame,
 ompt_data_t *parallel_data,
 unsigned int requested_team_size,
 int flags,
 const void *codeptr_ra
)
{
  hpcrun_safe_enter();

  ompt_parallel_begin_internal(parallel_data, flags);

  hpcrun_safe_exit();
}


static void
ompt_parallel_end
(
 ompt_data_t *parallel_data,
 ompt_data_t *task_data,
 int flag,
 const void *codeptr_ra
)
{
  hpcrun_safe_enter();

#if 0
  uint64_t parallel_id = parallel_data->value;
  uint64_t task_id = task_data->value;

  ompt_data_t *parent_region_info = NULL;
  int team_size = 0;
  hpcrun_ompt_get_parallel_info(0, &parent_region_info, &team_size);
  uint64_t parent_region_id = parent_region_info->value;

  TMSG(DEFER_CTXT, "team end   id=0x%lx task_id=%x ompt_get_parallel_id(0)=0x%lx", parallel_id, task_id,
       parent_region_id);
#endif

  ompt_parallel_end_internal(parallel_data, flag);

  hpcrun_safe_exit();
}

#if USE_IMPLICIT_TASK_CALLBACKS == 1
static void
ompt_implicit_task_internal_begin
(
 ompt_data_t *parallel_data,
 ompt_data_t *task_data,
 unsigned int team_size,
 unsigned int index
)
{
  task_data->ptr = NULL;

#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
  typed_stack_elem_ptr(region) region_data =
    (typed_stack_elem_ptr(region))parallel_data->ptr;

#else
  typed_stack_elem_ptr(region) region_data =
    (typed_stack_elem_ptr(region)) ATOMIC_LOAD_RD(parallel_data);
#endif
  if (region_data == NULL) {
    // there are no parallel region callbacks for the initial task.
    // region_data == NULL indicates that this is an initial task. 
    // do nothing for initial tasks.
    return;
  }

  cct_node_t *prefix = region_data->call_path;

  // Only full call path can be memoized.
  if (ompt_eager_context_p()) {
    // region_depth is not important in this situation
    task_data_set_cct(task_data, prefix);
  }

  if (!ompt_eager_context_p()) {
    // connect task_data with region_data
    task_data_set_depth(task_data, region_data->depth);
#if DETECT_IDLENESS_LAST_BARRIER
    // mark that thread has finished waiting on the last implicit barrier
    // of the previous region
    waiting_on_last_implicit_barrier = false;
    // If any idle samples remained from the previous parallel region,
    // attribute them to the outermost context
    attr_idleness2outermost_ctx();
#endif
  }

#if VI3_DEBUG == 1
  printf("implicit_task_begin >>> REGION_STACK: %p, REG: %p, REG_ID: %lx, THREAD_NUM: %d\n",
         &region_stack, region_data, region_data->region_id, hpcrun_ompt_get_thread_num(0));
#endif
}


static void
ompt_implicit_task_internal_end
(
 ompt_data_t *parallel_data,
 ompt_data_t *task_data,
 unsigned int team_size,
 unsigned int index
)
{

  if (!ompt_eager_context_p()) {
    // try to resolve some regions, if any
    //ompt_resolve_region_contexts_poll();
#if VI3_DEBUG == 1
    typed_random_access_stack_elem(region) *top = typed_random_access_stack_top(region)(region_stack);
    if (top) {
      typed_stack_elem(region) *top_reg = top->notification->region_data;
      printf("implicit_task_end >>> REGION_STACK: %p, TOP_REG: %p, TOP_REG_ID: %lx, THREAD_NUM: %d\n",
             &region_stack, top_reg, top_reg->region_id, index);
    } else {
      printf("initial implicit_task_end >>> REGION_STACK: %p, TOP_REG: nil, TOP_REG_ID: nil, THREAD_NUM: %d\n",
             &region_stack, 0);
    }
#endif

  }
  // ompt_task_release(task_data);
}


void
ompt_implicit_task
(
 ompt_scope_endpoint_t endpoint,
 ompt_data_t *parallel_data,
 ompt_data_t *task_data,
 unsigned int team_size,
 unsigned int index,
 int flags
)
{
  if (flags == ompt_thread_initial && parallel_data == NULL)  {
    // implicit task for implicit parallel region. nothing to do here.
    return;
  }

  hpcrun_safe_enter();

  if (endpoint == ompt_scope_begin) {
    ompt_implicit_task_internal_begin(parallel_data, task_data, team_size, index);
  } else if (endpoint == ompt_scope_end) {
    ompt_implicit_task_internal_end(parallel_data, task_data, team_size, index);
  } else {
    // should never occur. should we add a message to the log?
  }

  hpcrun_safe_exit();
}
#endif

static typed_stack_elem_ptr(region)
ompt_region_alloc
(
 void
)
{
  typed_stack_elem_ptr(region) r =
    (typed_stack_elem_ptr(region)) hpcrun_malloc(sizeof(typed_stack_elem(region)));
  return r;
}


static typed_stack_elem_ptr(region)
ompt_region_freelist_get
(
 void
)
{
  typed_stack_elem_ptr(region) r =
    typed_channel_steal(region)(&region_freelist_channel);
  return r;
}


static void
ompt_region_freelist_put
(
 typed_stack_elem_ptr(region) r
)
{
#if FREELISTS_DEBUG
#if DEBUG_BARRIER_CNT
  // just debug
  int old = atomic_fetch_add(&r->barrier_cnt, 0);
  if (old >= 0) {
    printf("ompt_region_release >>> Region should be inactive: %d.\n", old);
  }
#endif
  atomic_fetch_sub(&r->owner_free_region_channel->region_used, 1);
#endif
#if KEEP_PARENT_REGION_RELATIONSHIP
  // disconnect from parent, otherwise the whole parent-child chain
  // will be added to freelist
  typed_stack_next_set(region, sstack)(r, 0);
#endif
  r->region_id = 0xdeadbeef;
  typed_channel_private_push(region)(&region_freelist_channel, r);
}


typed_stack_elem_ptr(region)
ompt_region_acquire
(
 void
)
{
#if FREELISTS_ENABLED
  typed_stack_elem_ptr(region) r = ompt_region_freelist_get();
  if (r == 0) {
    r = ompt_region_alloc();
    //ompt_region_debug_region_create(r);
  }
#if FREELISTS_DEBUG
  r->owner_free_region_channel = &region_freelist_channel;
  atomic_fetch_add(&r->owner_free_region_channel->region_used, 1);
#endif
  // invalidate previous content of all region_data's fields
  memset(r, 0, sizeof(typed_stack_elem(region)));
  return r;
#else
  return ompt_region_alloc();
#endif
}


void
ompt_region_release
(
 typed_stack_elem_ptr(region) r
)
{
  // mark that region is resolved
  ompt_region_debug_region_freed(r);
  assert(r->owner_free_region_channel == &region_freelist_channel);
  ompt_region_freelist_put(r);
}


#if DETECT_IDLENESS_LAST_BARRIER
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
#if VI3_DEBUG == 1
  if (kind == ompt_sync_region_barrier_implicit_last) {
    printf("ompt_sync_region_barrier_implicit_last: region_stack: %p, reg_id: %lx, Thread id = %d, \tBarrier %s\n",
        &region_stack, parallel_data ? ((typed_stack_elem_ptr(region))parallel_data->ptr)->region_id : 0,
        hpcrun_ompt_get_thread_num(0), endpoint==1?"begin":"end");
  } else if (kind == ompt_sync_region_barrier_implicit){
    printf("ompt_sync_region_barrier_implicit: parallel_data: %p, Thread id = %d, \tBarrier %s\n",
           parallel_data, hpcrun_ompt_get_thread_num(0), endpoint==1?"begin":"end");
  }
#endif

  // mark that thread is (not) waiting om last implicit barrier
  // at the end of the innermost parallel region
  if (kind == ompt_sync_region_barrier_implicit_last) {
    // thread starts waiting on the last implicit barrier
    if (endpoint == ompt_scope_begin) waiting_on_last_implicit_barrier = true;
  }
}
#endif

//*****************************************************************************
// interface operations
//*****************************************************************************

// initialize support for regions
void
ompt_regions_init
(
 void
)
{
  typed_channel_init(region)(&region_freelist_channel);
  ompt_region_debug_init();
}

#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 0
#include <tool/hpcrun/trace.h>
#endif

void 
ompt_parallel_region_register_callbacks
(
 ompt_set_callback_t ompt_set_callback_fn
)
{
  int retval;
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 0
  // FIXME ompt_eager_context is initialized inside ompt_callstack_init_deferred
  //   which is called after this function.
  //if (ompt_eager_context_p()) {
  if (hpcrun_trace_isactive()) {
#endif
    retval = ompt_set_callback_fn(ompt_callback_parallel_begin,
                                  (ompt_callback_t) ompt_parallel_begin);
    assert(ompt_event_may_occur(retval));
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 0
  }
#endif

  if (!hpcrun_trace_isactive()) {
    // Only lazy approach needs parallel end callback.
    retval = ompt_set_callback_fn(ompt_callback_parallel_end,
                                  (ompt_callback_t)ompt_parallel_end);
    assert(ompt_event_may_occur(retval));
  }

#if USE_IMPLICIT_TASK_CALLBACKS == 1
  retval = ompt_set_callback_fn(ompt_callback_implicit_task,
                                (ompt_callback_t)ompt_implicit_task);
  assert(ompt_event_may_occur(retval));
#endif
#if DETECT_IDLENESS_LAST_BARRIER
  retval = ompt_set_callback_fn(ompt_callback_sync_region_wait,
                                (ompt_callback_t)ompt_sync);
  assert(ompt_event_may_occur(retval));
#endif
}


int
initialize_region
(
  int level
)
{
#if 0
  ompt_data_t* parallel_data = NULL;
  int team_size;
  int ret_val =
      hpcrun_ompt_get_parallel_info(level, &parallel_data, &team_size);
#else
  int flags0;
  ompt_frame_t *frame0;
  ompt_data_t *task_data = NULL;
  ompt_data_t *parallel_data = NULL;
  int thread_num = -1;
  int ret_val = hpcrun_ompt_get_task_info(level, &flags0, &task_data, &frame0,
                            &parallel_data, &thread_num);
#endif
  assert(ret_val == 2);
  assert(parallel_data != NULL);

  if (flags0 & ompt_task_initial) {
    // ignore initial tasks
    return -1;
  }

  typed_stack_elem(region) *old_reg = ATOMIC_LOAD_RD(parallel_data);
  if (old_reg) {
    //printf("initialize_one_region >>> region_data initialized\n");
#if VI3_PARALLEL_DATA_DEBUG == 1
    assert(old_reg->parallel_data == parallel_data);
#endif
    return old_reg->depth;
  }

  // initialize parent region if needed
  int parent_depth = initialize_region(level + 1);
  // If there's no parent region, parent_depth will be -1.

  // try to initilize region_data
  typed_stack_elem(region) *new_reg =
      ompt_region_data_new(hpcrun_ompt_get_unique_id(), NULL);
  new_reg->depth = parent_depth + 1;

  #if VI3_PARALLEL_DATA_DEBUG == 1
  new_reg->parallel_data = parallel_data;
#endif

  if (!ATOMIC_CMP_SWP_RD(parallel_data, old_reg, new_reg)) {
#if VI3_PARALLEL_DATA_DEBUG == 1
    atomic_fetch_add(&new_reg->process, 1);
#endif
    // region_data has been initialized by other thread
    // free new_reg
    // It is safe to push to private stack of the region free channel.
    ompt_region_release(new_reg);
  } else {
    old_reg = new_reg;
  }
#if VI3_PARALLEL_DATA_DEBUG == 1
  assert(old_reg->parallel_data == parallel_data);
#endif
  return old_reg->depth;
}

// FIXME VI3: check whether all pointers stored in region_data exists
//   even after its creator has been destroyed
