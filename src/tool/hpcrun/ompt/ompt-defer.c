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
  typed_stack_elem_ptr(region) region_data,
  cct_node_t *unresolved_cct
)
{
  typed_stack_elem_ptr(notification) notification = hpcrun_ompt_notification_alloc();
  notification->region_data = region_data;
  notification->unresolved_cct = unresolved_cct;
  notification->notification_channel = &thread_notification_channel;
  return notification;
}

#if 0
void
swap_and_free
(
 typed_stack_elem_ptr(region) region_data
)
{
  int depth = region_data->depth;
  typed_random_access_stack_elem(region) *stack_element =
      typed_random_access_stack_get(region)(region_stack, depth);
  // store region data at corresponding position on the stack.
  stack_element->region_data = region_data;
  // invalidate previous value unresolved_cct
  stack_element->unresolved_cct = NULL;
#if 0
  // invalidate value of team_master,
  // will be set in function add_region_and_ancestors_to_stack
  stack_element->team_master = 0;
  // thread hasn't taken a sample in this region yet
  stack_element->took_sample = 0;
#endif
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
      && region_data->region_id == el->region_data->region_id) {
    // el->region_data represents the region that is
    // at this moment on the stack of active regions
    // (el is element of stack that is being processed at the moment).
    // If it's (el->region_data) id is equal to id of
    // "region (region_data) at the region_level",
    // stop further processing of stack elements.
    // Stack is updated properly.
    return 1;
  }

  // If previous condition fails, the stack (el) is outdated and we need
  // to update region that is contained in el variable.
  // We'll do that by calling below function, which will add currently
  // active region at depth equal to stack index of el.
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

#if 0
  // Values of argument team_master says if the thread is the master of region_data
  typed_random_access_stack_top(region)(region_stack)->team_master = team_master;
#endif
}
#endif


void
register_to_region
(
  typed_stack_elem_ptr(region) region_data,
  cct_node_t *unresolved_cct
)
{
  // get new notification
  typed_stack_elem_ptr(notification) notification =
      help_notification_alloc(region_data, unresolved_cct);

  ompt_region_debug_notify_needed(notification);

  // invalidate value of notification->next pointer
  typed_stack_next_set(notification, cstack)(notification, 0);

#if DEBUG_BARRIER_CNT
  // debug information
  int old_value = atomic_fetch_add(&region_data->barrier_cnt, 0);
  if (old_value < 0) {
    // FIXME seems it may happened
    printf("register_to_region >>> To late to try registering. Old value: %d\n", old_value);
  }
#endif

  // register thread for region's call path by pushing
  // notification to to region's wait free queue
  typed_stack_push(notification, cstack)(&region_data->notification_stack,
                                         notification);

#if DEBUG_BARRIER_CNT
  // debug information
  old_value = atomic_fetch_add(&region_data->barrier_cnt, 0);
  if (old_value < 0) {
    // FIXME seems it may happened
    printf("register_to_region >>> To late, but registered. Old value: %d\n", old_value);
  }
#endif

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

#if 0
  // FIXME vi3 >>> If we use this approach, then field took_sample is not needed
  // thread took a sample in this region before
  if (el->took_sample) {
    // skip this region, and go to the inner one
    goto return_label;
  }
#endif
  if (el->unresolved_cct) {
    // this should not happen
    printf("Trying to register multiple times for the same region???\n");
  }

  // Check if the address that corresponds to the region
  // has already been inserted in parent_cct subtree.
  cct_addr_t *region_addr = &ADDR2(UNRESOLVED,el->region_data->region_id);
  cct_node_t *found_cct = hpcrun_cct_find_addr(parent_cct, region_addr);
  if (found_cct) {
    // The following scenario happened be possible:
    //     1. Thread take sample as a worker in region nested inside region N.
    //        Register for N's call path
    //        Insert unresolved_cct that corresponds to N.
    //     2. Thread take sample as a worker in region nested inside region M.
    //        Register for M's call path
    //        Insert unresolved_cct that corresponds to M.
    //     3. Thread take sample as a worker in new region nested inside region N
    //        No need to register for N's call path again
    //        No need to insert unresolved_cct that corresponds to N again,
    //           since it was inserted in step 1, just reuse it.
#if 0
    el->took_sample = true;
#endif
    el->unresolved_cct = found_cct;
    goto return_label;
  }
#if 0
  // mark that thread took sample in this region for the first time
  el->took_sample = true;
#endif
  // insert cct node with region_addr in the parent_cct's subtree
  el->unresolved_cct = hpcrun_cct_insert_addr(parent_cct, region_addr);

  if (!hpcrun_ompt_is_thread_region_owner(el->region_data)) {
    // Worker thread should register for the region's call path
    register_to_region(el->region_data, el->unresolved_cct);
  } else {
    // Master thread also needs to resolve this region at the very end (ompt_parallel_end)
    unresolved_cnt++;
  }

  return_label: {
#if 0
    // If region is marked as the last to register (see function register_to_all_regions),
    // then stop further processing.
    // FIXME vi3 >>> this should be removed.
    if (el->region_data->depth >= vi3_last_to_register) {
      // Indication that processing of active regions should stop.
      return 1;
     }
#endif
    // continue processing descendants
    // store new parent_cct node for the inner region
    *parent_cct_ptr = el->unresolved_cct;
    // Indication that processing of active regions should continue
    return 0;
  };
}

#if 0
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
  typed_stack_elem(region) *stack_reg = el->region_data;

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
    // FIXME vi3 >>> for-nested-regions.c after adding waiting for region_used to be zero
    msg_deferred_resolution_breakpoint("Stack_reg and runtime_reg are different\n");
  }

  // thread has already taken sample inside this region
  if (el->unresolved_cct) {
    // least common ancestor is found
    return 1;
  }

  return_label:
  // continue processing outer regions that are present on stack
  *level_ptr = level + 1;

  return 0;
}
#endif

#if 0
typedef struct lca_args_s {
  int level;
  typed_stack_elem(region) *region_data;
} lca_args_t;


bool
lca_el_fn
(
  typed_random_access_stack_elem(region) *el,
  void *arg
)
{
  lca_args_t *args = (lca_args_t *)arg;
  int level = args->level;
  typed_stack_elem(region) *reg = args->region_data;
  // The sample was previously taken in this region
  if (el->region_data) {
    // If the region has not been change since the last sample was taken,
    // then the least common ancestor is found,
    if (el->region_data->region_id == reg->region_id) {
      // indicator to stop processing stack elements
      return 1;
    }
  }
  // update stack element
  // store region
  el->region_data = reg;
  // check if thread is the master (owner) of the reg
  el->team_master = hpcrun_ompt_is_thread_region_owner(reg);
  // invalidate previous values
  el->unresolved_cct = NULL;
  el->took_sample = false;

  //return_label:
  // update arguments
  args->level = level + 1;
#if KEEP_PARENT_REGION_RELATIONSHIP
  // In order to prevent NULL values get from hpcrun_ompt_get_region_data,
  // use parent as next region to process.
  args->region_data = typed_stack_next_get(region, sstack)(reg);
#else
  args->region_data = hpcrun_ompt_get_region_data(args->level);
#endif
  // indicator to continue processing stack element
  return 0;
}

// return value:
// false - no active parallel regions (sequential code)
// true - thread is inside parallel region
// td_region_depth - region_depth stored at TD(omp_task_context)
// Possible values:
// -1   - thread not waiting on the barrier, but some edge case found in
//        ompt_elide_runtime_frame function. The sample should be attributed
//        to the innermost region
// >= 0 - depth of the region to which sample should be attributed.
//        Ignore all regions deeper than td_region_depth
bool
least_common_ancestor
(
  typed_random_access_stack_elem(region) **lca,
  int td_region_depth
)
{
  typed_stack_elem(region) *innermost_reg = hpcrun_ompt_get_region_data(0);
  if (!innermost_reg) {
    // There is no parallel region active.
    // Thread should be executing sequential code.
    *lca = NULL;
    return false;
  }

  int ancestor_level = 0;
  if (td_region_depth >= 0) {
    // skip regions deeper than td_region_depth
    while(innermost_reg->depth > td_region_depth) {
      // skip me by using my parent
      innermost_reg = typed_stack_next_get(region, sstack)(innermost_reg);
      // increment ancestor level
      ancestor_level++;
    }
  } else {
    if (td_region_depth != -1) {
      printf("Some invalid value: %d.\n", td_region_depth);
    }
  }


  // update top of the stack
  typed_random_access_stack_top_index_set(region)(innermost_reg->depth, region_stack);
  // update the last region that should be checked during registration process
  vi3_last_to_register = innermost_reg->depth;

  // update stack of active regions and try to find region in which
  // thread took previous sample
  lca_args_t args;
  // Assume that thread is not worker
  args.level = ancestor_level;
  args.region_data = innermost_reg;
  *lca = typed_random_access_stack_forall(region)(region_stack,
                                                  lca_el_fn,
                                                  &args);
  // thread is inside parallel region
  return true;
}
#endif

#if 0
// temporary copy from ompt-callstack.c
static ompt_state_t
check_state
(
  void
)
{
  uint64_t wait_id;
  return hpcrun_ompt_get_state(&wait_id);
}


