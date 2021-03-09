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



void
msg_deferred_resolution_breakpoint
(
  char *msg
)
{
  printf("********** %s\n", msg);
}


// added by vi3
#define VI3_LAST_PROBLEMS 0


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
#if VI3_LAST_PROBLEMS == 1
  notification->region_id = region_data->region_id;
#endif
  return notification;
}


#if VI3_LAST_PROBLEMS
static ompt_state_t
check_state
(
  void
)
{
  uint64_t wait_id;
  return hpcrun_ompt_get_state(&wait_id);
}
#endif

void
register_to_region
(
  typed_stack_elem_ptr(region) region_data,
  cct_node_t *unresolved_cct
)
{
#if VI3_LAST_PROBLEMS == 1
  if (region_data->call_path) {
    assert(0);
  }
  assert(atomic_load(&region_data->process) == 0);
  assert(atomic_load(&region_data->resolved) == 0);
#endif
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

#if VI3_LAST_PROBLEMS == 1
  printf("Register>>> thr: %p, reg: %p, reg_id: %lx, next: %p, state: %x\n",
         &thread_notification_channel, region_data, region_data->region_id,
         typed_stack_next_get(notification, cstack)(notification), check_state());
#endif

#if DEBUG_BARRIER_CNT
  // debug information
  old_value = atomic_fetch_add(&region_data->barrier_cnt, 0);
  if (old_value < 0) {
    // FIXME seems it may happened
    printf("register_to_region >>> To late, but registered. Old value: %d\n", old_value);
  }
#endif
#if VI3_LAST_PROBLEMS == 1
  atomic_fetch_add(&region_data->registered, 1);
#endif
  // increment the number of unresolved regions
  unresolved_cnt++;
}

