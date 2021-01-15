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

#include <stdio.h>



//*****************************************************************************
// local includes
//*****************************************************************************

#include <hpcrun/safe-sampling.h>
#include <hpcrun/sample_event.h>
#include <hpcrun/thread_data.h>

#include "omp-tools.h"

#include "ompt-callback.h"
#include "ompt-interface.h"
#include "ompt-region.h"
#include "ompt-task.h"
#include "ompt-callstack.h"
#include "ompt-thread.h"


#if 0
struct ompt_task_data_t {
  ompt_task_flag_t task_type;
  cct_node_t *callpath;
  struct ompt_task_data_t *next;
}; 

static __thread ompt_task_data_t *task_free_list = 0;


ompt_task_flag_t
ompt_task_type
(
 ompt_data_t *task_data
);


//*****************************************************************************
// private operations
//*****************************************************************************

static void 
ompt_task_init
(
 ompt_task_data_t *t,
 cct_node_t *callpath,
 ompt_task_flag_t task_type
)
{
  t->task_type = task_type;
  t->callpath = callpath;
  t->next = 0;
}

static ompt_task_data_t* 
ompt_task_alloc
(
 void
)
{
  ompt_task_data_t* t = (ompt_task_data_t*) hpcrun_malloc(sizeof(ompt_task_data_t));
  return t;
}


static ompt_task_data_t* 
ompt_task_freelist_get
(
 void
)
{
  ompt_task_data_t* t = 0; 
  if (task_free_list) {
    t = task_free_list;
    task_free_list = task_free_list->next;
  }
  return t;
}


static void
ompt_task_freelist_put
(
 ompt_task_data_t *t 
)
{
  if (t) {
    t->next = task_free_list;
    task_free_list = t;
  }
}


ompt_task_data_t*
ompt_task_acquire
(
 cct_node_t *callpath,
 ompt_task_flag_t task_type
)
{
  ompt_task_data_t* t = ompt_task_freelist_get();
  if (t == 0) {
    t = ompt_task_alloc();
  }
  ompt_task_init(t,callpath, task_type);
  return t;
}


void
ompt_task_release
(
 ompt_data_t *t
)
{
  ompt_task_freelist_put((ompt_task_data_t *) t->ptr);
  t->ptr = 0;
}

#endif

//----------------------------------------------------------------------------
// note the creation context for an OpenMP task
//----------------------------------------------------------------------------
static void 
ompt_task_begin_internal
(
 ompt_data_t* task_data,
 int flags
)
{
  thread_data_t *td = hpcrun_get_thread_data();

  td->overhead ++;

  // record the task creation context into task structure (in omp runtime)
  cct_node_t *cct_node = NULL;
  ucontext_t uc;
  getcontext(&uc);

  hpcrun_metricVal_t zero_metric_incr_metricVal;
  zero_metric_incr_metricVal.i = 0;
  cct_node = hpcrun_sample_callpath(&uc, 0, zero_metric_incr_metricVal,
      1, 1, NULL).sample_node;
  // store cct_node in task_data
  task_data_set_cct(task_data, cct_node);
#if 0
  else {
    ompt_data_t *parallel_info = NULL;
    int team_size = 0;
    hpcrun_ompt_get_parallel_info(0, &parallel_info, &team_size);
    typed_stack_elem_ptr(region) region_data = (typed_stack_elem_ptr(region)) parallel_info->ptr;
    cct_node = region_data->call_path;
  }

    if (cct_node) {
      // set cct_node, if available (in case of eagerly collecting region context)
      task_data_set_cct(task_data, cct_node);
    } else {
      // otherwise, store depth of the innermost region
#if 0
      task_data_set_depth(task_data,
          typed_random_access_stack_top_index_get(region)(region_stack));
#endif
      task_data_set_depth(task_data, region_data->depth);
    }
  }
#endif

  td->overhead --;
}


static void
ompt_task_create
(
 ompt_data_t *parent_task_data,    // data of parent task
 const ompt_frame_t *parent_frame, // frame data for parent task
 ompt_data_t *new_task_data,       // data of created task
 int flags,
 int has_dependences,
 const void *codeptr_ra
)
{
  hpcrun_safe_enter();

  new_task_data->ptr = NULL;

  if (flags != ompt_task_initial) {
    ompt_task_begin_internal(new_task_data, flags);
  }

  hpcrun_safe_exit();
}

#if 0

static void
ompt_task_schedule
(
 ompt_data_t *prior_task_data,    
 ompt_task_status_t prior_task_status, 
 ompt_data_t *next_task_data    
)
{
  hpcrun_safe_enter();

  if ((ompt_task_type(prior_task_data) & ompt_task_implicit) &&
      (ompt_task_type(next_task_data) & ompt_task_explicit)) {
    ompt_idle_end();
  }

  if ((ompt_task_type(prior_task_data) & ompt_task_explicit) &&
      (ompt_task_type(next_task_data) & ompt_task_implicit)) {
    ompt_idle_begin();
  }


  if (prior_task_status == ompt_task_complete) {
    ompt_task_release(prior_task_data);
  }
  hpcrun_safe_exit();
}



//*****************************************************************************
// interface operations
//*****************************************************************************

cct_node_t *
ompt_task_callpath
(
 ompt_data_t *task_data
)
{
  ompt_task_data_t *t = (ompt_task_data_t *) task_data->ptr;
  return t ? t->callpath : 0;
}


ompt_task_flag_t
ompt_task_type
(
 ompt_data_t *task_data
)
{
  ompt_task_data_t *t = (ompt_task_data_t *) task_data->ptr;
  return t ? t->task_type : 0;
}

#endif

void
ompt_task_register_callbacks
(
 ompt_set_callback_t ompt_set_callback_fn
)
{
  if (ompt_task_full_context_p()) {
    // If there's need to determine task creation context,
    // then register the following callback.
    int retval;
    retval = ompt_set_callback_fn(ompt_callback_task_create,
                                  (ompt_callback_t)ompt_task_create);
    assert(ompt_event_may_occur(retval));
  }
}


void
task_data_set_cct
(
  ompt_data_t *task_data,
  cct_node_t *cct
)
{
  task_data->ptr = cct;
}


void
task_data_set_depth
(
  ompt_data_t *task_data,
  int region_depth
)
{
  uint64_t val = (uint64_t)region_depth;
  val = val << 1u;
  uint64_t mask = 1;
  val = val | mask;
  task_data->value = val;
}

// returns 0 if cct is present
// returns 1 if region_depth is present
// returns 2 if no information are present
char
task_data_value_get_info
(
  void *task_data_content,
  cct_node_t **cct,
  int *region_depth
)
{
  if (!task_data_content)
    return 2;
  uint64_t task_data_value = (uint64_t)task_data_content;
  uint64_t mask = 1;
  uint64_t info_type = task_data_value & mask;

  if (!info_type) {
    // store cct_node_t
    *cct = (cct_node_t*)task_data_value;
  } else {
    // calculate and store region depth
    uint64_t val = task_data_value;
    val = val >> 1u;
    *region_depth = val;
  }

  return (char)info_type;
}

