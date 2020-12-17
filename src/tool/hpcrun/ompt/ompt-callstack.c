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

__thread int nested_regions_before_explicit_task = 0;

static void
ompt_elide_runtime_frame(
  backtrace_info_t *bt, 
  uint64_t region_id, 
  int isSync
)
{
  // invalidate omp_task_context of previous sample
  // FIXME vi3 >>> It should be ok to do this.
  TD_GET(omp_task_context) = 0;

  vi3_idle_collapsed = false;
  nested_regions_before_explicit_task = 0;
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
  switch(check_state()) {
    case ompt_state_wait_barrier:
    case ompt_state_wait_barrier_implicit: // mark it idle
      // previous two states should be deprecated
    case ompt_state_wait_barrier_implicit_parallel:
      TD_GET(omp_task_context) = 0;
      if (hpcrun_ompt_get_thread_num(0) != 0) {
        collapse_callstack(bt, &ompt_placeholders.ompt_idle_state);
        goto return_label;
      }
      break;
    case ompt_state_wait_barrier_explicit: // attribute them to the corresponding
      // We are inside implicit or explicit task.
      // If we are collecting regions' callpaths synchronously, then we
      // are sure that hpcrun_ompt_get_task_data(0) exists.
      // If we are asynchronously resolving call paths, then
      // TD_GET(omp_task_context) will be NULL. We will handle this
      // in ompt_cct_cursor_finalize.
#if 0
      // Old code
      TD_GET(omp_task_context) = hpcrun_ompt_get_task_data(0)->ptr;
      // collapse barriers on non-master ranks
      if (hpcrun_ompt_get_thread_num(0) != 0) {
	      collapse_callstack(bt, &ompt_placeholders.ompt_barrier_wait_state);
	      goto return_label;
      }
      break;
#endif
      on_explicit_barrier = 1;
      break;
    case ompt_state_idle:
      // collapse idle state
      TD_GET(omp_task_context) = 0;
      collapse_callstack(bt, &ompt_placeholders.ompt_idle_state);
      goto return_label;
    default:
      break;
  }

  int i = 0;
  frame_t *it = NULL;

  ompt_frame_t *frame0 = hpcrun_ompt_get_task_frame(i);

  TD_GET(omp_task_context) = 0;

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
#if USE_IMPLICIT_TASK_CALLBACKS == 1
    // If hpcrun is using ompt_callback_implicit_task, then check
    // only if task_data of explicit task is uninitialized.
    if (flags0 & ompt_task_explicit) {
#endif
      // check if task_data is not initialized
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
      if (task_data && task_data->ptr == NULL
          && parallel_data && parallel_data->ptr) {
#else
      if (task_data && task_data->ptr == NULL
          && parallel_data && ATOMIC_LOAD_RD(parallel_data)) {
#endif
        // try to initialize task_data if can
#if USE_OMPT_CALLBACK_PARALLEL_BEGIN == 1
        typed_stack_elem(region) *region_data = parallel_data->ptr;
#else
        typed_stack_elem(region) *region_data = ATOMIC_LOAD_RD(parallel_data);
#endif
        if (ompt_eager_context_p()) {
          task_data_set_cct(task_data, region_data->call_path);
        } else {
          task_data_set_depth(task_data, region_data->depth);
        }
      }
#if USE_IMPLICIT_TASK_CALLBACKS == 1
    }
#endif

    cct_node_t *omp_task_context = NULL;
    if (task_data)
      omp_task_context = task_data->ptr;
    
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

    if (exit0_flag && omp_task_context) {
      TD_GET(omp_task_context) = omp_task_context;
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

#if 0
    ompt_frame_t *help_frame = region_stack[top_index-i+1].parent_frame;
    if (!ompt_eager_context && !reenter1_flag && help_frame) {
      frame1 = help_frame;
      reenter1_flag = interval_contains(low_sp, high_sp, fp_enter(frame1));
      // printf("THIS ONLY HAPPENS IN MASTER: %d\n", TD_GET(master));
    }
#endif

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

      nested_regions_before_explicit_task++;

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
      TD_GET(omp_task_context) = 0;
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


static void
ompt_backtrace_finalize
(
 backtrace_info_t *bt, 
 int isSync
) 
{
  // ompt: elide runtime frames
  // if that is the case, then it will later become available in a deferred fashion.
  int master = TD_GET(master);
  if (!master) {
    if (need_defer_cntxt()) {
//      resolve_cntxt();
    }
    if (!ompt_eager_context)
      resolve_cntxt();
  }
  uint64_t region_id = TD_GET(region_id);

  ompt_elide_runtime_frame(bt, region_id, isSync);
#if 0
  // Old code
  // If backtrace is collapsed and if worker thread in team is waiting
  // on explicit barrier, it (worker threads) also needs to register
  // itself for the call path of all active regions.
  bool wait_on_exp_barr = check_state() == ompt_state_wait_barrier_explicit;
  if(!isSync && !ompt_eager_context_p()
             && (!bt->collapsed || wait_on_exp_barr)){
    register_to_all_regions();
  }
#endif

  // FIXME vi3 >>> I think it is ok that thread tries to register itself
  //  for regions' call paths.
  //  Note: see  register_to_all_regions and ompt_cct_cursor_finalize functions
  //    for more information.
  if(!isSync && !ompt_eager_context_p()) {
    register_to_all_regions();
  }
}



//******************************************************************************
// interface operations
//******************************************************************************

#if 0
void
add_unresolved_cct_to_parent_region_if_needed
(
  region_stack_el_t *stack_element
)
{
  // unresolved_cct has been already created
  // either in this explicit task or while registering
  // for the region where thread is not the master.
  if (stack_element->notification->unresolved_cct)
    return;
  // going from the thread root and insert UNRESOLVED_CCT nodes
  cct_node_t *parent_cct = hpcrun_get_thread_epoch()->csdata.thread_root;
  region_stack_el_t *current;
  for(current = (region_stack_el_t *) &region_stack; current <= stack_element; current++) {
    // vi3>> mozda optimizacija da krenemo od onog gde nismo master

    // already created UNRESOLVED_CCT for this region
    // when thread has been registered for the region
    if (current->notification->unresolved_cct) {
      parent_cct = current->notification->unresolved_cct;
      continue;
    }

    current->notification->unresolved_cct =
      hpcrun_cct_insert_addr(parent_cct,
        &ADDR2(UNRESOLVED, current->notification->region_data->region_id));
    parent_cct = current->notification->unresolved_cct;
  }

}
#endif

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
    printf("***************** Why this happens: ompt-callstack.c:%d\n", line_of_code);
    return default_value;
  }

  return to_return;
}


// For debugging purposes
void
vi3_unexpected
(
  void
)
{

}

#if 0
// For debugging purposes
void
check_shallower_stack
(
  typed_stack_elem(region) *top_reg,
  typed_stack_elem(region) *inner
)
{
  // master took a sample immediately after regions has been finished, but before parallel end
  // suppose it is safe to mark that region is still active and to mark that thread took sample here
  // ASSERT:
  // 1) inner is on the stack at the propper posiotion
  // 2) thread is the master of the top_reg

  if (!top_reg) {
    printf("shallower_stack >>> TOP_REG is missing\n");
    vi3_unexpected();
    return;
  }

  if (!inner) {
    printf("shallower_stack >>> INNER is missing\n");
    vi3_unexpected();
    return;
  }

  int top_depth = top_reg->depth;
  int inner_depth = inner->depth;

  if (top_depth <= inner_depth) {
    vi3_unexpected();
    printf("shallower_stack >>> This is bad\n");
  }

  typed_random_access_stack_elem(region) *top_el =
      typed_random_access_stack_get(region)(region_stack, top_depth);

  if (!top_el->team_master) {
    vi3_unexpected();
    printf("shallower_stack >>> Thread is not master in top_reg\n");
  }

  typed_random_access_stack_elem(region) *inner_el =
      typed_random_access_stack_get(region)(region_stack, inner_depth);

  if (inner_el->notification->region_data != inner) {
    vi3_unexpected();
    printf("shallower_stack >>> Inner is not on the stack\n");
  }

  // check barrier_cnt for top_reg and inner
  int old_count_top = atomic_fetch_add(&top_reg->barrier_cnt, 0);
  if (old_count_top < 0) {
    vi3_unexpected();
    printf("shallower_stack >>> TOP_REG marked as finished\n");
  }

  int old_count_inner = atomic_fetch_add(&inner->barrier_cnt, 0);
  if (old_count_inner < 0) {
    vi3_unexpected();
    printf("shallower_stack >>> INNER marked as finished\n");
  }
}


// For debugging purposes
void
check_same_depth_stack
(
  typed_stack_elem(region) *top_reg,
  typed_stack_elem(region) *inner
)
{
  // Worker thread took sample immediately after region has been finished.
  // Most likely inside implicit_task_end or at the end of the last implicit barrier.
  if (!top_reg) {
    printf("same_depth_stack >>> TOP_REG is missing\n");
    vi3_unexpected();
    return;
  }

  if (!inner) {
    printf("same_depth_stack >>> INNER is missing\n");
    vi3_unexpected();
    return;
  }

  int top_depth = top_reg->depth;
  int inner_depth = inner->depth;

  if (top_depth != inner_depth) {
    vi3_unexpected();
    printf("same_depth_stack >>> This is bad\n");
  }

  typed_random_access_stack_elem(region) *top_el =
      typed_random_access_stack_get(region)(region_stack, top_depth);

  if (top_el->team_master) {
    vi3_unexpected();
    printf("same_depth_stack >>> Thread is master\n");
  }

  // check barrier_cnt for top_reg and inner
  int old_count_top = atomic_fetch_add(&top_reg->barrier_cnt, 0);
  if (old_count_top >= 0) {
    vi3_unexpected();
    printf("same_depth_stack >>> TOP_REG still active\n");
  }

  int old_count_inner = atomic_fetch_add(&inner->barrier_cnt, 0);
  if (old_count_inner < 0) {
    vi3_unexpected();
    printf("same_depth_stack >>> INNER marked as finished\n");
  }
}


// For debugging purposes
void
check_deeper_stack
(
  typed_stack_elem(region) *top_reg,
  typed_stack_elem(region) *inner
)
{
  // Worker thread took sample immediately after region has been finished.
  // Most likely inside implicit_task_end or at the end of the last implicit barrier.
  if (!top_reg) {
    vi3_unexpected();
    printf("deeper_stack >>> TOP_REG is missing\n");
    return;
  }

  if (!inner) {
    vi3_unexpected();
    printf("deeper_stack >>> INNER is missing\n");
    return;
  }

  int top_depth = top_reg->depth;
  int inner_depth = inner->depth;

  if (top_depth >= inner_depth) {
    vi3_unexpected();
    printf("deeper_stack >>> This is bad\n");
  }

#if 0
  typed_random_access_stack_elem(region) *top_el =
      typed_random_access_stack_get(region)(region_stack, top_depth);

  if (top_el->team_master) {
    printf("deeper_stack >>> Thread is master\n");
  }
#endif

  int inner_thread_num = hpcrun_ompt_get_thread_num(0);
  if (inner_thread_num != 0) {
    // happen
    // Thread can be worker
    //vi3_unexpected();
    //printf("deeper_stack >>> Thread is worker\n");
  }

  // check barrier_cnt for top_reg and inner
  int old_count_top = atomic_fetch_add(&top_reg->barrier_cnt, 0);
  if (old_count_top < 0) {
    vi3_unexpected();
    printf("deeper_stack >>> TOP_REG marked as finished\n");
  }

  int old_count_inner = atomic_fetch_add(&inner->barrier_cnt, 0);
  if (old_count_inner < 0) {
    vi3_unexpected();
    printf("deeper_stack >>> INNER marked as finished\n");
  }
}


// For debugging purposes
void
check_inner_unavailable
(
  typed_stack_elem(region) *top_reg,
  typed_stack_elem(region) *inner
)
{
  // Worker thread is waiting on last implicit barrier.
  // It is not the member of any team.
  if (!top_reg) {
    // didn't happen
    vi3_unexpected();
    printf("inner_unavailable >>> TOP_REG is missing\n");
    return;
  }

  if (inner) {
    //vi3_unexpected();
    // happened
    // This can happen. In the middle of processing sample, parallel data becomes available
    //printf("inner_unavailable >>> INNER is available\n");

    if (!waiting_on_last_implicit_barrier) {
      // didn't happen
      vi3_unexpected();
      printf("inner_unavailable >>> INNER available >>> thread not on barrier\n");
    }

    int old_count_top = atomic_fetch_add(&top_reg->barrier_cnt, 0);
    if (old_count_top >= 0) {
      // didn't happen
      vi3_unexpected();
      printf("inner_unavailable >>> INNER available >>> TOP_REG active\n");
    }
  }

  if (!waiting_on_last_implicit_barrier) {
    // didn't happen
    vi3_unexpected();
    printf("inner_unavailable >>> Thread is not waiting on last implicit barrier\n");
  }

  typed_random_access_stack_elem(region) *top_el =
      typed_random_access_stack_top(region)(region_stack);

  if (top_el->team_master) {
    // didn't happen
    vi3_unexpected();
    printf("inner_unavailable >>> Thread is master\n");
  }

  // check barrier_cnt for top_reg
  int old_count_top = atomic_fetch_add(&top_reg->barrier_cnt, 0);
  if (old_count_top >= 0) {
    // happened
    //vi3_unexpected();
    //printf("inner_unavailable >>> TOP_REG still active\n");

    // In the middle of sample processing, master marked that top_reg is finished,
    // while inner and outer became available.
    if (!waiting_on_last_implicit_barrier) {
      // didn't happen
      vi3_unexpected();
      printf("inner_unavailable >>> TOP_REG active >>> thread not on barrier\n");
    }

    typed_stack_elem(region) *inner = hpcrun_ompt_get_region_data(0);
    typed_stack_elem(region) *outer = hpcrun_ompt_get_region_data(1);
    if (inner) {
      // happened
      // here, top_reg is marked as finished
      //old_count_top = atomic_fetch_add(&top_reg->barrier_cnt, 0);
      //printf("inner_unavailable >>> TOP_REG active >>> inner is available: %d\n", old_count_top);
    }

    if (outer) {
      // happened
      // here, top_reg is marked as finished
      //old_count_top = atomic_fetch_add(&top_reg->barrier_cnt, 0);
      //printf("inner_unavailable >>> TOP_REG active >>> outer is available: %d\n", old_count_top);
    }
  }
}

typedef void (*handle_exception_fn)(void);

void
thread_should_create_new_region
(
  void
)
{
  // what happens with parallel_data and stack
  typed_random_access_stack_elem(region) *top =
      typed_random_access_stack_top(region)(region_stack);
  typed_stack_elem(region) *inner =
      hpcrun_ompt_get_region_data(0);
  typed_stack_elem(region) *top_reg = top->notification->region_data;

  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context), &omp_task_context, &region_depth);

