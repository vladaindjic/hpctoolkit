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


//******************************************************************************
// local includes  
//******************************************************************************

#include <hpcrun/cct_backtrace_finalize.h>
#include <hpcrun/hpcrun-initializers.h>
#include <hpcrun/sample_event.h>
#include <hpcrun/thread_data.h>
#include <hpcrun/thread_finalize.h>
#include <hpcrun/trace.h>
#include <hpcrun/unresolved.h>

#include "ompt-callstack.h"
#include "ompt-defer.h"
#include "ompt-interface.h"
#include "ompt-placeholders.h"
#include "ompt-thread.h"
#include "ompt-task.h"

#if defined(HOST_CPU_PPC) 
#include "ompt-gcc4-ppc64.h"
#define ADJUST_PC
#elif defined(HOST_CPU_x86) || defined(HOST_CPU_x86_64)
#include "ompt-gcc4-x86.h"
#define ADJUST_PC
#elif defined(HOST_CPU_ARM64)
#else
#error "invalid architecture type"
#endif



//******************************************************************************
// macros
//******************************************************************************

#define DEREFERENCE_IF_NON_NULL(ptr) (ptr ? *(void **) ptr : 0)

#define OMPT_DEBUG 0
#define ALLOW_DEFERRED_CONTEXT 1


#if OMPT_DEBUG
#define elide_debug_dump(t, i, o, r) \
  if (ompt_callstack_debug) stack_dump(t, i, o, r)
#define elide_frame_dump() if (ompt_callstack_debug) frame_dump()
#else
#define elide_debug_dump(t, i, o, r)
#define elide_frame_dump() 
#endif


#define FP(frame, which) (frame->which ## _frame.ptr)
#define FF(frame, which) (frame->which ## _frame_flags)

#define ff_is_appl(flags) (flags & ompt_frame_application)
#define ff_is_rt(flags)   (!ff_is_appl(flags))
#define ff_is_fp(flags)   (flags & ompt_frame_framepointer)



//******************************************************************************
// private variables 
//******************************************************************************

static cct_backtrace_finalize_entry_t ompt_finalizer;

#if ALLOW_DEFERRED_CONTEXT
static thread_finalize_entry_t ompt_thread_finalizer;
#endif

static closure_t ompt_callstack_init_closure;

static int ompt_eager_context = 0;

#if OMPT_DEBUG
static int ompt_callstack_debug = 0;
#endif

// thread local variabled
// set it to ompt_state_undefined by default
static __thread ompt_state_t current_state = ompt_state_undefined;
// keep and old_state in order to detect state transition
static __thread ompt_state_t old_state = ompt_state_undefined;


//******************************************************************************
// private  operations
//******************************************************************************

static void *
fp_exit
(
 ompt_frame_t *frame
)
{
  void *ptr = FP(frame, exit);
#if defined(HOST_CPU_PPC) 
  int flags = FF(frame, exit);
  // on power: ensure the enter frame pointer is CFA for runtime frame
  if (ff_is_fp(flags)) {
    ptr = DEREFERENCE_IF_NON_NULL(ptr);
  }
#endif
  return ptr;
}


static void *
fp_enter
(
 ompt_frame_t *frame
)
{
  void *ptr = FP(frame, enter);
#if defined(HOST_CPU_PPC) 
  int flags = FF(frame, enter);
  // on power: ensure the enter frame pointer is CFA for runtime frame
  if (ff_is_fp(flags) && ff_is_rt(flags)) {
    ptr = DEREFERENCE_IF_NON_NULL(ptr);
  }
#endif
  return ptr;
}


static void 
__attribute__ ((unused))
stack_dump
(
 char *tag, 
 frame_t *inner, 
 frame_t *outer, 
 uint64_t region_id
) 
{
  EMSG("-----%s start", tag); 
  for (frame_t* x = inner; x <= outer; ++x) {
    void* ip;
    hpcrun_unw_get_ip_unnorm_reg(&(x->cursor), &ip);

    load_module_t* lm = hpcrun_loadmap_findById(x->ip_norm.lm_id);
    const char* lm_name = (lm) ? lm->name : "(null)";

    EMSG("ip = %p (%p), sp = %p, load module = %s", 
	 ip, x->ip_norm.lm_ip, x->cursor.sp, lm_name);
  }
  EMSG("-----%s end", tag); 
  EMSG("<0x%lx>\n", region_id); 
}


