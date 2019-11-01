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
// local includes
//*****************************************************************************

#include "ompt-queues.h"

//*****************************************************************************
// interface functions
//*****************************************************************************

#ifndef NULL
#define NULL 0
#endif


#define Ad(q) q.aptr
#define Ap(q) q->aptr

#define s_element_invalid ((s_element_t*)~0)

void
sstack_ptr_set
(
  s_element_ptr_t *p,
  s_element_t *v
)
{
  atomic_store_explicit(&Ap(p), v, memory_order_relaxed);
}


s_element_t *
sstack_ptr_get
(
  s_element_ptr_t *e
)
{
  return atomic_load_explicit(&Ap(e), memory_order_relaxed);
}


s_element_t *
sstack_swap
(
  s_element_ptr_t *q,
  s_element_t *r
)
{
  return atomic_exchange_explicit(&Ap(q), r, memory_order_relaxed);
}

// push the whole chain
void
sstack_push
(
  s_element_ptr_t *q,
  s_element_t *e
)
{
#if 0
  s_element_t *first = atomic_load_explicit(&Ap(q), memory_order_relaxed);
  atomic_store_explicit(&(e->Ad(next)), first, memory_order_relaxed);
  atomic_store_explicit(&Ap(q), e, memory_order_relaxed);
#endif

  s_element_t *new_head = e;

  // push a singleton or a chain on the list
  for (;;) {
    s_element_t *enext = atomic_load(&e->Ad(next));
    if (enext == 0) break;
    e = enext;
  }

  s_element_t *old_head = atomic_load_explicit(&Ap(q), memory_order_relaxed);
  atomic_store_explicit(&e->Ad(next), old_head, memory_order_relaxed);
  atomic_store_explicit(&Ap(q), new_head, memory_order_relaxed);

}


s_element_t *
sstack_pop
(
  s_element_ptr_t *q
)
{
  s_element_t *e = atomic_load_explicit(&Ap(q), memory_order_relaxed);
  if (e) {
    s_element_t *next = atomic_load_explicit(&(e->Ad(next)), memory_order_relaxed);
    atomic_store_explicit(&Ap(q), next, memory_order_relaxed);
    atomic_store_explicit(&(e->Ad(next)), 0, memory_order_relaxed);
  }
  return e;
}


s_element_t *
sstack_steal
(
  s_element_ptr_t *q
)
{
  s_element_t *e = sstack_swap(q, 0);

  return e;
}

// vi3 >> not used
void
sstack_reverse
(
  s_element_ptr_t *q
)
{
  s_element_t *prev = NULL;
  s_element_t *e = atomic_load_explicit(&Ap(q), memory_order_relaxed);
  while (e) {
    s_element_t *next = atomic_load_explicit(&(e->Ad(next)), memory_order_relaxed);
    atomic_store_explicit(&(e->Ad(next)), prev, memory_order_relaxed);
    prev = e;
    e = next;
  }
  atomic_store_explicit(&Ap(q), prev, memory_order_relaxed);
}

// vi3 >> not used
void
sstack_forall
(
  s_element_ptr_t *q,
  stack_forall_fn_t fn,
  void *arg
)
{
  s_element_t *current = atomic_load_explicit(&Ap(q), memory_order_relaxed);
  while (current) {
    fn(current, arg);
    current = atomic_load_explicit(&current->Ad(next), memory_order_relaxed);
  }
}

s_element_t*
sstack_next_get
(
  s_element_t* e
)
{
  return e ? atomic_load_explicit(&e->Ad(next), memory_order_relaxed) : NULL;
}

void
sstack_next_set
(
  s_element_t* e,
  s_element_t* next_e
)
{
  if (e) {
    atomic_store_explicit(&e->Ad(next), next_e, memory_order_relaxed);
  }
}


// concurrent implementation

void
cstack_ptr_set
(
  s_element_ptr_t *e,
  s_element_t *v
)
{
  atomic_init(&Ap(e), v);
}


s_element_t *
cstack_ptr_get
(
  s_element_ptr_t *e
)
{
  return atomic_load(&Ap(e));
}


s_element_t *
cstack_swap
(
  s_element_ptr_t *q,
  s_element_t *r
)
{
  s_element_t *e = atomic_exchange(&Ap(q), r);

  return e;
}


