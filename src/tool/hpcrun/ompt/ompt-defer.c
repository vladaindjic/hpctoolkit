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
  typed_stack_elem_ptr(region) region_data,
  cct_node_t *unresolved_cct
)
{
  typed_stack_elem_ptr(notification) notification = hpcrun_ompt_notification_alloc();
  notification->region_data = region_data;
  notification->unresolved_cct = unresolved_cct;
  notification->region_prefix = NULL;
  notification->notification_channel = &thread_notification_channel;
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
  // store region data at corresponding position on the stack.
  stack_element->region_data = region_data;
  // invalidate previous value unresolved_cct
  stack_element->unresolved_cct = NULL;
  // invalidate value of team_master,
  // will be set in function add_region_and_ancestors_to_stack
  stack_element->team_master = 0;
  // thread hasn't taken a sample in this region yet
  stack_element->took_sample = 0;
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

  // Values of argument team_master says if the thread is the master of region_data
  typed_random_access_stack_top(region)(region_stack)->team_master = team_master;
}


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

  // FIXME vi3 >>> If we use this approach, then field took_sample is not needed
  // thread took a sample in this region before
  if (el->took_sample) {
    // skip this region, and go to the inner one
    goto return_label;
  }

  // Check if the address that corresponds to the region
  // has already been inserted in parent_cct subtree.
  // This means that thread took sample in this region before.
  cct_addr_t *region_addr = &ADDR2(UNRESOLVED, el->region_id);
  cct_node_t *found_cct = hpcrun_cct_find_addr(parent_cct, region_addr);
  if (found_cct) {
    // Region was on the stack previously, so the thread does not need
    // neither to insert cct nor to register itself for the region's call path.
    // Mark that thread took a sample, store found_cct in el and skip the region.
    el->took_sample = true;
    el->unresolved_cct = found_cct;
    goto return_label;
  }

  // mark that thread took sample in this region for the first time
  el->took_sample = true;
  // insert cct node with region_addr in the parent_cct's subtree
  el->unresolved_cct = hpcrun_cct_insert_addr(parent_cct, region_addr);

  if (!el->team_master) {
    // Worker thread should register for the region's call path
    register_to_region(el->region_data, el->unresolved_cct);
  } else {
    // Master thread also needs to resolve this region at the very end (ompt_parallel_end)
    unresolved_cnt++;
    // If the initial master register for the call path, then it is responsible
    // to process notification and notify other worker about the resolving
    // process. Since ompt_state_idle is not handle properly, the resolving
    // will happen at the time thread is going to be shut down. At that point,'
    // all other worker threads will be destroyed and they cannot be notified
    // about unresolved regions.
    // If ompt_state_idle were handled properly, it's not guaranteed that
    // master will notify other worker threads (registered before master)
    // about unresolved regions.
  }

  return_label: {
    // If region is marked as the last to register (see function register_to_all_regions),
    // then stop further processing.
    // FIXME vi3 >>> this should be removed.
    if (el->region_data->depth >= vi3_last_to_register) {
      // Indication that processing of active regions should stop.
      return 1;
     }
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


typedef struct lca_args_s {
  int level;
  typed_stack_elem(region) *region_data;
} lca_args_t;


void
check_thread_num
(
  typed_random_access_stack_elem(region) *el,
  int level
)
{
  int old_level = level;
  typed_stack_elem(region) *rd0 = hpcrun_ompt_get_region_data(level);

  if (rd0 != el->region_data) {
    printf("Something is really wrong\n");
  }

  int flags0;
  ompt_frame_t *frame0;
  ompt_data_t *task_data = NULL;
  ompt_data_t *parallel_data = NULL;
  int thread_num = -1;
  hpcrun_ompt_get_task_info(level, &flags0, &task_data, &frame0,
                            &parallel_data, &thread_num);

  typed_stack_elem(region) *rd1 = ATOMIC_LOAD_RD(parallel_data);
  if (rd1 && rd0 && rd1 != rd0) {
    if (level == 0) {
      printf("This shouldn't be the case\n");
    }
    if (rd1->depth > rd0->depth) {
      level++;
    } else if (rd1->depth < rd0->depth) {
      level--;
    } else {
      printf("How to handle this?\n");
    }
    // FIXME:
  }
  thread_num = -1;
  hpcrun_ompt_get_task_info(level, &flags0, &task_data, &frame0,
                            &parallel_data, &thread_num);

  rd1 = ATOMIC_LOAD_RD(parallel_data);
  if (rd1 && rd0 && rd1 != rd0) {
    printf("Why\n");
  }

  if (!el->team_master) {
    if (thread_num == 0) {
      printf("Problem: level: %d, old_level: %d\n", level, old_level);
      hpcrun_ompt_get_task_info(level, &flags0, &task_data, &frame0,
                                &parallel_data, &thread_num);
    }
  }
}

bool
is_thread_owner_of_the_region
(
  typed_random_access_stack_elem(region) *el,
  int level
)
{
#if 0
  int old_level = level;
  typed_stack_elem(region) *rd0 = hpcrun_ompt_get_region_data(level);

  int flags0;
  ompt_frame_t *frame0;
  ompt_data_t *task_data = NULL;
  ompt_data_t *parallel_data = NULL;
  int thread_num = -1;
  int retVal = hpcrun_ompt_get_task_info(level, &flags0, &task_data, &frame0,
                            &parallel_data, &thread_num);
  if (retVal != 2) {
    printf("No parallel_data at this level: %d\n", level);
    exit(-1);
  }

  typed_stack_elem(region) *rd1 = ATOMIC_LOAD_RD(parallel_data);
  if (rd0 && rd1) {
    if (rd0 != rd1) {
      // adjust level
      if (rd0->depth > rd1->depth) {
        level--;
      } else if (rd0->depth < rd1->depth) {
        level++;
      } else {
        printf("Cannot adjust: rd0->depth: %d, rd1->depth: %d\n", rd0->depth, rd1->depth);
      }
    }
  } else {
    // FIXME VI3: DEBUG
    printf("Something is missing - rd0: %p, rd1: %p\n", rd0, rd1);
    // TODO vi3: initialization isn't done properly
  }

  retVal = hpcrun_ompt_get_task_info(level, &flags0, &task_data, &frame0,
                            &parallel_data, &thread_num);
  if (retVal != 2) {
    printf("No parallel_data at this level: %d, old_level: %d\n", level, old_level);
    //exit(-1);
  }
  rd1 = ATOMIC_LOAD_RD(parallel_data);
  if (!rd1) {
    // FIXME VI3: DEBUG
    printf("No rd1 at this level: %d\n", level);
    // TODO vi3: It is possible that rd1 is not initialized.
  }
  // Master of the region has thread_num equals to zero.
  return thread_num == 0;
#endif
  int flags0;
  ompt_frame_t *frame0;
  ompt_data_t *task_data = NULL;
  ompt_data_t *parallel_data = NULL;
  int thread_num = -1;
  int retVal = hpcrun_ompt_get_task_info(level, &flags0, &task_data, &frame0,
                                         &parallel_data, &thread_num);
  return thread_num == 0;
}



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
    if (el->region_id == reg->region_id) {
      // indicator to stop processing stack elements
      return 1;
    }
  }
  // update stack element
  // store region
  el->region_data = reg;
  // store region_id as persistent field
  el->region_id = reg->region_id;
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
  // check if thread is the master (owner) of the reg
  el->team_master = hpcrun_ompt_is_thread_region_owner(reg);
  //check_thread_num(el, level);