void
innermost_region_present_but_not_parent
(
  typed_stack_elem(region) *innermost_region,
  typed_stack_elem(region) *parent_region
)
{
  if (hpcrun_ompt_is_thread_region_owner(innermost_region)) {
    // maybe this can help to debug
    // thread is the master of the innermost region

    // stack frame content for some cases that occur

    // case 0:
    // __kmp_free_thread
    // __kmp_free_team
    // __kmp_join_call
    // __kmp_api_GOMP_parallel_40_alias
    // g
    // f
    // e._omp_fn.1
    // ... (frames that will be probably be clipped by elider)

    // case 1:
    // __kmp_free_implicit_task
    // __kmp_free_thread
    // __kmp_free_team
    // __kmp_join_call
    // __kmp_api_GOMP_parallel_40_alias
    // g
    // f
    // e._omp_fn.1
    // ... (frames that will be probably be clipped by elider)
  }
}
#endif

static inline bool
unresolved_cct_belongs_to_region
(
  cct_node_t *unresolved_cct,
  typed_stack_elem_ptr(region) region_data
)
{
  // unresolved_cct belongs to region if its lm_ip matches
  // to region_data->region_id
  return hpcrun_cct_addr(unresolved_cct)->ip_norm.lm_ip == region_data->region_id;
}

bool
lca_el_fn
(
  typed_random_access_stack_elem(region) *el,
  void *arg
)
{
  typed_stack_elem(region) **expected_reg_ptr =
      (typed_stack_elem(region) **) arg;
  // Get active region which depth is equal to el's index on the stack and
  // mark it as expected_reg. This region should be already stored inside el.
  typed_stack_elem(region) *expected_reg = *expected_reg_ptr;

  if (el->region_data == expected_reg) {
    // Stack el already contains expected_reg.
    if (el->unresolved_cct) {
      // Presence of el->unresolved_cct should mean that thread took sample
      // inside this region, unless the el->region_data has been reused.
      // If el->unresolved_cct belongs to el->region_data, then el->region_data
      // has not been reused, thread already took sample inside it so
      // el->region_data represents the least common ancestor.
      if (unresolved_cct_belongs_to_region(el->unresolved_cct, el->region_data)) {
        return 1;
      }
      // FIXME vi3 consider this scenario again
    } else {
      // Thread has not taken sample inside expected_reg, so it cannot be the
      // least common ancestor. Continue searching for it.
      goto return_label;
    }
  }

  //printf("It seems I've finished here\n");
  // Since expected_reg is not present on the stack inside el,
  // it represents the region in which thread is not involved.
  // TODO FIXME vi3: (Try to optimized upper part)
  // Invalidate previous content of el and store expected_reg.
  memset(el, 0, sizeof(typed_random_access_stack_elem(region)));
  el->region_data = expected_reg;

  return_label:
  // Next expected_reg is the parent region of the current expected_reg stored
  // inside el->region_data. (parent region = the first enclosing region)
  expected_reg = typed_stack_next_get(region, sstack)(el->region_data);
  if (!expected_reg) {
    // Since el->region_data is the outermost region,
    // it represents the least common ancestor
    return 1;
  }

  // continue looking for the least common ancestor
  *expected_reg_ptr = expected_reg;
  return 0;
}

typed_random_access_stack_elem(region) *
least_common_ancestor
(
  void
)
{
  // FIXME use innermost instead of top_reg
  typed_stack_elem(region) *innermost_reg =
      typed_random_access_stack_top(region)(region_stack)->region_data;

  return typed_random_access_stack_forall(region)(region_stack,
                                                  lca_el_fn, &innermost_reg);
}


bool
safe_to_register_for_active_regions
(
  void
)
{
  if (!hpcrun_ompt_is_thread_part_of_team()) {
    // Thread is not part of any team, so there's no region to register.
    return false;
  }

  // NOTE: innermost region == region on the top of the stack

  // Thread is master of the innermost region, which means that all regions
  // on the stack are active and is safe to register for their call paths.
  if (hpcrun_ompt_is_thread_master_of_the_innermost_region()) {
    return true;
  }

  // Thread is the worker in the innermost region.
  ompt_region_execution_phase_t exec_phase =
      hpcrun_ompt_get_current_region_execution_phase();

  if (exec_phase == ompt_region_execution_phase_implicit_task_begin) {
    // Thread took sample while executing implicit task,
    // which means that innermost region and all enclosing regions are active.
    // Safe to register for all active regions.
    return true;
  } else if (exec_phase == ompt_region_execution_phase_last_implicit_barrier_enter) {
    // thread entered last implicit barrier
    int task_type_flags, thread_num;
    ompt_data_t *task_data =
        vi3_hpcrun_ompt_get_task_data(0, &task_type_flags, &thread_num);
    if (!task_data) {
      // thread cannot be sure if executing task that belongs to the innermost
      // region. Not safe to register
      return false;
    }

    if (task_type_flags & ompt_task_explicit) {
      // Thread is executing explicit task while waiting on last implicit barrier.
      // Innermost region and all enclosing regions are still active.
      // Safe to register.
      return true;
    }

    // task_data belongs to implicit task. Thread cannot be sure if
    // innermost region or any of enclosing regions are still active.
    // (The following scenario may happen: While thread is waiting on
    // the last implicit barrier for more work, the innermost region's team
    // was torn apart. Also, enclosing regions may be finished too.)
    // Not safe to register.
    return false;
  } else {
    // Thread finished with waiting on the last implicit barrier of the
    // innermost region.
    // It should either be finalized or become part of the team that belongs
    // to the next parallel region.
    // Region present on the top of the stack should have been finished.
    // Obviously, it is not safe to register for that region or any of
    // enclosing regions, because they might be finished too.
    return false;
  }

}

void
register_to_all_regions
(
  void
)
{
#if 0
#if 0
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
  vi3_last_to_register = top_reg->depth;
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
#endif

#if 0
  // invalidate value
  vi3_last_to_register = -1;
  // If there is no regions on the stack, just return.
  // Thread should be executing sequential code.
  bool stack_is_empty = typed_random_access_stack_empty(region)(region_stack);
  if (stack_is_empty)
    return;

  typed_stack_elem(region) *top_reg = hpcrun_ompt_get_top_region_on_stack();
  // assume that thread will register for all regions present on stack
  vi3_last_to_register = top_reg->depth;

  // check if thread_data is available and contains any useful information
  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context),
      &omp_task_context, &region_depth);
  if (info_type == 1) {
    // thread_data contains depth of the region to which sample should be attributed.
    // This region will be the last region for which call path thread is going to register.
    // Eventually, deeper regions will be skipped during registration process.
    // (NOTE by registration vi3 means "thread registers itself for region's call path")
    vi3_last_to_register = region_depth;
  } else if (info_type == 2) {
    // thread_data contains null
    if (waiting_on_last_implicit_barrier) {
      // Thread has reached the last implicit barrier of the innermost region.
      // It cannot guarantee that innermost region or any of its ancestor
      // are still active. Safe thing to do is to skip registration process
      // and attribute the sample to the thread local (idle) placeholder.
      return;
    }
    // Thread still hasn't reached the last implicit barrier,
    // so the sample will be attributed to region on the top of the stack (top_reg)
    // Thread is going to register itself for call paths of all regions present on the stack.
  } else {
    // This should never happen since !ompt_eager_context_p() is true.
    assert(0);
  }

  // Mark that thread took sample in regions presented on the stack, eventually
  // avoid regions which depths are greater than vi3_last_to_register.
  // Initial idea was to process all regions present on the stack.
  // To save some time, it is possible to find the region in which thread
  // has already taken sample and process only nested regions.
  // In order to find that region, apply least_common_ancestor function
  // on each region present on the stack

  int level = 0;
  typed_random_access_stack_elem(region) *lca =
      typed_random_access_stack_forall(region)(region_stack,
                                               least_common_ancestor,
                                               &level);

  // If aforementioned region (least common ancestor, lca_reg shorter) exists,
  // then use its corresponding unresolved cct node
  // as parent_cct and process regions which depths are >= lca->depth + 1.
  // Otherwise, process all regions present on the stack and use
  // thread_root as initial parent_cct.

  int start_from = 0;
  cct_node_t *parent_cct = hpcrun_get_thread_epoch()->csdata.thread_root;
  if (lca) {
    typed_stack_elem(region) *lca_reg = lca->region_data;
    start_from = lca_reg->depth + 1;
    parent_cct = lca->unresolved_cct;
  }

  typed_random_access_stack_reverse_iterate_from(region)(start_from,
                                                         region_stack,
                                                         thread_take_sample,
                                                         &parent_cct);
#endif