#if 0
  if (!(top_reg->depth != region_depth || top_reg->depth >= inner->depth || info_type != 1)) {
    printf("As expected :)\n");
  }
#endif

  if (top_reg->depth != region_depth || top_reg->depth >= inner->depth || info_type != 1) {
    printf(":( :( :( Thread should be creating new region, "
           "so thread_data should contain top_reg->depth, "
           "and top_reg->depth < inner->depth\n");
  }

}


void
check_top_reg_and_inner
(
  char *called_from,
  handle_exception_fn fn
)
{
  // what happens with parallel_data and stack
  typed_random_access_stack_elem(region) *top =
      typed_random_access_stack_top(region)(region_stack);
  typed_stack_elem(region) *inner =
      hpcrun_ompt_get_region_data(0);
  typed_stack_elem(region) *top_reg = NULL;

  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context), &omp_task_context, &region_depth);


  if (top && top->notification) {
    top_reg = top->notification->region_data;
    if (top_reg != inner) {
      printf("<<< %s >>> Top region is not equal to innermost region, info_type: %d\n",
             called_from, info_type);
      // further processing
      if (fn) fn();
    }
  } else {
    printf("<<< %s >>> Top region is missing\n", called_from);
  }
}

void
regions_should_be_actvie
(
  void
)
{
  // what happens with parallel_data and stack
  typed_random_access_stack_elem(region) *top =
      typed_random_access_stack_top(region)(region_stack);
  typed_stack_elem(region) *inner =
      hpcrun_ompt_get_region_data(0);
  typed_stack_elem(region) *top_reg = NULL;

  int old_top_reg = 3333;
  int old_inner = 3333;

  if (top && top->notification) {
    top_reg = top->notification->region_data;
    old_top_reg = atomic_fetch_add(&top_reg->barrier_cnt, 0);
  }

  if (inner) {
    old_inner = atomic_fetch_add(&inner->barrier_cnt, 0);
  }

  if (old_top_reg != 0) {
    printf("regions_should_be_active >>> top_reg is not active: %d\n", old_top_reg);
  }

  if (old_inner != 0) {
    printf("regions_should_be_active >>> inner is not active: %d\n", old_inner);
  }

  check_top_reg_and_inner("ompt-callstack.c:1224 (regions_should_be_active)", NULL);

}