void
cstack_push
(
  s_element_ptr_t *q,
  s_element_t *e
)
{

  s_element_t *new_head = e;

  // push a singleton or a chain on the list
  for (;;) {
    // wait for next pointer if it is invalid
    s_element_t *enext = cstack_next_get(e);
    if (enext == 0) break;
    e = enext;
  }

#if 0
  // Treiber stack push
  do {
    atomic_store(&e->Ad(next), head);
  } while (!atomic_compare_exchange_strong(&Ap(q), &head, new_head));
#endif

  // set next pointer to invalid value
  cstack_next_set(e, s_element_invalid);
  // set new head
  s_element_t *head = (s_element_t *)atomic_exchange(&Ap(q), new_head);
  // connect chain wih old head
  atomic_store(&e->Ad(next), head);
}

// popping from stack can happen after all pushing has finished
s_element_t *
cstack_pop
(
  s_element_ptr_t *q
)
{
#if 0
  s_element_t *oldhead = atomic_load(&Ap(q));
  s_element_t *next = 0;

  do {
    if (oldhead == 0) return 0;
    next = atomic_load(&oldhead->Ad(next));
  } while (!atomic_compare_exchange_strong(&Ap(q), &oldhead, next));

  atomic_store(&oldhead->Ad(next), 0);

  return oldhead;
#endif
  // load head
  s_element_t *head = atomic_load(&Ap(q));
  if (head) {
    // wait until next pointer is set properly
    s_element_t *next = cstack_next_get(head);
    // store new head
    atomic_store(&Ap(q), next);
    // invalidate next pointer of old head
    cstack_next_set(head, 0);
  }
  return head;
}


s_element_t *
cstack_steal
(
  s_element_ptr_t *q
)
{
  s_element_t *e = cstack_swap(q, 0);

  return e;
}


void
cstack_forall
(
  s_element_ptr_t *q,
  stack_forall_fn_t fn,
  void *arg
)
{
  s_element_t *current = atomic_load(&Ap(q));
  while (current) {
    fn(current, arg);
#if 0
    current = atomic_load(&current->Ad(next));
#endif
    current = cstack_next_get(current);
  }
}

s_element_t*
cstack_next_get
(
  s_element_t* e
)
{
  // vi3 >> just for easier debugging
  if (!e) {
    return s_element_invalid;
  }

  s_element_t *next;
  // wait until next pointer is set valid value
  for(;;) {
    next = atomic_load(&e->Ad(next));
    if (next != s_element_invalid) break;
  }
  return next;
}

void
cstack_next_set
(
  s_element_t* e,
  s_element_t* next_e
)
{
  atomic_init(&e->Ad(next), next_e);
}


// ==================================== Multi Producer Single Consumer Channel
void
mpsc_channel_init
(
  mpsc_channel_t *c
)
{
  atomic_store_explicit(&c->Ad(shared), 0, memory_order_relaxed);
  atomic_store_explicit(&c->Ad(private), 0, memory_order_relaxed);
}

#include <stdio.h>
#include <stdlib.h>

void
mpsc_channel_shared_push
(
  mpsc_channel_t *c,
  s_element_t *e
)
{
  cstack_push(&c->shared, e);
}

void
mpsc_channel_private_push
(
  mpsc_channel_t *c,
  s_element_t *e
)
{
  sstack_push(&c->private, e);
}

s_element_t *
mpsc_channel_steal
(
  mpsc_channel_t *c
)
{
  // private stack is empty
  if (!sstack_ptr_get(&c->private)) {
    // steal from shared stack
    s_element_t *el = cstack_steal(&c->shared);
    // push stolen elements to private stack
    if (el) {
      // in order to get proper next element,
      // need to wait for it to be set
      cstack_push(&c->private, el);
    }
  }
  // pop from private stack, if it contains any elements
  return sstack_pop(&c->private);
}


// ====================================

// ==================================== Random Access Stack
#include <stdio.h>
#include <stdlib.h>
#include <tool/hpcrun/memory/hpcrun-malloc.h>

random_access_stack_t *
random_access_stack_init
(
  size_t max_elements,
  size_t element_size
)
{
  random_access_stack_t *stack = hpcrun_malloc(sizeof(random_access_stack_t));
  stack->array = hpcrun_malloc(max_elements * element_size);
//  random_access_stack_t *stack = malloc(sizeof(random_access_stack_t));
//  stack->array = malloc(max_elements * element_size);

  stack->current = stack->array - element_size;
  return stack;
}

