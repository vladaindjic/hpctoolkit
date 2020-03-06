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
// johnmc merge
#if 0
#include "ompt-parallel-region-map.h"
#endif

#include <hpcrun/unresolved.h>
#include <hpcrun/utilities/timer.h>

#include "ompt-callstack.h"
#include "ompt-defer.h"
#include "ompt-interface.h"
//#include "ompt-queues.h"
#include "ompt-region-debug.h"
#include "ompt-placeholders.h"
#include "ompt-thread.h"
#include "ompt-region.h"
#include "ompt-task.h"



//*****************************************************************************
// macros
//*****************************************************************************

#define DEFER_DEBUGGING 0

// vi3: used to memoize the call path that corresponds to the
// user level functions before eventual extension with
// cct nodes that correspond to the kernel functions
__thread cct_node_t *cct_path_before_kernel_extension = NULL;

//*****************************************************************************
// private operations
//*****************************************************************************

//
// TODO: add trace correction info here
// FIXME: merge metrics belongs in a different file. it is not specific to 
// OpenMP
//
static void
merge_metrics
(
 cct_node_t *a, 
 cct_node_t *b, 
 merge_op_arg_t arg
)
{
  // if nodes a and b are the same, no need to merge
  if (a == b) return;

  metric_data_list_t* mset_a = hpcrun_get_metric_data_list(a);
  metric_data_list_t* mset_b = hpcrun_get_metric_data_list(b);

  if (!mset_a || !mset_b) return;

  int num_kind_metrics = hpcrun_get_num_kind_metrics();
  for (int i = 0; i < num_kind_metrics; i++) {
    cct_metric_data_t *mdata_a = hpcrun_metric_set_loc(mset_a, i);
    cct_metric_data_t *mdata_b = hpcrun_metric_set_loc(mset_b, i);

    // FIXME: this test depends upon dense metric sets. sparse metrics
    //        should ensure that node a has the result
    if (!mdata_a || !mdata_b) continue;

    metric_desc_t *mdesc = hpcrun_id2metric(i);
    switch(mdesc->flags.fields.valFmt) {
    case MetricFlags_ValFmt_Int:  mdata_a->i += mdata_b->i; break;
    case MetricFlags_ValFmt_Real: mdata_a->r += mdata_b->r; break;
    default:
      TMSG(DEFER_CTXT, "in merge_op: what's the metric type");
      monitor_real_exit(1);
    }
  }
}


#if 0
static void
deferred_resolution_breakpoint
(
)
{
  // set a breakpoint here to identify problematic cases
  printf("I guess you did something wrong!\n");
}
#endif


void
msg_deferred_resolution_breakpoint
(
  char *msg
)
{
  printf("********** %s\n", msg);
}


static void
omp_resolve
(
 cct_node_t* cct, 
 cct_op_arg_t a, 
 size_t l
)
{
  return;
  cct_node_t *prefix;
  thread_data_t *td = (thread_data_t *)a;
  uint64_t my_region_id = (uint64_t)hpcrun_cct_addr(cct)->ip_norm.lm_ip;

  TMSG(DEFER_CTXT, "omp_resolve: try to resolve region 0x%lx", my_region_id);
  uint64_t partial_region_id = 0;
  prefix = hpcrun_region_lookup(my_region_id);
  if (prefix) {
    TMSG(DEFER_CTXT, "omp_resolve: resolve region 0x%lx to 0x%lx", my_region_id, is_partial_resolve(prefix));
    // delete cct from its original parent before merging
    hpcrun_cct_delete_self(cct);
    TMSG(DEFER_CTXT, "omp_resolve: delete from the tbd region 0x%lx", hpcrun_cct_addr(cct)->ip_norm.lm_ip);

    partial_region_id = is_partial_resolve(prefix);

    if (partial_region_id == 0) {
#if 1
      cct_node_t *root = ompt_region_root(prefix);
      prefix = hpcrun_cct_insert_path_return_leaf(root,  prefix);
#else
      prefix = hpcrun_cct_insert_path_return_leaf
	((td->core_profile_trace_data.epoch->csdata).tree_root, prefix);
#endif
    }
    else {
      prefix = hpcrun_cct_insert_path_return_leaf
	((td->core_profile_trace_data.epoch->csdata).unresolved_root, prefix);

      //FIXME: Get region_data_t and update refcnt
//      ompt_region_map_refcnt_update(partial_region_id, 1L);

      TMSG(DEFER_CTXT, "omp_resolve: get partial resolution to 0x%lx\n", partial_region_id);
    }
    // adjust the callsite of the prefix in side threads to make sure they are the same as
    // in the master thread. With this operation, all sides threads and the master thread
    // will have the unified view for parallel regions (this only works for GOMP)
    if (td->team_master) {
      hpcrun_cct_merge(prefix, cct, merge_metrics, NULL);
    }
    else {
      hpcrun_cct_merge(prefix, cct, merge_metrics, NULL);
      // must delete it when not used considering the performance
      TMSG(DEFER_CTXT, "omp_resolve: resolve region 0x%lx", my_region_id);

      //ompt_region_map_refcnt_update(my_region_id, -1L);
    }
  }
}