static void 
__attribute__ ((unused))
frame_dump
(
 void
) 
{
  EMSG("-----frame start");
  for (int i=0;; i++) {
    ompt_frame_t *frame = hpcrun_ompt_get_task_frame(i);
    if (frame == NULL) break;

    void *ep = fp_enter(frame);
    int ef = FF(frame, enter);

    void *xp = fp_exit(frame);
    int xf = FF(frame, exit);

    EMSG("frame %d: enter=(%p,%x), exit=(%p,%x)", i, ep, ef, xp, xf);
  }
  EMSG("-----frame end"); 
}


static int
interval_contains
(
 void *lower, 
 void *upper, 
 void *addr
)
{
  uint64_t uaddr  = (uint64_t) addr;
  uint64_t ulower = (uint64_t) lower;
  uint64_t uupper = (uint64_t) upper;
  
  return ((ulower <= uaddr) & (uaddr <= uupper));
}


static ompt_state_t
check_state
(
 void
)
{
  uint64_t wait_id;
  return hpcrun_ompt_get_state(&wait_id);
}


static void 
set_frame
(
 frame_t *f, 
 ompt_placeholder_t *ph
)
{
  if (ph == (&ompt_placeholders.ompt_idle_state)) {
    vi3_idle_collapsed = true;
  }
  f->cursor.pc_unnorm = ph->pc;
  f->ip_norm = ph->pc_norm;
  f->the_function = ph->pc_norm;
}


static void
collapse_callstack
(
 backtrace_info_t *bt, 
 ompt_placeholder_t *placeholder
)
{
  if (placeholder == (&ompt_placeholders.ompt_idle_state)) {
    vi3_idle_collapsed = true;
  }
  set_frame(bt->last, placeholder);
  bt->begin = bt->last;
  bt->bottom_frame_elided = false;
  bt->partial_unwind = false;
  bt->collapsed = true;
//  bt->fence = FENCE_MAIN;
}