#else
  el->team_master = is_thread_owner_of_the_region(el, level);
#endif
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
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
  args->region_data = hpcrun_ompt_get_region_data(args->level);
#else
  args->region_data = hpcrun_ompt_get_region_data_from_task_info(args->level);
#endif
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
  int ancestor_level = 0;
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
  typed_stack_elem(region) *innermost_reg = hpcrun_ompt_get_region_data(ancestor_level);
#else
  typed_stack_elem(region) *innermost_reg = hpcrun_ompt_get_region_data_from_task_info(ancestor_level);
#endif
  if (!innermost_reg) {
    // There is no parallel region active.
    // Thread should be executing sequential code.
    *lca = NULL;
    return false;
  }

  if (td_region_depth >= 0) {
    // skip regions deeper than td_region_depth
    while(innermost_reg->depth > td_region_depth) {
#if KEEP_PARENT_REGION_RELATIONSHIP
      // skip me by using my parent
      innermost_reg = typed_stack_next_get(region, sstack)(innermost_reg);
#else
      // skip this region and access to parent
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
      innermost_reg = hpcrun_ompt_get_region_data(++ancestor_level);
#else
      innermost_reg = hpcrun_ompt_get_region_data_from_task_info(++ancestor_level);
#endif

#endif
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


void
register_to_all_regions
(
  void
)
{
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

  // invalidate value
  vi3_last_to_register = -1;

  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context),
                                           &omp_task_context, &region_depth);

#if DETECT_IDLENESS_LAST_BARRIER
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
#else
  // FIXME vi3: Any information get from runtime that may help?
  if (info_type == 2) {
    // If elider cannot find task data, I guess it is safe to skip registering,
    // unless we get some secure information from runtime
    return;
  }
#endif

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
  // NOTE vi3 >>> I put this function call inside ompt_cct_cursor_finalize function
  //   since I'm still not sure why/how thread_data can change its value while
  //   our tool is in the middle of sample processing.
      // If any idle samples have been previously taken inside this region,
      // attribute them to it.
      // attr_idleness2region_at(vi3_last_to_register);
}