static void
omp_resolve_and_free
(
 cct_node_t* cct, 
 cct_op_arg_t a, 
 size_t l
)
{
  return;
  omp_resolve(cct, a, l);
}


int
need_defer_cntxt
(
 void
)
{
  if (ENABLED(OMPT_LOCAL_VIEW)) return 0;

  // master thread does not need to defer the context
  if ((hpcrun_ompt_get_parallel_info_id(0) > 0) && !TD_GET(master)) {
    thread_data_t *td = hpcrun_get_thread_data();
    td->defer_flag = 1;
    return 1;
  }
  return 0;
}


uint64_t
is_partial_resolve
(
 cct_node_t *prefix
)
{
  //go up the path to check whether there is a node with UNRESOLVED tag
  cct_node_t *node = prefix;
  while (node) {
    cct_addr_t *addr = hpcrun_cct_addr(node);
    if (IS_UNRESOLVED_ROOT(addr))
      return (uint64_t)(addr->ip_norm.lm_ip);
    node = hpcrun_cct_parent(node);
  }
  return 0;
}


//-----------------------------------------------------------------------------
// Function: resolve_cntxt
// 
// Purpose: 
//   resolve contexts of parallel regions.
//
// Description:
// (1) Compute the outer-most region id; only the outer-most region needs to be 
//     resolved
// (2) If the thread has a current region id that is different from its previous 
//     one; and the previous region id is non-zero, resolve the previous region.
//     The previous region id is recorded in td->region_id.
// (3) If the thread has a current region id that is different from its previous
//     one; and the current region id is non-zero, add a slot into the 
//     unresolved tree indexed by the current region_id
// (4) Update td->region_id to be the current region id

