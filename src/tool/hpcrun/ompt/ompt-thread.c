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
// Copyright ((c)) 2002-2014, Rice University
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


//******************************************************************************
// local includes  
//******************************************************************************

#include "ompt-thread.h"



//******************************************************************************
// global variables 
//******************************************************************************

__thread typed_channel_elem(notification) thread_notification_channel;

// freelists
__thread typed_stack_elem_ptr(notification) notification_freelist_head = NULL;
__thread typed_channel_elem(region) region_freelist_channel;


// stack for regions
__thread typed_random_access_stack_struct(region) *region_stack = NULL;

// number of unresolved regions
__thread int unresolved_cnt = 0;

// FIXME vi3: just a temp solution
__thread typed_stack_elem_ptr(region) ending_region = NULL;

__thread int depth_last_sample_taken = -1;
__thread uint64_t region_id_last_sample_taken = 0;

// maybe this variable should be renamed
// The idea is to mark that thread does nothing
// until it reaches begin of the parallel region (implicit_task_begin callback)
__thread bool waiting_on_last_implicit_barrier = true;
__thread bool innermost_parallel_data_avail = false;
__thread bool vi3_forced_null = false;
__thread bool vi3_forced_diff = false;
__thread int vi3_last_to_register = -1;
__thread bool vi3_idle_collapsed = false;
__thread cct_node_t *local_idle_placeholder = NULL;

#if FREELISTS_ENABLED
__thread long notification_used = 0;
#endif

#if ENDING_REGION_MULTIPLE_TIMES_BUG_FIX == 1
__thread typed_random_access_stack_struct(runtime_region) *runtime_master_region_stack = NULL;
#endif
//******************************************************************************
// private variables 
//******************************************************************************

static __thread int ompt_thread_type = ompt_thread_unknown;



//******************************************************************************
// interface operations
//******************************************************************************

void
ompt_thread_type_set
(
 ompt_thread_t ttype
)
{
  ompt_thread_type = ttype;
}


ompt_thread_t 
ompt_thread_type_get
(
)
{
  return ompt_thread_type; 
}