bool
random_access_stack_empty
(
  random_access_stack_t *stack
)
{
  return stack->current < stack->array;
}

void *
random_access_stack_push
(
  random_access_stack_t *stack,
  size_t element_size
)
{
  stack->current += element_size;
  return stack->current;
}

void *
random_access_stack_pop
(
  random_access_stack_t *stack,
  size_t element_size
)
{
  if (random_access_stack_empty(stack)) {
    // nothing to pop
    return NULL;
  }
  void *old_top = stack->current;
  stack->current -= element_size;
  return old_top;
}

void *
random_access_stack_get
(
  random_access_stack_t *stack,
  size_t element_size,
  int index
)
{
  // NOTE vi3: this function returns pointer to specified element.
  // Don't care if element is invalid if e.g stack is empty.
  return stack->array + element_size * index;
}

void *
random_access_stack_top
(
  random_access_stack_t *stack
)
{
  if (random_access_stack_empty(stack)) {
    return NULL;
  }
  return stack->current;
}

int
random_access_stack_top_index_get
(
  random_access_stack_t *stack,
  size_t element_size
)
{
  if (random_access_stack_empty(stack)) {
    return -1;
  }
  // NOTE vi3: It is possible to get warning because of conversion
  return (stack->current - stack->array) / element_size;
}

void
random_access_stack_top_index_set
(
  int top_index,
  random_access_stack_t *stack,
  size_t element_size
)
{
  if (top_index < 0) {
    // set -1 as indicator that stack is empty
    top_index = -1;
  }
  // update top element
  stack->current = stack->array + top_index * element_size;
}

void
random_access_stack_forall
(
  random_access_stack_t *stack,
  random_access_stack_forall_fn_t fn,
  void *arg,
  size_t element_size
)
{
  void *e;
  bool end;
  for (e = stack->current; e >= stack->array; e -= element_size) {
    end = fn(e, arg);
    if (end)
      break;
  }
#if 0
  // It is possile to use function below with this parameters
  int start_index = (stack->current - stack->array) / element_size
  random_access_stack_iterate_from(start_index, stack, fn, arg, element_size);
#endif
}

void
random_access_stack_iterate_from
(
  int start_from,
  random_access_stack_t *stack,
  random_access_stack_forall_fn_t fn,
  void *arg,
  size_t element_size
)
{
  void *e;
  bool end;
  for (e = stack->array + start_from * element_size; e >= stack->array; e -= element_size) {
    end = fn(e, arg);
    if (end)
      break;
  }
}

// ====================================


//*****************************************************************************
// unit test
//*****************************************************************************

#define UNIT_TEST 1
#if UNIT_TEST

#include <stdlib.h>
#include <stdio.h>
#include <omp.h>

typedef struct {
  s_element_ptr_t next;
  int value;
} typed_stack_elem(int); //int_q_element_t

typed_stack_declare_type(int);

typed_stack_declare(int, cstack);

typed_stack_declare(int, sstack);

typed_stack_impl(int, cstack);

typed_stack_impl(int, sstack);

typed_stack_elem_ptr(int) queue;

// channel
typedef struct {
  typed_stack_elem_ptr(int) shared;
  typed_stack_elem_ptr(int) private;
} typed_channel_elem(int);
// declare type
typed_channel_declare_type(int);
// declare api functions
typed_channel_declare(int);
// implement api functions
typed_channel_impl(int);
// channel
typed_channel_elem_ptr(int) channel;

// random access stack
typedef struct {
  int value;
  int other_value;
} typed_random_access_stack_elem(int);

typedef struct {
  typed_random_access_stack_elem(int) *array;
  typed_random_access_stack_elem(int) *current;
} typed_random_access_stack_struct(int);

// declare type
typed_random_access_stack_declare_type(int);
// declare api functions
typed_random_access_stack_declare(int);
// implement api functions
typed_random_access_stack_impl(int);
// pointer to stack
typed_random_access_stack_ptr(int) random_access_stack;

void
print(typed_stack_elem(int) *e, void *arg)
{
  printf("%d\n", e->value);
}

void
test_stack
(
  void
)
{
  printf("%lu, %lu\n", sizeof(s_element_ptr_t), sizeof(s_element_t));
  printf("============= Stack Unit Test\n");
  printf("Value: %p\n", typed_stack_elem_ptr_get(int, cstack)(&queue));
  int i;
  for (i = 0; i < 10; i++) {
    typed_stack_elem_ptr(int) item = (typed_stack_elem_ptr(int)) malloc(sizeof(typed_stack_elem(int)));
    item->value = i;
    typed_stack_next_set(int, cstack)(item, 0);
    typed_stack_push(int, cstack)(&queue, item);

  }
  typed_stack_forall(int, cstack)(&queue, print, 0);
}


