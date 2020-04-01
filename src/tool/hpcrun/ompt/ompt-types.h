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

#include <hpcrun/cct/cct.h>
#include "ompt-queues.h"


//******************************************************************************
// macros
//******************************************************************************
#define qtype cqueue


//******************************************************************************
// forward declarations of types
//******************************************************************************

//******************************************************************************
// type declarations 
//******************************************************************************

#define FREELISTS_ENABLED 1
#define FREELISTS_DEBUG FREELISTS_ENABLED && 1
#define FREELISTS_DEBUG_WAIT_FOR_REGIONS FREELISTS_DEBUG && 1


struct ompt_region_data_s;
struct ompt_notification_s;
struct mpsc_channel_region_s;
struct mpsc_channel_notification_s;

// struct used to store information about parallel region
typedef struct ompt_region_data_s {
  // used in stack and channel implementation, inherited from s_element_t
  s_element_ptr_t next;
  // region's wait free queue
  struct ompt_notification_s *notification_stack;
  // region's freelist which belongs to thread
  struct mpsc_channel_region_s *owner_free_region_channel;
  // region id
  uint64_t region_id;
  // call_path to the region
  cct_node_t *call_path;
  // depth of the region, starts from zero
  int depth;
  // fields used for debug purposes only
  // vi3: I think that this is used for debug purpose
  struct ompt_region_data_s *next_region;
  // barrier counter which indicates if region is active
  _Atomic(int) barrier_cnt;
} typed_stack_elem(region);

// declare pointer to previous struct
typed_stack_declare_type(region);

// notification that contains information about region
// used in inter-thread communication
typedef struct ompt_notification_s {
  // used in stack and channel implementation, inherited from s_element_t
  s_element_ptr_t next;
  struct ompt_region_data_s *region_data;
  // pseudo cct node which corresponds to the region that should be resolve
  cct_node_t *unresolved_cct;
  struct mpsc_channel_notification_s *notification_channel;
} typed_stack_elem(notification);

// declare pointer to previous struct
typed_stack_declare_type(notification);


// ============ region stacks declaration
// declare api functions of sequential stack of regions
typed_stack_declare(region, sstack);
// declare api functions sequential stack of regions
typed_stack_declare(region, cstack);

// ============ notification stacks declaration
// declare api functions of sequential stack of regions
typed_stack_declare(notification, sstack);
// declare api functions of concurrent stack of regions
typed_stack_declare(notification, cstack);



// ============ multi-producer single-consumer channels of regions
// struct that represents channel of region_data structure
typedef struct mpsc_channel_region_s {
  typed_stack_elem_ptr(region) shared;
  typed_stack_elem_ptr(region) private;
#if FREELISTS_DEBUG
  _Atomic(long) region_used;
#endif
} typed_channel_elem(region);
// declare pointer to previous struct
typed_channel_declare_type(region);
// declare api functions of mpsc channel of regions
typed_channel_declare(region);

// ============ multi-producer single-consumer channels of notifications
// struct that represents channel of notification structure
typedef struct mpsc_channel_notification_s {
  typed_stack_elem_ptr(notification) shared;
  typed_stack_elem_ptr(notification) private;
} typed_channel_elem(notification);
// declare pointer to previous struct
typed_channel_declare_type(notification);
// declare api functions of mpsc channel of regions
typed_channel_declare(notification);


// tmp data struct
typedef struct old_region_s {
  struct old_region_s *next;
  typed_stack_elem(region) *region;
  typed_stack_elem(notification) *notification;
} old_region_t;


// ========================= Random Access Stack of active parallel regions
// Structure that represents a single element of random access stack of active parallel regions.
// The first field of the structure represents the pointer to the corresponding
// notification. Other two fields say if the thread, which is the owher of the stack,
// took sample in the region and also if the
// thread is the master of that region.
typedef struct region_stack_el_s {
  // region at depth equal to index on the stack
  typed_stack_elem_ptr(region) region_data;
  // placeholder that keeps call paths of samples attributed to region_data
  cct_node_t *unresolved_cct;
  bool took_sample;
  // should be safe to remove this
  bool team_master;
  old_region_t *old_region_list;
} typed_random_access_stack_elem(region);
// declare pointer to previous struct
typed_random_access_stack_declare_type(region);
// structure that represents random access stack of active parallel regions
typedef struct {
  typed_random_access_stack_elem(region) *array;
  typed_random_access_stack_elem(region) *current;
} typed_random_access_stack_struct(region);
// declare api functions of random access stack of regions
typed_random_access_stack_declare(region);

// FIXME vi3: ompt_data_t freelist manipulation
#endif

