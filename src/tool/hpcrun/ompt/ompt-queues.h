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

#ifndef STACK_H
#define STACK_H

//*****************************************************************************
// Description:
//
//   interface for sequential and concurrent LIFO stacks (AKA stacks)
//
//*****************************************************************************



//*****************************************************************************
// local includes
//*****************************************************************************

#include <lib/prof-lean/stdatomic.h>
//#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>



//*****************************************************************************
// macros
//*****************************************************************************

#define ignore(x) ;
#define show(x) x

#define typed_stack_declare_type(type)		\
  typedef typed_stack_elem(type) * typed_stack_elem_ptr(type)

#define typed_stack_declare(type, flavor)		\
  typed_stack(type, flavor, ignore)

#define typed_stack_impl(type, flavor)			\
  typed_stack(type, flavor, show)

// routine name for a stack operation
#define stack_op(qtype, op) \
  qtype ## _ ## op

// typed stack pointer
#define typed_stack_elem_ptr(type) \
  type ## _ ## s_element_ptr_t

#define typed_stack_elem(type) \
  type ## _ ## s_element_t

#define typed_stack_elem_fn(type, fn) \
  type ## _ ## s_element_ ## fn

// routine name for a typed stack operation
#define typed_stack_op(type, qtype, op) \
  type ## _ ## qtype ## _ ## op

// ptr set routine name for a typed stack
#define typed_stack_elem_ptr_set(type, qtype) \
  typed_stack_op(type, qtype, ptr_set)

// ptr get routine name for a typed stack
#define typed_stack_elem_ptr_get(type, qtype) \
  typed_stack_op(type, qtype, ptr_get)

// swap routine name for a typed stack
#define typed_stack_swap(type, qtype) \
  typed_stack_op(type, qtype, swap)

// push routine name for a typed stack
#define typed_stack_push(type, qtype) \
  typed_stack_op(type, qtype, push)

// pop routine name for a typed stack
#define typed_stack_pop(type, qtype) \
  typed_stack_op(type, qtype, pop)

// steal routine name for a typed stack
#define typed_stack_steal(type, qtype) \
  typed_stack_op(type, qtype, steal)

// forall routine name for a typed stack
#define typed_stack_forall(type, qtype) \
  typed_stack_op(type, qtype, forall)

// next_set will set next pointer to specified value
#define typed_stack_next_set(type, qtype) \
  typed_stack_op(type, qtype, next_set)

// next_get will return value of next pointer and eventually wait (see cstack)
#define typed_stack_next_get(type, qtype) \
  typed_stack_op(type, qtype, next_get)


// define typed wrappers for a stack type
#define typed_stack(type, qtype, macro) \
  void \
  typed_stack_elem_ptr_set(type, qtype) \
  (typed_stack_elem_ptr(type) e, typed_stack_elem(type) *v) \
  macro({ \
    stack_op(qtype,ptr_set)((s_element_ptr_t *) e, \
      (s_element_t *) v); \
  }) \
\
  typed_stack_elem(type) * \
  typed_stack_elem_ptr_get(type, qtype) \
  (typed_stack_elem_ptr(type) *e) \
  macro({ \
    typed_stack_elem(type) *r = (typed_stack_elem(type) *) \
    stack_op(qtype,ptr_get)((s_element_ptr_t *) e); \
    return r; \
  }) \
\
  typed_stack_elem(type) * \
  typed_stack_swap(type, qtype) \
  (typed_stack_elem_ptr(type) *q, typed_stack_elem(type) *v) \
  macro({ \
    typed_stack_elem(type) *e = (typed_stack_elem(type) *) \
    stack_op(qtype,swap)((s_element_ptr_t *) q, \
      (s_element_t *) v); \
    return e; \
  }) \
\
  void \
  typed_stack_push(type, qtype) \
  (typed_stack_elem_ptr(type) *q, typed_stack_elem(type) *e) \
  macro({ \
    stack_op(qtype,push)((s_element_ptr_t *) q, \
    (s_element_t *) e); \
  }) \