void
resolve_one_region_context
(
  cct_node_t *region_call_path,
  cct_node_t *unresolved_cct
)
{
  // region to resolve
  cct_node_t *parent_unresolved_cct = hpcrun_cct_parent(unresolved_cct);

  if (parent_unresolved_cct == NULL) {
    msg_deferred_resolution_breakpoint("resolve_one_region_context: Parent of unresolved_cct node is missing\n");
  } else if (region_call_path == NULL) {
    msg_deferred_resolution_breakpoint("resolve_one_region_context: Region call path is missing\n");
  } else {
    // region prefix (region_call_path) should be put
    // between unresolved_cct and parent_unresolved_cct
    // Note: hpcrun_cct_insert_path_return_leaf ignores top cct of the path
    cct_node_t *prefix =
        hpcrun_cct_insert_path_return_leaf(parent_unresolved_cct,
            region_call_path);

    if (prefix == NULL) {
      msg_deferred_resolution_breakpoint("resolve_one_region_context: Prefix is not properly inserted\n");
    }

    if (prefix != unresolved_cct) {
      // prefix node should change the unresolved_cct
      // FIXME vi3: Some cct nodes lost theirs parents.
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

  // check if the notification needs to be forwarded
  typed_stack_elem_ptr(notification) next =
    typed_stack_pop(notification, sstack)(&old_head->region_data->notification_stack);
  if (next) {
    // store region_prefix and notify next worker in the chain
    next->region_prefix = old_head->region_prefix;
    typed_channel_shared_push(notification)(next->notification_channel, next);
  } else {
    // notify creator of region that region_data can be put in region's freelist
    hpcrun_ompt_region_free(old_head->region_data);
    // FIXME vi3 >>> Need to review freeing policies for all data types (structs)
  }

  resolve_one_region_context(old_head->region_prefix, old_head->unresolved_cct);
  // free notification
  hpcrun_ompt_notification_free(old_head);

  return 1;
}

#if DETECT_IDLENESS_LAST_BARRIER
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
attr_idleness2region_at
(
  int depth
)
{
  // This if may be put inside attr_idleness2_cct_node.
  // The problem that may occur is that thread asks for unresolved_cct
  // which corresponds to the region present on the top of the stack
  // and then finds that there is no idle samples under idle placeholder.
  // In that case, thread just lost cycles getting information about
  // unresolved_cct that won't use.
  if (any_idle_samples_remained()) {
    // NOTE vi3: Happened in nestedtasks.c.
    attr_idleness2_cct_node(hpcrun_ompt_get_top_unresolved_cct_on_stack());
  }
}
#endif

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
#if DETECT_IDLENESS_LAST_BARRIER
  // If any idle samples remained from the previous parallel region,
  // attribute them to the outermost context
  attr_idleness2outermost_ctx();
#endif

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

void
initialize_regions_if_needed
(
  void
)
{
  int flags0;
  ompt_frame_t *frame0;
  ompt_data_t *task_data = NULL;
  ompt_data_t *parallel_data = NULL;
  int thread_num = 0;
  hpcrun_ompt_get_task_info(0, &flags0, &task_data, &frame0,
                            &parallel_data, &thread_num);
  // TODO: Any more edge cases about sequential code.
  if (flags0 & ompt_task_initial) {
    // executing sequential code
    //printf("Sequential code\n");
    return;
  }

  if (parallel_data) {
    initialize_region(0);
  } else {
    // TODO: Go one level above?
  }
}

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