void 
resolve_cntxt
(
 void
)
{
  return;
  cct_node_t* tbd_cct = (hpcrun_get_thread_epoch()->csdata).unresolved_root;
  thread_data_t *td = hpcrun_get_thread_data();

  //---------------------------------------------------------------------------
  // step 1:
  //
  // a pure worker thread's outermost region is the same as its innermost region.
  //
  // a sub-master thread's outermost region was memoized when the thread became
  // a sub-master. the identity of the sub-master's outermost region in this case is
  // available in td->outer_region_id.
  //
  // the post condition for the following code is that outer_region_id contains
  // the outermost region id in the current thread.
  //---------------------------------------------------------------------------

  uint64_t innermost_region_id = hpcrun_ompt_get_parallel_info_id(0);
  uint64_t outer_region_id = 0;

  if (td->outer_region_id > 0) {
    uint64_t enclosing_region_id = hpcrun_ompt_get_parallel_info_id(1);
    if (enclosing_region_id == 0) {
      // we are currently in a single level parallel region.
      // forget the previously memoized outermost parallel region.
      td->outer_region_id = 0;
    } else {
      outer_region_id = td->outer_region_id; // outer region for submaster
    }
  }

  // if outer_region_id has not already been set, it defaults to the innermost
  // region.
  if (outer_region_id == 0) {
    outer_region_id = innermost_region_id;
  }

  TMSG(DEFER_CTXT, "resolve_cntxt: outermost region id is 0x%lx, "
       "innermost region id is 0x%lx", outer_region_id, innermost_region_id);

  //---------------------------------------------------------------------------
  // step 2:
  //
  // if we changed parallel regions, try to resolve contexts for past regions.
  //---------------------------------------------------------------------------
  if (td->region_id != 0) {
    // we were in a parallel region when the last sample was received
    if (td->region_id != outer_region_id) {
      // the region we are in now (if any) differs from the region where
      // the last sample was received.
      TMSG(DEFER_CTXT, "exited region 0x%lx; attempting to resolve contexts", td->region_id);
      hpcrun_cct_walkset(tbd_cct, omp_resolve_and_free, td);
    }
  }

  //
  // part 3: insert the current region into unresolved tree (tbd tree)
  //
  // update the use count when come into a new omp region
  if ((td->region_id != outer_region_id) && (outer_region_id != 0)) {
    // end_team_fn occurs at master thread, side threads may still
    // in a barrier waiting for work. Now the master thread may delete
    // the record if no samples taken in it. But side threads may take
    // samples in the waiting region (which has the same region id with
    // the end_team_fn) after the region record is deleted.
    // solution: consider such sample not in openmp region (need no
    // defer cntxt)


    // FIXME: Accomodate to region_data_t
    // FIXME: Focus on this
    // (ADDR2(UNRESOLVED, get from region_data)
    // watch for monitoring variables

//    if (ompt_region_map_refcnt_update(outer_region_id, 1L))
//      hpcrun_cct_insert_addr(tbd_cct, &(ADDR2(UNRESOLVED, outer_region_id)));
//    else
//      outer_region_id = 0;

  }

#ifdef DEBUG_DEFER
  int initial_td_region = td->region_id;
#endif

  //
  // part 4: update the td->region_id, indicating the region where this thread
  // most-recently takes a sample.
  //
  // td->region_id represents the out-most parallel region id
  td->region_id = outer_region_id;

#ifdef DEBUG_DEFER
  // debugging code
  if (innermost_region_id) {
    ompt_region_map_entry_t *record = ompt_region_map_lookup(innermost_region_id);
    if (!record || (ompt_region_map_entry_refcnt_get(record) == 0)) {
      EMSG("no record found innermost_region_id=0x%lx initial_td_region_id=0x%lx td->region_id=0x%lx ",
	   innermost_region_id, initial_td_region, td->region_id);
    }
  }
#endif
}


void
resolve_cntxt_fini
(
 thread_data_t *td
)
{
  //printf("Resolving for thread... region = %d\n", td->region_id);
  //printf("Root children = %p\n", td->core_profile_trace_data.epoch->csdata.unresolved_root->children);
  hpcrun_cct_walkset(td->core_profile_trace_data.epoch->csdata.unresolved_root,
                     omp_resolve_and_free, td);
}


cct_node_t *
hpcrun_region_lookup
(
 uint64_t id
)
{
  cct_node_t *result = NULL;

  // FIXME: Find another way to get infor about parallel region

//  ompt_region_map_entry_t *record = ompt_region_map_lookup(id);
//  if (record) {
//    result = ompt_region_map_entry_callpath_get(record);
//  }

  return result;
}

// added by vi3

typed_stack_elem_ptr(notification)
help_notification_alloc
(
 typed_stack_elem_ptr(region) region_data
)
{
  typed_stack_elem_ptr(notification) notification = hpcrun_ompt_notification_alloc();
  notification->region_data = region_data;
  notification->region_id = region_data->region_id;
  notification->notification_channel = &thread_notification_channel;
  // reset unresolved_cct for the new notification
  notification->unresolved_cct = NULL;

  return notification;
}


void
swap_and_free
(
 typed_stack_elem_ptr(region) region_data
)
{
  int depth = region_data->depth;
  typed_random_access_stack_elem(region) *stack_element =
      typed_random_access_stack_get(region)(region_stack, depth);
  typed_stack_elem_ptr(notification) notification = stack_element->notification;

  // Notification can be freed either if the thread is the master of the region
  // or the thread did not take a sample inside the region
  if (notification && (stack_element->team_master || !stack_element->took_sample)) {
    //hpcrun_ompt_notification_free(notification);
  }
  // get and store new notification on the stack
  notification = help_notification_alloc(region_data);
  stack_element->notification = notification;
  // invalidate value of team_master,
  // will be set in function add_region_and_ancestors_to_stack
  stack_element->team_master = 0;
  // thread hasn't taken a sample in this region yet
  stack_element->took_sample = 0;
  // cache region
}