\
  typed_stack_elem(type) * \
  typed_stack_pop(type, qtype) \
  (typed_stack_elem_ptr(type) *q) \
  macro({ \
    typed_stack_elem(type) *e = (typed_stack_elem(type) *) \
    stack_op(qtype,pop)((s_element_ptr_t *) q); \
    return e; \
  }) \
\
  typed_stack_elem(type) * \
  typed_stack_steal(type, qtype) \
  (typed_stack_elem_ptr(type) *q) \
  macro({ \
    typed_stack_elem(type) *e = (typed_stack_elem(type) *) \
    stack_op(qtype,steal)((s_element_ptr_t *) q); \
    return e; \
  }) \
\
 void typed_stack_forall(type, qtype) \
 (typed_stack_elem_ptr(type) *q, void (*fn)(typed_stack_elem(type) *, void *), void *arg) \
  macro({ \
    stack_op(qtype,forall)((s_element_ptr_t *) q, (stack_forall_fn_t) fn, arg);	\
  }) \
\
  void \
  typed_stack_next_set(type, qtype) \
  (typed_stack_elem_ptr(type) e, typed_stack_elem(type) *next_e) \
  macro({ \
    stack_op(qtype,next_set)((s_element_t *) e, \
      (s_element_t *) next_e); \
  }) \
\
  typed_stack_elem(type) * \
  typed_stack_next_get(type, qtype) \
  (typed_stack_elem_ptr(type) e) \
  macro({ \
    typed_stack_elem(type) *r = (typed_stack_elem(type) *) \
    stack_op(qtype,next_get)((s_element_t *) e); \
    return r; \
  })

  
  
  
  
// ====================================================================== Multi Producer Single Consumer Channel
// Functions
// 1. INIT (initialize both stacks in channel)
// 2. SHARED_PUSH (push element to shared stack of channel)
// 3. PRIVATE_PUSH (push element to private stack of channel)
// 4. STEAL (take element from channel)


#define typed_channel_declare_type(type)		\
  typedef typed_channel_elem(type) * typed_channel_elem_ptr(type)

#define typed_channel_declare(type)		\
  typed_channel(type, ignore)

#define typed_channel_impl(type)			\
  typed_channel(type, show)

// routine name for a channel operation
#define channel_op(op) \
  mpsc_channel ## _ ## op

#if 1
// typed channel pointer
#define typed_channel_elem_ptr(type) \
  type ## _ ## mpsc_channel_ptr_t
#endif

#define typed_channel_elem(type) \
  type ## _ ## mpsc_channel_t

#define typed_channel_elem_fn(type, fn) \
  type ## _ ## mpsc_channel_ ## fn

// routine name for a typed channel operation
#define typed_channel_op(type, op) \
  type ## _ ## op

// swap routine name for a typed channel
#define typed_channel_init(type) \
  typed_channel_op(type, init)

// push routine name for a typed channel
#define typed_channel_shared_push(type) \
  typed_channel_op(type, shared_push)

// pop routine name for a typed channel
#define typed_channel_private_push(type) \
  typed_channel_op(type, private_push)

// steal routine name for a typed channel
#define typed_channel_steal(type) \
  typed_channel_op(type, steal)


// define typed wrappers for a channel type
#define typed_channel(type, macro) \
\
  void \
  typed_channel_init(type) \
  (typed_channel_elem_ptr(type) c) \
  macro({ \
    channel_op(init)((mpsc_channel_t *) c); \
  }) \
\
  void \
  typed_channel_shared_push(type) \
  (typed_channel_elem_ptr(type) c, typed_stack_elem(type) *e) \
  macro({ \
    channel_op(shared_push)((mpsc_channel_t *) c, \
    (s_element_t *) e); \
  }) \
\
  void \
  typed_channel_private_push(type) \
  (typed_channel_elem_ptr(type) c, typed_stack_elem(type) *e) \
  macro({ \
    channel_op(private_push)((mpsc_channel_t *) c, \
    (s_element_t *) e); \
  }) \
