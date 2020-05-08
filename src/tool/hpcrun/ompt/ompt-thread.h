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

#ifndef __OMPT_THREAD_H__
#define __OMPT_THREAD_H__


//******************************************************************************
// local includes  
//******************************************************************************

#include "ompt-types.h"



//******************************************************************************
// macros
//******************************************************************************

#define MAX_NESTING_LEVELS 128


//******************************************************************************
// global data
//******************************************************************************

extern __thread typed_channel_elem(notification) thread_notification_channel;

// thread's list of notification that can be reused
extern __thread typed_stack_elem_ptr(notification) notification_freelist_head;

// public freelist where all thread's can enqueue region_data to be reused
extern __thread typed_channel_elem(region) region_freelist_channel;

// stack that contains all active parallel regions
extern __thread typed_random_access_stack_struct(region) *region_stack;


// FIXME vi3: just a temp solution
extern __thread typed_stack_elem_ptr(region) ending_region;

// number of unresolved regions
extern __thread int unresolved_cnt;

extern __thread int depth_last_sample_taken;
extern __thread uint64_t region_id_last_sample_taken;

// thread is waiting on last implicit barrier
// at the very end of the innermost region
extern __thread bool waiting_on_last_implicit_barrier;
extern __thread bool innermost_parallel_data_avail;
// tmp vi3 solution (see ompt-defer.c)
extern __thread bool vi3_forced_null;
extern __thread bool vi3_forced_diff;
extern __thread int vi3_last_to_register;
extern __thread bool vi3_idle_collapsed;
// place where all idle samples will be put before resolving
extern __thread cct_node_t *local_idle_placeholder;

#if FREELISTS_ENABLED
#if FREELISTS_DEBUG
extern __thread long notification_used;
#endif
#endif


extern __thread bool registration_safely_applied;
//******************************************************************************
// interface operations 
//******************************************************************************

void 
ompt_thread_type_set
(
  ompt_thread_t ttype
);


ompt_thread_t 
ompt_thread_type_get
(
  void
);


#endif