static void
ompt_elide_runtime_frame(
  backtrace_info_t *bt, 
  uint64_t region_id, 
  int isSync
)
{
  // invalidate task_ancestor_level
  task_ancestor_level = -1;

  vi3_idle_collapsed = false;
  frame_t **bt_outer = &bt->last;
  frame_t **bt_inner = &bt->begin;

  frame_t *bt_outer_at_entry = *bt_outer;

  ompt_thread_t thread_type = ompt_thread_type_get();
  switch(thread_type) {
  case ompt_thread_initial:
    break;
  case ompt_thread_worker:
    break;
  case ompt_thread_other:
  case ompt_thread_unknown:
  default:
    goto return_label;
  }

  int on_explicit_barrier = 0;
  // collapse callstack if a thread is idle or waiting in a barrier
  switch(current_state) {
    case ompt_state_wait_barrier:
    case ompt_state_wait_barrier_implicit: // mark it idle
      // previous two states should be deprecated
    case ompt_state_wait_barrier_implicit_parallel:
    case ompt_state_idle:
      // collapse idle state
      collapse_callstack(bt, &ompt_placeholders.ompt_idle_state);
      goto return_label;
    case ompt_state_wait_barrier_explicit: // attribute them to the corresponding
      // We are inside implicit or explicit task.
      // If we are collecting regions' callpaths synchronously, then we
      // are sure that hpcrun_ompt_get_task_data(0) exists.
      // If we are asynchronously resolving call paths, then
      // TD_GET(omp_task_context) will be NULL. We will handle this
      // in ompt_cct_cursor_finalize.
      on_explicit_barrier = 1;
      break;
    default:
      break;
  }

  int i = 0;
  frame_t *it = NULL;

  ompt_frame_t *frame0 = hpcrun_ompt_get_task_frame(i);

  elide_debug_dump("ORIGINAL", *bt_inner, *bt_outer, region_id); 
  elide_frame_dump();

  //---------------------------------------------------------------
  // handle all of the corner cases that can occur at the top of 
  // the stack first
  //---------------------------------------------------------------

  if (!frame0) {
    // corner case: the innermost task (if any) has no frame info. 
    // no action necessary. just return.
    goto clip_base_frames;
  }

  while ((fp_enter(frame0) == 0) && 
         (fp_exit(frame0) == 0)) {
    // corner case: the top frame has been set up, 
    // but not filled in. ignore this frame.
    frame0 = hpcrun_ompt_get_task_frame(++i);

    if (!frame0) {
      if (thread_type == ompt_thread_initial) return;

      // corner case: the innermost task (if any) has no frame info. 
      goto clip_base_frames;
    }
  }

  if (fp_exit(frame0) &&
      (((uint64_t) fp_exit(frame0)) <
        ((uint64_t) (*bt_inner)->cursor.sp))) {
    // corner case: the top frame has been set up, exit frame has been filled in; 
    // however, exit_frame.ptr points beyond the top of stack. the final call 
    // to user code hasn't been made yet. ignore this frame.
    frame0 = hpcrun_ompt_get_task_frame(++i);
  }

  if (!frame0) {
    // corner case: the innermost task (if any) has no frame info. 
    goto clip_base_frames;
  }

  if (fp_enter(frame0)) {
    // the sample was received inside the runtime; 
    // elide frames from top of stack down to runtime entry
    int found = 0;
    for (it = *bt_inner; it <= *bt_outer; it++) {
      if ((uint64_t)(it->cursor.sp) >= (uint64_t)fp_enter(frame0)) {
        if (isSync) {
          // for synchronous samples, elide runtime frames at top of stack
          *bt_inner = it;
        } else if (on_explicit_barrier) {
          // bt_inner points to __kmp_api_GOMP_barrier_10_alias
          *bt_inner = it - 1;
          // replace the last frame with explicit barrier placeholder
          set_frame(*bt_inner, &ompt_placeholders.ompt_barrier_wait_state);
        }
        found = 1;
        break;
      }
    }

    if (found == 0) {
      // enter_frame not found on stack. all frames are runtime frames
      goto clip_base_frames;
    }
    // frames at top of stack elided. continue with the rest
  }

  // general case: elide frames between frame1->enter and frame0->exit
  while (true) {
    frame_t *exit0 = NULL, *reenter1 = NULL;
    ompt_frame_t *frame1;

    int flags0;
    ompt_data_t *task_data = NULL;
    ompt_data_t *parallel_data = NULL;
    int thread_num = 0;
    hpcrun_ompt_get_task_info(i, &flags0, &task_data, &frame0,
        &parallel_data, &thread_num);

    if (!frame0) break;
    void *low_sp = (*bt_inner)->cursor.sp;
    void *high_sp = (*bt_outer)->cursor.sp;

    // if a frame marker is inside the call stack, set its flag to true
    bool exit0_flag = 
      interval_contains(low_sp, high_sp, fp_exit(frame0));

    /* start from the top of the stack (innermost frame). 
       find the matching frame in the callstack for each of the markers in the
       stack. look for them in the order in which they should occur.

       optimization note: this always starts at the top of the stack. this can
       lead to quadratic cost. could pick up below where you left off cutting in 
       previous iterations.
    */
    it = *bt_inner; 
    if (exit0_flag) {
      for (; it <= *bt_outer; it++) {
        if ((uint64_t)(it->cursor.sp) >= (uint64_t)(fp_exit(frame0))) {
          int offset = ff_is_appl(FF(frame0, exit)) ? 0 : 1;
          exit0 = it - offset;
          break;
        }
      }
    }

    if (exit0_flag) {
      // If exit task frame is on thread's stack, then clip
      // everything below it. Remained frames will be put undearneath
      // execution context of the task at level i.
      // In order to do that, memoize i inside task_ancestor_level.
      task_ancestor_level = i;
      *bt_outer = exit0 - 1;
      break;
    }

    frame1 = hpcrun_ompt_get_task_frame(++i);
    if (!frame1) break;

    //-------------------------------------------------------------------------
    //  frame1 points into the stack above the task frame (in the
    //  runtime from the outer task's perspective). frame0 points into
    //  the the stack inside the first application frame (in the
    //  application from the inner task's perspective) the two points
    //  are equal. there is nothing to elide at this step.
    //-------------------------------------------------------------------------
    if ((fp_enter(frame1) == fp_exit(frame0)) &&
	(ff_is_appl(FF(frame0, exit)) &&
	 ff_is_rt(FF(frame1, enter))))
      continue;


    bool reenter1_flag = 
      interval_contains(low_sp, high_sp, fp_enter(frame1));

    if (reenter1_flag) {
      for (; it <= *bt_outer; it++) {
        if ((uint64_t)(it->cursor.sp) >= (uint64_t)(fp_enter(frame1))) {
          reenter1 = it - 1;
          break;
        }
      }
    }



    if (exit0 && reenter1) {



      // FIXME: IBM and INTEL need to agree
      // laksono 2014.07.08: hack removing one more frame to avoid redundancy with the parent
      // It seems the last frame of the master is the same as the first frame of the workers thread
      // By eliminating the topmost frame we should avoid the appearance of the same frame twice 
      //  in the callpath


      //------------------------------------
      // The prefvous version DON'T DELETE
      memmove(*bt_inner+(reenter1-exit0+1), *bt_inner,
	      (exit0 - *bt_inner)*sizeof(frame_t));

      *bt_inner = *bt_inner + (reenter1 - exit0 + 1);

      exit0 = reenter1 = NULL;
      // --------------------------------
    } else if (exit0 && !reenter1) {
      // corner case: reenter1 is in the team master's stack, not mine. eliminate all
      // frames below the exit frame.
      *bt_outer = exit0 - 1;
      break;
    }
  }

  if (*bt_outer != bt_outer_at_entry) {
    bt->bottom_frame_elided = true;
    bt->partial_unwind = false;
    if (*bt_outer < *bt_inner) {
      //------------------------------------------------------------------------
      // corner case: 
      //   the thread state is not ompt_state_idle, but we are eliding the 
      //   whole call stack anyway. 
      // how this arises:
      //   when a sample is delivered between when a worker thread's task state 
      //   is set to ompt_state_work_parallel but the user outlined function 
      //   has not yet been invoked. 
      // considerations:
      //   bt_outer may be out of bounds 
      // handling:
      //   (1) reset both cursors to something acceptable
      //   (2) collapse context to an <openmp idle>
      //------------------------------------------------------------------------
      *bt_outer = *bt_inner;
      // It is possible that we are inside explicit task
      // and delete its context here.
      // Because of that we are going to unnecessarily go into provider.
      // It is possible that task_ancestor_level is set inside while loop,
      // so invalidate it now.
      task_ancestor_level = -1;
      collapse_callstack(bt, &ompt_placeholders.ompt_idle_state);
    }
  }

  elide_debug_dump("ELIDED", *bt_inner, *bt_outer, region_id);
  goto return_label;

 clip_base_frames:
  {
    int master = TD_GET(master);
    if (!master) {
      set_frame(*bt_outer, &ompt_placeholders.ompt_idle_state);
      *bt_inner = *bt_outer;
      bt->bottom_frame_elided = false;
      bt->partial_unwind = false;
      goto return_label;
    }

#if 0
    /* runtime frames with nothing else; it is harmless to reveal them all */
    uint64_t idle_frame = (uint64_t) hpcrun_ompt_get_idle_frame();

    if (idle_frame) {
      /* clip below the idle frame */
      for (it = *bt_inner; it <= *bt_outer; it++) {
        if ((uint64_t)(it->cursor.sp) >= idle_frame) {
          *bt_outer = it - 2;
              bt->bottom_frame_elided = true;
              bt->partial_unwind = true;
          break;
        }
      }
    } else {
      /* no idle frame. show the whole stack. */
    }
    
    elide_debug_dump("ELIDED INNERMOST FRAMES", *bt_inner, *bt_outer, region_id);
    goto return_label;
#endif
  }


 return_label:
    return;
}