#if 0
  // Used to provide some debug information

  // invalidate value
  vi3_last_to_register = -1;

  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context),
                                           &omp_task_context, &region_depth);

  cct_node_t *inner_task_cct = NULL;
  int inner_reg_depth = -1;
  int inner_info_type = -1;

  cct_node_t *outer_task_cct = NULL;
  int outer_reg_depth = -1;
  int outer_info_type = -1;

  ompt_data_t *inner_task_data = hpcrun_ompt_get_task_data(0);
  ompt_data_t *outer_task_data = hpcrun_ompt_get_task_data(1);

  typed_stack_elem(region) *inner_reg = hpcrun_ompt_get_region_data(0);
  typed_stack_elem(region) *outer_reg = hpcrun_ompt_get_region_data(1);

  if (!inner_task_data) {
    //printf("No inner task data\n");
    // vi3_last_to_register should be -1
    // waiting_on_last_implicit_barrier should be true
    // inner_reg and outer_reg should be NULL

    if (!waiting_on_last_implicit_barrier) {
      // never happened
      printf("Thread should be waiting: %d\n", waiting_on_last_implicit_barrier);
    }

    if (vi3_last_to_register != -1) {
      // never happened
      printf("vi3_last_to_register (%d) should be -1\n", vi3_last_to_register);
    }

    if (inner_reg) {
      // printf("inner_reg exists: %d\n", inner_reg->depth);
      // inner_reg exists not finished
      // vi3_idle_collapsed

      if (!vi3_idle_collapsed) {
        // never happened
        printf("Stack not collapsed, inner_reg exists\n");
      }

      int old_inner_reg = atomic_fetch_add(&inner_reg->barrier_cnt, 0);
      if (old_inner_reg != 0) {
        // never hapenned
        printf("inner_outer_reg finished: %d\n", old_inner_reg);
      }

      // case 0:
      // __kmp_fork_barrier
      // __kmp_launch_thread
      // __kmp_launch_worker

      // case 1:
      // sched_yield
      // __kmp_fork_barrier
      // __kmp_launch_thread
      // __kmp_launch_worker
    }

    if (outer_reg) {
      // This makes sense to debug if put return in if (inner_reg) {return;}
      // We care about case when inner_reg is NULL and outer_reg isn't.
      // outer_reg exists not finished
      // can happen when inner_reg does not exists.
      if (!inner_reg) {
        // here, inner_reg is null
        //printf("outer_reg exists: %d, inner_reg: %p\n", outer_reg->depth, inner_reg);
        //inner_reg = hpcrun_ompt_get_region_data(0);
        // here inner_reg has value
        // assume also that inner_task_data has value
        //printf("check again outer_reg exists: %d, inner_reg: %p\n", outer_reg->depth, inner_reg);
      }

      // no inner_task_data and outer_task_data
      // no inner_reg
      // outer_reg still not finished
      // vi3_idle_collapsed = true
      // vi3_last_to_register = -1
      // I tried to call hpcrun_ompt_get_region_data(0) and get active region
      // hpcrun_ompt_get_task_data(0) also returns task_data {0x0}
      // Is it possible that region_data and task_data became available
      // in the meantime?


      // case 0
      // sched_yield
      // __kmp_fork_barrier
      // __kmp_launch_thread
      // __kmp_launch_worker



      if (!vi3_idle_collapsed) {
        // never happened
        printf("Stack not collapsed, inner_reg exists\n");
      }

      int old_outer_reg = atomic_fetch_add(&outer_reg->barrier_cnt, 0);
      if (old_outer_reg != 0) {
        // never happened
        printf("old_outer_reg finished: %d\n", old_outer_reg);
      }

    }

    if (outer_task_data) {

      // FIXME vi3
      // happened once (0x5)
      // inner_task_data is null
      //printf("before: Why outer_task_data exists: %p, %p\n", outer_task_data->ptr, inner_task_data);
      //inner_task_data = hpcrun_ompt_get_task_data(0);
      // not inner_task_data became available
      //printf("after: Why outer_task_data exists: %p, %p\n", outer_task_data->ptr, inner_task_data);;
      // both inner_reg and outer_reg exists and are active
      // vi3_last_to_register = -1
      // info_type = 2
      // hpcrun_ompt_get_task_data(0) = {0x0} (it became available in the meantime)
      //

      // case 0
      // sched_yield
      // __kmp_fork_barrier
      // __kmp_launch_thread
      // __kmp_launch_worker


    }

    return;
  }

  if (!outer_task_data) {
    // printf("No outer task data\n");
    // There is inner task data, but not outer task data.
    // waiting on last implicit barrier
    // inner_data->value = 0x7 | inner_data->value = 0x0
    // vi3_last_to_register = -1
    // inner_reg = outer_reg = NULL

    // case 0:
    // sched_yield
    // __kmp_fork_barrier
    // __kmp_launch_thread
    // __kmp_launch_worker
    //

    return;
  }

  inner_info_type = task_data_value_get_info(inner_task_data->ptr,
      &inner_task_cct, &inner_reg_depth);
  outer_info_type = task_data_value_get_info(outer_task_data->ptr,
      &outer_task_cct, &outer_reg_depth);


  if (!waiting_on_last_implicit_barrier) {
    if (info_type == 2) {
      // some edge case where task_frames are not set properly
      // still not reach the last implicit barrier.

      // inner_reg_depth == inner_reg->depth
      // outer_reg_depth == outer_reg->depth

      if (inner_reg_depth != inner_reg->depth) {
        // never happened
        printf("inner_reg_depth (%d) != inner_reg->depth (%d)\n",
            inner_reg_depth, inner_reg->depth);
      }

      if (outer_reg_depth != outer_reg->depth) {
        // FIXME happened once (1) != (2)
        printf("outer_reg_depth (%d) != outer_reg->depth (%d)\n",
               outer_reg_depth, outer_reg->depth);
      }

      // In most cases vi3_last_to_register is -1
      if (vi3_last_to_register != -1) {
        // vi3_last_to_register == 0

        // case 0:
        // __kmp_invoke_microtask
        // __kmp_invoke_task_func
        // __kmp_launch_thread
        // __ kmp_launch_worker
        // ...

        // case 1:
        // __kmp_api_GOMP_parallel_40_alias
        // g
        // f
        // e.__omp_fn.1
        // ...

        // case 2:
        // __kmp_invoke_task_func
        // __kmp_launch_thread
        // __ kmp_launch_worker
        // ...


        // It seems that vi3_last_to_register can be zero only
        if (vi3_last_to_register != 0) {
          // never happened
          printf("vi3_last_to_register should be 0: %d", vi3_last_to_register);
        }
      }

      if (inner_info_type == 2 || outer_info_type == 2) {
        // never happened
        printf("inner_info_type: %d, outer_info_type: %d\n",
            inner_info_type, outer_info_type);
      }

      if (!inner_task_data) {
        // never happened
        printf("inner_task_data missing\n");
      }

      if (!outer_task_data) {
        // never happened
        printf("outer_task_data missing\n");
      }

      if (!inner_reg) {
        // never happened
        printf("inner_reg missing\n");
      }

      if (!outer_reg) {
        // never happened
        printf("outer_reg missing\n");
      }

    } else {

      if (inner_task_data) {
        if (inner_reg) {
          // depths should be equal
          if (inner_reg_depth != inner_reg->depth) {
            // printf("inner_reg_depth (%d) != inner_reg->depth (%d)\n",
            //     inner_reg_depth, inner_reg->depth);

            // inner_reg depth may be -1
            if (inner_reg_depth == -1) {
              // printf("Why is task_data empty: %p\n", inner_task_data->ptr);
              // vi3_last_index == 1
              // outer_task_data contains something

              // FIXME vi3 check this.

              // case 0:
              // pthread_create@@GLIBC_2.2.5
              // pthread_create
              // __kmp_create_worker
              // __kmp_allocate_thread
              // __kmp_fork_call
              // kmp_GOMP_fork_call
              // __kmp_api_GOMP_parallel_40_alias
              // e
              // d
              // c.__omp_fn.2


              // case 2:
              // clone
              // do_clone.constprop
              // pthread_create@@GLIBC_2.2.5
              // pthread_create
              // __kmp_create_worker
              // __kmp_allocate_thread
              // __kmp_fork_call
              // kmp_GOMP_fork_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e.__omp_fn.2

              //

            }


          }
        } else {
          printf("inner_task_data present, but not inner_reg\n");
        }
      } else {
        if (inner_reg) {
          printf("inner_task_data not present, but inner_reg is\n");
        }
      }



      if (outer_task_data) {
        if (outer_reg) {
          // depths should be equal
          if (outer_reg_depth != outer_reg->depth) {
            printf("outer_reg_depth (%d) != outer_reg->depth (%d)\n",
                   outer_reg_depth, outer_reg->depth);
          }
        } else {
          // FIXME vi3 >>> happened once
          printf("outer_task_data present, but not outer_reg\n");
        }
      } else {
        if (outer_reg) {
          printf("outer_task_data not present, but outer_reg is\n");
        }
      }



    }
  }

  if (inner_reg && !outer_reg && inner_task_data && outer_task_data) {
    // vi3_idle_collapsed changes value
    //printf("Am I waiting: %d? Idle: %d\n", waiting_on_last_implicit_barrier, vi3_idle_collapsed);

    if (!waiting_on_last_implicit_barrier) {
      // never happened.
      printf("Should be waitin\n");
    }

    typed_stack_elem(region) *reg0 = hpcrun_ompt_get_region_data(0);
    typed_stack_elem(region) *reg1 = hpcrun_ompt_get_region_data(1);
    typed_stack_elem(region) *reg2 = hpcrun_ompt_get_region_data(2);
    typed_stack_elem(region) *reg3 = hpcrun_ompt_get_region_data(3);
    //typed_stack_elem(region) *reg4 = hpcrun_ompt_get_region_data(4);

    ompt_data_t *task0 = hpcrun_ompt_get_task_data(0);
    ompt_data_t *task1 = hpcrun_ompt_get_task_data(1);
    ompt_data_t *task2 = hpcrun_ompt_get_task_data(2);
    // it seems that this may produce segfault inside runtime
    // if I try to access task data that is to much outer.
    // see picture too-much-outer.png
    // That occur at the very beginning of the program
    ompt_data_t *task3 = hpcrun_ompt_get_task_data(3);
    //ompt_data_t *task4 = hpcrun_ompt_get_task_data(4);


    if (vi3_idle_collapsed) {
      // What are depths of tasks and regions.
//      printf("Idle: depth0: %d, r0: %p, r1: %p, r2: %p, r3: %p, r4: %p, t0: %p, t1: %p, t2: %p, t3: %p, t4: %p\n",
//          reg0 ? reg0->depth: -1, reg0, reg1, reg2, reg3, reg4, task0, task1, task2, task3, task4);


      // case 0:
      // sched_yield
      // __kmp_fork_barrier
      // __kmp_launch_thread
      // __kmp_launch_worker

    } else {
//      printf("not Idle: depth0: %d, r0: %p, r1: %p, r2: %p, r3: %p, r4: %p, t0: %p, t1: %p, t2: %p, t3: %p, t4: %p\n",
//             reg0 ? reg0->depth: -1, reg0, reg1, reg2, reg3, reg4, task0, task1, task2, task3, task4);

    }

  }


  if (!waiting_on_last_implicit_barrier) {
    typed_stack_elem(region) *reg0 = hpcrun_ompt_get_region_data(0);
    typed_stack_elem(region) *reg1 = hpcrun_ompt_get_region_data(1);
    typed_stack_elem(region) *reg2 = hpcrun_ompt_get_region_data(2);
    typed_stack_elem(region) *reg3 = hpcrun_ompt_get_region_data(3);
    //typed_stack_elem(region) *reg4 = hpcrun_ompt_get_region_data(4);

    ompt_data_t *task0 = hpcrun_ompt_get_task_data(0);
    ompt_data_t *task1 = hpcrun_ompt_get_task_data(1);
    ompt_data_t *task2 = hpcrun_ompt_get_task_data(2);
    // it seems that this may produce segfault inside runtime
    // if I try to access task data that is to much outer.
    // see picture too-much-outer.png
    // That occur at the very beginning of the program
    ompt_data_t *task3 = hpcrun_ompt_get_task_data(3);
    //ompt_data_t *task4 = hpcrun_ompt_get_task_data(4);

    if (!reg0) {
      printf("How is this possible??? \n");
      return;
    }

    if (reg0->depth >= 3) {

      if (!task0 || task0->value != 0x7) {
        // hapenned
        printf("task0 unexpected: %p, %lx\n", task0, task0 ? task0->value: -1);

        // case 0:
        // pthread_create
        // __kmp_create_worker
        // __kmp_allocate_thread
        // __kmp_fork_call
        // __kmp_GOMP_fork_call
        // __kmp_api_GOMP_parallel_40_alias

        // case 0:
        // __memset_sse2
        // __kmp_allocate_thread
        // __kmp_fork_call
        // __kmp_GOMP_fork_call
        // __kmp_api_GOMP_parallel_40_alias
      }

      if (!task1 || task1->value != 0x5) {
        // happened
        printf("task1 unexpected: %p, %lx\n", task1, task1 ? task1->value: -1);
      }

      if (!task2 || task2->value != 0x3) {
        // hapenned
        printf("task2 unexpected: %p, %lx\n", task2, task2 ? task2->value: -1);
      }

      if (!task3 || task3->value != 0x1) {
        // hapenned
        printf("task3 unexpected: %p, %lx\n", task3, task3 ? task3->value: -1);
      }



      if (!reg1 || reg1->depth != 2) {
        // never hapenned
        printf("reg1 problem: %p, %d\n", reg1, reg1 ? reg1->depth : -1);
      }

      if (!reg2 || reg2->depth != 1) {
        // never happenned
        printf("reg2 problem: %p, %d\n", reg2, reg2 ? reg2->depth : -1);
      }


      if (!reg3 || reg3->depth != 0) {
        // never hapenned
        printf("reg3 problem: %p, %d\n", reg3, reg3 ? reg3->depth : -1);
      }
    }




  }