void
test_channel
(
  void
)
{
  printf("============= Channel Unit Test\n");
  channel = (typed_channel_elem_ptr(int)) malloc(sizeof(typed_stack_elem(int)));
  int i;
  for (i = 0; i < 10; i++) {
    typed_stack_elem_ptr(int) item = (typed_stack_elem_ptr(int)) malloc(sizeof(typed_stack_elem(int)));
    item->value = i;
    typed_stack_elem_ptr_set(int, cstack)(item, 0);
    if (i % 2 == 0) {
      typed_channel_shared_push(int)(channel, item);
    } else {
      typed_channel_private_push(int)(channel, item);
    }

  }

  // expected order: {9, 7, 5, 3, 1, 8, 6, 4, 2, 0}
  for(i = 0; i < 10; i++) {
    typed_stack_elem(int) *el = typed_channel_steal(int)(channel);
    printf(">> %d\n", el ? el->value : 0);
  }
}

bool
show_fn(
  typed_random_access_stack_elem(int) *e,
  void *arg
)
{
  printf("--- Value: %d\n", e->value);
  return 0;
}

bool
show_first_10_elements
(
  typed_random_access_stack_elem(int) *e,
  void *arg
)
{
  int *index = (int *)arg;
  printf("--- Index: %d, Value: %d\n", (*index)++, e->value);
  return *index > 9;
}


void
test_random_access_stack
(
  void
)
{
  printf("============= Random Access Stack Unit Test\n");
  random_access_stack = typed_random_access_stack_init(int)(10);
  printf("Initial>> Stack is empty: %d\n", typed_random_access_stack_empty(int)(random_access_stack));
  printf(">>Top index: %d\n", typed_random_access_stack_top_index_get(int)(random_access_stack));
  printf(">>Top element: %p\n", typed_random_access_stack_top(int)(random_access_stack));
  printf("\n\n\n");
  int i;
  for (i = 0; i < 10; i++) {
    // first push
    typed_random_access_stack_elem(int) *item = typed_random_access_stack_push(int)(random_access_stack);
    // then store value
    item->value = i;
  }
  printf("Pushing finished>> Stack is empty: %d\n", typed_random_access_stack_empty(int)(random_access_stack));
  printf(">>Top index: %d\n", typed_random_access_stack_top_index_get(int)(random_access_stack));
  printf(">>Top element: %p\n", typed_random_access_stack_top(int)(random_access_stack));
  printf("\n\n\n");
  typed_random_access_stack_forall(int)(random_access_stack, show_fn, 0);

  typed_random_access_stack_elem(int) *item = typed_random_access_stack_top(int)(random_access_stack);
  printf("Top element: %d\n", item->value);

  for(; !typed_random_access_stack_empty(int)(random_access_stack);) {
    typed_random_access_stack_elem(int) *item = typed_random_access_stack_pop(int)(random_access_stack);
    printf("Popped Value: %d\n", item->value);
  }

  printf("After popping>> Stack is empty: %d\n", typed_random_access_stack_empty(int)(random_access_stack));
  printf(">>Top index: %d\n", typed_random_access_stack_top_index_get(int)(random_access_stack));
  printf(">>Top element: %p\n", typed_random_access_stack_top(int)(random_access_stack));
  printf("\n\n\n");
  printf("Iterate now\n");
  typed_random_access_stack_forall(int)(random_access_stack, show_fn, 0);
  printf("Anythig???\n");

  printf("Pushing again\n");
  for (i = 0; i < 10; i++) {
    // first push
    typed_random_access_stack_elem(int) *item = typed_random_access_stack_push(int)(random_access_stack);
    // then store value
    item->value = i;
  }
  printf("Pushing finished>> Stack is empty: %d\n", typed_random_access_stack_empty(int)(random_access_stack));
  printf(">>Top index: %d\n", typed_random_access_stack_top_index_get(int)(random_access_stack));
  printf(">>Top element: %p\n", typed_random_access_stack_top(int)(random_access_stack));
  printf("\n\n\n");
  typed_random_access_stack_forall(int)(random_access_stack, show_fn, 0);

  typed_random_access_stack_pop(int)(random_access_stack);
  typed_random_access_stack_pop(int)(random_access_stack);
  typed_random_access_stack_pop(int)(random_access_stack);
  printf("Pop three times\n");
  printf(">>Top index: %d\n", typed_random_access_stack_top_index_get(int)(random_access_stack));
  printf(">>Top element: %p\n", typed_random_access_stack_top(int)(random_access_stack));
  printf("\n\n\n");

  printf("Pushing again\n");
  for (i = 0; i < 10; i++) {
    // first push
    typed_random_access_stack_elem(int) *item = typed_random_access_stack_push(int)(random_access_stack);
    // then store value
    item->value = i + 33;
  }

  printf("Pushing finished>> Stack is empty: %d\n", typed_random_access_stack_empty(int)(random_access_stack));
  printf(">>Top index: %d\n", typed_random_access_stack_top_index_get(int)(random_access_stack));
  printf(">>Top element: %p\n", typed_random_access_stack_top(int)(random_access_stack));
  printf("\n\n\n");

  typed_random_access_stack_forall(int)(random_access_stack, show_fn, 0);
  printf("\n\n\n");
  int index = 0;
  typed_random_access_stack_forall(int)(random_access_stack, show_first_10_elements, &index);
  printf("\n\n\n");
  typed_random_access_stack_iterate_from(int)(10, random_access_stack, show_fn, 0);
  printf("\n\n\n");
  index = 0;
  typed_random_access_stack_iterate_from(int)(11, random_access_stack, show_first_10_elements, &index);

  printf("\n\n\n");
  typed_random_access_stack_top_index_set(int)(6, random_access_stack);
  printf("Top index: %d\n", typed_random_access_stack_top_index_get(int)(random_access_stack));
  typed_random_access_stack_forall(int)(random_access_stack, show_fn, 0);
  typed_random_access_stack_top_index_set(int)(-10, random_access_stack);
  printf("Top index: %d\n", typed_random_access_stack_top_index_get(int)(random_access_stack));
  printf("Print now\n");
  printf("Anything??? Stack is empty: %d\n", typed_random_access_stack_empty(int)(random_access_stack));
}