bool
add_one_region_to_stack
(
  typed_random_access_stack_elem(region) *el,
  void *arg
)
{
  int *region_level = (int *) arg;

  // get information about region at the specified level (region_level)
  typed_stack_elem(region) *region_data = hpcrun_ompt_get_region_data(*region_level);

  // Check if region was previously added on the stack.
  if (region_data->depth <= typed_random_access_stack_top_index_get(region)(region_stack)
      && region_data->region_id == el->notification->region_id) {
    // el->notification->region_data represents the region that is
    // at this moment on the stack of active regions
    // (el is element of stack that is being processed at the moment).
    // If it's (el->notification->region_data) id is equal to id of
    // "region (region_data) at the region_level",
    // stop further processing of stack elements.
    // Stack is updated properly.
    return 1;
  }

  // If previous condition fails, the stack is outdated and we need
  // to update region which is contained in el variable, by calling below function,
  // which will add corresponding notification on stack.
  swap_and_free(region_data);

  // update region level (one region above)
  (*region_level)++;

  // Continue with updating of stack of active regions
  return 0;
}

void
add_region_and_ancestors_to_stack
(
 typed_stack_elem_ptr(region) region_data,
 bool team_master
)
{
  if (!region_data) {
    msg_deferred_resolution_breakpoint("add_region_and_ancestors_to_stack: Region data is missing\n");
    return;
  }

  int level = 0;
  // get the depth of the innermost region
  int depth = hpcrun_ompt_get_region_data(level)->depth;

  // Add active regions on stack starting from the innermost.
  // Stop when one of the following conditions is satisfied:
  // 1. all active regions are added to stack
  // 2. encounter on region that is active and that was previously added to the stack
  typed_random_access_stack_iterate_from(region)(depth, region_stack, add_one_region_to_stack, &level);

  // region_data is the new top of the stack
  typed_random_access_stack_top_index_set(region)(region_data->depth, region_stack);

  // NOTE vi3: This should be right
  // If the stack content does not corresponds to ancestors of the region_data,
  // then thread could only be master of the region_data, but not to its ancestors.

  // Values of argument team_master says if the thread is the master of region_data
  typed_random_access_stack_top(region)(region_stack)->team_master = team_master;
}


void
register_to_region
(
 typed_stack_elem_ptr(notification) notification
)
{
  typed_stack_elem_ptr(region) region_data = notification->region_data;

  ompt_region_debug_notify_needed(notification);

  // register thread to region's wait free queue
  typed_stack_next_set(notification, cstack)(notification, 0);

  int old_value = atomic_fetch_add(&region_data->barrier_cnt, 0);
  if (old_value < 0) {
    printf("register_to_region >>> To late to try registering. Old value: %d\n", old_value);
  }

  typed_stack_push(notification, cstack)(&region_data->notification_stack, notification);

  old_value = atomic_fetch_add(&region_data->barrier_cnt, 0);
  if (old_value < 0) {
    printf("register_to_region >>> To late, but registered. Old value: %d\n", old_value);
  }

  // increment the number of unresolved regions
  unresolved_cnt++;
}


bool
thread_take_sample
(
  typed_random_access_stack_elem(region) *el,
  void *arg
)
{
  cct_node_t **parent_cct_ptr = (cct_node_t **)arg;
  // pseudo cct node which corresponds to the parent region
  cct_node_t *parent_cct = *parent_cct_ptr;
  // notification that corresponds to the active region that is being processed at the moment
  typed_stack_elem(notification) *notification = el->notification;

  // FIXME vi3 >>> If we use this approach, then field took_sample is not needed
  // thread took a sample in this region before
  if (el->took_sample) {
    // skip this region, and go to the inner one
    goto return_label;
  }

  // Check if the address that corresponds to the region
  // has already been inserted in parent_cct subtree.
  cct_addr_t *region_addr = &ADDR2(UNRESOLVED,notification->region_data->region_id);
  cct_node_t *found_cct = hpcrun_cct_find_addr(parent_cct, region_addr);
  if (found_cct) {
    // Region was on the stack previously, so the thread does not need
    // neither to insert cct nor to register itself for the region's call path.
    // Mark that thread took a sample, store found_cct in notification and skip the region.
    el->took_sample = true;
    notification->unresolved_cct = found_cct;
    goto return_label;
  }

  if (!el->team_master) {
    register_to_region(notification);
  } else {
    // Master thread also needs to resolve this region at the very end (ompt_parallel_end)
    unresolved_cnt++;
  }

  // mark that thread took sample in this region for the first time
  el->took_sample = true;
  // insert cct node with region_addr in the parent_cct's subtree
  notification->unresolved_cct = hpcrun_cct_insert_addr(parent_cct, region_addr);

  return_label: {
    // store new parent_cct node for the inner region
    *parent_cct_ptr = notification->unresolved_cct;

    // If region is marked as last to register (see function register_to_all_regions),
    // then stop processing regions here.
    if (vi3_forced_diff) {
      if (el->notification->region_data->depth >= vi3_last_to_register) {
        // Indication that processing of active regions should stop.
        return 1;
      }
    }

    // Indication that processing of active regions should continue
    return 0;
  };
}


