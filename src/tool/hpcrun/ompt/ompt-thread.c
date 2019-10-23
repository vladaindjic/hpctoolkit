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
__thread region_stack_el_t region_stack[MAX_NESTING_LEVELS];
// index of the last element
__thread int top_index = -1;

// number of unresolved regions
__thread int unresolved_cnt = 0;

// FIXME vi3: just a temp solution
__thread typed_stack_elem_ptr(region) ending_region = NULL;



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


region_stack_el_t*
top_region_stack
(
 void
)
{
  // FIXME: is invalid value for region ID
  return (top_index) > -1 ? &region_stack[top_index] : NULL;
}

region_stack_el_t*
pop_region_stack
(
 void
)
{
  return (top_index) > -1 ? &region_stack[top_index--] : NULL;
}


void
push_region_stack
(
 typed_stack_elem_ptr(notification) notification,
 bool took_sample, 
 bool team_master
)
{
  // FIXME: potential place of segfault, when stack is full
  top_index++;
  region_stack[top_index].notification = notification;
  region_stack[top_index].took_sample = took_sample;
  region_stack[top_index].team_master = team_master;
}


void
clear_region_stack
(
 void
)
{
  top_index = -1;
}


int
is_empty_region_stack
(
 void
)
{
  return top_index < 0;
}
