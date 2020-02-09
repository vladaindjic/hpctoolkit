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
cache_old_region
(
  typed_random_access_stack_elem(region) *el,
  typed_stack_elem_ptr(region) region_data,
  typed_stack_elem_ptr(notification) notification
)
{
  old_region_t *new = hpcrun_malloc(sizeof(old_region_t));
  // current head is new->next
  new->next = el->old_region_list;
  // store new head
  el->old_region_list = new;
  new->notification = notification;
  new->region = region_data;
}

old_region_t *
adding_region_twice
(
  typed_random_access_stack_elem(region) *el,
  typed_stack_elem_ptr(region) region_data
)
{
  old_region_t *current;
  for(current = el->old_region_list; current != NULL; current = current->next) {
    if (current->region->region_id == region_data->region_id) {
      return current;
    }
  }
  return NULL;
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

#if 0
  if (notification && notification->region_data) {
    typed_stack_elem_ptr(region) old_reg = notification->region_data;
    int old_barrier_cnt_value = atomic_fetch_add(&old_reg->barrier_cnt, 0);
    if (old_barrier_cnt_value >= 0) {
      printf("Why is this region still active??? %d\n", old_barrier_cnt_value);
    }
  }
#endif

  old_region_t *previous_region = adding_region_twice(stack_element, region_data);
  if (previous_region){
    //printf("Nooo, this region again! :'(\n");
    stack_element->notification = previous_region->notification;
    stack_element->team_master = 0;
    stack_element->took_sample = previous_region->notification->unresolved_cct != NULL;
    return;
  }

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
  cache_old_region(stack_element, region_data, notification);
}

#if 1

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

#elif
// Old implementation
void
add_region_and_ancestors_to_stack
(
 typed_stack_elem_ptr(region) region_data,
 bool team_master
)
{

  if (!region_data) {
    deferred_resolution_breakpoint();
    return;
  }

  typed_stack_elem_ptr(region) current = region_data;
  int level = 0;
  int depth;

  // Add active regions on stack starting from the innermost.
  // Stop when one of the following conditions is satisfied:
  // 1. all active regions are added to stack
  // 2. encounter on region that is active and that was previously added to the stack
  while (current) {
    depth = current->depth;
    // found region which was previously added to the stack
    if (depth <= typed_random_access_stack_top_index_get(region)(region_stack)
          && typed_random_access_stack_get(region)(region_stack,
              depth)->notification->region_data->region_id == current->region_id) {
      break;
    }
    // add corresponding notification on stack
    swap_and_free(current);
    // get the parent
    current = hpcrun_ompt_get_region_data(++level);
  }

  // region_data is the new top of the stack
  typed_random_access_stack_top_index_set(region)(region_data->depth, region_stack);

  // NOTE vi3: This should be right
  // If the stack content does not corresponds to ancestors of the region_data,
  // then thread could only be master of the region_data, but not to its ancestors.

  // Values of argument team_master says if the thread is the master of region_data
  typed_random_access_stack_top(region)(region_stack)->team_master = team_master;

}
#endif


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
  typed_stack_push(notification, cstack)(&region_data->notification_stack, notification);
  // increment the number of unresolved regions
  unresolved_cnt++;
}

#if 0
bool
try_to_notify_master
(
  typed_stack_elem_ptr(notification) notification
)
{
  typed_stack_elem_ptr(region) region_data = notification->region_data;
  int old_value = atomic_fetch_add(&region_data->barrier_cnt, 1);
  if (old_value < 0) {
    // invalidate previous incrementing
    // FIXME vi3 >>> Think this is ok
    atomic_fetch_sub(&region_data->barrier_cnt, 1);
  }
  // indication that region is still active
  return old_value >= 0;
}
#endif

bool
thread_take_sample
(
  typed_random_access_stack_elem(region) *el,
  void *arg
)
{
  cct_node_t **previous_cct_ptr = (cct_node_t **)arg;
  // pseudo cct node of the inner region
  cct_node_t *previous_cct = *previous_cct_ptr;
  // notification that corresponds to the active region that is being processed at the moment
  typed_stack_elem(notification) *notification = el->notification;

  // thread took a sample in this region before
  if (el->took_sample) {
    // connect with children pseudo node
    if (previous_cct) {
      hpcrun_cct_insert_node(notification->unresolved_cct, previous_cct);
    }
    // Indication that processing of active regions should stop.
    return 1;
  }

  if (!el->team_master) {
    // Worker thread tries to register itself for the region's call path
    typed_stack_elem_ptr(region) region_data = notification->region_data;
    int old_value = atomic_fetch_add(&region_data->barrier_cnt, 1);
    if (old_value < 0) {
      // Master thread marked that this region is finished.
      // Invalidate previous incrementing
      atomic_fetch_sub(&region_data->barrier_cnt, 1);
      // Skip processing this region
      return 0;
    }
    register_to_region(notification);
    // This thread finished with registering itself for region's call path
    atomic_fetch_sub(&region_data->barrier_cnt, 1);
  } else {
    // Master thread also needs to resolve this region at the very end (ompt_parallel_end)
    unresolved_cnt++;
  }

  // mark that thread took sample in this region for the first time
  el->took_sample = true;

  // add pseudo cct node that corresponds to the region
  notification->unresolved_cct =
      cct_node_create_from_addr_vi3(&ADDR2(UNRESOLVED,
                                           notification->region_data->region_id));
  // connect it with pseudo node of the inner region
  if (previous_cct) {
    hpcrun_cct_insert_node(notification->unresolved_cct, previous_cct);
  }

  // check if the region is outermost
  if (notification->region_data->depth == 0) {
    // connect the pseudo node of outermost region with thread root
    hpcrun_cct_insert_node(hpcrun_get_thread_epoch()->csdata.thread_root, notification->unresolved_cct);
    // Indication that all active regions have been processed
    return 1;
  }

  // set previous_cct for the outer region
  *previous_cct_ptr = notification->unresolved_cct;

  // Indication that processing of active regions should continue
  return 0;

}


