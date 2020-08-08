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
// Copyright ((c)) 2002-2019, Rice University
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


static void
ompt_region_release
(
 typed_stack_elem_ptr(region) r
);


//*****************************************************************************
// private operations
//*****************************************************************************

#define MAX_THREAD_IN_TEAM -101

static typed_stack_elem_ptr(region)
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
  typed_stack_elem_ptr(region) region_data =
    ompt_region_data_new(hpcrun_ompt_get_unique_id(), NULL);
  parallel_data->ptr = region_data;

  uint64_t region_id = region_data->region_id;
  thread_data_t *td = hpcrun_get_thread_data();

  // FIXME vi3: check if this is right
  // the region has not been changed yet
  // that's why we say that the parent region is
  // hpcrun_ompt_get_current_region_data
  typed_stack_elem_ptr(region) parent_region =
    hpcrun_ompt_get_current_region_data();
  if (!parent_region) {
    // mark the master thread in the outermost region
    // (the one that unwinds to FENCE_MAIN)
    td->master = 1;
    region_data->depth = 0;
  } else {
    region_data->depth = parent_region->depth + 1;
  }

#if KEEP_PARENT_REGION_RELATIONSHIP
  // don't need for concurrency
  // memoized parent_region
  typed_stack_next_set(region, sstack)(region_data, parent_region);
#endif

#if ENDING_REGION_MULTIPLE_TIMES_BUG_FIX == 1
  // FIXME vi3 >>> Any good reason why I implemented push operation like this?
  // Push new region in which thread is master.
  typed_random_access_stack_elem(runtime_region) *runtime_top_el =
      typed_random_access_stack_push(runtime_region)(runtime_master_region_stack);
  runtime_top_el->region_data = region_data;
#endif

  if (ompt_eager_context_p()) {
     region_data->call_path =
       ompt_parallel_begin_context(region_id, 
				   flags & ompt_parallel_invoker_program);
  }
#if VI3_DEBUG == 1
  printf("parallel_begin >>> REGION_STACK: %p, REG: %p, REG_ID: %lx, THREAD_NUM: %d\n",
         &region_stack, region_data, region_data->region_id, 0);
#endif
}


static void
ompt_parallel_end_internal
(
 ompt_data_t *parallel_data,    // data of parallel region
 int flags
)
{
  typed_stack_elem_ptr(region) region_data =
    (typed_stack_elem_ptr(region))parallel_data->ptr;

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

  if (!ompt_eager_context_p()){
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
      typed_stack_pop(notification, sstack)(&region_data->notification_stack);

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

    // If master took a sample in this region, it needs to resolve its call path.
    if (stack_el) {
      // CASE: thread took sample in an explicit task,
      // so we need to resolve everything under pseudo node
      resolve_one_region_context(region_data, stack_el->unresolved_cct);
      // mark that master resolved this region
      unresolved_cnt--;
      // Since we never do real push and pop operations, it is possible that
      // this region_data will be reused by new region at the same depth.
      // In that case, thread could think that it already register for
      // the new region.
      // Invalidating value of stack_el->region_data will prevent this.
      stack_el->region_data = NULL;
    }

    if (to_notify){
      // notify next thread
      typed_channel_shared_push(notification)(to_notify->notification_channel, to_notify);
    } else {
      // if none, you can reuse region
      // this thread is region creator, so it could push to private stack of region channel
      ompt_region_release(region_data);
    }
#if 1
    // Instead of popping in ompt_implicit_task_end, master of the region
    // will pop region here.
    typed_random_access_stack_pop(region)(region_stack);
#endif
    // mark that no region is ending
    ending_region = NULL;
  }

  // FIXME: vi3: what is this?
  // FIXME: not using team_master but use another routine to
  // resolve team_master's tbd. Only with tasking, a team_master
  // need to resolve itself
  if (ompt_task_full_context_p()) {
    TD_GET(team_master) = 1;
    thread_data_t* td = hpcrun_get_thread_data();
    resolve_cntxt_fini(td);
    TD_GET(team_master) = 0;
  }

  // FIXME: vi3 do we really need to keep this line
  hpcrun_get_thread_data()->region_id = 0;
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
  printf("Callbacks have been registered\n");
  // ompt_parallel_begin_internal(parallel_data, flags);

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

  // ompt_parallel_end_internal(parallel_data, flag);

  hpcrun_safe_exit();
}


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

  typed_stack_elem_ptr(region) region_data =
    (typed_stack_elem_ptr(region))parallel_data->ptr;

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
#if 0
    add_region_and_ancestors_to_stack(region_data, index==0);
    task_data_set_depth(task_data,
        typed_random_access_stack_top_index_get(region)(region_stack));
#endif
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
    ompt_resolve_region_contexts_poll();
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

#if 0
    if (index != 0) {
      // Need to check if current thread took a sample in the innermost region.
      if (!top) {
        // Do nothing
        return;
      }
      // resolve region call path
      if (top->took_sample) {
        //resolve_one_region_context_vi3(top->notification);
      }
      // =========================================== A bit newer version
      // Pop region from the stack, if thread is not the master of this region.
      // Master thread will pop in ompt_parallel_end callback
      typed_random_access_stack_pop(region)(region_stack);
    }
#endif
  }
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
  // if (flags == ompt_thread_initial && parallel_data == NULL)  {
  //   // implicit task for implicit parallel region. nothing to do here.
  //   return;
  // }

  hpcrun_safe_enter();

  // if (endpoint == ompt_scope_begin) {
  //   ompt_implicit_task_internal_begin(parallel_data, task_data, team_size, index);
  // } else if (endpoint == ompt_scope_end) {
  //   ompt_implicit_task_internal_end(parallel_data, task_data, team_size, index);
  // } else {
  //   // should never occur. should we add a message to the log?
  // }

  hpcrun_safe_exit();
}


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
    ompt_region_debug_region_create(r);
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


static void
ompt_region_release
(
 typed_stack_elem_ptr(region) r
)
{
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

void 
ompt_parallel_region_register_callbacks
(
 ompt_set_callback_t ompt_set_callback_fn
)
{
  int retval;
  retval = ompt_set_callback_fn(ompt_callback_parallel_begin,
                    (ompt_callback_t)ompt_parallel_begin);
  assert(ompt_event_may_occur(retval));

  retval = ompt_set_callback_fn(ompt_callback_parallel_end,
                    (ompt_callback_t)ompt_parallel_end);
  assert(ompt_event_may_occur(retval));

  retval = ompt_set_callback_fn(ompt_callback_implicit_task,
                                (ompt_callback_t)ompt_implicit_task);
  assert(ompt_event_may_occur(retval));

#if DETECT_IDLENESS_LAST_BARRIER
  retval = ompt_set_callback_fn(ompt_callback_sync_region_wait,
                                (ompt_callback_t)ompt_sync);
  assert(ompt_event_may_occur(retval));
#endif
}