#endif

  // focus still on this
#if 0
  if (!waiting_on_last_implicit_barrier) {
    typed_stack_elem(region) *innermost_reg =
        hpcrun_ompt_get_region_data(0);
    if (!innermost_reg) {
      // never happened
      printf("Is this even possible\n");
    } else {
      typed_stack_elem(region) *parent_reg =
          hpcrun_ompt_get_region_data(1);
      typed_stack_elem(region) *grandpa_reg =
          hpcrun_ompt_get_region_data(2);
      typed_stack_elem(region) *grand_grandpa_reg =
          hpcrun_ompt_get_region_data(3);
      typed_stack_elem(region) *should_be_null =
          hpcrun_ompt_get_region_data(4);

      if (innermost_reg->depth >= 1) {
        if (!parent_reg) {
          // never happened
          printf("Parent region missing: %p\n", parent_reg);
        }
      }

      if (innermost_reg->depth >= 2) {
        if (!grandpa_reg) {
          // never happened
          printf("grandpa region missing: %p\n", grandpa_reg);
        }
      }

      if (innermost_reg->depth >= 3) {
        if (!grand_grandpa_reg) {
          printf("grand_grandpa region missing: %p\n", grand_grandpa_reg);
        }
        if (innermost_reg->depth < 4 && should_be_null) {
          printf("Why region exists here??? Depth: %d\n", should_be_null->depth);
        }
      }

    }
  }
  else {
    typed_stack_elem(region) *innermost_reg = hpcrun_ompt_get_region_data(0);
    if (innermost_reg) {
      int old = atomic_fetch_add(&innermost_reg->barrier_cnt, 0);
      if (old < 0) {
        printf("Region has been finished: %d\n", old);
      }

      // how about checking if my ancestor are finished
      typed_stack_elem(region) *curr = innermost_reg;
      int count_to_minus_one = innermost_reg->depth;
      while (curr) {
        old = atomic_fetch_add(&curr->barrier_cnt, 0);
        if (old < 0) {
          // it seems that this happened once for the innermost region,
          // which make sense, since it is possible that master free
          // the region in the meantime.
          printf("Ancestor: %lx on depth: %d has been finished\n", curr->region_id, curr->depth);
        }
        curr = typed_stack_next_get(region, sstack)(curr);
        count_to_minus_one--;
      }

      if (count_to_minus_one != -1) {
        printf("Something is wrong: %d\n", count_to_minus_one);
      }

      cct_node_t *omp_task_context = NULL;
      int region_depth = -1;
      int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context),
                                               &omp_task_context, &region_depth);

      if (info_type == 2) {
        if (!vi3_idle_collapsed) {
          //printf("Elider does not know what to do, but didn't collapsed everything: %d\n", info_type);
          // case 0:
          // hpcrun_safe_enter
          // ompt_implicit_task (endpoint = end)
          // __ompt_implicit_task_end
          // __kmp_fork_barrier
          // __kmp_launch_thread
          // __kmp_launch_worker


          // case 1:
          // pthread_getspecific
          // hpcrun_get_thread_data_specific
          // hpcrun_safe_enter
          // ompt_implicit_task (endpoint = begin)
          // __kmp_invoke_task_func
          // __kmp_launch_thread
          // __kmp_launch_worker

        }
      }


      if (innermost_reg->depth >= 1) {
        typed_stack_elem(region) *parent_reg = hpcrun_ompt_get_region_data(1);
        if (!parent_reg) {
          //printf("Parent region is missing: %p\n", parent_reg);
          // sched_yield
          // __kmp_fork_barrier
          // __kmp_launch_thread
          // __kmp_launch_worker

          if (!vi3_idle_collapsed) {
            if (info_type == 2) {
              // never happened
              printf("What edge case is this?\n");
            } else if (info_type == 1) {
              // printf("Elider said this sample belongs to region at depth: %d, but innermost is on: %d\n",
              //     region_depth, innermost_reg->depth);

              // case 0:
              // __kmp_free_thread
              // __kmp_free_team
              // __kmp_join_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e.__omp_fn.1

              // case 1:
              // __kmp_free_implicit_task
              // __kmp_free_thread
              // __kmp_free_team
              // __kmp_join_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e.__omp_fn.1


              // case 2:
              // pthread_mutex_lock
              // __kmp_lock_suspend_mx
              // __kmp_free_thread
              // __kmp_free_team
              // __kmp_join_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e.__omp_fn.1


              if (region_depth >= innermost_reg->depth) {
                // never happened
                printf("Sample belongs to innermost_reg\n");
              }
            }
          }
        }
      }
    }
  }


  // is it possible that elider stores something and that parallel_data is unavailable

  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context),
                                           &omp_task_context, &region_depth);

  if (info_type == 1) {
    typed_stack_elem(region) *innermost_reg = hpcrun_ompt_get_region_data(0);
    if (!innermost_reg) {
      // never happened
      printf("No innermost region\n");
    }
  }

  typed_stack_elem(region) *innermost_reg = hpcrun_ompt_get_region_data(0);
  if (!innermost_reg) {
    typed_stack_elem(region) *parent_reg = hpcrun_ompt_get_region_data(1);
    if (parent_reg) {
      //printf("No inner, but parent at depth is present: %d\n", parent_reg->depth);
      // case 0:
      // sched_yield
      // __kmp_fork_barrier
      // __kmp_launch_thread
      // __kmp_launch_worker

      // parent_reg->depth = 2 (still active)
      // grandpa_reg->depth = 1 (still active)
      // grand_grandpa->depth = 1 (still active)
      // vi3_idle_collapsed = true

      // case 1:
      // __kmp_fork_barrier
      // __kmp_launch_thread
      // __kmp_launch_worker

    }
  }

  if (innermost_reg) {
    int old = atomic_fetch_add(&innermost_reg->barrier_cnt, 0);
    if (old < 0) {
      // never happened
      printf("Region has been finished: %d\n", old);
    }
  }

  // Can you be worker in innermost region and omp_task_context points to parent???
  typed_stack_elem(region) *innermost_reg = hpcrun_ompt_get_region_data(0);
  if (innermost_reg) {
    bool master = hpcrun_ompt_is_thread_region_owner(innermost_reg);
    if (!master || innermost_reg->owner_free_region_channel != (&region_freelist_channel)) {
      // thread is worker
      cct_node_t *omp_task_context = NULL;
      int region_depth = -1;
      int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context),
                                               &omp_task_context, &region_depth);
      if (info_type == 1) {
        if (region_depth != innermost_reg->depth) {
          // is it possible that elider points to outer region
          // in which thread should not be involved
          printf("This is also possible: %d, %d\n",
              region_depth, innermost_reg->depth);
        }
      }
    }
  }