void
register_to_all_regions
(
 void
)
{
  cct_node_t *previous_cct = NULL;
  // Mark that thread took a sample in all active regions on the stack.
  // Stop at region in which thread took a sample before.
  // Add pseudo cct nodes for regions in which thread took a sample for the first time.
  typed_random_access_stack_forall(region)(region_stack, thread_take_sample, &previous_cct);
#if 0
  // Old Implementation
  int i;
  for (i = typed_random_access_stack_top_index_get(region)(region_stack); i >= 0; i--) {
    current_el = typed_random_access_stack_get(region)(region_stack, i);
    current_notification = current_el->notification;
    // thread took a sample in this region before
    if (current_el->took_sample) {
      // connect with children pseudo node
      if (previous_cct) {
        current_cct = current_notification->unresolved_cct;
        hpcrun_cct_insert_node(current_cct, previous_cct);
      }
      // stop
      return;
    }

    // mark that thread took sample in this region
    current_el->took_sample = true;
    // worker thread register itself for the region's call path
    if (!current_el->team_master) {
      register_to_region(current_el->notification);
    }

    // add pseudo cct node
    current_notification->unresolved_cct =
        cct_node_create_from_addr_vi3(&ADDR2(UNRESOLVED,
            current_notification->region_data->region_id));
    current_cct = current_notification->unresolved_cct;
    // connect it with children pseudo node (previous one)
    if (previous_cct) {
      hpcrun_cct_insert_node(current_cct, previous_cct);
    }

    previous_cct = current_cct;
  }
#endif
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





#if 0
void
resolve_one_region_context_vi3
(
  typed_stack_elem_ptr(notification) notification
)
{
  typed_stack_elem_ptr(region) region_data = notification->region_data;

  // ================================== resolving part
  cct_node_t *unresolved_cct = notification->unresolved_cct;
  // this region has already been resolved
  if (!notification->unresolved_cct) {
    return;
  }

  cct_node_t *parent_unresolved_cct = hpcrun_cct_parent(unresolved_cct);

  if (atomic_fetch_add(&region_data->barrier_cnt, 0) >= 0)
    return;

  for(;;) {
    cct_node_t *value = region_data->call_path;
    if (value != NULL) break;
  }

  if (parent_unresolved_cct == NULL || region_data->call_path == NULL) {
    deferred_resolution_breakpoint();
  } else {
    // prefix should be put between unresolved_cct and parent_unresolved_cct
    cct_node_t *prefix = NULL;
    cct_node_t *region_call_path = region_data->call_path;

    // FIXME: why hpcrun_cct_insert_path_return_leaf ignores top cct of the path
    prefix = hpcrun_cct_insert_path_return_leaf(parent_unresolved_cct, region_call_path);

    if (prefix == NULL) {
      deferred_resolution_breakpoint();
    }

    if (prefix != unresolved_cct) {
      // prefix node should change the unresolved_cct
      hpcrun_cct_merge(prefix, unresolved_cct, merge_metrics, NULL);
      // delete unresolved_cct from parent
      hpcrun_cct_delete_self(unresolved_cct);
    } else {
      deferred_resolution_breakpoint();
    }
  }
  // ==================================
  // FIXME vi3 >>> notification mem leak.
  // FIXME vi3 >>> region_data mem leak.

  // invalidate unresolved_cct
  notification->unresolved_cct = NULL;
}
#endif

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


void
tmp_end_region_resolve
(
 typed_stack_elem_ptr(notification) notification,
 cct_node_t* prefix
)
{
  cct_node_t *unresolved_cct = notification->unresolved_cct;

  if (prefix == NULL) {
    msg_deferred_resolution_breakpoint("tmp_end_region_resolve: Prefix is missing.\n");
    return;
  }

  // if the prefix and unresolved_cct are already equal,
  // no action is necessary
  if (prefix != unresolved_cct) {
    // prefix node should change the unresolved_cct
    hpcrun_cct_merge(prefix, unresolved_cct, merge_metrics, NULL);
    // delete unresolved_cct from parent
    hpcrun_cct_delete_self(unresolved_cct);
  } else {
    msg_deferred_resolution_breakpoint("tmp_end_region_resolve: Prefix is equal to unresolved_cct");
  }
}