\
  typed_stack_elem(type) * \
  typed_channel_steal(type) \
  (typed_channel_elem_ptr(type) c) \
  macro({ \
    typed_stack_elem(type) *e = (typed_stack_elem(type) *) \
    channel_op(steal)((mpsc_channel_t *) c); \
    return e; \
  }) \
// ========================================================


//*****************************************************************************
// types
//*****************************************************************************

typedef struct s_element_ptr_u {
  _Atomic(struct s_element_s*) aptr;
} s_element_ptr_t;


typedef struct s_element_s {
  s_element_ptr_t next;
} s_element_t;

typedef struct mpsc_channel_s {
  s_element_ptr_t shared;
  s_element_ptr_t private;
} mpsc_channel_t;

typedef void (*stack_forall_fn_t)(s_element_t *e, void *arg);



//*****************************************************************************
// interface functions
//*****************************************************************************

//-----------------------------------------------------------------------------
// sequential LIFO stack interface operations
//-----------------------------------------------------------------------------

void
sstack_ptr_set
(
  s_element_ptr_t *e,
  s_element_t *v
);


s_element_t *
sstack_ptr_get
(
  s_element_ptr_t *e
);


// set q->next to point to e and return old value of q->next
s_element_t *
sstack_swap
(
  s_element_ptr_t *q,
  s_element_t *e
);


// push a singleton e or a chain beginning with e onto q
void
sstack_push
(
  s_element_ptr_t *q,
  s_element_t *e
);


// pop a singlegon from q or return 0
s_element_t *
sstack_pop
(
  s_element_ptr_t *q
);


// steal the entire chain rooted at q
s_element_t *
sstack_steal
(
  s_element_ptr_t *q
);


// reverse the entire chain rooted at q and set q to be the previous tail
void
sstack_reverse
(
  s_element_ptr_t *q
);


// iterate over each element e in the stack, call fn(e, arg)
void
sstack_forall
(
  s_element_ptr_t *q,
  stack_forall_fn_t fn,
  void *arg
);

s_element_t*
sstack_next_get
(
  s_element_t* e
);

void
sstack_next_set
(
  s_element_t* e,
  s_element_t* next_e
);

//-----------------------------------------------------------------------------
// concurrent LIFO stack interface operations
//-----------------------------------------------------------------------------

void
cstack_ptr_set
(
  s_element_ptr_t *e,
  s_element_t *v
);


s_element_t *
cstack_ptr_get
(
  s_element_ptr_t *e
);


// set q->next to point to e and return old value of q->next
s_element_t *
cstack_swap
(
  s_element_ptr_t *q,
  s_element_t *e
);


// push a singleton e or a chain beginning with e onto q
void
cstack_push
(
  s_element_ptr_t *q,
  s_element_t *e
);


// pop a singlegon from q or return 0
s_element_t *
cstack_pop
(
  s_element_ptr_t *q
);


// steal the entire chain rooted at q
s_element_t *
cstack_steal
(
  s_element_ptr_t *q
);


// iterate over each element e in the stack, call fn(e, arg)
// note: unsafe to use concurrently with cstack_steal or cstack_pop
void
cstack_forall
(
  s_element_ptr_t *q,
  stack_forall_fn_t fn,
  void *arg
);

s_element_t*
cstack_next_get
(
  s_element_t* e
);

void
cstack_next_set
(
  s_element_t* e,
  s_element_t* next_e
);

// ==================================== Multi Producer Single Consumer Channel
void
mpsc_channel_init
(
  mpsc_channel_t *c
);

void
mpsc_channel_shared_push
(
  mpsc_channel_t *c,
  s_element_t *e
);

void
mpsc_channel_private_push
(
  mpsc_channel_t *c,
  s_element_t *e
);

s_element_t *
mpsc_channel_steal
(
  mpsc_channel_t *c
);


// ====================================

// ==================================== Random Access Stack


#define typed_random_access_stack_declare_type(type)		\
  typedef typed_random_access_stack_elem(type) * typed_random_access_stack_elem_ptr(type)