cct_node_t *
ompt_region_root
(
 cct_node_t *_node
)
{
  cct_node_t *root;
  cct_node_t *node = _node;
  while (node) {
    cct_addr_t *addr = hpcrun_cct_addr(node);
    if (IS_UNRESOLVED_ROOT(addr)) {
      root = hpcrun_get_thread_epoch()->csdata.unresolved_root; 
      break;
    } else if (IS_PARTIAL_ROOT(addr)) {
      root = hpcrun_get_thread_epoch()->csdata.partial_unw_root; 
      break;
    }
    node = hpcrun_cct_parent(node);
  }
  if (node == NULL) root = hpcrun_get_thread_epoch()->csdata.tree_root; 
  return root;
}


//-------------------------------------------------------------------------------
// Unlike other compilers, gcc4 generates code to invoke the master
// task from the program itself rather than the runtime, as shown 
// in the sketch below
//
// user_function_f () 
// {
//  ...
//         omp_parallel_start(task, ...)
//  label_1:
//         task(...)
//  label_2: 
//         omp_parallel_end(task, ...)
//  label_3: 
//  ...
// }
//
// As a result, unwinding from within a callback from either parallel_start or 
// parallel_end will return an address marked by label_1 or label_2. unwinding
// on the master thread from within task will have label_2 as a return address 
// on its call stack.
//
// Cope with the lack of an LCA by adjusting unwinds from within callbacks 
// when entering or leaving a task by moving the leaf of the unwind representing 
// the region within user_function_f to label_2. 
//-------------------------------------------------------------------------------
static cct_node_t *
ompt_adjust_calling_context
(
 cct_node_t *node,
 ompt_scope_endpoint_t se_type
)
{
#ifdef ADJUST_PC
  // extract the load module and offset of the leaf CCT node at the 
  // end of a call path representing a parallel region
  cct_addr_t *n = hpcrun_cct_addr(node);
  cct_node_t *n_parent = hpcrun_cct_parent(node); 
  uint16_t lm_id = n->ip_norm.lm_id; 
  uintptr_t lm_ip = n->ip_norm.lm_ip;
  uintptr_t master_outlined_fn_return_addr;

  // adjust the address to point to return address of the call to 
  // the outlined task in the master
  if (se_type == ompt_scope_begin) {
    void *ip = hpcrun_denormalize_ip(&(n->ip_norm));
    uint64_t offset = offset_to_pc_after_next_call(ip);
    master_outlined_fn_return_addr = lm_ip + offset;
  } else { 
    uint64_t offset = length_of_call_instruction();
    master_outlined_fn_return_addr = lm_ip - offset;
  }
  // ensure that there is a leaf CCT node with the proper return address
  // to use as the context. when using the GNU API for OpenMP, it will 
  // be a sibling to one returned by sample_callpath.
  cct_node_t *sibling = hpcrun_cct_insert_addr
    (n_parent, &(ADDR2(lm_id, master_outlined_fn_return_addr)));
  return sibling;
#else
  return node;
#endif
}