#endif

#if 0
  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context),
                                           &omp_task_context, &region_depth);

  typed_stack_elem(region) *innermost_reg = hpcrun_ompt_get_region_data(0);
  ompt_state_t thread_state = check_state();
  if (innermost_reg) {
    if (innermost_reg->depth >= 1) {
      // innermost_reg is nested inside at least one region
      // check if its parent exist
      typed_stack_elem(region) *parent_reg = hpcrun_ompt_get_region_data(1);
      if (!parent_reg) {

        typed_stack_elem(region) *hack_parent_reg = typed_stack_next_get(region, sstack)(innermost_reg);

        bool innermost_owner = hpcrun_ompt_is_thread_region_owner(innermost_reg);
        bool hack_parent_owner = hpcrun_ompt_is_thread_region_owner(hack_parent_reg);

        ompt_data_t* parallel_data = NULL;
        int team_size;
        int ret_val = hpcrun_ompt_get_parallel_info(1, &parallel_data, &team_size);
        // ret_val is always zero
        // ompt_initialized = 1 (static variable from ompt-interface.c
        // OpenMP specification:
        // "The entry point returns 2 if there is a
        //  parallel region at the specified ancestor level and the information is available, 1 if there is a parallel
        //  region at the specified ancestor level but the information is currently unavailable, and 0 otherwise."
        if (ret_val != 0) {
          // never happened
          printf("Region is present, but information is unavailable: %d\n", ret_val);
        }

        if (thread_state == ompt_state_wait_barrier_implicit) {
          if (innermost_owner || hack_parent_owner) {
            // never happened
            printf("Thread is master of inner or parent\n");
          } else {
            // thread is the worker inside innermost region and should not be involved
            // inside parent region at all.

            // FIXME vi3 to vi3 NOTE: innermost_reg MAY BE FINISHED, so its not safe to register

            // case 0:
            // sched_yield
            // __kmp_fork_barrier
            // __kmp_launch_thread
            // __kmp_launch_worker


          }
        }
        else if (thread_state == ompt_state_overhead) {
          if (!innermost_owner) {
            // never happened
            printf("Not master of the innermost region\n");
          }
          else {
            if (info_type != 1) {
              // never happened
              printf("Is it possible that elider didn't store depth of the parent region\n");
            }
            else {
              if (hack_parent_reg->depth != region_depth) {
                // never happened
                printf("Elider points to region which is not parent\n");
              }
              else {
                // elider says that sample should be attributed to the parent region
                //printf("I need parent here\n");

                // case 0:
                // __kmp_free_thread
                // __kmp_free_team
                // __kmp_join_call
                // __kmp_api_GOMP_parallel_40_alias
                // g
                // f
                // e._omp_fn.1
                // ..

                // case 1:
                // pthread_mutex_unlock
                // __kmp_unlock_suspend_mx
                // __kmp_free_thread
                // __kmp_free_team
                // __kmp_join_call
                // __kmp_api_GOMP_parallel_40_alias
                // g
                // f
                // e._omp_fn.1
                // ..


                // case 2:
                // __kmp_free_implicit_task
                // __kmp_free_thread
                // __kmp_free_team
                // __kmp_join_call
                // __kmp_api_GOMP_parallel_40_alias
                // g
                // f
                // e._omp_fn.1
                // ..


              }
            }

          }
        }
        else {
          // never happened
          printf("1865 Some other state: %d\n", thread_state);
        }
      }
    }
  }

  if (thread_state == ompt_state_wait_barrier_implicit_parallel) {
    // never happened
    printf("This may happened: %d\n", thread_state);
  }

  if (thread_state == ompt_state_wait_barrier_implicit) {
    if (!waiting_on_last_implicit_barrier) {
      printf("This may also happened\n");
    }
  }

  if (where_am_I == vi3_my_enum_parallel_begin) {
    typed_stack_elem(region) *new_reg =
        typed_random_access_stack_top(runtime_region)(runtime_master_region_stack)->region_data;

    if (thread_state == ompt_state_overhead) {
       // printf("State overhead may be expected\n");

       // case 0:
       // clone
       // do_clone.constprop.4
       // pthread_create@@GLIBC_2.2.5
       // pthread_create
       // __kmp_create_worker
       // __kmp_allocate_thread
       // __kmp_allocate_team
       // __kmp_fork_call
       // __kmp_GOMP_fork_call
       // __kmp_api_GOMP_parallel_40_alias
       // a
       // main

       // case 1:
       // __kmp_wait_4_ptr
       // __kmp_acquire_ticket_lock
       // __kmp_fork_call
       // __kmp_GOMP_fork_call
       // __kmp_api_GOMP_parallel_40_alias
       // e
       // d
       // c._omp_fn.2
       // ...


      // case 2:
      // __kmp_wait_4_ptr
      // __kmp_acquire_ticket_lock
      // __kmp_fork_call
      // __kmp_GOMP_fork_call
      // __kmp_api_GOMP_parallel_40_alias
      // g
      // f
      // e._omp_fn.1
      // ...

      // case 3:
      // sched_yield
      // __kmp_wait_4_ptr
      // __kmp_acquire_ticket_lock
      // __kmp_fork_call
      // __kmp_GOMP_fork_call
      // __kmp_api_GOMP_parallel_40_alias
      // g
      // f
      // e._omp_fn.1
      // ...

      // case 4:
      //

    }
    else if (thread_state == ompt_state_work_parallel) {
      // printf("Why is state work parallel\n");

      // case 0:
      // __kmp_fork_barrier
      // __kmp_fork_call
      // __kmp_GOMP_fork_call
      // g
      // f
      // e._omp_fn.1
      // ...

      // case 1:
      // __kmp_task_team_setup
      // __kmp_fork_barrier
      // __kmp_fork_call
      // __kmp_GOMP_fork_call
      // g
      // f
      // e._omp_fn.1
      // ...

      // case 2:
      // sched_yield
      // __kmp_wat_4_ptr
      // __kmp_acquire_ticket_lock
      // __kmp_task_team_setup
      // __kmp_fork_barrier
      // __kmp_fork_call
      // __kmp_GOMP_fork_call
      // g
      // f
      // e._omp_fn.1
      // ...

      // case 3:
      // __kmp_release_ticket_lock
      // __kmp_fork_call
      // __kmp_GOMP_fork_call
      // g
      // f
      // e._omp_fn.1
      // ...

      // case 4:
      // __kmp_wat_4_ptr
      // __kmp_acquire_ticket_lock
      // __kmp_task_team_setup
      // __kmp_fork_barrier
      // __kmp_fork_call
      // __kmp_GOMP_fork_call
      // g
      // f
      // e._omp_fn.1
      // ...


      // case 5:
      // __kmp_acquire_ticket_lock
      // __kmp_task_team_setup
      // __kmp_fork_barrier
      // __kmp_fork_call
      // __kmp_GOMP_fork_call
      // g
      // f
      // e._omp_fn.1
      // ...

    }
    else {
      // never happened
      printf("Even this state is possible: %x\n", thread_state);
    }

    if (!innermost_reg) {
      // it may happen according to standard
      // "Between a parallel-begin event and an implicit-task-begin event, a call to
      //  ompt_get_parallel_info(0,...) may return information about the outer parallel team,
      //  the new parallel team or an inconsistent state."
      // FIXME check when this happened???
      printf("Innermost region not present: %x\n", where_am_I);
    }
    else {
      if (innermost_reg != new_reg) {
        // innermost_reg should be the parent region
        if (innermost_reg->depth + 1 != new_reg->depth) {
          // never happened
          printf("What kind of region is innermost_reg\n");
        }
        else {
          // innermost_reg is the parent of the new region
          if (info_type != 1) {
            // never happened
            printf("Elider didn't find anything\n");
          }
          else {
            if (region_depth != innermost_reg->depth) {
              // never happened
              printf("Elider doesn't point out parent region\n");
            }
          }
        }
      }
    }
  }

  if (innermost_reg) {
    int old = atomic_fetch_add(&innermost_reg->barrier_cnt, 0);
    if (old < 0) {
      // FIXME this happened only once.
      printf("Skip this one: %d\n", old);
    }
  }

  if (waiting_on_last_implicit_barrier) {
    if (info_type == 2) {
      if (thread_state == ompt_state_wait_barrier_implicit) {

      }
      else if (thread_state == ompt_state_overhead) {
        // OpenMP Specification:
        // "The value ompt_state_overhead indicates that the thread is in the overhead state at any point
        // while executing within the OpenMP runtime, except while waiting at a synchronization point."

        // case 0:
        // __omp_implicit_task_end
        // __kmp_fork_barrier
        // __kmp_launch_thread
        // __kmp_launch_worker

        // case 1:
        // __kmp_fork_barrier
        // __kmp_launch_thread
        // __kmp_launch_worker

        // case 2:
        // pthread_getspecifi
        // hpcrun_get_thread_data_specific_avail
        // hpcrun_safe_enter
        // ompt_implicit_task
        // __omp_implicit_task_end
        // __kmp_fork_barrier
        // __kmp_launch_thread
        // __kmp_launch_worker

        // case 3:


      }
      else if (thread_state == ompt_state_work_parallel) {
        // case 0:
        // hpcrun_get_thread_data_specific
        // hpcrun_safe_enter
        // ompt_implicit_task(begin)
        // __kmp_invoke_task_func
        // __kmp_launch_thread
        // __kmp_launch_worker

        // case 1:
        // __kmp_invoke_task_func
        // __kmp_launch_thread
        // __kmp_launch_worker

        // case 0:
        // pthread_getspecific@plt
        // hpcrun_get_thread_data_specific
        // hpcrun_safe_enter
        // ompt_implicit_task(begin)
        // __kmp_invoke_task_func
        // __kmp_launch_thread
        // __kmp_launch_worker


      }
      else if (thread_state == ompt_state_idle) {

        // case 0:
        // __ompt_implicit_task_end
        // __kmp_fork_barrier
        // __kmp_launch_thread
        // __kmp_launch_worker

      }
      else if (thread_state == ompt_state_work_serial){
        // I guess thread is executing sequential code
      }
      else {
        printf("2102 Some other state: %x\n", thread_state);
      }
    }
  }

  if (thread_state == ompt_state_undefined) {
    printf("State is undefined\n");
  }

  if (!innermost_reg) {
    if (thread_state != ompt_state_wait_barrier_implicit) {
      // this can happen
      // state was: 0x101 (ompt_state_overhead)
      printf("No innermost_reg. wait: %d, where: %x, state: %x\n",
             waiting_on_last_implicit_barrier, where_am_I, thread_state);
    }

    if (info_type == 1) {
      // never happened
      printf("Did elider find something. Reg depth: %d\n", region_depth);
    }

    typed_stack_elem(region) *parent_reg = hpcrun_ompt_get_region_data(1);
    if (parent_reg) {
      // case 0:
      // sched_yield
      // __kmp_fork_barrier
      // __kmp_launch_thread
      // __kmp_launch_worker

      // case 1:
      // __kmp_fork_barrier
      // __kmp_launch_thread
      // __kmp_launch_worker
    }

    ompt_data_t *inner_td = hpcrun_ompt_get_task_data(0);
    if (inner_td) {
      // inner_td = {0x0}
      // parent_td = {0x5}

      // case 0:
      // sched_yield
      // __kmp_fork_barrier
      // __kmp_launch_thread
      // __kmp_launch_worker

    }
  }

  if (innermost_reg) {
    ompt_data_t *inner_td = hpcrun_ompt_get_task_data(0);
    if (!inner_td) {
      // case 0:
      // sched_yield
      // __kmp_fork_barrier
      // __kmp_launch_thread
      // __kmp_launch_worker

      // case 1:
      //

    }
  }

  int task_type_flags = -1;
  int thread_num = -1;
  ompt_data_t *inner_td = vi3_hpcrun_ompt_get_task_data(0, &task_type_flags, &thread_num);

  cct_node_t *cct_null = NULL;
  int td_reg_depth = -1;
  int info_type2 = 2;
  if (inner_td)
    task_data_value_get_info(inner_td->ptr, &cct_null, &td_reg_depth);

  if (innermost_reg && inner_td) {
    if (info_type2 == 1) {
      if (innermost_reg->depth == td_reg_depth) {
        if ((thread_num == 0) != hpcrun_ompt_is_thread_region_owner(innermost_reg)) {
          // never happened
          printf("No correspondence in checking region ownershio\n");
        }
      }
    }
  }

  if (waiting_on_last_implicit_barrier) {
    if (!innermost_reg) {
      if (info_type != 2) {
        printf("No region data, but elider found something %d\n", info_type);
      }
    }

    if (innermost_reg) {
      if (!inner_td) {
        if (info_type != 2) {
          printf("No task data, but elider found something: %d\n", info_type);
        }

        if (hpcrun_ompt_is_thread_region_owner(innermost_reg)) {
          printf("It seems I'm master of this region: %lx, (%lx)\n",
                 innermost_reg->region_id, my_upper_bits);
        }
      }
    }

    if (innermost_reg && inner_td) {
      if (inner_td->ptr == 0x0) {
        if (info_type == 1) {
          bool owner = hpcrun_ompt_is_thread_region_owner(innermost_reg);
          if (!owner) {
            printf("I'm not the owner\n");
          }
        }
      }
    }

    if (innermost_reg && inner_td) {
      bool implicit = (task_type_flags & ompt_task_implicit) == ompt_task_implicit;
      if (!implicit) {
        printf("May not be implicit task\n");
      }

      bool master_by_td = thread_num == 0;
      bool master_by_reg = hpcrun_ompt_is_thread_region_owner(innermost_reg);

      if (master_by_reg) {
        if (info_type != 1) {
          // printf("Elider didn't find anything. Inner_depth: %d, td_reg_depth: %d\n",
          //    innermost_reg->depth, td_reg_depth);

          // innermost_reg->depth == td_reg_depth

          if (innermost_reg->depth != td_reg_depth) {
            // never happened
            printf("2264: This never happened for now\n");
          }
          else {
            //printf("I think this should go to master.\n");

            // case 0:
            // __kmp_join_call
            // __kmp_api_GOMP_parallel_40_alias
            // g
            // f
            // e._omp_fn.1

          }
        }
        else {

          if (innermost_reg->depth != region_depth + 1) {

            if (where_am_I == vi3_my_enum_parallel_begin) {
              if (td_reg_depth != region_depth) {
                // never happened
                printf("Can this happen???\n");
              }

              // case 0:
              // sched_yield
              // __kmp_wait_4_ptr
              // __kmp_acquire_ticket_lock
              // __kmp_fork_call
              // __kmp_GOMP_fork_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e._omp_fn.1

            }
            else if (where_am_I == vi3_my_enum_impl_task_begin) {

              // case 0:
              // pthread_getspecific
              // hpcrun_get_thread_data_specific
              // hpcrun_safe_enter
              // ompt_implicit_task (end)
              // __kmp_join_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e._omp_fn.1
              // ...

              // case 1:
              // pthread_getspecific
              // hpcrun_get_thread_data_specific_aval
              // hpcrun_safe_enter
              // ompt_implicit_task (end)
              // __kmp_join_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e._omp_fn.1
              // ...

              // case 2:
              //

            }
            else if (where_am_I == vi3_my_enum_impl_task_end) {
              // case 0:
              // hpcrun_safe_exit
              // ompt_implicit_task (end)
              // __kmp_join_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e._omp_fn.1
              // ...

              // case 1:
              // __ompt_get_task_info_object
              // __kmp_join_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e._omp_fn.1
              // ...

              // case 2:
              // __tls_get_addr
              // __kmp_get_global_thread_id
              // __ompt_get_task_info_object
              // __kmp_join_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e._omp_fn.1
              // ...

              // case 3:
              // __kmp_join_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e._omp_fn.1
              // ...
            }
            else if (where_am_I == vi3_my_enum_parallel_end) {
              // case 0:
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e._omp_fn.1

              // case 1:
              // __kmp_fork_call
              // __kmp_GOMP_fork_call
              // __kmp_api_GOMP_parallel_40_alias
              // g
              // f
              // e._omp_fn.1

              // case 2:
              // g
              // f
              // e._omp_fn.1
              // ...
            }

          }



        }
      }
      else {
        if (info_type == 1) {
          // printf("inner->ptr: %p, td_reg_depth: %d, region_depth: %d\n",
          //     inner_td->ptr, td_reg_depth, region_depth);

          if (td_reg_depth != region_depth) {
            // never happened
            printf("2403: td_reg_depth != region_depth\n");
          }

          if (innermost_reg->depth != td_reg_depth) {
            // never happened
            printf("2407: innermost_reg->depth != td_reg_depth");
          }

          if (innermost_reg->depth != region_depth) {
            // never happened
            printf("2411: innermost_reg->depth != region_depth");
          }

          // innermost_reg->depth == td_reg_depth == region_depth

          if (where_am_I == vi3_my_enum_parallel_begin) {
            // case 0:
            // sched_yield
            // __kmp_wait_4_ptr
            // __kmp_acquire_ticket_lock
            // __kmp_fork_call
            // __kmp_GOMP_fork_call
            // __kmp_api_GOMP_parallel_40_alias
            // g
            // f
            // e._omp_fn.1
            // ...
          }
          else if (where_am_I == vi3_my_enum_impl_task_end) {
            // case 0:
            // __kmp_release_ticket_lock
            // __kmp_join_call
            // __kmp_api_GOMP_parallel_40_alias
            // g
            // f
            // e._omp_fn.1
            // ...

            // case 1:
            // hpcrun_safe_enter
            // ompt_prallel_end
            // __kmp_join_call
            // __kmp_api_GOMP_parallel_40_alias
            // g
            // f
            // e._omp_fn.1

            // case 2:
            // __kmp_join_call
            // __kmp_api_GOMP_parallel_40_alias
            // g
            // f
            // e._omp_fn.1
            // ...
          }
          else if (where_am_I == vi3_my_enum_parallel_end) {
            // case 0:
            // hpcrun_safe_enter
            // ompt_parallel_begin
            // __kmp_fork_call
            // __kmp_GOMP_fork_call
            // __kmp_api_GOMP_parallel_40_alias
            // g
            // f
            // e._omp_fn.1
            // ...

            // case 1:
            // __kmp_GOMP_fork_call
            // __kmp_api_GOMP_parallel_40_alias
            // g
            // f
            // e._omp_fn.1
            // ...

            // case 2:
            // __kmp_fork_call
            // __kmp_GOMP_fork_call
            // __kmp_api_GOMP_parallel_40_alias
            // g
            // f
            // e._omp_fn.1
            // ...


          }
          else {
            // never happened
            printf("What this can be??? %d\n", where_am_I);
          }


        }
      }


#if 0
      if (thread_num == 0) {
        // BIG NOTE VI3: should be safe to register for the innermost region

        // thread is master
        // expect that elider found parent task_data
        if (info_type != 1) {
          // case 0:
          // __kmp_join_barrier
          // __kmp_internal_join
          // __kmp_api_GOMP_parallel_40_alias
          // a
          // main

          // initial master thread
        }
        else {
          if (region_depth + 1 != innermost_reg->depth) {

            if (region_depth != innermost_reg->depth) {
              // never happened
              printf("It should be at least innermost region\n");
            }
            else {
              if (where_am_I == vi3_my_enum_parallel_begin) {
                // FIXME vi3: this kind of waiting_on_lasti_implicit_barrier is not accurate
                // case 0:
                // sched_yield
                // __kmp_wait_4_ptr
                // __kmp_acquire_ticket_lock
                // __kmp_fork_call
                // __kmp_GOMP_fork_call
                // __kmp_api_GOMP_parallel_40_alias
                // g
                // f
                // e._omp_fn.1
                // ...

              }
              else if (where_am_I == vi3_my_enum_impl_task_begin) {
                // case 0:
                // sched_yield
                // __kmp_wait_4_ptr
                // __kmp_acquire_ticket_lock
                // __kmp_join_call
                // __kmp_api_GOMP_parallel_40_alias
                // g
                // f
                // e._omp_fn.1
              }
              else if (where_am_I == vi3_my_enum_impl_task_end) {
                // case 0:
                // __kmp_release_ticket_lock
                // __kmp_join_call
                // __kmp_api_GOMP_parallel_40_alias
                // e
                // d
                // c._omp_fn.2

                // case 1:
                // __kmp_release_ticket_lock
                // __kmp_join_call
                // __kmp_api_GOMP_parallel_40_alias
                // g
                // f
                // e._omp_fn.2
                // ...

              }
              else if (where_am_I == vi3_my_enum_parallel_end) {
                // case 0:
                // sched_yield
                // __kmp_join_barrier
                // __kmp_internal_join
                // __kmp_api_GOMP_parallel_40_alias
                // e
                // d
                // c._omp_fn.2
                // ...
              }
              else {
                printf("2277: Unexpected state\n");
              }
            }



          }
        }
      }
      else {
        if (inner_td->ptr != 0x0) {
          printf("Da li je realno ovo???\n");
        }
      }
#endif

    }


  }

  if (innermost_reg && inner_td) {
    if (!hpcrun_ompt_is_thread_region_owner(innermost_reg)) {
      ompt_frame_t *frame0 = hpcrun_ompt_get_task_frame(0);
      if (info_type == 2) {
        if (!frame0) {
          if (thread_state == ompt_state_wait_barrier_implicit) {
            // case 0:
            // sched_yield
            // __kmp_fork_barrier
            // __kmp_launch_thread
            // __kmp_launch_worker
          }
          else {
            printf("I should always have it: %p, Thread state: %x\n", frame0, thread_state);
          }
        }
        else {
          if (frame0->enter_frame.value != 0x0 || frame0->exit_frame.value !=0x0) {
            if (frame0->exit_frame.value != 0x0) {
              // case 0:
              // __kmp_invoke_task_func
              // __kmp_launch_thread
              // __kmp_launch_worker

              // case 1:
              // hpcrun_safe_enter
              // ompt_implicit_task (end)
              // __ompt_implicit_task_end
              // __kmp_fork_barrier
              // __kmp_launch_thread
              // __kmp_launch_worker


              // vi3_idle_collapsed = 0
            }

            if (frame0->enter_frame.value != 0x0) {
              printf("enter: %p\n", frame0->enter_frame.ptr);
              
            }
          }
        }

        // whats the value of inner_td->ptr

        if (thread_state == ompt_state_wait_barrier_implicit) {

        }
        else if (thread_state == ompt_state_work_parallel) {
          // case 0:
          // __kmp_invoke_task_func
          // __kmp_launch_thread
          // __kmp_launch_worker

          if (inner_td->ptr != 0x0) {
            // case 0:
            // __kmp_invoke_task_func
            // __kmp_launch_thread
            // __kmp_launch_worker

            // case 1:
            // __kmp_run_after_invoket_task
            // __kmp_invoke_task_func
            // __kmp_launch_thread
            // __kmp_launch_worker
          }

        }
        else if (thread_state == ompt_state_overhead) {
          // case 0:
          // ompt_implicit_task (end)
          // __omp_implicit_task_end
          // __kmp_fork_barrier
          // __kmp_launch_thread
          // __kmp_launch_worker

          // case 1:
          // __kmp_launch_thread
          // __kmp_launch_worker

          // case 2:
          // __kmp_fork_barrier
          // __kmp_launch_thread
          // __kmp_launch_worker

          // case 3:
          // pthread_getspecfic
          // hpcrun_get_thread_data_specifi
          // hpcrun_safe_enter
          // ompt_implicit_task (end)
          // __ompt_implicit_task_end
          // __kmp_fork_barrier
          // __kmp_launch_thread
          // __kmp_launch_worker

          if (inner_td->ptr != 0x0) {
            // case 0:
            // __tls_get_addr
            // ompt_sync
            // __kmp_join_barrier
            // __kmp_launch_thread
            // __kmp_launch_worker

            // case 1:
            // ompt_sync
            // __kmp_join_barrier
            // __kmp_launch_thread
            // __kmp_launch_worker

          }
        }
        else if (thread_state == ompt_state_idle) {
        }
        else {
          printf("Some other state: %x\n", thread_state);
        }
      }
      else {

        if (vi3_idle_collapsed) {
          // never
          printf("May be idling\n");
        }


        if (!frame0) {
          // never happened
          printf("This may happen\n");
        }
        else {
          if (frame0->exit_frame.value == 0x0 && frame0->enter_frame.value == 0x0) {
            // never happened
            printf("No exit or enter frame\n");
          }
        }


        if (inner_td->ptr == 0x0) {
          // vi3 to vi3 - use this ... this won't happen
          printf("Is this possible\n");
        }
        if (thread_state == ompt_state_wait_barrier_implicit) {
          // never happened
          printf("No disjunct\n");
        }
        else if (thread_state == ompt_state_work_parallel) {
          // case 0:
          // loop2
          // g._omp_fn.0
        }
        else if (thread_state == ompt_state_overhead) {
          // case 0:
          // __kmp_wait_4_ptr
          // __kmp_acquire_ticket_lock
          // __kmp_fork_call
          // __kmp_GOMP_fork_call
          // __kmp_api_GOMP_prallel_40_alias
          // e
          // d
          // c._omp_fn.2

          // case 1:
          // __kmp_bakery_check
          // __kmp_wait_4_ptr
          // __kmp_acquire_ticket_lock
          // __kmp_fork_call
          // __kmp_GOMP_fork_call
          // __kmp_api_GOMP_prallel_40_alias
          // g
          // f
          // e._omp_fn.1

          // case 2:
          // sched_yield
          // __kmp_wait_4_ptr
          // __kmp_acquire_ticket_lock
          // __kmp_fork_call
          // __kmp_GOMP_fork_call
          // __kmp_api_GOMP_prallel_40_alias
          // g
          // f
          // e._omp_fn.1
        }
        else {
          printf("What else to expect to see this: %x\n", thread_state);
        }
      }
    }
  }