bool
least_common_ancestor
(
  typed_random_access_stack_elem(region) *el,
  void *arg
)
{
  int *level_ptr = (int *) arg;
  int level = *level_ptr;

  typed_stack_elem(region) *runtime_reg = hpcrun_ompt_get_region_data(level);
  typed_stack_elem(region) *stack_reg = el->notification->region_data;

  if (!runtime_reg) {
    // FIXME vi3 >>> debug this case
    goto return_label;
  }

  if (stack_reg->depth > runtime_reg->depth) {
    // stack_reg is probably not active, so skip it
    return 0;
  } else if (stack_reg->depth < runtime_reg->depth) {
    // there are fewer regions on the stack than in runtime
    // try to find corresponding
    level = runtime_reg->depth - stack_reg->depth;
    runtime_reg = hpcrun_ompt_get_region_data(level);
  }

  if (!runtime_reg) {
    goto return_label;
  }

  // NOTE: debugging only
  if (stack_reg->depth != runtime_reg->depth) {
    msg_deferred_resolution_breakpoint("Depths of stack_reg and runtime_reg are different\n");
  }

  // NOTE: debugging only
  if (stack_reg != runtime_reg) {
    msg_deferred_resolution_breakpoint("Stack_reg and runtime_reg are different\n");
  }

  // thread has already taken sample inside this region
  if (el->notification->unresolved_cct) {
    // least common ancestor is found
    return 1;
  }

  return_label:
  // continue processing outer regions that are present on stack
  *level_ptr = level + 1;

  return 0;
}


void
register_to_all_regions
(
  void
)
{
  // If there is no regions on the stack, just return.
  // Thread should be executing sequential code.
  bool stack_is_empty = typed_random_access_stack_empty(region)(region_stack);
  if (stack_is_empty)
    return;
#if 0
  // NOTE vi3: The code that explain a lot of edge cases

  // Check if the innermost parallel_data is available
  typed_stack_elem(region) *inner = hpcrun_ompt_get_region_data(0);
  if (!inner) {
    // Innermost parallel_data is unavailable
    // Ex worker should be waiting on the last implicit barrier for more work.
    // It cannot be guaranteed that any of the region on the stack is still active,
    // so avoid registering for any of theirs call paths.
    vi3_forced_null = true;
    return;
  }

  // check if the innermost region_data on the stack is equal to region_data
  // provided by runtime at the level 0
  typed_random_access_stack_elem(region) *top = typed_random_access_stack_top(region)(region_stack);
  typed_stack_elem(region) *top_reg = top->notification->region_data;
  if (top_reg != inner) {
    // mark that runtime changed innermost region data before ompt_implicit_task_end/parallel_end
    vi3_forced_diff = true;

    if (top_reg->depth < inner->depth) {
      // Worker thread tries to create new parallel region in which it is going to be the master.
      // Thread should mark that it took sample in all regions that are currently presented the stack.
      // Just to be sure, we will memoize the innermost region's depth as the last element
      // on the stack that thread_take_sample function will process later.
      vi3_last_to_register = top_reg->depth;
    } else if (top_reg->depth > inner->depth) {
      // Ex master thread has recently finished the region.
      // Thread should mark that it took sample in all regions on the stack
      // except the one on the top of the stack (top_reg), which has been recently finished.
      // The last region that function thread_take_sample will process is
      // at index inner->depth on the stack.
      vi3_last_to_register = inner->depth;

      // This is just for debugging purposes.
      // It should be expected that inner is still on the stack at index inner->depth.
      if (typed_random_access_stack_get(region)(region_stack,
          inner->depth)->notification->region_data != inner) {
        // this happened once
        // call stack
        // __ompt_implicit_task_end
        // __kmp_fork_barrier
        // __kmp_launch_thread
        // __kmp_launch_worker
        // task_data = {value = 0x0, ptr = 0x0}
        // info_type = 2 (call path of sample will be put under thread root => see ompt_cct_cursor_finalize)
        // omp_task_context = NULL
        // region_depth = -1

        // just for debug purposes
        cct_node_t *omp_task_context = NULL;
        int region_depth = -1;
        int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context), &omp_task_context, &region_depth);
        if (info_type != 2) {
          printf(">>> INFO_TYPE: %d, TASK_CONTEXT: %p, REGION_DEPTH: %d\n",
              info_type, omp_task_context, region_depth);
          msg_deferred_resolution_breakpoint(
              "register_to_all_regions >>> INNER is not on the stack.\n");
        }
        // It seems like the thread was worker in region that was finished.
        // Now it is waiting for work.
        // I guess that safest thing to do is to skip registering.
        return;
      }
    } else {
      // Some example of the frames found in debugger
      // __ompt_implicit_task_end / sched_yield
      // __kmp_fork_barrier
      // __kmp_launch_worker
      // __kmp_launch_thread
      // It seems like that this case is similiar to case
      // when innermost parallel_data is unavailable.
      return;
    }

  }

  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context), &omp_task_context, &region_depth);
  if (info_type == 2) {
    // task_data does not contain any useful information (task_data = {0x0}
    // Thread cannot connect sample with any regions on the stack.
    // skip registering
    return;
  }
