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
#include <hpcrun/cct_insert_backtrace.h>

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
ompt_elide_runtime_frame_internal(
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

    frame0 = hpcrun_ompt_get_task_frame(i);

    if (!frame0) break;

    ompt_data_t *task_data = hpcrun_ompt_get_task_data(i);
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


// non-zero value means that something was wrong
int
next_region_matching_task
(
  int *task_level,
  int *region_level,
  typed_stack_elem(region) **child_region,
  typed_stack_elem(region) **parent_region
)
{
  int local_task_level = *task_level;
  int local_region_level = *region_level;
  typed_stack_elem(region) *local_child_region = *child_region;
  typed_stack_elem(region) *local_parent_region = *parent_region;

  // after this function, task_data->depth and parent_region->depth
  // must match
  ompt_data_t *td = hpcrun_ompt_get_task_data(local_task_level);

  if (!td) {
    // TODO vi3 (08/26)
    // have no idea what to do
    printf("638\n");
    return - 1;
  }

  int td_depth = -1;
  cct_node_t *td_ctx = NULL;
  int info_type = task_data_value_get_info(td->ptr, &td_ctx, &td_depth);

  int flags0 = hpcrun_ompt_get_task_flags(local_task_level);
  ompt_frame_t *frame0 = hpcrun_ompt_get_task_frame(local_task_level);

  if (info_type != 1 && !(flags0 & ompt_task_initial)) {
    if (flags0 & ompt_task_implicit) {
      if (local_task_level == 0) {
        if (fp_exit(frame0) && !fp_enter(frame0)) {
          // 1.
          // ompt_implici_task (scope=begin)
          // hpcrun_safe_enter

          // 2.
          // __kmp_tid_from_gtid
          // __kmp_GOMP_fork_call
          // __kmp_api_GOMP_parallel
          // local_task_level = 0
          // local_region_level = -1
          //

          // Inermost task frame is set properly and corresponds to the implicit
          // task. Task data has not been filled in yet, since
          // ompt_implicit_task_begin hasn't been called yet.
          // Innermost parallel data corresponds to the innermost task frame
          //assert(local_region_level == -1);
          local_child_region = local_parent_region;
          local_parent_region = hpcrun_ompt_get_region_data(++local_region_level);
          goto return_label;
        }
      } else {
        // TODO vi3 (08/26)
        //printf("vi3 provide: How I omit this???\n");
      }
      printf("678\n");
    } else {
        //printf("vi3 provide: Missing necessary information: %p,"
        //     " task_depth: %d, reg_depth: %d\n", td->ptr, local_task_level,
        //     local_region_level);
      printf("682\n");
    }
    return -2;
  }

  if (local_region_level == -1 || local_parent_region == NULL) {
    // no region at this level, increment level, and try to get parent_region
    local_child_region = local_parent_region;
    local_parent_region = hpcrun_ompt_get_region_data(++local_region_level);
    if (!local_parent_region && !(flags0 & ompt_task_initial)) {
      printf("vi3 provide: No region at this level\n");
      return -3;
    }
  }

  if (td_depth == local_parent_region->depth) {
    // parent_region matching depth of the task
    goto return_label;
  } else if (local_parent_region->depth - 1 == td_depth) {
    // take outer region to match task
    local_child_region = local_parent_region;
    local_parent_region = hpcrun_ompt_get_region_data(++local_region_level);
    goto return_label;
  } else if ((flags0 & ompt_task_initial) && local_parent_region->depth == 0) {
    // we found both outermost task and region
    local_child_region = local_parent_region;
    local_parent_region = hpcrun_ompt_get_region_data(++local_region_level);
    goto return_label;
  } else if (td_depth > local_parent_region->depth) {
    // TODO for-nested-functions (USE_NESTED_TASKS)
    // This makes no sense.
    printf("Makes no sense\n");
    return -4;
  } else {
    // for-nested-functions.c (USE_NESTED_TASKS)
    // Task by levels:
    // 0 - explicit task (exit_frame set)
    // 1 - implicit untied (why???)
    // 2 - implicit (enter = 0, exit = 0) Suspended I guess
    // 3 - implicit (enter and exit set)
    // 4 - implicit (enter and exit set)
    // 5 - implicit (enter and exit set)
    // 6 - initial (enter=0, exit set)
    // TODO vi3 (08/26)
    // Don't understand this situation at all.
    printf("vi3 provide: Something wasn't done properly before: flags: %x,"
           " td_depth: %d, parent_depth: %d\n",
           flags0, td_depth, local_parent_region->depth);
    return -5;
  }

  return_label:
  {
    *task_level = local_task_level;
    *region_level = local_region_level;
    *child_region = local_child_region;
    *parent_region = local_parent_region;
    return 0;
  };


}


static void
provide_region_refix_segment
(
  frame_t **region_prefix_inner,
  frame_t **region_prefix_outer,
  typed_stack_elem(region) **region_data
)
{
  // Provide segment of region prefix
  (*region_data)->call_path =
      hpcrun_cct_insert_backtrace(hpcrun_get_thread_epoch()->csdata.unresolved_root,
                                  *region_prefix_outer, *region_prefix_inner);
  // invalidate passed values
  *region_prefix_outer = NULL;
  *region_prefix_inner = NULL;
  *region_data = NULL;
}

#if 0
int vi3_edge_case(
    ompt_frame_t **f0,
    ompt_frame_t **f1,
    ompt_frame_t **f2,
    ompt_frame_t **f3,
    ompt_frame_t **f4,
    ompt_frame_t **f5,
    ompt_frame_t **f6) {
  ompt_frame_t  *frame0 = hpcrun_ompt_get_task_frame(0);
  ompt_frame_t  *frame1 = hpcrun_ompt_get_task_frame(1);
  ompt_frame_t  *frame2 = hpcrun_ompt_get_task_frame(2);
  ompt_frame_t  *frame3 = hpcrun_ompt_get_task_frame(3);
  ompt_frame_t  *frame4 = hpcrun_ompt_get_task_frame(4);
  ompt_frame_t  *frame5 = hpcrun_ompt_get_task_frame(5);
  ompt_frame_t  *frame6 = hpcrun_ompt_get_task_frame(6);


  *f0 = frame0;
  *f1 = frame1;
  *f2 = frame2;
  *f3 = frame3;
  *f4 = frame4;
  *f5 = frame5;
  *f6 = frame6;


  if (frame0 && frame1 && frame2 && frame3 && frame4 && frame5 && frame6) {
    if (fp_enter(frame0) == 0 && fp_exit(frame0) != 0) {
      if (fp_enter(frame1) == 0 && fp_exit(frame1) == 0) {
        if (fp_enter(frame2) == 0 && fp_exit(frame2) == 0) {
          if (fp_exit(frame3) == fp_enter(frame4)) {
            //printf("Zastoooooooooooooooooooooo???\n");
            return 1;
          }
        }
      }
    }
  }
  return 0;
}
#endif

// Return value
// zero     - call path is provided
// non-zero - some of the edge case that is not handled for now
// FIXME vi3: pay attention to this if needed
static int
ompt_provide_callpaths_while_elide_runtime_frame_internal
(
  backtrace_info_t *bt,
  uint64_t region_id,
  int isSync
)
{
#if 0
  VI3_EDGE = 0;
#endif
  // invalidate omp_task_context of previous sample
  // FIXME vi3 >>> It should be ok to do this.
  TD_GET(omp_task_context) = 0;

  vi3_idle_collapsed = false;
  nested_regions_before_explicit_task = 0;
  frame_t **bt_outer = &bt->last;
  frame_t **bt_inner = &bt->begin;

  frame_t *bt_outer_to_return = bt->last;
  frame_t *bt_inner_to_return = bt->begin;

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
      //printf("vi3 provide: Thread type: %d (other/unknown)\n", thread_type);
      return 1;
      goto return_label;
  }

  int on_explicit_barrier = 0;
  ompt_state_t thread_state = check_state();
  // collapse callstack if a thread is idle or waiting in a barrier
  switch(thread_state) {
    case ompt_state_wait_barrier:
    case ompt_state_wait_barrier_implicit: // mark it idle
      //printf("This should be deprecated\n");
      return -1;
    case ompt_state_wait_barrier_implicit_parallel:
      // printf("vi3 provide: Wait on the implicit barrier "
      //        "(may be implicit): %x\n", check_state());
      // Thread is the master of this region.
      // It finishes implicit task and is waiting on the last
      // implicit barrier. Enter and exit frames of the task should be zero.
      // Task data should contain depth that matches innermost region depth.
      // return 2;
      break;
    case ompt_state_wait_barrier_explicit: // attribute them to the corresponding
      //printf("vi3 provide: Wait on the explicit barrier\n");
      return 3;
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
      //printf("vi3 provide: Idle state\n");
      return 4;
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
  int flags0 = hpcrun_ompt_get_task_flags(i);
  int reg_anc_lev = -1;
  typed_stack_elem(region) *child_region = NULL;
  typed_stack_elem(region) *parent_region = NULL;
  frame_t *child_prefix_inner = NULL;
  frame_t *child_prefix_outer = NULL;


  TD_GET(omp_task_context) = 0;

  elide_debug_dump("ORIGINAL", *bt_inner, *bt_outer, region_id); 
  elide_frame_dump();

  //---------------------------------------------------------------
  // handle all of the corner cases that can occur at the top of 
  // the stack first
  //---------------------------------------------------------------

  if (!frame0) {
    //printf("vi3 provide: No innermost frame\n");
    return 5;
    // corner case: the innermost task (if any) has no frame info.
    // no action necessary. just return.
    goto clip_base_frames;
  }
  // find corresponding region if any
  next_region_matching_task(&i, &reg_anc_lev, &child_region, &parent_region);

#if 0
  if (flags0 & ompt_task_implicit || flags0 & ompt_task_initial) {
    // get corresponding parallel region
    child_region = parent_region;
    parent_region = hpcrun_ompt_get_region_data(++reg_anc_lev);
  }
#endif

  bool processed_it = false;
  while ((fp_enter(frame0) == 0) &&
         (fp_exit(frame0) == 0)) {

    if (thread_state == ompt_state_work_parallel
        && i == 0 && (flags0 & ompt_task_implicit)) {
      // TODO vi3: Something similar to this can be added to condition
      //  which calls this function for providing. I think that if we check this,
      //  then worker who's creating the new region, or master (outer worker)
      //  who's destroying the region ca be found.
      if (reg_anc_lev == -1) {
        // Thread is creating the new region
        // Innermost parallel data should be present
        child_region = parent_region;
        parent_region = hpcrun_ompt_get_region_data(++reg_anc_lev);
        // TODO vi3 (08/23/2020): how about checking parent_region->depth,
        //  not sure if this is enough
        if (!parent_region) {
          //printf("vi3: Parallel data hasn't set yet");
          return -3;
        }
        // task_data hasn't been filled yet, since implicit task hasn't
        // been called. This should happen between ompt_callback_parallel_begin
        // and ompt_implicit_task_begin.
      } else if (parent_region) {
        // Thread is destroying the parent_region
        // Task_data is not deleted yet, which means it contains depth of
        // the parent_region.
        // Just skip this frame and provide call path for the parent_region.
        // This happens before ompt_callback_parallel_end and team hasn't been
        // torn apart. FIXME vi3 hopes.
      } else {
        //printf("vi3: Thread is not creating nor "
        //       "destroying the region\n");
        return -4;
      }
    } else if (thread_state == ompt_state_work_parallel
               && i == 0 && (flags0 & ompt_task_explicit)) {
      if (reg_anc_lev == -1) {
        // Thread is creating new explicit task. I think parent_region
        // should be present, so get it. After that, skip frame.
        child_region = parent_region;
        parent_region = hpcrun_ompt_get_region_data(++reg_anc_lev);
        if (!parent_region) {
          //printf("vi3 provide: Why is parallel data unavailable?\n");
          return -5;
        }
      } else if (parent_region) {
        // Thread is either creating a new task (task frame),
        // or is destroying the suspended one (task_frames invalidated).
        // Skip this frame.
      } else {
        //printf("vi3 provide: Thread is not creating nor"
        //       "destroying explicit task.\n");
        return -6;
      }
    } else if (thread_state == ompt_state_wait_barrier_implicit_parallel
               && i == 1 && (flags0 & ompt_task_implicit)
               && parent_region && !child_region) {
      // Task at level 1 represent implicit task that corresponds
      // to the parent_region. This task has been suspended and thread
      // is waiting on the last implicit barrier probably finishing the
      // explicit task (at level 0). Skip this task, and try to
      // provide prefix for the parent_region
      processed_it = true;
    } else if (thread_state == ompt_state_work_parallel && i == 1
               && (flags0 & ompt_task_implicit)
               && parent_region && !child_region) {
      // The same as previous, but thread_state of the master
      // thread hasn't been chnged yet.
      processed_it = true;
    } else if (thread_state != ompt_state_wait_barrier_implicit_parallel
        && thread_state != ompt_state_overhead) {
      // Handle only samples taken while waiting on the last implicit barrier.
      //printf("vi3 provide: No innermost frame\n");
      // TODO for-nested-functions (USE_NESTED_TASKS)
      return 6;
    }

    if (i > 0 && !processed_it) {
      // Assume for now that all outer task frames are set properly
      //printf("vi3: More than one empty task_frames\n");
      // FIXME vi3: for-nested-tasks (USE_NESTED_FRAMES)
      return -7;
    }

    // Innermost task has been suspended,
    // corresponding parallel region is still active, so try to provide prefix.

    // corner case: the top frame has been set up,
    // but not filled in. ignore this frame.
    frame0 = hpcrun_ompt_get_task_frame(++i);
    flags0 = hpcrun_ompt_get_task_flags(i);

    if (!frame0) {
      //printf("vi3: Why this happens??? -3");
      return -3;
      if (thread_type == ompt_thread_initial) goto return_label;

      // corner case: the innermost task (if any) has no frame info.
      goto clip_base_frames;
    }

    next_region_matching_task(&i, &reg_anc_lev, &child_region, &parent_region);
  }

  if (fp_exit(frame0) &&
      (((uint64_t) fp_exit(frame0)) <
       ((uint64_t) (*bt_inner)->cursor.sp))) {
    if (i == 0 && parent_region) {
      // parent_region matches this new innermost task
      // (task belongs to this region).
      // Skip task and then try to provide call path for the parent region.
    } else {
      // I suppose that the innermost region will match task at level i+1.
      // But wait for this to happen to be sure.
      return 7;
    }
    //printf("vi3 provide: No call to user code has been made\n");
    // corner case: the top frame has been set up, exit frame has been filled in;
    // however, exit_frame.ptr points beyond the top of stack. the final call
    // to user code hasn't been made yet. ignore this frame.
    frame0 = hpcrun_ompt_get_task_frame(++i);
    flags0 = hpcrun_ompt_get_task_flags(i);
    // TODO vi3: when this happens
    next_region_matching_task(&i, &reg_anc_lev, &child_region, &parent_region);
#if 0
    if (flags0 & ompt_task_implicit || flags0 & ompt_task_initial) {
      // get corresponding region
      child_region = parent_region;
      parent_region = hpcrun_ompt_get_region_data(++reg_anc_lev);
    }
#endif
  }

  if (!frame0) {
    //printf("vi3 provide: Innermost frame has no info\n");
    return 8;
    // corner case: the innermost task (if any) has no frame info.
    goto clip_base_frames;
  }

  if (fp_enter(frame0)) {
    //printf("vi3 provide: Sample was taken inside runtime\n");
    // return 9;
    // the sample was received inside the runtime;
    // elide frames from top of stack down to runtime entry
    int found = 0;
    for (it = *bt_inner; it <= *bt_outer; it++) {
      if ((uint64_t)(it->cursor.sp) >= (uint64_t)fp_enter(frame0)) {
        if (isSync) {
          // for synchronous samples, elide runtime frames at top of stack
          *bt_inner = it;
          bt_inner_to_return = *bt_inner;

        } else if (on_explicit_barrier) {
          // bt_inner points to __kmp_api_GOMP_barrier_10_alias
          *bt_inner = it - 1;
          bt_inner_to_return = *bt_inner;
          // replace the last frame with explicit barrier placeholder
          set_frame(*bt_inner, &ompt_placeholders.ompt_barrier_wait_state);

          // FIXME vi3 (08/21/2020): LLVM Runtime does not rerturn this kind of barrier,
          //   so I won't pay attention to this
        } else {
          // FIXME vi3: Think good about this
          //   tests to run:
          //      - simple.c (with for loop)
          if (thread_state == ompt_state_wait_barrier_implicit_parallel) {
            // i == 1 if innermost task is suspended implicit
            // i == 2 if innermost task is suspended explicit
            //assert(i == 1 || i == 2);
            //assert(child_region != NULL);
            // level of child_region
            //assert(reg_anc_lev - 1 == 0);
            // innermost frame of the child region (should be the innermost region)
            child_prefix_inner = it;
          } else {
            if (i == 0 && parent_region && !child_region) {
              // Thread left user code of the innermost task (corresponds to the
              // parent_region). I think it is going either to create a new task
              // or it has suspended nested task which is not on the stack anymore.
              // Since children_region is not set, or it has been removed, cannot
              // provide prefix for it. Just skip this case.
            } else if (i == 0 && parent_region && child_region) {
              // The same as previous, but children_region has been set,
              // or it's not removed ye, so provide prefix for children_region.
              child_prefix_inner = it;
            } else if (i == 1 && parent_region && child_region) {
              // Similar to previous two with difference that task innermost task
              // (that corresponds to the child_region) is either created,
              // but not filled yet, or it has been suspended, but still
              // not removed from the stack.
              // Since child_region exists, prefix can be provided.
              child_prefix_inner = it;
            } else if (i == 1 && parent_region &&
                      !child_region && (flags0 & ompt_task_explicit)) {
              // Inside runtime code called from explicit task. Since child_region
              // is not found, cannot provide its prefix.
            } else if (i == 1 && parent_region && !child_region
                      && (flags0 & ompt_task_implicit)) {
              // Sample is taken inside runtime code called from
              // implicit tasks that matches parent_region.
              // Cannot provide prefix, so just do nothing.
              // FIXME vi3what??? In order to provide prefix for the parent_region,
              // set child_region to match parent_region (only in this case).
              // child_region = parent_region;
              // child_prefix_inner = it;
            } else if (i == 2 && parent_region && child_region
                      && (flags0 & ompt_task_implicit)){
              // Task frame at level 0 should be new/suspended explicit task.
              // Task frame at level 1 should be suspended implicit task.
              // We can provide prefix for the child region
              // Since implicit task at level 2 matches parent_region,
              // it should be possible to provide prefix for child_region.
              child_prefix_inner = it;
            } else if (i == 1 && !parent_region && child_region) {
              // The child_region should be at depth zero.
              // Enter_frame of task (should be initial) represents the
              // innermost frame of the child_region prefix.
              child_prefix_inner = it;
            } else if (i == 0 &&
                      (flags0 & ompt_task_initial) && !parent_region
                      && child_region) {
              // Child_region is the only active region.
              // Frame0 is the initial task, so its enter_frame represents
              // the innermost frame of child_region prefix.
              child_prefix_inner = it;
            } else {
              // TODO for-nested-functions (USE_NESTED_TASKS)
              //printf("vi3 provide: Think good about taking"
              //       " sample inside runtime frames: %d, %p, %p\n", i, parent_region, child_region);
              return -100;
            };
          }

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

#if 0
  ompt_frame_t *tf0 = NULL;
  ompt_frame_t *tf1 = NULL;
  ompt_frame_t *tf2 = NULL;
  ompt_frame_t *tf3 = NULL;
  ompt_frame_t *tf4 = NULL;
  ompt_frame_t *tf5 = NULL;
  ompt_frame_t *tf6 = NULL;

  VI3_EDGE = vi3_edge_case(&tf0,&tf1,&tf2,&tf3,&tf4,&tf5,&tf6);
#endif

  // general case: elide frames between frame1->enter and frame0->exit
  while (true) {
    frame_t *exit0 = NULL, *reenter1 = NULL;
    ompt_frame_t *frame1 = NULL;
    int flags1 = 0;

    frame0 = hpcrun_ompt_get_task_frame(i);
    flags0 = hpcrun_ompt_get_task_flags(i);
    // this region corresponds to the frame0
    parent_region = hpcrun_ompt_get_region_data(reg_anc_lev);

    if (!frame0) break;

    ompt_data_t *task_data = hpcrun_ompt_get_task_data(i);
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
          child_prefix_outer = exit0 - 1;
          if (child_prefix_inner && child_prefix_outer && child_region) {
            provide_region_refix_segment(&child_prefix_inner, &child_prefix_outer,
                                         &child_region);
          }
          break;
        }
      }
    } else {
      if (child_prefix_inner && child_region && child_region->depth == 0) {
        // Provide call path for the outermost region.
        // Outermost frame of prefix should be the last frame in backtrace.
        child_prefix_outer = *bt_outer;
        provide_region_refix_segment(&child_prefix_inner, &child_prefix_outer,
                                     &child_region);
      }
    }
    // reset frames
    child_prefix_inner = NULL;
    child_prefix_outer = NULL;

    if (exit0_flag && omp_task_context) {
      if (bt_outer_to_return == bt->last) {
        TD_GET(omp_task_context) = omp_task_context;
        // set frame boundaries only once
        bt_outer_to_return = exit0 -1;
        bt_inner_to_return = *bt_inner;
      }
      // *bt_outer = exit0 - 1;
      // no need to provide parent region
      if (parent_region->call_path) {
        break;
      }
    }
    //frame0 = frame1;
    //flags0 = flags1;
    frame1 = hpcrun_ompt_get_task_frame(++i);
    flags1 = hpcrun_ompt_get_task_flags(i);
    if (!frame1) break;

    next_region_matching_task(&i, &reg_anc_lev, &child_region, &parent_region);
    // If thread is not the master of child region, it should stop providing.
    // No need to provide call path if its already provided.
    if (child_region &&
        (!hpcrun_ompt_is_thread_region_owner(child_region) || child_region->call_path)) {
      break;
    }

#if 0
    if (fp_enter(frame1) == 0 && fp_exit(frame1) == 0 && (flags1 & ompt_task_implicit)) {
      // skip finished implicit task, since its frame is invalidated
      frame1 = hpcrun_ompt_get_task_frame(++i);
      flags1 = hpcrun_ompt_get_task_flags(i);
      if (!frame1) break;
      next_region_matching_task(&i, &reg_anc_lev, &child_region, &parent_region);
      // If thread is not the master of child region, it should stop providing.
      // No need to provide call path if its already provided.
      if (child_region &&
          (!hpcrun_ompt_is_thread_region_owner(child_region) || child_region->call_path)) {
        break;
      }
    }
#endif

    // FIXME vi3 (08/22/2020) It is possible that and frame1->enter=frame0->enter=0
    //  The question is is this ok to happen???
    //  Run region-in-task3.c
    while (fp_enter(frame1) == 0) {
      // skip frame1, since I don't think this should happen.
      //frame0 = frame1;
      //flags0 = flags1;
      frame1 = hpcrun_ompt_get_task_frame(++i);
      flags1 = hpcrun_ompt_get_task_flags(i);
      if (!frame1) break;
      next_region_matching_task(&i, &reg_anc_lev, &child_region, &parent_region);
      // If thread is not the master of child region, it should stop providing.
      // No need to provide call path if its already provided.
      if (child_region &&
          (!hpcrun_ompt_is_thread_region_owner(child_region)
             || child_region->call_path)) {
        goto break_the_inner_loop;
      }
    }

    //-------------------------------------------------------------------------
    //  frame1 points into the stack above the task frame (in the
    //  runtime from the outer task's perspective). frame0 points into
    //  the the stack inside the first application frame (in the
    //  application from the inner task's perspective) the two points
    //  are equal. there is nothing to elide at this step.
    //-------------------------------------------------------------------------
    if ((fp_enter(frame1) == fp_exit(frame0)) &&
        (ff_is_appl(FF(frame0, exit)) &&
         ff_is_rt(FF(frame1, enter)))) {
      if (flags0 & ompt_task_implicit) {
        // Going from implicit to implicit task, or from
        // implicit to explicit task. Both outer task should
        // be start of the prefix
        // I think it should be child innermost frame
        child_prefix_inner = it;
      }
      continue;
    }


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
          // FIXME vi3 (08/18): or it should be it - 1?
          int vi3_offet = ff_is_appl(FF(frame1, enter)) ? 1 : 0;
          if (vi3_offet) {
            //printf("vi3 provide: What to do know??? :(");
          }
          child_prefix_inner = it - vi3_offet;
          break;

        }
      }
    }



    if (exit0 && reenter1) {

    // I think I don't need this
#if 0
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
#endif
    } else if (exit0 && !reenter1) {
      // corner case: reenter1 is in the team master's stack, not mine. eliminate all
      // frames below the exit frame.
      // *bt_outer = exit0 - 1;
      bt_outer_to_return = exit0 - 1;
      // FIXME vi3 (08/18): handle this
      // TODO vi3: is this important, since it can happen
      //   This happens in the following examples:
      //      - simple-task.c (sample taken in loop2)
      //      - region-in-task3.c
      //      - for-nested-functions (USE_NESTED_TASKS 1)
      if (fp_enter(frame1) < low_sp) {
        // Have no idea how is this even possible.
        // TODO vi3:
        //   - for-nested-functions (USE_NESTED_TASKS 1)
        // This frame is upper on the stack than lower bound.
      } else if (parent_region && child_region
                 && !hpcrun_ompt_is_thread_region_owner(parent_region)){
        // Thread is not the owner of the parent region,
        // so frame1->enter may not ne on thread's stack.
        // It's possible to have untied implicit task
      } else if (parent_region
                 && hpcrun_ompt_is_thread_region_owner(parent_region)
                 && fp_enter(frame1) > high_sp) {
        // TODO vi3:
        //   - for-nested-functions (USE_NESTED_TASKS 1)
        // Why this happens???
      } else {
        // never happened
        //printf("vi3 provide: Can this happen??? %p\n", fp_enter(frame1));
      }
      // TODO: 0x6000002 for task?
      break;
    }
  }

  break_the_inner_loop:

  if (bt_outer_to_return != bt_outer_at_entry) {
    bt->bottom_frame_elided = true;
    bt->partial_unwind = false;
    if (bt_outer_to_return < bt_inner_to_return) {
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
    } else {
      // Now modify the original backtrace
      *bt_outer = bt_outer_to_return;
      *bt_inner = bt_inner_to_return;
    }
  }

  elide_debug_dump("ELIDED", *bt_inner, *bt_outer, region_id);
  goto return_label;

 clip_base_frames:
  {
    //printf("vi3 provide: I'm not clippin base frames\n");
    return 10;
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
    return 0;
}