#endif

#if 0
  typed_stack_elem(region) *innermost_region = hpcrun_ompt_get_region_data(0);
  if (innermost_region && innermost_region->depth >= 1) {
    // there is at least one outer parallel region
    typed_stack_elem(region) *parent_region = hpcrun_ompt_get_region_data(1);
    if (!parent_region) {
      innermost_region_present_but_not_parent(innermost_region, parent_region);
    }
  }

  // invalidate value
  vi3_last_to_register = -1;

  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context),
                                           &omp_task_context, &region_depth);

  if (waiting_on_last_implicit_barrier) {
    // check if thread_data is available and contains any useful information
    if (info_type == 2) {
      // Thread is waiting on the last implicit barrier.
      // OMPT frames are not set properly (see ompt_elide_runtime_frame).
      // Thread cannot guarantee that it is still part of any parallel team.
      // Safe thing to do is to skip registration process
      // and attribute the sample to the thread local (idle) placeholder.
      return;
    }
  }


  // Try to find active region in which thread took previous sample
  // (in further text lca->region_data)
  typed_random_access_stack_elem(region) *lca;
  if (!least_common_ancestor(&lca, region_depth)) {
    // There is no active regions, so there is no regions to register for.
    // Just return, since thread should be executing sequential code.
    return;
  }

  int start_from = 0;
  cct_node_t *parent_cct = NULL;

  if (lca) {
    // Optimization: Thread will register for regions nested
    // inside lca->region_data
    start_from = lca->region_data->depth;
    parent_cct = lca->unresolved_cct;
  } else {
    // Thread must register for all active region,
    // starting from the outermost one.
    start_from = 0;
    parent_cct = hpcrun_get_thread_epoch()->csdata.thread_root;
  }

  if (!parent_cct) {
    printf("least_common_ancestor - parent_cct missing: %d (%lx, %p)\n",
        start_from, lca->region_data->region_id, lca->unresolved_cct);
  }

  // registration process
  // start_from: thread will register for regions which
  //   depths are >= "start_from"
  // parent_cct: the parent cct_node of the unresolved_cct
  //   which corresponds to the region at depth "start_from".
  typed_random_access_stack_reverse_iterate_from(region)(start_from,
                                                         region_stack,
                                                         thread_take_sample,
                                                         &parent_cct);