cct_node_t *
ompt_region_context_eager
(
  uint64_t region_id, 
  ompt_scope_endpoint_t se_type, 
  int adjust_callsite
)
{
  cct_node_t *node;
  ucontext_t uc;
  getcontext(&uc);

  // levels to skip will be broken if inlining occurs.
  hpcrun_metricVal_t zero = {.i = 0};
  node = hpcrun_sample_callpath(&uc, 0, zero, 0, 1, NULL).sample_node;
  TMSG(DEFER_CTXT, "unwind the callstack for region 0x%lx", region_id);

  TMSG(DEFER_CTXT, "unwind the callstack for region 0x%lx", region_id);
  if (node && adjust_callsite) {
    node = ompt_adjust_calling_context(node, se_type);
  }
  return node;
}


void
ompt_region_context_lazy
(
  uint64_t region_id,
  ompt_scope_endpoint_t se_type, 
  int adjust_callsite
)
{
  ucontext_t uc;
  getcontext(&uc);

  hpcrun_metricVal_t blame_metricVal;
  blame_metricVal.i = 0;

  // side-effect: set region context
  (void) hpcrun_sample_callpath(&uc, 0, blame_metricVal, 0, 33, NULL);

  TMSG(DEFER_CTXT, "unwind the callstack for region 0x%lx", region_id);
}


cct_node_t *
ompt_parallel_begin_context
(
 ompt_id_t region_id, 
 int adjust_callsite
)
{
  cct_node_t *context = NULL;
  if (ompt_eager_context) {
    context = ompt_region_context_eager(region_id, ompt_scope_begin,
                                  adjust_callsite);
  }
  return context;
}

// ====================== vi3: idle_blame_shifting

static inline
void
find_current_thread_state
(
  void
)
{
  // memoize old state
  old_state = current_state;
  // inquire current state
  current_state = check_state();
}

