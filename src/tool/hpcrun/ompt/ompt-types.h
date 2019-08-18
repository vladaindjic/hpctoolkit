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

#ifndef __OMPT_TYPES_H__ 
#define __OMPT_TYPES_H__ 

//******************************************************************************
// OMPT
//******************************************************************************

#include "omp-tools.h"



//******************************************************************************
// local includes  
//******************************************************************************

#include <lib/prof-lean/stdatomic.h>
#include <lib/prof-lean/queues.h>

#include <hpcrun/cct/cct.h>



//******************************************************************************
// macros
//******************************************************************************
#define qtype cqueue


//******************************************************************************
// forward declarations of types
//******************************************************************************

typedef q_element_ptr_t typed_queue_elem_ptr(notification);
typedef q_element_ptr_t typed_queue_elem_ptr(region);

//******************************************************************************
// type declarations 
//******************************************************************************


typedef struct ompt_region_data_s {
  // region freelist, must be at the begin because od up/downcasting
  // like inherited class in C++, inheretes from base_t
  q_element_ptr_t next;

  // region's wait free queue
  typed_queue_elem_ptr(notification) queue;

  // region's freelist which belongs to thread
  typed_queue_elem_ptr(region) *thread_freelist;

  // region id
  uint64_t region_id;

  // call_path to the region
  cct_node_t *call_path;

  // depth of the region, starts from zero
  int depth;

  struct ompt_region_data_s *next_region;
} typed_queue_elem(region);

typed_queue(region, qtype)


typedef struct {
  // it can also cover freelist to, we do not need another next_freelist
  q_element_ptr_t next;

  typed_queue_elem(region) *region_data;

  // region id
  uint64_t region_id;

  // struct ompt_threads_queue_s *threads_queue;
  typed_queue_elem_ptr(notification) *threads_queue;

  // pointer to the cct pseudo node of the region that should be resolve
  cct_node_t* unresolved_cct;
} typed_queue_elem(notification);

typed_queue(notification, qtype)


// region stack element which points to the corresponding
// notification, and says if thread took sample and if the
// thread is the master in team
typedef struct region_stack_el_s {
  typed_queue_elem(notification) *notification;
  bool took_sample;
  bool team_master;
} region_stack_el_t;


// FIXME vi3: ompt_data_t freelist manipulation

#endif