void
check_what_thread_data_contains
(
  char *called_from,
  handle_exception_fn contains_region_depth_fn,
  handle_exception_fn contains_null_fn
)
{
  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context), &omp_task_context, &region_depth);
  if (info_type == 1) {
    if (contains_region_depth_fn)
      contains_region_depth_fn();
  } else if (info_type == 2){
    if (contains_null_fn)
      contains_null_fn();
  } else {
    printf("This should not happen: info_type: %d, region_depth: %d, omp_task_context: %p\n",
        info_type, region_depth, omp_task_context);
  }

}


void
are_top_reg_and_inner_actve
(
  void
)
{
  // what happens with parallel_data and stack
  typed_random_access_stack_elem(region) *top =
      typed_random_access_stack_top(region)(region_stack);
  typed_stack_elem(region) *inner =
      hpcrun_ompt_get_region_data(0);
  typed_stack_elem(region) *top_reg = NULL;

  if (top && top->notification) {
    top_reg = top->notification->region_data;
    if (top_reg) {
      int old_top_reg = atomic_fetch_add(&top_reg->barrier_cnt, 0);
      if (old_top_reg < 0) {
        printf("TOP REGION IS NOT ACTIVE: %d\n", old_top_reg);
      } else {
//        printf("t: %d\n", old_top_reg);
      }
    } else {
      printf("TOP_REG MISSING: ompt-defer.c:820\n");
    }
  } else {
    printf("TOP_REG MISSING: ompt-defer.c:823\n");
  }

  if (inner) {
    int old_inner = atomic_fetch_add(&top_reg->barrier_cnt, 0);
    if (old_inner < 0) {
      printf("INNER IS NOT ACTIVE: %d\n", old_inner);
    } else {
//      printf("i: %d\n", old_inner);
    }
  } else {
    printf("INNER MISSING: ompt-defer.c:832\n");
  }
}