static void
ompt_elide_runtime_frame(
  backtrace_info_t *bt, 
  uint64_t region_id, 
  int isSync
)
{
#if EARLY_PROVIDE_REGION_PREFIX && 1
  // TODO vi3: Worker that is creating a new region, won't enter here,
  //   until it becomes a master.
  if (!ompt_eager_context_p()) {
    typed_stack_elem(region) *region_data = hpcrun_ompt_get_region_data(0);
    // Call path of the innermost region hasn't been provided yet.
    if (region_data && !(region_data->call_path) &&
        hpcrun_ompt_is_thread_region_owner(region_data)) {
      // zero value means that everything is handled properly
      //   and that call path has been provided
      // non-zero value means that some edge case was handled and that
      //   call path wasn't provided. Call old elider instead.
      int ret = ompt_provide_callpaths_while_elide_runtime_frame_internal(bt, region_id, isSync);
      if (!ret)
        return;
      else
        printf("vi3 provide: Edge case: %d\n", ret);
    }
  }
#endif
  ompt_elide_runtime_frame_internal(bt, region_id, isSync);
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
#if EARLY_PROVIDE_REGION_PREFIX
    return check_and_return_non_null(typed_random_access_stack_get(region)(
        region_stack, region_depth)->cct_node, cct_cursor, 901);
#else
    return check_and_return_non_null(typed_random_access_stack_get(region)(
        region_stack, region_depth)->unresolved_cct, cct_cursor, 901);
#endif
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