#endif
#endif

  // check if thread should register for active regions
  if (!safe_to_register_for_active_regions()) {
    // Not safe to register for call paths of regions present on the stack
    // For more information see safe_to_register_for_active_regions.

    // Notify ompt_cct_cursor_finalize that sample should not be attributed
    // to any region present on the stack.
    registration_safely_applied = false;
    return;
  }

  // Least common ancestor algorithm will try to find active region in which
  // thread already took a sample.
  // If there is no that kind of region, it will return the outermost region.
  typed_random_access_stack_elem(region) *lca_el = least_common_ancestor();

  cct_node_t *parent_cct;
  int start_from;

  if (lca_el->unresolved_cct) {
    // thread took sample inside this region, so register only for nested regions
    parent_cct = lca_el->unresolved_cct;
    start_from = lca_el->region_data->depth + 1;
  } else {
    // lca_el->region_data is the outermost region and thread haven't taken
    // sample in it, so it needs to register for all regions
    // present on the stack.
    parent_cct = hpcrun_get_thread_epoch()->csdata.thread_root;
    start_from = 0;
  }

  // Registration process
  typed_random_access_stack_reverse_iterate_from(region)(start_from,
      region_stack, thread_take_sample, &parent_cct);

  // Notify ompt_cct_cursor_finalize that cursor should be unresolved_cct
  // of the innermost region
  registration_safely_applied = true;

  // All remained samples attributed to the local placeholder belongs to
  // innermost region which is still active.
  attr_idleness2innermost_region();
}