#define typed_random_access_stack_declare(type)		\
  typed_random_access_stack(type, ignore)

#define typed_random_access_stack_impl(type)			\
  typed_random_access_stack(type, show)

// routine name for a random_access_stack operation
#define random_access_stack_op(op) \
  random_access_stack_ ## op

// typed random_access_stack pointer
#define typed_random_access_stack_elem_ptr(type) \
  type ## _ ## random_access_element_ptr_t

#define typed_random_access_stack_elem(type) \
  type ## _ ## random_access_element_t

#define typed_random_access_stack_ptr(type) \
  type ## _ ## random_access_stack_t *

#define typed_random_access_stack_struct(type) \
  type ## _ ## random_access_stack_t


#define typed_random_access_stack_elem_fn(type, fn) \
  type ## _ ## s_element_ ## fn

// routine name for a typed random_access_stack operation
#define typed_random_access_stack_op(type, op) \
  type ## _random_access_stack_ ## op

// init routine name for a typed random_access_stack
#define typed_random_access_stack_init(type) \
  typed_random_access_stack_op(type, init)

// init routine name for a typed random_access_stack
#define typed_random_access_stack_empty(type) \
  typed_random_access_stack_op(type, empty)

// push routine name for a typed random_access_stack
#define typed_random_access_stack_push(type) \
  typed_random_access_stack_op(type, push)

// pop routine name for a typed random_access_stack
#define typed_random_access_stack_pop(type) \
  typed_random_access_stack_op(type, pop)

// get routine name for a typed random_access_stack
#define typed_random_access_stack_get(type) \
  typed_random_access_stack_op(type, get)

// top routine name for a typed random_access_stack
#define typed_random_access_stack_top(type) \
  typed_random_access_stack_op(type, top)

// top_index_get routine name for a typed random_access_stack
#define typed_random_access_stack_top_index_get(type) \
  typed_random_access_stack_op(type, top_index_get)

// top_index_set routine name for a typed random_access_stack
#define typed_random_access_stack_top_index_set(type) \
  typed_random_access_stack_op(type, top_index_set)

// forall routine name for a typed random_access_stack
#define typed_random_access_stack_forall(type) \
  typed_random_access_stack_op(type, forall)

// iterate_from routine name for a typed random_access_stack
#define typed_random_access_stack_iterate_from(type) \
  typed_random_access_stack_op(type, iterate_from)


// define typed wrappers for a random_access_stack type
#define typed_random_access_stack(type, macro) \
  typed_random_access_stack_ptr(type) \
  typed_random_access_stack_init(type) \
  (size_t max_elements) \
  macro({ \
    typed_random_access_stack_ptr(type) stack = \
        (typed_random_access_stack_ptr(type))random_access_stack_op(init)(max_elements, \
                                                sizeof(typed_random_access_stack_elem(type))); \
    return stack; \
  }) \
\
  bool \
  typed_random_access_stack_empty(type) \
  (typed_random_access_stack_ptr(type) stack) \
  macro({ \
    return random_access_stack_op(empty)((random_access_stack_t *) stack); \
  }) \
  \
  typed_random_access_stack_elem(type) * \
  typed_random_access_stack_push(type) \
  (typed_random_access_stack_ptr(type) stack) \
  macro({ \
    typed_random_access_stack_elem(type) *e = (typed_random_access_stack_elem(type) *) \
    random_access_stack_op(push)((random_access_stack_t *) stack, \
        sizeof(typed_random_access_stack_elem(type))); \
    return e; \
  }) \
\
  typed_random_access_stack_elem(type) * \
  typed_random_access_stack_pop(type) \
  (typed_random_access_stack_ptr(type) stack) \
  macro({ \
    typed_random_access_stack_elem(type) *e = (typed_random_access_stack_elem(type) *) \
    random_access_stack_op(pop)((random_access_stack_t *) stack, \
        sizeof(typed_random_access_stack_elem(type))); \
    return e; \
  }) \
