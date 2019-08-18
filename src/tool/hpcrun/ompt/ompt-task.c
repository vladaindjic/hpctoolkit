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
// Copyright ((c)) 2002-2013, Rice University
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



//*****************************************************************************
// private operations
//*****************************************************************************

//----------------------------------------------------------------------------
// note the creation context for an OpenMP task
//----------------------------------------------------------------------------
static void 
ompt_task_begin_internal
(
 ompt_data_t* task_data
)
{
  thread_data_t *td = hpcrun_get_thread_data();

  td->overhead ++;

  // record the task creation context into task structure (in omp runtime)
  cct_node_t *cct_node = NULL;
  if (ompt_task_full_context_p()){
    ucontext_t uc; 
    getcontext(&uc);

    hpcrun_metricVal_t zero_metric_incr_metricVal;
    zero_metric_incr_metricVal.i = 0;
    cct_node = hpcrun_sample_callpath(&uc, 0, zero_metric_incr_metricVal, 1, 1, NULL).sample_node;
  } else {
    ompt_data_t *parallel_info = NULL;
    int team_size = 0;
    hpcrun_ompt_get_parallel_info(0, &parallel_info, &team_size);
    typed_queue_elem(region)* region_data =
      (typed_queue_elem(region)*) parallel_info->ptr;
    cct_node = region_data->call_path;
  }

  task_data->ptr = cct_node;

  if (!ompt_eager_context_p()) {
    // this says that we have explicit task but not it's path
    // we use this in elider (clip frames above the frame of the explicit task)
    // and int ompt_cursor_finalize.
    // only possible when ompt_eager_context == false,
    // because we don't collect call path for the innermost region eagerly
    // FIXME vi3: I guess it is possible that one thread executes this
    // callback and another execute the task.
    // Instead of memoizing the pointer to the stack element,
    // we are going to memoize top_index which is the same as region depth.
    // If the top_index is 0. that is equal to NULL, which will tell
    // elider and ompt_cct_cursror_finalizer that thread is not inside
    // explicit task. The workarround is to add some constant greater than 1,
    // because top_index can be in range [-1, 128]. If we add only 1 and if
    // top_index is -1. than we are facing againg to aforementioned problem.
    task_data->value = (uint64_t)top_index + 33;
  }

  td->overhead --;
}


static void
ompt_task_create
(
 ompt_data_t *parent_task_data,    // data of parent task
 const ompt_frame_t *parent_frame, // frame data for parent task
 ompt_data_t *new_task_data,       // data of created task
 ompt_task_flag_t type,
 int has_dependences,
 const void *codeptr_ra
)
{
  hpcrun_safe_enter();

  new_task_data->ptr = NULL;

  if (type != ompt_task_initial) {
    ompt_task_begin_internal(new_task_data);
  }

  hpcrun_safe_exit();
}



//*****************************************************************************
// interface operations
//*****************************************************************************

void
ompt_task_register_callbacks
(
 ompt_set_callback_t ompt_set_callback_fn
)
{
  int retval;
  retval = ompt_set_callback_fn(ompt_callback_task_create,
                                (ompt_callback_t)ompt_task_create);
  assert(ompt_event_may_occur(retval));
}