// Check whether the state matches one of the waiting state that
// represents idling in sense of blaming.
static inline
bool
is_state_in_idle_group
(
  ompt_state_t thread_state
)
{
  // TODO vi3: improve this implementation
  switch (thread_state) {
    case ompt_state_wait_barrier:
    case ompt_state_wait_barrier_implicit_parallel:
    case ompt_state_wait_barrier_implicit_workshare:
    case ompt_state_wait_barrier_implicit:
    case ompt_state_wait_barrier_explicit:
    case ompt_state_wait_taskwait:
    case ompt_state_wait_taskgroup:
    case ompt_state_idle:
      return true;
    default:
      return false;
  }
  // I guess this states does not represent idling
  //  ompt_state_wait_target
  //  ompt_state_wait_target_map
  //  ompt_state_wait_target_update
  //  ompt_state_wait_mutex
  //  ompt_state_wait_lock
  //  ompt_state_wait_critical
  //  ompt_state_wait_atomic
  //  ompt_state_wait_ordered
}

static
void
idle_blame_shift
(
  void
)
{
  // check whether the thread was idling previously
  bool old_idle_group = is_state_in_idle_group(old_state);

  // check whether thread is idling at the moment
  bool current_idle_group = is_state_in_idle_group(current_state);

  // Check whether the state has been changed since the previous sample.
  // This is done in the sense of idling, which means that we care if thread
  // change the group of states that represents idling.
  // See is_thread_state_blame_idling_state for more.
  if (!old_idle_group && current_idle_group) {
    // Thread was working and now is idling, which means that idle began
    // since the last sample.
    ompt_idle_begin();
  } else if (old_idle_group && !current_idle_group) {
    // Thread was idling and now is working, which means that idle ended since
    // the last sample.
    ompt_idle_end();
  }
}

// ======================

// ====================== resolve idleness if waiting
void
resolve_if_waiting
(
    void
)
{
  if (is_state_in_idle_group(current_state)) {
    ompt_resolve_region_contexts_poll();
  }
}
// ======================


static void
ompt_backtrace_finalize
(
 backtrace_info_t *bt, 
 int isSync
) 
{
  find_current_thread_state();

  uint64_t region_id = TD_GET(region_id);

  ompt_elide_runtime_frame(bt, region_id, isSync);

  if (!ompt_eager_context_p()
      && !ending_region && task_ancestor_level >= 0) {
    // Thread is not executing ompt_callback_parallel_end.
    // Elider detect that task frames are present on thread stack.
    lazy_active_region_processing();
  }

  // FIXME vi3 >>> I think it is ok that thread tries to register itself
  //  for regions' call paths.
  //  Note: see  register_to_all_regions and ompt_cct_cursor_finalize functions
  //    for more information.
  // FIXME vi3: check synchronous samples

  if (ompt_idle_blame_shift_enabled()) {
    // check whether thread change the group state
    idle_blame_shift();
  }

  if (!ompt_eager_context_p() && !ending_region) {
    // Thread is not executing the ompt_callback_parallel_end.
    // If thread is in wait state, then try to resolved
    // remained unresolved regions.
    resolve_if_waiting();
  }

}



//******************************************************************************
// interface operations
//******************************************************************************


// For debugging purposes
cct_node_t *
check_and_return_non_null
(
  cct_node_t *to_return,
  cct_node_t *default_value,
  int line_of_code
)
{
  if (!default_value) {
    assert(0);
  }

  if (!to_return) {
    printf("***************** Why this happens: ompt-callstack.c:%d, state: %x\n", line_of_code, current_state);
    return default_value;
  }

  return to_return;
}


static inline bool
expl_task_full_ctx
(
  int flags
)
{
  // Does thread need to provide full creation context of an explicit task?
  return ompt_task_full_context_p() && (flags & ompt_task_explicit);
  // This is also true for untied tasks too, since they're explicit task.
}