\
  typed_random_access_stack_elem(type) * \
  typed_random_access_stack_get(type) \
  (typed_random_access_stack_ptr(type) stack, int index) \
  macro({ \
    typed_random_access_stack_elem(type) *e = (typed_random_access_stack_elem(type) *) \
    random_access_stack_op(get)((random_access_stack_t *) stack, \
        sizeof(typed_random_access_stack_elem(type)), index); \
    return e; \
  }) \
\
  typed_random_access_stack_elem(type) * \
  typed_random_access_stack_top(type) \
  (typed_random_access_stack_ptr(type) stack) \
  macro({ \
    typed_random_access_stack_elem(type) *e = (typed_random_access_stack_elem(type) *) \
    random_access_stack_op(top)((random_access_stack_t *) stack); \
    return e; \
  }) \
\
  int \
  typed_random_access_stack_top_index_get(type) \
  (typed_random_access_stack_ptr(type) stack) \
  macro({ \
    return random_access_stack_op(top_index_get)((random_access_stack_t *) stack, \
        sizeof(typed_random_access_stack_elem(type))); \
  }) \
\
  void \
  typed_random_access_stack_top_index_set(type) \
  (int top_index, typed_random_access_stack_ptr(type) stack) \
  macro({ \
    random_access_stack_op(top_index_set)(top_index, (random_access_stack_t *) stack, \
        sizeof(typed_random_access_stack_elem(type))); \
  }) \
\
 void \
 typed_random_access_stack_forall(type) \
 (typed_random_access_stack_ptr(type) stack, \
     bool (*fn)(typed_random_access_stack_elem(type) *, void *), void *arg) \
  macro({ \
    random_access_stack_op(forall)( \
                                    (random_access_stack_t *) stack, \
                                    (random_access_stack_forall_fn_t) fn, \
                                    arg, \
                                    sizeof(typed_random_access_stack_elem(type)));	\
  }) \
\
 void \
 typed_random_access_stack_iterate_from(type) \
 (int start_from, typed_random_access_stack_ptr(type) stack, \
     bool (*fn)(typed_random_access_stack_elem(type) *, void *), void *arg) \
  macro({ \
    random_access_stack_op(iterate_from)( \
                                    start_from, \
                                    (random_access_stack_t *) stack, \
                                    (random_access_stack_forall_fn_t) fn, \
                                    arg, \
                                    sizeof(typed_random_access_stack_elem(type)));	\
  }) \
\



typedef struct random_access_stack_u {
  void *array;
  void *current;
} random_access_stack_t;

typedef bool (*random_access_stack_forall_fn_t)(void *el, void *arg);


random_access_stack_t *
random_access_stack_init
(
  size_t max_elements,
  size_t element_size
);

bool
random_access_stack_empty
(
  random_access_stack_t *stack
);

void *
random_access_stack_push
(
  random_access_stack_t *stack,
  size_t element_size
);

void *
random_access_stack_pop
(
  random_access_stack_t *stack,
  size_t element_size
);

void *
random_access_stack_get
(
  random_access_stack_t *stack,
  size_t element_size,
  int index
);

void *
random_access_stack_top
(
  random_access_stack_t *stack
);

int
random_access_stack_top_index_get
(
  random_access_stack_t *stack,
  size_t element_size
);

void
random_access_stack_top_index_set
(
  int top_index,
  random_access_stack_t *stack,
  size_t element_size
);

// iterate over each element e in the stack, call fn(e, arg)
// and eventully break if return value of fn is different than zero
void
random_access_stack_forall
(
  random_access_stack_t *stack,
  random_access_stack_forall_fn_t fn,
  void *arg,
  size_t element_size
);

// Iterate over elements of stack started from element at specified index.
// For each element e, call fn(e, arg)
// and eventully break if return value of fn is different than zero
void
random_access_stack_iterate_from
(
  int start_from,
  random_access_stack_t *stack,
  random_access_stack_forall_fn_t fn,
  void *arg,
  size_t element_size
);


#endif
