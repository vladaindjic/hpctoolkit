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
#include "ompt-queues.h"
#include "ompt-region-debug.h"
#include "ompt-placeholders.h"
#include "ompt-thread.h"
#include "ompt-region.h"



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


static void
deferred_resolution_breakpoint
(
)
{
  // set a breakpoint here to identify problematic cases
}


static void
omp_resolve
(
 cct_node_t* cct, 
 cct_op_arg_t a, 
 size_t l
)
{
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



// FIXME: move this function at better place

int 
get_stack_index
(
 ompt_region_data_t *region_data
)
{
  int i;
  for (i = top_index; i>=0; i--) {
    if (region_stack[i].notification->region_data->region_id == region_data->region_id) {
      return i;
    }
  }
  return -1;
}

/*
 * r0
 *   r1
 *     r2 -> r3 (thread 123) -> r4 -> r5
 *        -> r6 -> r7 -> r8
 *     r9 (thread 123) -> r10
 * */


ompt_notification_t*
help_notification_alloc
(
 ompt_region_data_t *region_data
)
{
  ompt_notification_t *notification = hpcrun_ompt_notification_alloc();
  notification->region_data = region_data;
  notification->region_id = region_data->region_id;
  notification->threads_queue = &threads_queue;
  // reset unresolved_cct for the new notification
  notification->unresolved_cct = NULL;

  return notification;
}


void
swap_and_free
(
 ompt_region_data_t* region_data
)
{

  int depth = region_data->depth;
  ompt_notification_t *notification;
  region_stack_el_t *stack_element;

  // If notification at depth index of the stack
  // does not have initialize next pointer, that means
  // that is not enqueue anywhere and is not in the freelist,
  // which means we can free it here.
  stack_element = &region_stack[depth];
  notification = stack_element->notification;
  // we can free notification either if the thread is the master of the region
  // or the thread did not take a sample inside region
  if (notification && (stack_element->team_master || !stack_element->took_sample)) {
    hpcrun_ompt_notification_free(notification);
  }

  // add place on the stack for the region
  notification = help_notification_alloc(region_data);
  region_stack[depth].notification = notification;
  // thread could be master only for region_data, see explanation
  // given in comment below
  region_stack[depth].team_master = 0;
  region_stack[depth].took_sample = 0;

  // I previosly use this as a condition to free notification,
  // which is bad and the explanation is below
  // Notification can be at the end of the queue
  // and this condition does not check thath
  //    if (OMPT_BASE_T_GET_NEXT(notification) == NULL
  //        && wfq_get_next(OMPT_BASE_T_STAR(notification)) == NULL) {

}

void
add_region_and_ancestors_to_stack
(
 ompt_region_data_t *region_data, 
 bool team_master
)
{

  if (!region_data) {
    // printf("*******************This is also possible. ompt-defer.c:394");
    return;
  }

  ompt_region_data_t *current = region_data;
  int level = 0;
  int depth;

  // up through region stack until first region which is on the stack
  // if none of region is on the stack, then stack will be completely changed
  while (current) {
    depth = current->depth;
    // found region which is on the stack
    if (depth <= top_index &&
        region_stack[depth].notification->region_data->region_id == current->region_id) {
      break;
    }
    swap_and_free(current);
    // get the parent
    current = hpcrun_ompt_get_region_data(++level);
  }

  // region_data is the new top of the stack
  top_index = region_data->depth;
  // FIXME vi3: should check if this is right
  // If the stack content does not corresponds to ancestors of the region_data,
  // then thread could only be master of the region_data, but not to its ancestors.
  // Values of argument team_master says if the thread is the master of region_data
  region_stack[top_index].team_master = team_master;

}


cct_node_t*
add_pseudo_cct
(
 ompt_region_data_t* region_data
)
{
  // should add cct inside the tree
  cct_node_t* new;
  if (top_index == 0) {
    // this is the first parallel region add pseudo cct
    // which corresponds to the region as a child of thread root
    new = hpcrun_cct_insert_addr((hpcrun_get_thread_epoch()->csdata).thread_root,
                                 &(ADDR2(UNRESOLVED, region_data->region_id)));
  } else {
    // add cct as a child of a previous pseudo cct
    new = hpcrun_cct_insert_addr(region_stack[top_index - 1].notification->unresolved_cct,
                                 &(ADDR2(UNRESOLVED, region_data->region_id)));
  }

  return new;
}


void
register_to_region
(
 ompt_notification_t* notification
)
{
  ompt_region_data_t* region_data = notification->region_data;

  ompt_region_debug_notify_needed(notification);

  // create notification and enqueu to region's queue
  OMPT_BASE_T_GET_NEXT(notification) = NULL;

  // register thread to region's wait free queue
  wfq_enqueue(OMPT_BASE_T_STAR(notification), &region_data->queue);

  // increment the number of unresolved regions
  unresolved_cnt++;
}

ompt_notification_t*
add_notification_to_stack
(
 ompt_region_data_t* region_data
)
{
    ompt_notification_t* notification = help_notification_alloc(region_data);
    notification->region_data = region_data;
    notification->threads_queue = &threads_queue;
    // push to stack
    push_region_stack(notification, 0, 0);
    return notification;
}


void
register_if_not_master
(
 ompt_notification_t *notification
)
{
  if (notification && not_master_region == notification->region_data) {
    register_to_region(notification);
    // should memoize the cct for not_master_region
    cct_not_master_region = notification->unresolved_cct;
  }
}


int
get_took_sample_parent_index
(
 void
)
{
  int i;
  for (i = top_index; i >= 0; i--) {
    if (region_stack[i].took_sample) {
      return i;
    }
  }
  return -1;
}


void
register_to_all_regions
(
 void
)
{
  // find ancestor on the stack in which we took a sample
  // a go until the top of the stack
  int start_register_index = get_took_sample_parent_index() + 1;

  // mark that all descendants of previously mentioned ancestor took a sample
  // for all descendants where thread is not the master in, register for descendant callpath
  int i;
  region_stack_el_t *current_el;
  cct_node_t *parent_cct;
  cct_node_t *new_cct;
  for (i = start_register_index; i <=top_index; i++) {
    current_el = &region_stack[i];
    // mark that we took sample
    current_el->took_sample = true;
    if (!current_el->team_master) {
      // register for region's call path if not the master
      register_to_region(current_el->notification);
      // add unresolved cct at some place underneath thread root
      // find parent of new_cct
      parent_cct = (i == 0) ? hpcrun_get_thread_epoch()->csdata.thread_root
              : region_stack[i-1].notification->unresolved_cct;

      if (current_el->notification->region_data->region_id == 0) {
	    deferred_resolution_breakpoint();
      }
      // insert cct as child of the parent_cct
      new_cct =
              hpcrun_cct_insert_addr(parent_cct,
                                     &(ADDR2(UNRESOLVED, current_el->notification->region_data->region_id)));
      // remebmer cct
      current_el->notification->unresolved_cct = new_cct;

      cct_not_master_region = current_el->notification->unresolved_cct;

    }
  }


}


// insert a path to the root and return the path in the root
cct_node_t*
hpcrun_cct_insert_path_return_leaf_tmp
(
 cct_node_t *root,
 cct_node_t *path
)
{
    if (!path) return root;
    cct_node_t *parent = hpcrun_cct_parent(path);
    if (parent) {
      root = hpcrun_cct_insert_path_return_leaf_tmp(root, parent);
    }
    return hpcrun_cct_insert_addr(root, hpcrun_cct_addr(path));
}

// return one if a notification was processed
int
try_resolve_one_region_context
(
 void
)
{
  ompt_notification_t *old_head = NULL;

  old_head = (ompt_notification_t*) 
    wfq_dequeue_private(&threads_queue, OMPT_BASE_T_STAR_STAR(private_threads_queue));

  if (!old_head) return 0;

  unresolved_cnt--;

  // region to resolve
  ompt_region_data_t *region_data = old_head->region_data;

  ompt_region_debug_notify_received(old_head);

  // ================================== resolving part
  cct_node_t *unresolved_cct = old_head->unresolved_cct;
  cct_node_t *parent_unresolved_cct = hpcrun_cct_parent(unresolved_cct);

  if (parent_unresolved_cct == NULL || region_data->call_path == NULL) {
    deferred_resolution_breakpoint();
  } else {
    // prefix should be put between unresolved_cct and parent_unresolved_cct
    cct_node_t *prefix = NULL;
    cct_node_t *region_call_path = region_data->call_path;

    // FIXME: why hpcrun_cct_insert_path_return_leaf ignores top cct of the path
    // when had this condtion, once infinity happen
//    if (parent_unresolved_cct == hpcrun_get_thread_epoch()->csdata.thread_root) {
//      // from initial region, we should remove the first one
//      prefix = hpcrun_cct_insert_path_return_leaf(parent_unresolved_cct, region_call_path);
//    } else {
      // for resolving inner region, we should consider all cct nodes from prefix
      prefix = hpcrun_cct_insert_path_return_leaf_tmp(parent_unresolved_cct, region_call_path);
//    }

    if (prefix == NULL) {
      deferred_resolution_breakpoint();
    }

    if (prefix != unresolved_cct) {
      // prefix node should change the unresolved_cct
      hpcrun_cct_merge(prefix, unresolved_cct, merge_metrics, NULL);
      // delete unresolved_cct from parent
      hpcrun_cct_delete_self(unresolved_cct);
    } else {
      //printf("*********************** try_resolve_one_region_context else branch\n");
      deferred_resolution_breakpoint();
    }
    // ==================================
  }

  // free notification
  hpcrun_ompt_notification_free(old_head);

  // check if the notification needs to be forwarded 
  ompt_notification_t* next = (ompt_notification_t*) wfq_dequeue_public(&region_data->queue);
  if (next) {
    wfq_enqueue(OMPT_BASE_T_STAR(next), next->threads_queue);
  } else {
    // notify creator of region that region_data can be put in region's freelist
    hpcrun_ompt_region_free(region_data);
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


void 
ompt_resolve_region_contexts
(
 int is_process
)
{
  struct timespec start_time;

  size_t i = 0;
  timer_start(&start_time);

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

  if (unresolved_cnt != 0 && hpcrun_ompt_region_check()) {
    // hang to let debugger attach
    volatile int x;
    for(;;) {
      x++;
    };
  }

  // FIXME vi3: find all memory leaks
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

void
top_cct_node
(
  cct_node_t **cct,
  frame_t **it,
  frame_t *end
)
{
  // path_beg ==> end
  // path_end ==> start
  // (everything is reversed compare to cct_insert_backtrace.c)
  // FIXME: find how to use extern version of retain_recursion
  bool retain_recursion = false;
  ip_normalized_t child_routine = ip_normalized_NULL;
  // cct_node that corresponds to the end frame address
  for(; *it < end; (*it)++) {
    if ( (! retain_recursion) &&
         (end >= *it + 1) &&
         ip_normalized_eq(&((*it)->the_function), &(child_routine)) &&
         ip_normalized_eq(&((*it)->the_function), &((*it + 1)->the_function))) {
      // do nothing
    } else {
      *cct = hpcrun_cct_parent(*cct);
    }
    child_routine = (*it)->the_function;
  }
}

#define UINT64_T(value) (uint64_t)value

// first_frame_below
frame_t*
first_frame_below
(
  frame_t *start,
  frame_t *end,
  uint64_t frame_address,
  int *index
)
{
  // if frame_address is below the outer_most frame
  // than below for loop makes no sense.
  if (frame_address > UINT64_T(end->cursor.sp)) {
    printf("ispao iz opsega\t");
    deferred_resolution_breakpoint();
    return NULL;
  }

  // path_beg ==> end
  // path_end ==> start (and it is changing (everything is reversed compare to cct_insert_backtrace.c)
  // FIXME: find how to use extern version of retain_recursion
  bool retain_recursion = false;
  ip_normalized_t child_routine = ip_normalized_NULL;
  frame_t *it;
  for (it = start; it <= end; it++) {
      if ( (! retain_recursion) &&
	          (end >= it + 1) &&
            ip_normalized_eq(&(it->the_function), &(child_routine)) &&
	          ip_normalized_eq(&(it->the_function), &((it + 1)->the_function))) {
        // do nothing
      } else {
        if (UINT64_T(it->cursor.sp) >= frame_address){
          return it;
        }
        (*index)++;
      }
      child_routine = it->the_function;
  }
//  for(it = start; it <= end; it++, (*index)++) {
//    // FIXME: exit frame of current should be the same as enter_frame.ptr of previous frane
//    if (UINT64_T(it->cursor.sp) >= frame_address){
//      return it;
//    }
//  }
  return NULL;
}

int
first_frame_below_tmp
(
  frame_t **it,
  uint64_t frame_address,
  frame_t *end,
  cct_node_t **cct
)
{
  // all frames are below, so take first
  if (UINT64_T((*it)->cursor.sp) > frame_address ) {
    return 1;
  }
  // all frames are above, so we can take the closest one
  // to the frame_address
  if (UINT64_T(end->cursor.sp) <= frame_address) {
//    *it = end;
//    *cct = hpcrun_cct_children(top_cct(*cct)); // top_cct is UNRESOLVED or THREAD_ROOT
    top_cct_node(cct, it, end);
    return 0; // FIXME vi3 >> consider equality ==
  }

  // path_beg ==> end
  // path_end ==> start (and it is changing (everything is reversed compare to cct_insert_backtrace.c)
  // FIXME: find how to use extern version of retain_recursion
  bool retain_recursion = false;
  ip_normalized_t child_routine = ip_normalized_NULL;
  for(; (*it) <= end; (*it)++) {
    if ( (! retain_recursion) &&
         (end >= *it + 1) &&
         ip_normalized_eq(&((*it)->the_function), &(child_routine)) &&
         ip_normalized_eq(&((*it)->the_function), &((*it + 1)->the_function))) {
      // do nothing
    } else {
      if (UINT64_T((*it)->cursor.sp) > frame_address){
        return 1;
      }
      *cct = hpcrun_cct_parent(*cct);
    }
    child_routine = (*it)->the_function;
  }
  // what should i else return
  return 0;
}

// first_frame_above
frame_t*
first_frame_above
(
  frame_t *start,
  frame_t *end,
  uint64_t frame_address,
  int *index
)
{



  // exit_frame points above the bt_outer
  // The best we can do is to set it = bt_outer
  // We are doing this in loop because we need to update the index.
  frame_t *it;
  if (frame_address > UINT64_T(end->cursor.sp)) {
    // path_beg ==> end
    // path_end ==> start
    // (everything is reversed compare to cct_insert_backtrace.c)
    // FIXME: find how to use extern version of retain_recursion
    bool retain_recursion = false;
    ip_normalized_t child_routine = ip_normalized_NULL;
    for (it = start; it <= end; it++) {
      if ( (! retain_recursion) &&
	          (end >= it + 1) &&
            ip_normalized_eq(&(it->the_function), &(child_routine)) &&
	          ip_normalized_eq(&(it->the_function), &((it + 1)->the_function))) {
        // do nothing
      } else {
        (*index)++;
      }
      child_routine = it->the_function;
    }

    // Indicater for the caller that we don't have anymore frames
    // on the stack. We pass through them all.
    // NOTE vi3: Index would point one frame above bt_outer.
    // That is not problem to us, because cct node which corresponds
    // to bt_outer frame has parent who is set in ompt_cct_cursor_finalize
    // function.
    return NULL;
  }

  // don't need to check if it == NULL, because
  // frame_address <= bt_outer
  it = first_frame_below(start, end, frame_address, index);

  // we are now one frame above, should go one frame below
  it--;
  (*index)--;

  if (frame_address > UINT64_T(it->cursor.sp)) {
    //printf("***********first_frame_above********Inside user code\n");
  } else if (frame_address == UINT64_T(it)) {
    // printf("***********first_frame_above********The same address\n");
  } else {
    deferred_resolution_breakpoint();
  }

  return it;
}

int
first_frame_above_tmp
(
  frame_t **it,
  uint64_t frame_address,
  frame_t *end,
  cct_node_t **cct
)
{
  // all frames are below, so we can take the closest one
  // to the frame_address
  if (UINT64_T((*it)->cursor.sp) >= frame_address) {
    return 1; // FIXME vi3>> check if this is right
  }
  // all frames are, so take the last one
  if (UINT64_T(end->cursor.sp) < frame_address) {
//    *it = end;
//    *cct = hpcrun_cct_children(top_cct(*cct)); // top cct node can be UNRESOLVED or THREAD_ROOT
    top_cct_node(cct, it, end);
    return 0;
  }

  // path_beg ==> end
  // path_end ==> start
  // (everything is reversed compare to cct_insert_backtrace.c)
  // FIXME: find how to use extern version of retain_recursion
  bool retain_recursion = false;
  ip_normalized_t child_routine = ip_normalized_NULL;
  for(; (*it) <= end - 1; (*it)++) {
    if ( (! retain_recursion) &&
         (end >= *it + 1) &&
         ip_normalized_eq(&((*it)->the_function), &(child_routine)) &&
         ip_normalized_eq(&((*it)->the_function), &((*it + 1)->the_function))) {
      // do nothing
    } else {

      if (UINT64_T((*it)->cursor.sp) < frame_address &&
          UINT64_T((*it + 1)->cursor.sp) >= frame_address) {
        return 1;
      }
      *cct = hpcrun_cct_parent(*cct);
    }
    child_routine = (*it)->the_function;
  }

  // what should I else return
  return 0;
}

cct_node_t*
get_cct_from_prefix
(
 cct_node_t* cct, 
 int index
)
{
  if (!cct)
    return NULL;

  // FIXME: this is just a temporary solution
  cct_node_t* current = cct;
  int current_index = 0;
  while(current) {
    if (current_index == index) {
      return current;
    }
    current = hpcrun_cct_parent(current);
    current_index++;
  }
  return NULL;
}


cct_node_t*
copy_prefix
(
 cct_node_t* top, 
 cct_node_t* bottom
)
{
  // FIXME: vi3 do we need to copy? find the best way to copy callpath
  // previous implementation
  // return bottom;

  // direct manipulation with cct nodes, which is probably not good way to solve this

  if (!bottom || !top){
    return NULL;
  }

  cct_node_t *prefix_bottom = hpcrun_cct_copy_just_addr(bottom);
  // it is possible that just one node is call path between regions
  if (top == bottom) {
    return prefix_bottom;
  }

  cct_node_t *it = hpcrun_cct_parent(bottom);
  cct_node_t *child = NULL;
  cct_node_t *parent = prefix_bottom;

  while (it) {
    child = parent;
    parent = hpcrun_cct_copy_just_addr(it);
    hpcrun_cct_set_parent(child, parent);
    hpcrun_cct_set_children(parent, child);
    // find the top
    if (it == top) {
      return prefix_bottom;
    }
    it = hpcrun_cct_parent(it);
  }

  return NULL;

}

#if 0
// Check whether we found the outermost region in which current thread is the master
// returns -1 when there is no regions
// returns 0 if the region is not the outermost region of which current thread is master
// returns 1 when the region is the outermost region of which current thread is master
int
is_outermost_region_thread_is_master
(
 int stack_index
)
{
  // no regions
  if (is_empty_region_stack())
    return -1;
  // thread is initial master, and we found the region which is on the top of the stack
  if (TD_GET(master) && stack_index == 0)
    return 1;

  // thread is not the master, and region that is upper on the stack is not_master_region
  if (!TD_GET(master) && stack_index && region_stack[stack_index - 1].notification->region_data == not_master_region){
    return 1;
  }

  return 0;
}
#endif

#if DEFER_DEBUGGING

void
print_prefix_info
(
 char *message, 
 cct_node_t *prefix, 
 ompt_region_data_t *region_data, 
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


// check if the thread cannot resolved region because of reasons mentioned below
static int
dont_resolve_region
(
 ompt_notification_t *current_notification
)
{
  // if current notification is null, or the thread is not the master
  // or the path is already resolved
  return !current_notification
         || current_notification->region_data == not_master_region
         || current_notification->region_data->call_path != NULL;
}

#if 0
int
prefix_length
(
 cct_node_t *bottom_prefix,
 cct_node_t *top_prefix
)
{
  int len = 0;
  cct_node_t *current = bottom_prefix;
  while (current) {
    len++;
    if (current == top_prefix) {
          break;
    }
    current = hpcrun_cct_parent(current);
  }

  return len;
}
#endif

void
provide_callpath_for_regions_if_needed
(
 backtrace_info_t *bt, 
 cct_node_t *cct
)
{
  // vi3: It is possible that thread takes a sample inside linux kernel.
  // Kernel cct nodes would be appended to the call path that corresponds
  // to the user level code.
  // OMPT Frames are aware only about user level code, so we need
  // the call path that corresponds to the user level code.
  // That is the call path before eventual extension with cct nodes of kernel
  // functions.
  cct = cct_path_before_kernel_extension ?
        cct_path_before_kernel_extension : cct;

  if (bt->partial_unwind) {
    deferred_resolution_breakpoint();
    return;
  }

  // not regions on the stack, should mean that we are not inside parallel region
  if (is_empty_region_stack()) {
    return;
  }

  // if thread is not the master of the region, or the region is resolved, then just return
  ompt_notification_t *current_notification = top_region_stack()->notification;
  if (dont_resolve_region(current_notification)) {
    return;
  }

  // Thread took a sample inside explicit task and received cct nodes that
  // correspond to the task from elider. It is not able to provide the call path
  // of the region, because it does not have cct nodes of the region's parent.
  cct_node_t *omp_task_context = TD_GET(omp_task_context);
  if (omp_task_context) {
    deferred_resolution_breakpoint();
    return;
  }

  ompt_frame_t *current_frame = hpcrun_ompt_get_task_frame(0);
  if (!current_frame) {
    deferred_resolution_breakpoint();
    return;
  }

  // use this as a condition to stop with providing
  int have_more_frames = 1;

  frame_t *bt_inner = bt->begin;
  frame_t *bt_outer = bt->last;
  // frame iterator
  frame_t *it = bt_inner;

  cct_node_t *bottom_prefix = NULL;
  cct_node_t *top_prefix = NULL;
  cct_node_t *prefix = NULL;

  if (UINT64_T(current_frame->enter_frame.ptr) == 0
    && UINT64_T(current_frame->exit_frame.ptr) != 0) {
    // thread take a sample inside user code of the parallel region
    // this region is the the innermost
    if (!first_frame_below_tmp(&it, UINT64_T(current_frame->exit_frame.ptr),
                               bt_outer, &cct)) {
      deferred_resolution_breakpoint();
      return;
    }
    // this happened
  } else if (UINT64_T(current_frame->enter_frame.ptr) <= UINT64_T(bt_inner->cursor.sp)
             && UINT64_T(bt_inner->cursor.sp) <= UINT64_T(current_frame->exit_frame.ptr)) { // FIXME should put =
    printf("vi3>> 1405\n");
    // thread take a simple in the region which is not the innermost
    // all innermost regions have been finished
    if (!first_frame_below_tmp(&it, UINT64_T(current_frame->exit_frame.ptr),
                               bt_outer, &cct)) {
      deferred_resolution_breakpoint();
      return;
    }
    // this happened
  } else if (UINT64_T(bt_inner->cursor.sp) < UINT64_T(current_frame->enter_frame.ptr)) {
    printf("vi3>> 1416\n"); // jednom u nestedtasks
    // take a sample inside the runtime
    return;
  } else if (UINT64_T(current_frame->enter_frame.ptr) == 0
             && UINT64_T(current_frame->exit_frame.ptr) == 0) {
    // FIXME: check what this means
    // this happened
    // Two possible options:
    if (top_index >= 0) { // I think that this check is redudant. (The same as at the beginning of the function)
      // 1. The task has been recently finished and removed from the stack.
      ompt_frame_t *tmp = hpcrun_ompt_get_task_frame(1);
      if (!first_frame_below_tmp(
                 &it,
                 UINT64_T(tmp->enter_frame.ptr),
                                 bt_outer, &cct)) {
        return;
      }
    } else {
      // 2. Initial task
      printf("We are inside initial task\n");
      return;
    }

  } else if (UINT64_T(current_frame->exit_frame.ptr) == 0
             && UINT64_T(bt_inner->cursor.sp) >= UINT64_T(current_frame->enter_frame.ptr)) {
    printf("vi3>> 1440\n");
    // FIXME vi3: this happened in the first region when master not took sample

    bottom_prefix = cct;
    // top_cct is either THREAD_ROOT or UNRESOLVED
    top_prefix = hpcrun_cct_children(top_cct(cct));

    prefix = copy_prefix(top_prefix, bottom_prefix);
    if (!prefix) {
      deferred_resolution_breakpoint();
      return;
    }
    current_notification->region_data->call_path = prefix;

    return;
  } else {
    printf("vi3>> 1456\n");
    deferred_resolution_breakpoint();
    return;
  }


  int frame_level = 0;
  int stack_index = top_index;


  // at the beginning of the loop we should be one frame below the exit_frame
  // which means that we are inside user code of the parent region or inside
  // the initial implicit task
  // if the current thread is the initial master
  // while loop
  for (;;) {
    frame_level++;
    current_frame = hpcrun_ompt_get_task_frame(frame_level);
    if (!current_frame) {
      deferred_resolution_breakpoint();
      return;
    }

    // we should be inside the user code of the parent parallel region
    // but let's check that
    if (UINT64_T(it->cursor.sp) >= UINT64_T(current_frame->enter_frame.ptr)) {
      // this happened

    } else {
      // do nothing for now
      deferred_resolution_breakpoint();
      return;
    }

    bottom_prefix = cct;
    if (!bottom_prefix) {
      deferred_resolution_breakpoint();
      return;
    }

    // we found the end of the parent, next thing is to find the beginning of the parent
    // if this is the last region on the stack, then all remaining cct should be in the prefix
    if (TD_GET(master) && stack_index == 0) {
      // top_cct is either THREAD_ROOT or UNRESOLVED
      top_prefix = hpcrun_cct_children(top_cct(bottom_prefix));
      prefix = copy_prefix(top_prefix, bottom_prefix);
      if (!prefix) {
        deferred_resolution_breakpoint();
        return;
      }
      current_notification->region_data->call_path = prefix;
      return;
    }

    // we are inside user code of outside region
    // should find one frame above exit_frame.ptr
    // It is possible that current_frame->exit_frame > bt_outer;
    // In that case, we will go until the bottom of the backtrace stack
    // and get the corresponding cct. After that, we can only finish.
    have_more_frames =
      first_frame_above_tmp(&it,
                            UINT64_T(current_frame->exit_frame.ptr),
                            bt_outer, &cct);
    top_prefix = cct;
    prefix = copy_prefix(top_prefix, bottom_prefix);
    if (!prefix) {
      deferred_resolution_breakpoint();
      return;
    }

    current_notification->region_data->call_path = prefix;

    if (!have_more_frames) {
      // There is no more frames on the stack,
      // so we should finish provider.
      // This if branch corresponds to the previous first_frame_above call.
      deferred_resolution_breakpoint();
      return;
    }

    // next thing to do is to get parent region notification from stack
    stack_index--;
    if (stack_index < 0) {
      deferred_resolution_breakpoint();
      return;
    }
    current_notification = region_stack[stack_index].notification;
    if (dont_resolve_region(current_notification)) {
      return;
    }
    // go to parent region frame and do everything again
    if (!first_frame_below_tmp(&it,
                               UINT64_T(current_frame->exit_frame.ptr),
                               bt_outer, &cct)) {
      // no more frames on the stack
      deferred_resolution_breakpoint();
      return;
    }

  }

}


void
provide_callpath_for_end_of_the_region
(
 backtrace_info_t *bt, 
 cct_node_t *cct
)
{
    if (bt->partial_unwind) {
      deferred_resolution_breakpoint();
    }

    ompt_frame_t *current_frame = hpcrun_ompt_get_task_frame(0);
    if (!current_frame) {
      return;
    }
    frame_t *bt_inner = bt->begin;
    frame_t *bt_outer = bt->last;
    // frame iterator
    frame_t *it = bt_inner;


    cct_node_t *bottom_prefix = NULL;
    cct_node_t *top_prefix = NULL;
    cct_node_t *prefix = NULL;
    // This function is called after implicit task end,
    // which means that this region is poped from the stack.

    if (UINT64_T(current_frame->enter_frame.ptr) == 0
        && UINT64_T(current_frame->exit_frame.ptr) != 0) {
      printf("vi3>> 1589\n");
      if (!first_frame_below_tmp(&it, UINT64_T(current_frame->exit_frame.ptr),
                                 bt_outer, &cct)) {
        deferred_resolution_breakpoint();
        return;
      }
      bottom_prefix = cct;
      ompt_frame_t *current_frame = hpcrun_ompt_get_task_frame(1);
      first_frame_above_tmp(&it,
                            UINT64_T(current_frame->exit_frame.ptr),
                            bt_outer, &cct);
      top_prefix = cct;
      prefix = copy_prefix(top_prefix, bottom_prefix);
      ending_region->call_path = prefix;

      // thread take a sample inside user code of the parallel region
        // this region is the the innermost
    } else if (UINT64_T(current_frame->enter_frame.ptr) <= UINT64_T(bt_inner->cursor.sp)
               && UINT64_T(bt_inner->cursor.sp) <= UINT64_T(current_frame->exit_frame.ptr)) { // FIXME should put =
        // thread take a simple in the region which is not the innermost
        // all innermost regions have been finished
        bottom_prefix = cct;
        first_frame_above_tmp(&it, UINT64_T(current_frame->exit_frame.ptr),
                              bt_outer, &cct);
        top_prefix = cct;
        prefix = copy_prefix(top_prefix, bottom_prefix);
        if (!prefix) {
          deferred_resolution_breakpoint();
          return;
        }
        ending_region->call_path = prefix;


    } else if (UINT64_T(bt_inner->cursor.sp) < UINT64_T(current_frame->enter_frame.ptr)) {
      printf("vi3>> 1632\n");
      // take a sample inside the runtime
    } else if (UINT64_T(current_frame->enter_frame.ptr) == 0
               && UINT64_T(current_frame->exit_frame.ptr) == 0) {
      printf("vi3>> 1636\n");
      // FIXME: check what this means
        // Note: this happens
        deferred_resolution_breakpoint();
    } else if (UINT64_T(current_frame->exit_frame.ptr) == 0
               && UINT64_T(bt_inner->cursor.sp) >= UINT64_T(current_frame->enter_frame.ptr)) {

        // FIXME vi3: this happened in the first region when master not took sample
        bottom_prefix = cct;
        top_prefix = hpcrun_cct_children(top_cct(cct)); // top_cct is UNRESOLVED or THREAD_ROOT

        // copy prefix
        prefix = copy_prefix(top_prefix, bottom_prefix);
        if (!prefix) {
          deferred_resolution_breakpoint();
          return;
        }

        ending_region->call_path = prefix;

        return;

    } else {
      printf("vi3>> 1648\n");
      // a case has not been covered
      deferred_resolution_breakpoint();
    }
}


void
tmp_end_region_resolve
(
 ompt_notification_t *notification, 
 cct_node_t* prefix
)
{
  cct_node_t *unresolved_cct = notification->unresolved_cct;

  if (prefix == NULL) {
    deferred_resolution_breakpoint();
    return;
  }

  // if the prefix and unresolved_cct are already equal,
  // no action is necessary
  if (prefix != unresolved_cct) {
    //printf("*********************** tmp_end_region_resolve if branch\n");
    // prefix node should change the unresolved_cct
    hpcrun_cct_merge(prefix, unresolved_cct, merge_metrics, NULL);
    // delete unresolved_cct from parent
    hpcrun_cct_delete_self(unresolved_cct);
  } else {
    //printf("*********************** tmp_end_region_resolve else branch\n");
    deferred_resolution_breakpoint();
  }
}

void
process_topomost_cct
(
  cct_node_t *cct,
  cct_node_t **top_most,
  int index,
  int region_depth
)
{
  // The topmost cct node must not be UNRESOLVED.
  // The topmost cct node can be thread root only if the region_depth is zero
  // (outermost region) and if the thread is the initial master.

  // top_most is UNRESOLVED
  // Take the cct node which is below it.
  if (hpcrun_cct_addr(*top_most)->ip_norm.lm_id == (uint16_t)UNRESOLVED) {
    *top_most = get_cct_from_prefix(cct, index - 1);
    return;
  }

  if (*top_most == hpcrun_get_thread_epoch()->csdata.thread_root) {

    if (region_depth == 0 && TD_GET(master)) {
      // The thread is the initial master
      // Region depth is zero
      // top_most cct node is the same as thread root
      // Everything is fine
      return;
    }

    // We need to take cct node that is underneath the top_most cct node.
    *top_most = get_cct_from_prefix(cct, index - 1);
  }

  // NOTE vi3: maybe we could use *top_most = (*top_most) -> children
  // instead of calling get_cct_from_prefix which take a lot of time
  // in log call chains
}
