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
#include "ompt-queues.h"
#include "ompt-region.h"
#include "ompt-region-debug.h"
#include "ompt-thread.h"
#include "ompt-task.h"



//*****************************************************************************
// variables
//****************************************************************************/

// private freelist from which only thread owner can reused regions
// FIXME vi3>> This should be initialized to NULL
static __thread typed_queue_elem_ptr(notification) private_region_freelist_head;


//*****************************************************************************
// forward declarations
//*****************************************************************************

static typed_queue_elem(region) *
ompt_region_acquire
(
 void
);

static void
ompt_region_release
(
 typed_queue_elem(region) *r
);


//*****************************************************************************
// private operations
//*****************************************************************************

static typed_queue_elem(region) *
ompt_region_data_new
(
 uint64_t region_id, 
 cct_node_t *call_path
)
{
  typed_queue_elem(region)* e = ompt_region_acquire();

  e->region_id = region_id;
  e->call_path = call_path;
  typed_queue_elem_ptr_set(notification, qtype)(&e->queue, NULL);

  // parts for freelist
  typed_queue_elem_ptr_set(region, qtype)(&e->next, NULL);
  e->thread_freelist = &public_region_freelist;
  e->depth = 0;

  return e;
}


static void 
ompt_parallel_begin_internal
(
 ompt_data_t *parallel_data,
 int flags
) 
{
  typed_queue_elem(region)* region_data =
    ompt_region_data_new(hpcrun_ompt_get_unique_id(), NULL);
  parallel_data->ptr = region_data;

  uint64_t region_id = region_data->region_id;
  thread_data_t *td = hpcrun_get_thread_data();

  // FIXME vi3: check if this is right
  // the region has not been changed yet
  // that's why we say that the parent region is
  // hpcrun_ompt_get_current_region_data
  typed_queue_elem(region) *parent_region =
    hpcrun_ompt_get_current_region_data();
  if (!parent_region) {
    // mark the master thread in the outermost region
    // (the one that unwinds to FENCE_MAIN)
    td->master = 1;
    region_data->depth = 0;
  } else {
    region_data->depth = parent_region->depth + 1;
  }

  if (ompt_eager_context_p()) {
     region_data->call_path =
       ompt_parallel_begin_context(region_id, 
				   flags & ompt_parallel_invoker_program);
  }
}


static void
ompt_parallel_end_internal
(
 ompt_data_t *parallel_data,    // data of parallel region
 int flags
)
{
  typed_queue_elem(region)* region_data =
    (typed_queue_elem(region)*)parallel_data->ptr;

  if (!ompt_eager_context_p()){
    // check if there is any thread registered that should be notified
    // that region call path is available
    typed_queue_elem(notification)* to_notify =
      typed_queue_pop(notification, qtype)(&region_data->queue);

    // mark that current region is ending
    ending_region = region_data;
    // implicit task end callback happened before, so top of the stack
    // is below the current region that is ending
    top_index++;

    region_stack_el_t *stack_el = &region_stack[top_index];
    typed_queue_elem(notification) *notification = stack_el->notification;

    // NOTE: These two conditions should be equal:
    // 1. notification->unresolved_cct != NULL
    // 2. stack_el->took_sample

    // Region call path is missing.
    // Some thread from the team took a sample in the region.
    if (!region_data->call_path &&
          (notification->unresolved_cct || to_notify)) {
      // the region has not been provided before, so we will do that now
      region_data->call_path = ompt_region_context_eager(region_data->region_id, ompt_scope_end,
                               flags & ompt_parallel_invoker_program);
    }

    // If master took a sample in this region, it needs to resolve its call path.
    if (notification->unresolved_cct) {
      // CASE: thread took sample in an explicit task,
      // so we need to resolve everything under pseudo node
      cct_node_t *parent_cct = hpcrun_cct_parent(notification->unresolved_cct);
      cct_node_t *prefix =
        hpcrun_cct_insert_path_return_leaf(parent_cct,
          region_data->call_path);

      // if combined this if branch with branch of next if
      // we will remove this line
      tmp_end_region_resolve(notification, prefix);
    }

    if (to_notify){
      // notify next thread
      typed_queue_push(notification, qtype)(to_notify->threads_queue, to_notify);
    } else {
      // if none, you can reuse region
      // this thread is region creator, so it could add to private region's list
      // FIXME vi3: check if you are right
      ompt_region_release(region_data);
      // or should use this
      // wfq_enqueue((ompt_base_t*)region_data, &public_region_freelist);
    }

    // return to outer region if any
    top_index--;
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

  typed_queue_elem(region)* region_data =
    (typed_queue_elem(region)*)parallel_data->ptr;

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
    task_data_set_info(task_data, prefix, -1);
  }

  if (!ompt_eager_context_p()) {
    // FIXME vi3: check if this is fine
    // add current region
    add_region_and_ancestors_to_stack(region_data, index==0);

    // FIXME vi3: move this to add_region_and_ancestors_to_stack
    // Memoization process vi3:
    if (index != 0){
      not_master_region = region_data;
    }
    task_data_set_info(task_data, NULL, top_index);
  }
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
    // the only thing we could do (certainly) here is to pop element from the stack
    // pop element from the stack
    pop_region_stack();
    ompt_resolve_region_contexts_poll();
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


static typed_queue_elem(region)*
ompt_region_alloc
(
 void
)
{
  typed_queue_elem(region)* r =
    (typed_queue_elem(region)*) hpcrun_malloc(sizeof(typed_queue_elem(region)));
  return r;
}


static typed_queue_elem(region)*
ompt_region_freelist_get
(
 void
)
{
  typed_queue_elem(region)* r =
    typed_queue_pop_or_steal(region, qtype)(&private_region_freelist_head,
      &public_region_freelist);
  return r;
}


static void
ompt_region_freelist_put
(
 typed_queue_elem(region) *r
)
{
  r->region_id = 0xdeadbeef;
  typed_queue_push(region, qtype)(&private_region_freelist_head, r);
}


typed_queue_elem(region)*
ompt_region_acquire
(
 void
)
{
  typed_queue_elem(region)* r = ompt_region_freelist_get();
  if (r == 0) {
    r = ompt_region_alloc();
    ompt_region_debug_region_create(r);
  }
  return r;
}


static void
ompt_region_release
(
 typed_queue_elem(region) *r
)
{
  ompt_region_freelist_put(r);
}

void
hpcrun_ompt_region_free
(
 typed_queue_elem(region) *region_data
)
{
  // reset call_path when freeing the region
  region_data->call_path = NULL;
  region_data->region_id = 0xdeadbead;
  typed_queue_push(region, qtype)(region_data->thread_freelist, region_data);
}



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
  typed_queue_elem_ptr_set(region, qtype)(&public_region_freelist, NULL);
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
}