cct_node_t *
ompt_cct_cursor_finalize
(
 cct_bundle_t *cct, 
 backtrace_info_t *bt, 
 cct_node_t *cct_cursor
)
{

  // Put region path under the unresolved root
  if (!ompt_eager_context_p() && ending_region) {
    // At the end of the region, master of the team is responsible for providing region's call path.
    // 1. It is possible that master didn't take a sample inside the region.
    // In that case, it has more sense to put call path underneath the unresolved root,
    // instead of putting somewhere in cct of the thread root, since the provided partial call path is
    // just a garbage for the master thread.
    // 2. The other, more important reason to use unresolved_root,
    // is that the master need to provide PARTIAL call path for the region.
    // If the call path is put underneath the pseudo cct node which is descendent of the thread root,
    // then the call path may not be partial. Some ancestor of the pseudo cct node may be other pseudo
    // nodes, and some of them may become later resolved.
    // Those ancestor cct nodes (resolved or unresolved) must not be
    // part of region's call path, because they will be used by other worker threads (of the same team)
    // in resolving process.
    return check_and_return_non_null(cct->unresolved_root, cct_cursor, 877);
  }

  if (task_ancestor_level < 0) {
    // Elider found out that task_frames are not present on thread's stack.
    return check_and_return_non_null(cct_cursor, cct_cursor, 2104);;
  }

  ompt_data_t *parallel_data, *task_data;
  int flags;
  int ret = hpcrun_ompt_get_task_info(task_ancestor_level, &flags, &task_data,
                                      NULL, &parallel_data, NULL);
  // Since task_ancestor_level is determined by ompt_elide_runtime_frame only
  // if the task frames of the task at this level are present on thread's stack,
  // then the valid information about the task are guaranteed to exist.
  assert(ret == 2);

  if (ompt_eager_context_p()) {
    // check if prefix is memoized
    cct_node_t *prefix = (cct_node_t *) task_data->ptr;
    if (!prefix || (flags & ompt_task_untied)
        || expl_task_full_ctx(flags)) {
      // Consider following cases.
      // case 1: If the region creation context hasn't been memoized, insert it
      // to thread's CCT now and memoize it.
      // case 2: If task is untied, it is possible that it changes execution
      // thread. In that case, memoized context may not belong to the thread's
      // CCT.
      // case 3: If the user is interested in full task creation context, then
      // the thread that creates a tasks provides the creation context and
      // stores it inside task_data. If other thread executes the task, then it
      // needs to insert task creation context in its own cct (If the sample is
      // taken inside an explicit task of course).

      // Since there's no mechanism to detect whether cct_node_t is created by
      // this thread and is placed inside thread's CCT, thread should always
      // try to insert the region creation context, even though it may be
      // already inserted. This should be somehow optimized.

      // load context
      // If the full task creation context is available, use it.
      // Otherwise, use region creation context as task calling context.
      cct_node_t *ctx = expl_task_full_ctx(flags)
          ? prefix : (cct_node_t *) parallel_data->ptr;
      // access to the CCT root
      cct_node_t *root = hpcrun_get_thread_epoch()->csdata.tree_root;
      // Insert task context to thread's CCT and use inserted
      // call path as sample prefix.
      prefix = hpcrun_cct_insert_path_return_leaf(root, ctx);
      // Memoize prefix in order to avoid calling previous function each time
      // sample is taken inside this region.
      task_data->ptr = prefix;
    }
    // TODO: Try to memoize this after insertion
    return check_and_return_non_null(prefix, cct_cursor, 2116);
  } else {
    // load region_data
    typed_stack_elem(region) *region_data = ATOMIC_LOAD_RD(parallel_data);
    assert(region_data);
    int region_depth = region_data->depth;
    // Use region depth and find corresponding unresolved_cct
    return check_and_return_non_null(typed_random_access_stack_get(region)(
        region_stack, region_depth)->unresolved_cct, cct_cursor, 2120);
  }

}


void
ompt_callstack_init_deferred
(
 void
)
{
#if ALLOW_DEFERRED_CONTEXT
  if (hpcrun_trace_isactive()) ompt_eager_context = 1;
  else {
    // set up a finalizer to propagate information about
    // openmp region contexts that have not been fully 
    // resolved across all threads.
    ompt_thread_finalizer.next = 0;
    ompt_thread_finalizer.fn = ompt_resolve_region_contexts;
    thread_finalize_register(&ompt_thread_finalizer);
  }
#else
  ompt_eager_context = 1;
#endif
}


void
ompt_callstack_init
(
 void
)
{
  ompt_finalizer.next = 0;
  ompt_finalizer.fn = ompt_backtrace_finalize;
  cct_backtrace_finalize_register(&ompt_finalizer);
  cct_cursor_finalize_register(ompt_cct_cursor_finalize);

  // initialize closure for initializer
  ompt_callstack_init_closure.fn = 
    (closure_fn_t) ompt_callstack_init_deferred; 
  ompt_callstack_init_closure.arg = 0;

  // register closure
  hpcrun_initializers_defer(&ompt_callstack_init_closure);
}


int
ompt_eager_context_p
(
 void
)
{
  return ompt_eager_context;
}