#endif

  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context), &omp_task_context, &region_depth);
  if (info_type == 2) {
    // task_data does not contain any useful information (task_data = {0x0}
    // Thread cannot connect sample with any regions on the stack.
    // skip registering
    // NOTE: For more clarification about edge cases, check commented code above
    return;
  }

  typed_stack_elem(region) *inner = hpcrun_ompt_get_region_data(0);
  // If task_data contains useful information, then parallel data at level 0 must be available
  // This code is for debug purposes only
  if (!inner) {
    msg_deferred_resolution_breakpoint("Task data available, but parallel data isn't");
    return;
  }

  typed_random_access_stack_elem(region) *top = typed_random_access_stack_top(region)(region_stack);
  typed_stack_elem(region) *top_reg = top->notification->region_data;
  if (top_reg->depth > inner->depth) {
    // Region at the top of the stack (top_reg) is deeper than
    // the region provided by the runtime (inner).
    // That means that top_reg is probably finished, but corresponding
    // end callback (implicit_task_end/parallel_end) has not been called yet.
    // This means that thread should avoid registering for the top_reg's call path
    // (and potentially all regions which depths are greater than inner->depth)
    vi3_last_to_register = inner->depth;
  }

  // Mark that thread took sample in regions presented on the stack, eventually
  // avoid regions which depths are greater that vi3_last_to_register.
  // Initial idea was to process all regions present on the stack.
  // To save some time, it is possible to find the region in which thread
  // has already taken sample and process only nested regions.
  // In order to find that region, apply least_common_ancestor function
  // on each region present on the stack

  int level = 0;
  typed_random_access_stack_elem(region) *lca = typed_random_access_stack_forall(region)
      (region_stack, least_common_ancestor, &level);
  // If aforementioned region (least common ancestor, lca_reg shorter) exists,
  // then use its corresponding unresolved cct node
  // as parent_cct and process regions which depths are >= lca->depth + 1.
  // Otherwise, process all regions present on the stack and use
  // thread_root as initial parent_cct.

  int start_from = 0;
  cct_node_t *parent_cct = hpcrun_get_thread_epoch()->csdata.thread_root;
  if (lca) {
    typed_stack_elem(region) *lca_reg = lca->notification->region_data;
    start_from = lca_reg->depth + 1;
    parent_cct = lca->notification->unresolved_cct;
  }
  typed_random_access_stack_reverse_iterate_from(region)(start_from, region_stack, thread_take_sample, &parent_cct);
}