int main(int argc, char **argv)
{
  test_stack();
  printf("\n\n\n");
  test_channel();
  printf("\n\n\n");
  test_random_access_stack();
}

#endif


#if 0

#include <stdlib.h>
#include <stdio.h>
#include <omp.h>

typedef struct {
    s_element_ptr_t next;
    int value;
} typed_stack_elem(int); //int_q_element_t


typed_stack_elem_ptr(int) queue;

#define qtype cstack

typed_stack(int, qtype)

typed_stack_elem(int) *
typed_stack_elem_fn(int,new)(int value)
{
    typed_stack_elem(int) *e =(typed_stack_elem(int) *) malloc(sizeof(int_s_element_t));
    e->value = value;
    typed_stack_elem_ptr_set(int, qtype)(&e->next, 0);
}


void
pop
(
 int n
)
{
  int i;
  for(i = 0; i < n; i++) {
    typed_stack_elem(int) *e = typed_stack_pop(int, qtype)(&queue);
    if (e == 0) {
      printf("%d queue empty\n", omp_get_thread_num());
      break;
    } else {
      printf("%d popping %d\n", omp_get_thread_num(), e->value);
    }
  }
}


void
push
(
 int min,
 int n
)
{
  int i;
  for(i = min; i < min+n; i++) {
    printf("%d pushing %d\n", omp_get_thread_num(), i);
    typed_stack_push(int, qtype)(&queue, typed_stack_elem_fn(int, new)(i));
  }
}


void
dump
(
 int_s_element_t *e
)
{
  int i;
  for(; e; e = (int_s_element_t *) typed_stack_elem_ptr_get(int,qtype)(&e->next)) {
    printf("%d stole %d\n", omp_get_thread_num(), e->value);
  }
}


int
main
(
 int argc,
 char **argv
)
{
  typed_stack_elem_ptr_set(int, qtype)(&queue, 0);
#pragma omp parallel
  {
    push(0, 30);
    pop(10);
    push(100, 12);
    // pop(100);
    int_s_element_t *e = typed_stack_steal(int, qtype)(&queue);
    dump(e);
    push(300, 30);
    typed_stack_push(int, qtype)(&queue, e);
    pop(100);
  }
}

#endif