void
waiting_on_barrier_thread_data_contains_sth
(
  void
)
{
  typed_random_access_stack_elem(region) *top =
      typed_random_access_stack_top(region)(region_stack);
  typed_stack_elem(region) *inner =
      hpcrun_ompt_get_region_data(0);
  typed_stack_elem(region) *top_reg = NULL;

  int old_top_reg = 3333;
  int old_inner = 3333;

  if (top && top->notification) {
    top_reg = top->notification->region_data;
    old_top_reg = atomic_fetch_add(&top_reg->barrier_cnt, 0);
  }

  if (inner) {
    old_inner = atomic_fetch_add(&inner->barrier_cnt, 0);
  }

  if (old_top_reg != 0) {
    printf("regions_should_be_active >>> top_reg is not active: %d\n", old_top_reg);
  }

  if (old_inner != 0) {
    printf("regions_should_be_active >>> inner is not active: %d\n", old_inner);
  }

  // =======================

  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context), &omp_task_context, &region_depth);

  typed_random_access_stack_elem(region) *el =
      typed_random_access_stack_get(region)(region_stack, region_depth);
  typed_stack_elem(region) *reg = NULL;
  if (el && el->notification) {
    reg = el->notification->region_data;
  }

  if (!reg) {
    printf("ompt-callstack.c:1349 >>> Something is wrong");
    return;
  }

  int old = atomic_fetch_add(&reg->barrier_cnt, 0);
  if (old != 0) {
    printf("ompt-callstack.c:1355 >>> Region at depth: %d is not active: %d\n", region_depth, old);
  }

  if (vi3_idle_collapsed) {
    printf("ompt-callstack.c:1359 >>> Thread should not being idling: %d\n", vi3_idle_collapsed);
  }


  if (top_reg->depth > region_depth) {
    //printf("greater: tr: %d, r:%d\n", top_reg->depth, region_depth);
    if (top_reg != inner) {
      //printf("this can happen too: tr: %d, r:%d, i:%d\n", top_reg->depth, region_depth, inner->depth);
      if (reg != inner) {
        printf("Problem\n");
      }
    }
  } else if (top_reg->depth == region_depth) {
    //printf("equal: tr: %d, r:%d\n", top_reg->depth, region_depth);
    if (top_reg != inner) {
      //printf("this can happen too: tr: %d, r:%d, i:%d\n", top_reg->depth, region_depth, inner->depth);
      if (inner->depth <= top_reg->depth) {
        printf("Cannot happen\n");
      }
      if (reg != top_reg) {
        printf("Problem\n");
      }
    } else {
      //printf("regular\n");
    }
  } else {
    printf("less: tr: %d, r:%d\n", top_reg->depth, region_depth);
  }


}