inline static void
worker_register_for_region_context
(
  typed_stack_elem(region) *region_data,
  cct_node_t *unresolved_cct,
  bool team_master
)
{
  if (!team_master) {
    // Worker thread should register for the region's call path
    register_to_region(region_data, unresolved_cct);
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

  worker_register_for_region_context(el->region_data,
                                     el->unresolved_cct, el->team_master);

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


typedef struct lca_args_s {
  int level;
  typed_stack_elem(region) *region_data;
} lca_args_t;



bool
is_thread_owner_of_the_region
(
  typed_random_access_stack_elem(region) *el,
  int level
)
{
  int flags0;
  ompt_frame_t *frame0;
  ompt_data_t *task_data = NULL;
  ompt_data_t *parallel_data = NULL;
  int thread_num = -1;
  int retVal = hpcrun_ompt_get_task_info(level, &flags0, &task_data, &frame0,
                                         &parallel_data, &thread_num);
  if (retVal != 2) {
    // Task either doesn't exist or the information about it isn't available.
    // I guess the false is the right value to return.
    return false;
  }
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
  assert(hpcrun_ompt_is_thread_region_owner(reg) == is_thread_owner_of_the_region(el, level));
#else
  el->team_master = is_thread_owner_of_the_region(el, level);
#endif
  // invalidate previous values
  el->unresolved_cct = NULL;
  el->took_sample = false;

#if KEEP_PARENT_REGION_RELATIONSHIP
  // In order to prevent NULL values get from hpcrun_ompt_get_region_data,
  // use parent as next region to process.
  args->region_data = typed_stack_next_get(region, sstack)(reg);
#else
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
  args->region_data = hpcrun_ompt_get_region_data(args->level);
#else

  // Since we're implicitly using ompt_get_task_info and is possible to face
  // with nested tasks, we may need to omit some of them that belongs to this
  // region.
  // TODO VI3: move this to function
  typed_stack_elem(region) *new_region = el->region_data;
  typed_stack_elem(region) *old_region = new_region;

  while(new_region) {
    if (new_region->depth + 1 == old_region->depth) {
      break;
    }
    old_region = new_region;
    // get information about outer task
    new_region = hpcrun_ompt_get_region_data_from_task_info(++level);
  }

  if (!new_region) {
    // just a debug stuff
    int flags0;
    ompt_frame_t *frame0;
    ompt_data_t *task_data = NULL;
    ompt_data_t *parallel_data = NULL;
    int thread_num = -1;
    int ret_val = hpcrun_ompt_get_task_info(level, &flags0, &task_data, &frame0,
                                            &parallel_data, &thread_num);
    if (ret_val == 2 && (flags0 & ompt_task_initial)) {
      // It is ok to encounter on initial task.
    } else {
      printf("Unexpected: %d, task_flags: %x\n", ret_val, flags0);
    }
  }

  args->region_data = new_region;
  args->level = level;

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
  int ancestor_level
)
{
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
  typed_stack_elem(region) *innermost_reg = hpcrun_ompt_get_region_data(ancestor_level);
#else
  typed_stack_elem(region) *innermost_reg =
      hpcrun_ompt_get_region_data_from_task_info(ancestor_level);
#endif
  assert(innermost_reg);

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
  int ancestor_level
)
{
  if (ancestor_level < 0) {
    // Skip registration process.
    return;
  }

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
#endif

  // FIXME vi3: check whether region at ancestor_level
  //   really has region_depth.

  // Try to find active region in which thread took previous sample
  // (in further text lca->region_data)
  typed_random_access_stack_elem(region) *lca;
  assert(least_common_ancestor(&lca, ancestor_level));

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

#if VI3_LAST_PROBLEMS == 1
  printf("Resolving>>> thr: %p, reg: %p, reg_id: %lx\n",
         &thread_notification_channel, old_head->region_data, old_head->region_data->region_id);
#endif
  unresolved_cnt--;
#if VI3_LAST_PROBLEMS == 1
  ompt_region_debug_notify_received(old_head);
  atomic_fetch_add(&old_head->region_data->resolved, 1);
  if (old_head->region_data->master_channel == &region_freelist_channel) {
    printf("Master should not finish here\n");
  }
  assert(old_head->region_data->master_channel != &region_freelist_channel);
  // check if the notification needs to be forwarded
#endif
  typed_stack_elem_ptr(notification) next =
    typed_stack_pop(notification, cstack)(&old_head->region_data->notification_stack);
  if (next) {
    // store region_prefix and notify next worker in the chain
    next->region_prefix = old_head->region_prefix;
    typed_channel_shared_push(notification)(next->notification_channel, next);
  } else {
    //old_head->region_data->notification_stack = (typed_stack_elem(notification) *)(0xdeadbeef);
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

#if 1
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
      try_resolve_one_region_context();
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
  if (unresolved_cnt > 0) {
    // attempt to resolve contexts by consuming any notifications that
    // are currently pending.
    while (try_resolve_one_region_context());
  };
}


void
initialize_regions_if_needed
(
  void
)
{
  //task_ancestor_level = try_to_detect_the_case();
  if (task_ancestor_level < 0) {
    // Cannot initialize anything
    return;
  }
  //assert(task_ancestor_level == 0);
  initialize_region(task_ancestor_level);
}

#if INTEGRATE_REG_INIT_AND_REGISTER == 1


static inline bool
region_init_in_outer_task
(
  typed_stack_elem(region) *region_data
)
{
  typed_random_access_stack_elem(region) *top = NULL;
  top = typed_random_access_stack_top(region)(region_stack);
  if (top && top->region_id == region_data->region_id) {
    // Consider the following case:
    // region 0      (ancestor_level = 2)
    //   task 0      (ancestor_level = 1)
    //     region 1  (ancestor_level = 0)
    // old_reg will be the same for levels 0 and 1, so return.
    return true;
  }
  return false;
}


void
add_new_region_on_stack
(
  typed_stack_elem(region) *new_region,
  bool team_master
)
{
  cct_node_t *parent_cct = NULL;
  typed_random_access_stack_elem(region) *curr_top = NULL;
  int depth = new_region->depth;
  if (depth == 0) {
    // insert region unresolved_cct underneath thread root
    parent_cct = hpcrun_get_thread_epoch()->csdata.thread_root;
  } else {
    // insert new_reg unresolved_cct underneath current top
    // region's unresolved_cct
    curr_top = typed_random_access_stack_top(region)(region_stack);
    assert(curr_top);
    assert(curr_top->region_data->depth + 1 == depth);
    parent_cct = curr_top->unresolved_cct;
    assert(parent_cct);
  }

  // new_reg is the new top, so set it properly
  typed_random_access_stack_top_index_set(region)(depth, region_stack);
  curr_top = typed_random_access_stack_top(region)(region_stack);
  curr_top->region_data = new_region;
  curr_top->region_id = new_region->region_id;
  curr_top->team_master = team_master;
  curr_top->took_sample = true;


  // need to insert new_reg->unresolved_cct
  cct_addr_t *region_addr = &ADDR2(UNRESOLVED, new_region->region_id);
  // unresolved_cct that corresponds to th region_id mustn't exists
  // inside thread's CCT
  assert(hpcrun_cct_find_addr(parent_cct, region_addr) == NULL);
  curr_top->unresolved_cct = hpcrun_cct_insert_addr(parent_cct, region_addr);
  // register for region creation context if needed
  worker_register_for_region_context(new_region,
                                     curr_top->unresolved_cct, team_master);

}


int
lazy_region_process
(
  int ancestor_level
)
{
  int flags;
  ompt_frame_t *frame;
  ompt_data_t *task_data = NULL;
  ompt_data_t *parallel_data = NULL;
  int thread_num = -1;
  int ret_val = hpcrun_ompt_get_task_info(ancestor_level, &flags, &task_data, &frame,
                                          &parallel_data, &thread_num);
  assert(ret_val == 2);
  assert(parallel_data != NULL);

  if (flags & ompt_task_initial) {
    // ignore initial tasks
    return -1;
  }

  typed_stack_elem(region) *old_reg = ATOMIC_LOAD_RD(parallel_data);
  if (old_reg) {
    // Update stack of active regions for regions that are below the old_reg,
    // including the old_reg too.
    register_to_all_regions(ancestor_level);
    assert(typed_random_access_stack_top_index_get(region)(region_stack)
        == old_reg->depth);
    return old_reg->depth;
  }

  // initialize parent region if needed
  int parent_depth = lazy_region_process(ancestor_level + 1);
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

  if (!region_init_in_outer_task(old_reg)) {
    // update top of the region active stack by adding old_reg
    add_new_region_on_stack(old_reg, thread_num == 0);
    assert(typed_random_access_stack_top_index_get(region)(region_stack)
           == old_reg->depth);
  }

  return old_reg->depth;
}
#endif

void
lazy_active_region_processing
(
  void
)
{
#if INTEGRATE_REG_INIT_AND_REGISTER == 0
  // initialize region_data if needed
  initialize_regions_if_needed();
  // register to active regions if needed
  register_to_all_regions(task_ancestor_level);
#else
  lazy_region_process(task_ancestor_level);
#endif
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