void
resolve_one_region_context
(
  typed_stack_elem_ptr(notification) notification
)
{
  // region to resolve
  typed_stack_elem_ptr(region) region_data = notification->region_data;
  cct_node_t *unresolved_cct = notification->unresolved_cct;
  cct_node_t *parent_unresolved_cct = hpcrun_cct_parent(unresolved_cct);

  if (parent_unresolved_cct == NULL) {
    msg_deferred_resolution_breakpoint("resolve_one_region_context: Parent of unresolved_cct node is missing\n");
  }
  else if (region_data->call_path == NULL) {
    printf("Region call path is missing\n");
    msg_deferred_resolution_breakpoint("resolve_one_region_context: Region call path is missing\n");
  } else {
    // prefix should be put between unresolved_cct and parent_unresolved_cct
    cct_node_t *prefix = NULL;
    cct_node_t *region_call_path = region_data->call_path;

    // FIXME: why hpcrun_cct_insert_path_return_leaf ignores top cct of the path
    prefix = hpcrun_cct_insert_path_return_leaf(parent_unresolved_cct, region_call_path);

    if (prefix == NULL) {
      msg_deferred_resolution_breakpoint("resolve_one_region_context: Prefix is not properly inserted\n");
    }

    if (prefix != unresolved_cct) {
      // prefix node should change the unresolved_cct
      hpcrun_cct_merge(prefix, unresolved_cct, merge_metrics, NULL);
      // delete unresolved_cct from parent
      hpcrun_cct_delete_self(unresolved_cct);
    } else {
      msg_deferred_resolution_breakpoint("resolve_one_region_context: Prefix is equal to unresolved_cct\n");
    }
  }
}


// return one if a notification was processed
int
try_resolve_one_region_context
(
 void
)
{
  // Pop from private queue, if anything
  // If nothing, steal from public queue.
  typed_stack_elem_ptr(notification) old_head =
    typed_channel_steal(notification)(&thread_notification_channel);

  if (!old_head) return 0;

  unresolved_cnt--;
  ompt_region_debug_notify_received(old_head);

  resolve_one_region_context(old_head);

  // free notification
  //hpcrun_ompt_notification_free(old_head);

  // check if the notification needs to be forwarded
  typed_stack_elem_ptr(notification) next =
    typed_stack_pop(notification, sstack)(&old_head->region_data->notification_stack);
  if (next) {
    typed_channel_shared_push(notification)(next->notification_channel, next);
  } else {
    // notify creator of region that region_data can be put in region's freelist
    //hpcrun_ompt_region_free(region_data);
    // FIXME vi3 >>> Need to review freeing policies for all data types (structs)
  }

  return 1;
}


void
update_unresolved_node
(
 cct_node_t* n, 
 cct_op_arg_t arg, 
 size_t level
)
{
  cct_addr_t *addr = hpcrun_cct_addr(n);

  // Note: GCC7 statically evaluates this as false and dead code
  // eliminates the body without the cast on UNRESOLVED
  if (addr->ip_norm.lm_id == (uint16_t) UNRESOLVED) { 
    addr->ip_norm.lm_ip = 
      ompt_placeholders.ompt_region_unresolved.pc_norm.lm_ip;
    addr->ip_norm.lm_id = 
      ompt_placeholders.ompt_region_unresolved.pc_norm.lm_id;
  }
}


void
update_any_unresolved_regions
(
 cct_node_t* root 
)
{
  void *no_arg = 0;
  hpcrun_cct_walkset(root, update_unresolved_node, no_arg);
}


void
mark_remaining_unresolved_regions
(
 void
)
{
  thread_data_t* td   = hpcrun_get_thread_data();
  cct_bundle_t* cct = &(td->core_profile_trace_data.epoch->csdata);

  // look for unresolved nodes anywhere we might find them
  update_any_unresolved_regions(cct->top);
  update_any_unresolved_regions(cct->tree_root);
  update_any_unresolved_regions(cct->partial_unw_root);
  update_any_unresolved_regions(cct->unresolved_root);
}