void
resolve_one_region_context
(
  typed_stack_elem_ptr(region) region_data,
  cct_node_t *unresolved_cct
)
{
  // region to resolve
  cct_node_t *parent_unresolved_cct = hpcrun_cct_parent(unresolved_cct);

  if (parent_unresolved_cct == NULL) {
    msg_deferred_resolution_breakpoint("resolve_one_region_context: Parent of unresolved_cct node is missing\n");
  }
  else if (region_data->call_path == NULL) {
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

  resolve_one_region_context(old_head->region_data, old_head->unresolved_cct);

  // check if the notification needs to be forwarded
  typed_stack_elem_ptr(notification) next =
    typed_stack_pop(notification, sstack)(&old_head->region_data->notification_stack);
  if (next) {
    typed_channel_shared_push(notification)(next->notification_channel, next);
  } else {
    // notify creator of region that region_data can be put in region's freelist
    hpcrun_ompt_region_free(old_head->region_data);
    // FIXME vi3 >>> Need to review freeing policies for all data types (structs)
  }

  // free notification
  hpcrun_ompt_notification_free(old_head);

  return 1;
}

bool
any_idle_samples_remained
(
  void
)
{
  // check if there is some non-attributed (unresolved) idle sample
  return local_idle_placeholder != NULL;
}


void
attr_idleness2_cct_node
(
  cct_node_t *cct_node
)
{
#if 1
  if (!cct_node) {
    printf("<<<ompt-defer.c:1027>>> Missing cct_node\n");
    return;
  }

  // merge children of local_idle_placeholder to cct_node
  hpcrun_cct_merge(cct_node, local_idle_placeholder, merge_metrics, NULL);
  // Remove and invalidate local_idle_placeholder as indication that idle samples
  // have been attributed to the proper position.
  hpcrun_cct_delete_self(local_idle_placeholder);
  local_idle_placeholder = NULL;
#endif
}


void
attr_idleness2outermost_ctx
(
  void
)
{
  // This if may be put inside attr_idleness2_cct_node.
  // The problem that may occur is that thread asks for thread_root
  // and then finds that there is no idle samples under idle placeholder.
  // In that case, thread just lost cycles getting information about
  // thread_root that won't use.
  if (any_idle_samples_remained()) {
    attr_idleness2_cct_node(hpcrun_get_thread_epoch()->csdata.thread_root);
  }
}


void
attr_idleness2innermost_region
(
  void
)
{
  // This if may be put inside attr_idleness2_cct_node.
  // The problem that may occur is that thread asks for unresolved_cct
  // which corresponds to the innermost region present on the top of
  // the stack and then finds that there is no idle samples under idle
  // placeholder. In that case, thread just lost cycles getting information
  // about unresolved_cct that won't use.
  if (any_idle_samples_remained()) {
    // NOTE vi3: Happened in nestedtasks.c.
    attr_idleness2_cct_node(hpcrun_ompt_get_top_unresolved_cct_on_stack());
  }
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
  // If any idle samples remained from the previous parallel region,
  // attribute them to the outermost context
  attr_idleness2outermost_ctx();

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

#if FREELISTS_ENABLED
#if FREELISTS_DEBUG
  if (notification_used != 0) {
    printf("*** Mem leak notifications: %ld\n", notification_used);
  }
#if FREELISTS_DEBUG_WAIT_FOR_REGIONS
  // Need to wait for other workers to resolve remaining regions' call paths.
  // After that, all region_data should be free.
  struct timespec start_time_reg;
  size_t ri = 0;
  timer_start(&start_time_reg);
  long region_used_val;
  for(;;ri++) {
    region_used_val = atomic_fetch_add(&region_freelist_channel.region_used, 0);
    if (region_used_val <= 0) {
      if (region_used_val < 0) {
        printf("*** Mem leak regions >>> problem with waiting: %ld\n", region_used_val);
      }
      break;
    }
    // Same as in previous for(;;) loop
    if (timer_elapsed(&start_time_reg) > 3.0) break;
  }
#else
  long region_used_val = atomic_fetch_add(&region_freelist_channel.region_used, 0);
  if (region_used_val != 0 ) {
    printf("*** Mem leak regions: %ld\n", region_used_val);
  }
#endif
#endif
#endif

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
// It seems it's not used anywhere.
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