void
waiting_on_barrier_thread_data_contains_null
(
  void
)
{
  typed_random_access_stack_elem(region) *top =
      typed_random_access_stack_top(region)(region_stack);
  typed_stack_elem(region) *inner =
      hpcrun_ompt_get_region_data(0);
  typed_stack_elem(region) *top_reg = NULL;

  int old_top_reg = 3333;
  int old_inner = 3333;

  if (top && top->notification) {
    top_reg = top->notification->region_data;
    old_top_reg = atomic_fetch_add(&top_reg->barrier_cnt, 0);
  }

  if (inner) {
    old_inner = atomic_fetch_add(&inner->barrier_cnt, 0);
  }

  if (old_top_reg != 0) {
    //printf("regions_should_be_active >>> top_reg is not active: %d\n", old_top_reg);
  } else {
    //printf("top_reg active\n");
  }

  if (old_inner != 0) {
    //printf("regions_should_be_active >>> inner is not active: %d\n", old_inner);
  } else {
    //printf("inner active\n");
  }

  ompt_state_t state = check_state();

  if (!vi3_idle_collapsed) {
    // statse = 0x1, 0x13, 0x101
    //printf("Not idling: %d, state: %x\n", vi3_idle_collapsed, state);
  }

  if (old_top_reg == 0 && old_inner == 0) {
    if (top_reg != inner) {
      //printf("top_reg != inner. state: %x\n", state);
      if (!vi3_idle_collapsed) {
        printf("How to handle this??? %d\n", vi3_idle_collapsed);
      }
    }
    //printf("both active, idling: %d\n", vi3_idle_collapsed);
  }

  if (top_reg == inner) {
    //printf("equal: %d, %d\n", old_top_reg, old_inner);
  }


}