#if 0
bool
resolve_if_worker
(
  typed_random_access_stack_elem(region) *el,
  void *arg
)
{
  // This should not happen, if I understand the order of thread finalze,
  // implicit task end and parallel end.
  if (el->team_master) {
    printf("*** thread_finalize ??? Why is this possible ???");
    return 0;
  }
  // Thread didn't take a sample in this region and outer regions too.
  if (!el->took_sample) {
    return 1;
  }
  // resolve the region
  resolve_one_region_context_vi3(el->notification);
  // continue with resolving
  return 0;
}
#endif

void 
ompt_resolve_region_contexts
(
 int is_process
)
{
  struct timespec start_time;

  size_t i = 0;
  timer_start(&start_time);


#if VI3_DEBUG == 1
  int thread_num = hpcrun_ompt_get_thread_num(0);

  typed_random_access_stack_elem(region) *top = typed_random_access_stack_top(region)(region_stack);
  if (top) {
    typed_stack_elem_ptr(region) top_reg = top->notification->region_data;
    printf("thread_finalize >>> REGION_STACK: %p, TOP_REG: %p, TOP_REG_ID: %lx, THREAD_NUM: %d\n",
        &region_stack, top_reg, top_reg->region_id, thread_num);
  } else {
    printf("thread_finalize >>> REGION_STACK: %p, TOP_REG: nil, TOP_REG_ID: nil,THREAD_NUM: %d\n", &region_stack, thread_num);
  }
#endif

  // attempt to resolve all remaining regions
  for(;;i++) {

    // if all regions resolved, we are done
    if (unresolved_cnt == 0) break; 

    // poll for a notification to resolve a region context
    try_resolve_one_region_context();

    // infrequently check for a timeout
    if (i % 1000) {
      
      // there are cases where not all region contexts can be
      // resolved. for instance, a user may initiate termination with
      // a Cntrl-C in the middle of a parallel region. if we have
      // tried to resolve region contexts for three seconds, that
      // should be enough. terminate after three seconds.
      if (timer_elapsed(&start_time) > 3.0) break;
    }
  }

#if 0
  if (unresolved_cnt != 0) {
    mark_remaining_unresolved_regions();
  }
#endif

  if (unresolved_cnt != 0) {
    printf("*** Unresolved regions: %d\n", unresolved_cnt);
  }

  if (unresolved_cnt != 0 && hpcrun_ompt_region_check()) {
    // hang to let debugger attach
    volatile int x;
    for(;;) {
      x++;
    };
  }

}


void 
ompt_resolve_region_contexts_poll
(
 void
)
{
  // if there are any unresolved contexts
  if (unresolved_cnt) {
    // attempt to resolve contexts by consuming any notifications that
    // are currently pending.
    while (try_resolve_one_region_context());
  };
}


#if 1
cct_node_t *
top_cct
(
 cct_node_t *current_cct
)
{  
  if (!current_cct)
    return NULL;

  cct_node_t *temp = current_cct;
  // FIXME: optimize this
  while(hpcrun_cct_parent(temp)) {
      temp = hpcrun_cct_parent(temp);
  }
  return temp;
}
#else
// return the top node in a call path
cct_node_t *
top_cct
(
 cct_node_t *node
)
{
  if (node) {
    for (;;) {
      cct_node_t *next = hpcrun_cct_parent(node);
      if (next == NULL) break;
      node = next;
    }
  }
  return node;
}
#endif


#if DEFER_DEBUGGING

void
print_prefix_info
(
 char *message, 
 cct_node_t *prefix, 
 typed_queue_elem(region) *region_data,
 int stack_index, 
 backtrace_info_t *bt, 
 cct_node_t *cct
)
{
    // prefix length
    int len_prefix = 0;
    cct_node_t *tmp_top = NULL;
    cct_node_t *tmp_bottom = NULL;
    cct_node_t *tmp = NULL;
    tmp_bottom = prefix;
    tmp = prefix;
    while (tmp) {
      len_prefix++;
      tmp_top = tmp;
      tmp = hpcrun_cct_parent(tmp);
    }
    // number of frames
    int len_bt = 0;
    frame_t *bt_inner = bt->begin;
    frame_t *bt_outer = bt->last;
    // frame iterator
    frame_t *it = bt_inner;
    while(it <= bt_outer) {
      len_bt++;
      it++;
    }

    // number of cct nodes
    int len_cct = 0;
    cct_node_t *current = cct;
    while (current) {
      len_cct++;
      current = hpcrun_cct_parent(current);
    }
}

#endif