void
plug_and_play
(
  void
)
{
  if (!waiting_on_last_implicit_barrier) {
    // If top_reg != inner => thread is creating new region
    //    thread_data contains top_reg->depth
    //    top_reg->depth < inner->depth
    //check_top_reg_and_inner("ompt-defer.c:727", thread_should_create_new_region);

    // thread_data contains NULL
    // a) immediately after implicit_task_end
    // b) just before last implicit barrier
    check_what_thread_data_contains("ompt-defer.c:791", NULL, regions_should_be_actvie);

    // check if regions are active (they should be)
    //are_top_reg_and_inner_actve();
  } else {
    //printf("Waiting\n");
    check_what_thread_data_contains("ompt-callstac.c:1323",
        waiting_on_barrier_thread_data_contains_sth,
        waiting_on_barrier_thread_data_contains_null);
  }
}
#endif

cct_node_t *
handle_idle_sample
(
  cct_node_t *cct_cursor
)
{
#if DETECT_IDLENESS_LAST_BARRIER
  if (typed_random_access_stack_empty(region)(region_stack)) {
    // No regions are present on the stack
    // It can be assumed that thread is executing the sequential code.
    return cct_cursor;
  }

  if (!local_idle_placeholder) {
    // create placeholder if needed
    local_idle_placeholder = hpcrun_cct_top_new(UNRESOLVED, 0);
  }
  // return local idle placeholder
  return local_idle_placeholder;
#else
  return cct_cursor;
#endif
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

  cct_node_t *omp_task_context = NULL;
  int region_depth = -1;
  int info_type = task_data_value_get_info((void*)TD_GET(omp_task_context), &omp_task_context, &region_depth);

#if 0
  // NOTE vi3: code that contains a lot of useful debug information about edge cases

  plug_and_play();

  if (!ompt_eager_context_p()) {

    typed_stack_elem(region) *inner = hpcrun_ompt_get_region_data(0);
    typed_random_access_stack_elem(region) *top = typed_random_access_stack_top(region)(region_stack);
    typed_stack_elem(region) *top_reg = NULL;
    if (top && top->notification ) {
      top_reg = top->notification->region_data;
    }

    if (vi3_idle_collapsed) {
      // the whole callstack has been collapsed to idle
      vi3_forced_null = false;
      vi3_forced_diff = false;
      return check_and_return_non_null(cct_cursor, cct_cursor, 859);
    }

    if (vi3_forced_null) {
      // parallel_data is unavailable
      // callstack should be collapsed to idle (previous if)
      // should net ended here
      vi3_unexpected();
      printf("ompt_cct_cursor_finalize >>> Callstack should be collapsed to idle\n");
      vi3_forced_null = false;
      if (!vi3_idle_collapsed) {
        vi3_unexpected();
        printf("ompt_cct_cursor_finalize >>> This may represent a problem\n");
      }
      check_inner_unavailable(top_reg, inner);
      return check_and_return_non_null(cct_cursor, cct_cursor, 859);
    }

    if (vi3_forced_diff) {
      vi3_forced_diff = false;

      if (info_type == 2) {
        // If info_type is 2, then task_data doesn't contain any info about corresponding region.
        // Put everything under thread_root (cct_cursor).
        return check_and_return_non_null(cct_cursor, cct_cursor, 859);
      }

      if (top_reg) {
        if (top_reg->depth < inner->depth) {
          // NOTE: See register_to_all_regions function
          check_deeper_stack(top_reg, inner);
          return check_and_return_non_null(typed_random_access_stack_get(region)
                 (region_stack, vi3_last_to_register)->notification->unresolved_cct, cct_cursor, 1200);
        } else if (top_reg->depth == inner->depth) {
          // This should never happen
          vi3_unexpected();
          // This is for debug purposes
          printf("ompt_cct_cursor_finalize >>> What to do with same_depth stack?\n");
          check_same_depth_stack(top_reg, inner);
          return check_and_return_non_null(cct->partial_unw_root, cct_cursor, 859);
        } else {
          // NOTE: See register_to_all_regions function
          // This is for debug purposes
          check_shallower_stack(top_reg, inner);
          return check_and_return_non_null(typed_random_access_stack_get(region)
                 (region_stack, vi3_last_to_register)->notification->unresolved_cct, cct_cursor, 1200);
        }
      }
    }
  }
#endif

  // FIXME: should memoize the resulting task context in a thread-local variable
  //        I think we can just return omp_task_context here. it is already
  //        relative to one root or another.
  if (info_type == 0) {
    cct_node_t *root;
#if 1
    root = ompt_region_root(omp_task_context);
#else
    if ((is_partial_resolve((cct_node_tt *)omp_task_context) > 0)) {
      root = hpcrun_get_thread_epoch()->csdata.unresolved_root;
    } else {
      root = hpcrun_get_thread_epoch()->csdata.tree_root;
    }
#endif
    return check_and_return_non_null(
        hpcrun_cct_insert_path_return_leaf(root, omp_task_context),
        cct_cursor, 919);
  } else if (info_type == 1) {
#if 0
    if (waiting_on_last_implicit_barrier) {
      ompt_state_t state = check_state();
      //printf("State is: %x\n", state);
      typed_random_access_stack_elem(region) *top = typed_random_access_stack_top(region)(region_stack);
      typed_stack_elem(region) *top_reg = NULL;
      typed_stack_elem(region) *inner = hpcrun_ompt_get_region_data(0);
      if (top && top->notification) {
        top_reg = top->notification->region_data;
        if (top_reg != inner) {

          if (top_reg->depth > inner->depth) {
            // region_depth == inner->depth && top->master (condition should be true)
            if (inner->depth != region_depth || !top->team_master) {
              printf("<ompt-callstack.c:1282> Unexpected.\n");
            }

            // NOTE vi3: master of top_reg is finishing top_reg

            // case 1:
            // __kmp_get_global_thread_id
            // __ompt_get_task_info_object
            // __kmp_join_call
            // __kmp_api_GOMP_parallel_40_alias
            // ...

            // case 2:
            // __kmp_release_ticket_lock
            // __kmp_join_call
            // __kmp_api_GOMP_parallel_40_alias
            // ...

            // case 3:
            // pthread_getspecific
            // hpcrun_get_thread_specific
            // hpcrun_safe_enter
            // ompt_parallel_end
            // __kmp_join_call
            // __kmp_api_GOMP_parallel_40_alias
            // ...

            // case 4:
            // __kmp_join_call
            // __kmp_api_GOMP_parallel_40_alias
            // ...


          } else if (top_reg->depth < inner->depth) {
            // top_reg->depth == region_depth

            if (top_reg->depth != region_depth) {
              printf("<ompt-callstack.c:1317> Unexpected");
            }

            // NOTE vi3: thread is creating new region

            if (top->team_master) {
              // NOTE vi3: master thread is creating new parallel region

              // case 1:
              // __memset_sse2
              // __kmp_allocate_thread
              // __kmp_fork_call
              // __kmp_GOMP_fork_call
              // __kmp_api_GOMP_parallel_40_alias
              // ...

              // case 2:
              // __kmp_release_ticket_lock
              // __kmp_fork_call
              // __kmp_GOMP_fork_call
              // __kmp_api_GOMP_parallel_40_alias
              // ...

              // case 3:
              // __kmp_get_global_thread_id
              // __ompt_get_team_info
              // __kmp_GOMP_fork_call
              // __kmp_api_GOMP_parallel_40_alias
              // ...


            } else {

              // NOTE vi3: worker thread is creating new parallel region

              // case 1:
              // __memset_sse2
              // __kmp_allocate_thread
              // __kmp_fork_call
              // __kmp_GOMP_fork_call
              // __kmp_api_GOMP_parallel_40_alias
              // ...

              // case 2:
              // pthread_mutex_lock
              // __kmp_lock_suspend_mx
              // __kmp_allocate_thread
              // __kmp_fork_call
              // __kmp_GOMP_fork_call
              // __kmp_api_GOMP_parallel_40_alias
              // ...

              // case 3:
              // __kmp_init_implicit_task
              // __kmp_allocate_thread
              // __kmp_fork_call
              // __kmp_GOMP_fork_call
              // __kmp_api_GOMP_parallel_40_alias
              // ...

              // case 4:
              // __memset_sse2
              // __kmp_initialize_info
              // __kmp_fork_call
              // __kmp_GOMP_fork_call
              // __kmp_api_GOMP_parallel_40_alias
              // ...

            }

          } else {
            // this never happen
            printf("<ompt-callstack.c:1390> Unexpected... top_reg: %d, reg: %d, inner: %d, master: %d\n",
                   top_reg->depth, region_depth, inner->depth, top->team_master);
          }

        } else {

          // case 1:
          // __kmp_wait_4_ptr
          // __kmp_acquire_ticket_lock
          // __kmp_join_call
          // __kmp_api_GOMP_parallel_40_alias

          // case 2:
          // sched_yield
          // __kmp_wait_4_ptr
          // __kmp_acquire_ticket_lock
          // __kmp_join_call
          // __kmp_api_GOMP_parallel_40_alias


        }
      } else {
        printf("<ompt-callstack.c:1398> >>> Top reg is missing\n");
      }
      //check_top_reg_and_inner("ompt-callstack.c:1286");
    }
    // just debug
    if (vi3_last_to_register != region_depth) {
      printf("<<<ompt-callstack.c:1769>>> vi3_last_to_register: %d, region_dept: %d\n",
             vi3_last_to_register, region_depth);
    }
#endif

#if DETECT_IDLENESS_LAST_BARRIER
    // If any idle samples have been previously taken inside this region,
    // attribute them to it.
    attr_idleness2region_at(vi3_last_to_register);
#endif
    return check_and_return_non_null(typed_random_access_stack_get(region)(
        region_stack, region_depth)->unresolved_cct, cct_cursor, 901);
  } else {
#if 0
    if (!ompt_eager_context_p()) {
      if (!waiting_on_last_implicit_barrier) {
        // region should be still active at this point
        check_top_reg_and_inner("ompt-callstack.c:1294", NULL);
        // check if there is some other state then ones previously found
        ompt_state_t state = check_state();
        if (state != ompt_state_overhead && state != ompt_state_work_parallel) {
          printf("Unexpected state: %x\n", state);
        }
#if 0
        if (vi3_idle_collapsed) {
          //printf("Idling: %d, %d\n", vi3_idle_collapsed, state);
          if (state == ompt_state_overhead) {
            // case 1:
            // __kmp_join_barrier
            // __kmp_launch_thread
            // __kmp_launch_worker

            // case 2:
            // __tls_get_addr
            // ompt_sync (scope = begin)
            // __kmp_join_barrier
            // __kmp_launch_thread
            // __kmp_launch_worker

            // case 3:
            // ompt_sync (scope = uknown)
            // __kmp_join_barrier
            // __kmp_launch_thread
            // __kmp_launch_worker
            check_top_reg_and_inner("ompt-callstack.c:1298");
          } else if (state == ompt_state_work_parallel) {
            // case 1:
            // __kmp_launch_thread
            // __kmp_launch_worker


            // case 2:
            // __kmp_GOMP_microtask_wrapper
            // __kmp_invoke_microtask
            // __kmp_invoke_task_func
            // __kmp_launch_thread
            // __kmp_launch_worker


            // case 3:
            // __kmp_GOMP_microtask_wrapper
            // __kmp_invoke_microtask
            // __kmp_invoke_task_func
            // __kmp_launch_thread
            // __kmp_launch_worker


            // case 4:
            // __kmp_api_GOMP_parallel_40_alias
            // g
            // f
            // e.__omp_fn.1
            // __kmp_GOMP_microtask_wrapper
            // __kmp_invoke_microtask
            // __kmp_invoke_task_func
            // __kmp_launch_thread
            // __kmp_launch_worker


            // case 5:
            // __kmp_finish_implicit_task
            // __kmp_invoke_task_func
            // __kmp_launch_thread
            // __kmp_launch_worker

            // case 6:
            // __kmp_invoke_microtask
            // __kmp_invoke_task_func
            // __kmp_launch_thread
            // __kmp_launch_worker

            // case 7:
            // __kmp_run_after_invoked_task
            // __kmp_invoke_task_func
            // __kmp_launch_thread
            // __kmp_launch_worker


            // case 8:
            // __kmp_invoke_microtask
            // __kmp_invoke_task_func
            // __kmp_launch_thread
            // __kmp_launch_worker

            check_top_reg_and_inner("ompt-callstack.c:1298");
          } else {
            printf("Other state: %d\n", ompt_state_work_parallel);
          }
        } else {
          // state = 257 = 0x101   ompt_state_overhead  = 0x101,
          // stack frame
          // case 1:
          // __kmp_run_after_invoked_task
          // __kmp_invoke_task
          // __kmp_launch_thread
          // __kmp_launch_worker

          // case 2:
          // __kmp_join_barrier
          // __kmp_launch_thread
          // __kmp_launch_worker


          // case 3:
          // __kmp_finish_implicit_task
          // __kmp_invoke_task_func
          // __kmp_launch_thread
          // __kmp_launch_worker

          // what happens with parallel_data and stack
          check_top_reg_and_inner("ompt-callstack.c:1322");
        }
#endif
      }

    }
#endif


#if DETECT_IDLENESS_LAST_BARRIER
    if (!ompt_eager_context_p()) {
      if (!waiting_on_last_implicit_barrier) {
        // Since thread still hasn't reached the last implicit barrier,
        // region at the top of the stack (top_reg) is still active.
        // If any idle samples have been previously taken inside this region,
        // attribute them to it.
        attr_idleness2region_at(vi3_last_to_register);
        // Attribute sample to unresolved cct that corresponds to the top_reg
        return check_and_return_non_null(
            hpcrun_ompt_get_top_unresolved_cct_on_stack(), cct_cursor, 1926);
      }

      // Thread has reached the last implicit barrier and thread_data
      // does not contain anything useful, so thread cannot guarantee
      // that any of the region present on the is still active.
      // This sample can be consider as idle or runtime overhead and
      // put either to thread local placeholder or to the outermost context.
      return check_and_return_non_null(handle_idle_sample(cct_cursor),
                                       cct_cursor, 1936);
      // FIXME vi3: Anything better if region is still active???
      //   Can we rely on thread state or any other runtime info.
      //   Review the output.
    }
#endif
  }

  return check_and_return_non_null(cct_cursor, cct_cursor, 904);
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

