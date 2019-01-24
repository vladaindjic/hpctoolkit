
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
// Copyright ((c)) 2002-2018, Rice University
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

#include <ompt.h>

#include <lib/prof-lean/placeholders.h>
#include <hpcrun/cct_backtrace_finalize.h>
#include <hpcrun/sample_event.h>
#include <hpcrun/trace.h>
#include <hpcrun/unresolved.h>
#include <unwind/common/backtrace_info.h>

#include "../hpcrun-initializers.h"

#include "ompt-callstack.h"
#include "ompt-interface.h"
#include "ompt-state-placeholders.h"
#include "ompt-defer.h"
#include "ompt-region.h"
#include "ompt-thread.h"
#include "ompt-task.h"
#include "../cct/cct.h"
#include "../thread_data.h"
#include "../cct_backtrace_finalize.h"
#include "../sample_event.h"
#include "../trace.h"
#include "../unresolved.h"

#if defined(HOST_CPU_PPC) 
#include "ppc64-gnu-omp.h"
#elif defined(HOST_CPU_x86) || defined(HOST_CPU_x86_64)
#include "x86-gnu-omp.h"
#else
#error "invalid architecture type"
#endif


//******************************************************************************
// macros
//******************************************************************************

#define OMPT_DEBUG 1

#if OMPT_DEBUG
#define elide_debug_dump(t,i,o,r) if (ompt_callstack_debug) stack_dump(t,i,o,r)
#define elide_frame_dump() if (ompt_callstack_debug) frame_dump()
#else
#define elide_debug_dump(t,i,o,r)
#define elide_frame_dump() if (ompt_callstack_debug) frame_dump()
#endif


//******************************************************************************
// private variables 
//******************************************************************************

static cct_backtrace_finalize_entry_t ompt_finalizer;

static closure_t ompt_callstack_init_closure;

int ompt_eager_context = 0;
static int ompt_callstack_debug = 1;



//******************************************************************************
// private  operations
//******************************************************************************

static void 
stack_dump(
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
frame_dump() 
{
  EMSG("-----frame start");
  for (int i=0;; i++) {
    ompt_frame_t *frame = hpcrun_ompt_get_task_frame(i);
    if (frame == NULL) break;

    void *r = frame->reenter_runtime_frame;
    void *e = frame->exit_runtime_frame;
    EMSG("frame %d: (r=%p, e=%p)", i, r, e);
  }
  EMSG("-----frame end"); 
}


static int
interval_contains(
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


static omp_state_t
check_state()
{
  uint64_t wait_id;
  return hpcrun_ompt_get_state(&wait_id);
}


static void 
set_frame(frame_t *f, ompt_placeholder_t *ph)
{
  f->cursor.pc_unnorm = ph->pc;
  f->ip_norm = ph->pc_norm;
  f->the_function = ph->pc_norm;
//  printf("------------------Just setting to idle\n");

}


static void
collapse_callstack(backtrace_info_t *bt, ompt_placeholder_t *placeholder)

{

  set_frame(bt->last, placeholder);
  bt->begin = bt->last;
  bt->bottom_frame_elided = false;
  bt->partial_unwind = false;
//  printf("*****************Collapsing to idle\n");
//  bt->trace_pc = (bt->begin)->cursor.pc_unnorm;
//  bt->fence = FENCE_MAIN;
}



static void
ompt_elide_runtime_frame_internal(
  backtrace_info_t *bt, 
  uint64_t region_id, 
  int isSync,
  int replay
)
{

  how_is_eliding_finished = 0;

  //return;
  frame_t **bt_outer = &bt->last;
  frame_t **bt_inner = &bt->begin;

  frame_t *bt_outer_at_entry = *bt_outer;

  //printf("Eliding\n");
  // eliding only if the thread is an OpenMP initial or worker thread

  switch(ompt_thread_type_get()) {
  case ompt_thread_initial:
    break;
  case ompt_thread_worker:
	break;
//    if (hpcrun_ompt_get_parallel_info_id(0) != ompt_parallel_id_none)
//      break;
//
//    if(!TD_GET(master)){
//      thread_data_t *td = hpcrun_get_thread_data();
//      td->core_profile_trace_data.epoch->csdata.top = main_top_root;
//      td->core_profile_trace_data.epoch->csdata.thread_root = main_top_root;
//      bt->fence = FENCE_MAIN;
//
//
//      hpcrun_cct_insert_node(main_top_root, td->core_profile_trace_data.epoch->csdata.top);
//      printf("\n\n\nThis is parent of top_root   : %p\n", td->core_profile_trace_data.epoch->csdata.top->parent);
//      printf("This is parent of thread_root: %p\n", td->core_profile_trace_data.epoch->csdata.thread_root->parent);
//
//
//    }
//    collapse_callstack(bt, &ompt_placeholders.ompt_barrier_wait);
//    goto return_label;
  case ompt_thread_other:
  case ompt_thread_unknown:
  default:
    goto return_label;
  }

  // collapse callstack if a thread is idle or waiting in a barrier
  switch(check_state()) {
    case omp_state_wait_barrier:
    case omp_state_wait_barrier_implicit:
    case omp_state_wait_barrier_explicit:
      break; // FIXME: skip barrier collapsing until the kinks are worked out.
//    if(!TD_GET(master)){
//
//      collapse_callstack(bt, &ompt_placeholders.ompt_barrier_wait);
//      goto return_label;
//    }

    case omp_state_idle:
//    if (!TD_GET(master)) {
      // FIXME vi3: I think we should delete task context when thread is idle-ing
      TD_GET(omp_task_context) = 0;
      collapse_callstack(bt, &ompt_placeholders.ompt_idle);
      how_is_eliding_finished = -1;
      goto return_label;
//    }
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



  while ((frame0->reenter_runtime_frame == 0) && 
         (frame0->exit_runtime_frame == 0)) {
    // corner case: the top frame has been set up, 
    // but not filled in. ignore this frame.
    frame0 = hpcrun_ompt_get_task_frame(++i);

    if (!frame0) {
      // corner case: the innermost task (if any) has no frame info. 
      goto clip_base_frames;
    }
  }

  if (frame0->exit_runtime_frame &&
      (((uint64_t) frame0->exit_runtime_frame) <
      //  ((uint64_t) (*bt_inner)->cursor.bp))) {
        ((uint64_t) (*bt_inner)->cursor.sp))) {
    // corner case: the top frame has been set up, exit frame has been filled in; 
    // however, exit_runtime_frame points beyond the top of stack. the final call 
    // to user code hasn't been made yet. ignore this frame.
    frame0 = hpcrun_ompt_get_task_frame(++i);
  }

  if (!frame0) {
    // corner case: the innermost task (if any) has no frame info. 
    goto clip_base_frames;
  }

  if (frame0->reenter_runtime_frame) { 
    // the sample was received inside the runtime; 
    // elide frames from top of stack down to runtime entry
    int found = 0;
    for (it = *bt_inner; it <= *bt_outer; it++) {
        // FIXME: different from ompt branch which used >
      if ((uint64_t)(it->cursor.sp) >= (uint64_t)frame0->reenter_runtime_frame) {
//      if ((uint64_t)(it->cursor.bp) >= (uint64_t)frame0->reenter_runtime_frame) {
	      if (isSync) {
          // for synchronous samples, elide runtime frames at top of stack
          *bt_inner = it;
        }
        found = 1;
        break;
      }
    }

    if (found == 0) {
      // reenter_runtime_frame not found on stack. all frames are runtime frames
      goto clip_base_frames;
    }
    // frames at top of stack elided. continue with the rest
  }

  // FIXME vi3: trouble with master thread when defering
  // general case: elide frames between frame1->enter and frame0->exit
  while (true) {
    frame_t *exit0 = NULL, *reenter1 = NULL;
    ompt_frame_t *frame1;

    frame0 = hpcrun_ompt_get_task_frame(i);

    if (!frame0) break;

    ompt_data_t *task_data = hpcrun_ompt_get_task_data(i);
    cct_node_t *omp_task_context = NULL;
    if(task_data)
      omp_task_context = task_data->ptr;
    
    void *low_sp = (*bt_inner)->cursor.sp;
    void *high_sp = (*bt_outer)->cursor.sp;

//    void *low_sp = (*bt_inner)->cursor.bp;
//    void *high_sp = (*bt_outer)->cursor.bp;


    // if a frame marker is inside the call stack, set its flag to true
    bool exit0_flag = 
      interval_contains(low_sp, high_sp, frame0->exit_runtime_frame);

    /* start from the top of the stack (innermost frame). 
       find the matching frame in the callstack for each of the markers in the
       stack. look for them in the order in which they should occur.

       optimization note: this always starts at the top of the stack. this can
       lead to quadratic cost. could pick up below where you left off cutting in 
       previous iterations.
    */
    it = *bt_inner; 
    if(exit0_flag) {
      for (; it <= *bt_outer; it++) {
        if((uint64_t)(it->cursor.sp) > (uint64_t)(frame0->exit_runtime_frame)) {
//        if((uint64_t)(it->cursor.bp) > (uint64_t)(frame0->exit_runtime_frame)) {
          exit0 = it - 1;
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

    bool reenter1_flag = 
      interval_contains(low_sp, high_sp, frame1->reenter_runtime_frame);

#if 1
    ompt_frame_t *help_frame = region_stack[top_index-i+1].parent_frame;
    if (!ompt_eager_context && !reenter1_flag && help_frame) {
      frame1 = help_frame;
      reenter1_flag = interval_contains(low_sp, high_sp, frame1->reenter_runtime_frame);
      // printf("THIS ONLY HAPPENS IN MASTER: %d\n", TD_GET(master));
    }
#endif

    if(reenter1_flag) {
      for (; it <= *bt_outer; it++) {
        if((uint64_t)(it->cursor.sp) > (uint64_t)(frame1->reenter_runtime_frame)) {
       // if((uint64_t)(it->cursor.bp) > (uint64_t)(frame0->exit_runtime_frame)) {
          reenter1 = it - 1;
          break;
        }
      }
    }



    // FIXME vi3: This makes trouble with master thread when defering
    if (exit0 && reenter1) {



      // FIXME: IBM and INTEL need to agree
      // laksono 2014.07.08: hack removing one more frame to avoid redundancy with the parent
      // It seems the last frame of the master is the same as the first frame of the workers thread
      // By eliminating the topmost frame we should avoid the appearance of the same frame twice 
      //  in the callpath

      // FIXME vi3: find better way to solve this  "This makes trouble with master thread when defering"
//      if(TD_GET(master)){
//        return;
//      }
//      if(omp_get_thread_num() == 0)
//        return;

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
  }

  bt->trace_pc = (*bt_inner)->cursor.pc_unnorm;

  elide_debug_dump("ELIDED", *bt_inner, *bt_outer, region_id);
  goto return_label;

 clip_base_frames:
  {
    int master = TD_GET(master);
    if (!master) {
      set_frame(*bt_outer, &ompt_placeholders.ompt_idle);
      *bt_inner = *bt_outer;
      bt->bottom_frame_elided = false;
      bt->partial_unwind = false;
      bt->trace_pc = (*bt_inner)->cursor.pc_unnorm;
      goto return_label;
    }

    /* runtime frames with nothing else; it is harmless to reveal them all */
    uint64_t idle_frame = (uint64_t) hpcrun_ompt_get_idle_frame();

    if (idle_frame) {
      /* clip below the idle frame */
      for (it = *bt_inner; it <= *bt_outer; it++) {
        if ((uint64_t)(it->cursor.sp) >= idle_frame) {
       // if ((uint64_t)(it->cursor.bp) >= idle_frame) {
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
  }


  return_label:
  {
    return;
  };


}

static void
ompt_elide_runtime_frame(
        backtrace_info_t *bt,
        uint64_t region_id,
        int isSync
)
{
  ompt_elide_runtime_frame_internal(bt, region_id, isSync, 0);
}

// For case when
// COLLAPSE_BARRIER_WAIT=0
// STICK_BARRIER_WITH_INNERMOST_REGION=0

// ompt_elide_runtime_frame_inside_region is closely related with ompt_cct_cursor_finalize
// ompt_elide_runtime_frame_inside_region elides runtime frames, and clips bt at some point
// ompt_cct_cursor_finalize put cct build upon bt at some place in thread tree
// (root of this tree is thread root)
//
// Explain how the elider works on the next example. Assume that we are inside parallel region
// Thread took a sample and bt looks like this:
// main
// a
// b
// c
// region1
// runtime_frame1
// runtime_frame2
// ...
// runtime_frameN
// task
// user_frame1
// user_frame2
// ...
// user_frameN
//
//
// There are couple of possibilities when we take a sample
//
// 1. Sample is taken inside user code (variable how_is_eliding_finished is set to 1) -
// Clip all stack frames that are above implicit/explicit task frame (frame0->exit)
// The results of elider will be bt that contains these frames
//
// task
// user_frame1
// user_frame2
// ...
// user_frameN
//
// When we create cct, we should stick it under current region (job of ompt_cct_cursor_finalize).
// If there is not any parallel region, then only master thread can run
// and cct will be stick under thread root (job of ompt_cct_cursor_finalize).
//
//
// 2. Sample is taken while thread is in idle state (how_is_eliding_finished is set to -1)
// Elider will collapse stack frames of bt to ompt_placeholders.ompt_idle.
// After ccts are created, they will be stick under the thread_root (job of ompt_cct_cursor_finalize)
//
//
// 3. Sample is taken while thread is on the barrier (how_is_eliding_finished is set to -2)
//
// a) In the first case, stack will be collapse ompt_placeholders.ompt_idle (how_is_eliding_finished is reset to -1)
// by elider and cct will be stick under the thread_root by ompt_cct_cursor_finalize
//
// b) In the second case, elider will elide all runtime frames from bt and won't clip anything.
// The reason while we don't clip anything is that we don't know where is exit runtime frame
// of task that has been finished (frame0->exit is 0 && frame0->reenter is 0).
// We could clip above the parent task, but then bt will contain frames that belongs to parent task.
// It is safer not to do clipping (reset how_is_eliding_finished to 2)
// and then ccts that are built upon bt stick either under the cct_cursor if thread
// is initial master (TD_GET(master) == 1) or under the cct_not_master
// if thread is created as a worker in top most region (TD_GET(master) == 0)
// (job of ompt_cct_cursor_finalize)
//
// 4. Thread took a sample in __kmp_launch_worker (how_is_eliding_finished = -3)
// ompt_thread_type_get() is equal to ompt_thread_unknown
// just return from elider
// ccts should be set under the root (FIXME vi3: is this what expected)
//
// 5. Thread took a sample in __kmp_acquire_ticket_lock (how_is_eliding_finished = -4)
// ompt_thread_type_get() is equal to omp_state_overhead
// Elide runtime frames, but do not clip anything
// After that, ccts should be put either under the cct_cursor (TD_GET(master) == 1)
// or under the cct_not_master (TD_GET(master) == 0)

#define COLLAPSE_BARRIER_WAIT 0

static void
ompt_elide_runtime_frame_inside_region(
        backtrace_info_t *bt,
        uint64_t region_id,
        int isSync
)
{

    how_is_eliding_finished = 0;

    //return;
    frame_t **bt_outer = &bt->last;
    frame_t **bt_inner = &bt->begin;

    frame_t *bt_outer_at_entry = *bt_outer;

    //printf("Eliding\n");
    // eliding only if the thread is an OpenMP initial or worker thread

    switch(ompt_thread_type_get()) {
        case ompt_thread_initial:
            break;
        case ompt_thread_worker:
            break;
//    if (hpcrun_ompt_get_parallel_info_id(0) != ompt_parallel_id_none)
//      break;
//
//    if(!TD_GET(master)){
//      thread_data_t *td = hpcrun_get_thread_data();
//      td->core_profile_trace_data.epoch->csdata.top = main_top_root;
//      td->core_profile_trace_data.epoch->csdata.thread_root = main_top_root;
//      bt->fence = FENCE_MAIN;
//
//
//      hpcrun_cct_insert_node(main_top_root, td->core_profile_trace_data.epoch->csdata.top);
//      printf("\n\n\nThis is parent of top_root   : %p\n", td->core_profile_trace_data.epoch->csdata.top->parent);
//      printf("This is parent of thread_root: %p\n", td->core_profile_trace_data.epoch->csdata.thread_root->parent);
//
//
//    }
//    collapse_callstack(bt, &ompt_placeholders.ompt_barrier_wait);
//    goto return_label;
        case ompt_thread_other:
        case ompt_thread_unknown:
        default:
            how_is_eliding_finished = -3;
            goto return_label;
    }

    // collapse callstack if a thread is idle or waiting in a barrier
    switch(check_state()) {
        case omp_state_wait_barrier:
        case omp_state_wait_barrier_implicit:
        case omp_state_wait_barrier_explicit:
#if COLLAPSE_BARRIER_WAIT == 1
            TD_GET(omp_task_context) = 0;
            collapse_callstack(bt, &ompt_placeholders.ompt_barrier_wait);
            how_is_eliding_finished = -2;
            goto return_label;
#else
            how_is_eliding_finished = -2;
            break; // FIXME: skip barrier collapsing until the kinks are worked out.
#endif

//    if(!TD_GET(master)){
//
//      collapse_callstack(bt, &ompt_placeholders.ompt_barrier_wait);
//      goto return_label;
//    }

        case omp_state_idle:
//    if (!TD_GET(master)) {
            // FIXME vi3: I think we should delete task context when thread is idle-ing
            TD_GET(omp_task_context) = 0;
            collapse_callstack(bt, &ompt_placeholders.ompt_idle);
            how_is_eliding_finished = -1;
            goto return_label;
//    }
        case omp_state_overhead:
            how_is_eliding_finished = -4;
            //printf("***************************************This happened\n");
            break;

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

//    if (frame0->reenter_runtime_frame == 0 && frame0->exit_runtime_frame == 0) {
//      if (top_index >= 0) {
//        frame0 = region_stack[top_index].parent_frame;
//      }
//    }


    while ((frame0->reenter_runtime_frame == 0) &&
           (frame0->exit_runtime_frame == 0)) {
        // FIXME vi3: force unwinding
        //how_is_eliding_finished = (how_is_eliding_finished == 0) ? 33 : how_is_eliding_finished;
        if (how_is_eliding_finished == -2 ) {
            how_is_eliding_finished = 2;
        }


        // corner case: the top frame has been set up,
        // but not filled in. ignore this frame.
        frame0 = hpcrun_ompt_get_task_frame(++i);

        if (!frame0) {
            // corner case: the innermost task (if any) has no frame info.
            goto clip_base_frames;
        }
    }

    if (frame0->exit_runtime_frame &&
        (((uint64_t) frame0->exit_runtime_frame) <
         //  ((uint64_t) (*bt_inner)->cursor.bp))) {
         ((uint64_t) (*bt_inner)->cursor.sp))) {
        // corner case: the top frame has been set up, exit frame has been filled in;
        // however, exit_runtime_frame points beyond the top of stack. the final call
        // to user code hasn't been made yet. ignore this frame.
        frame0 = hpcrun_ompt_get_task_frame(++i);
    }

    if (!frame0) {
        // corner case: the innermost task (if any) has no frame info.
        goto clip_base_frames;
    }

    if (frame0->reenter_runtime_frame) {
        // the sample was received inside the runtime;
        // elide frames from top of stack down to runtime entry
        int found = 0;
        for (it = *bt_inner; it <= *bt_outer; it++) {
            // FIXME: different from ompt branch which used >
            if ((uint64_t)(it->cursor.sp) >= (uint64_t)frame0->reenter_runtime_frame) {
//      if ((uint64_t)(it->cursor.bp) >= (uint64_t)frame0->reenter_runtime_frame) {
                if (isSync) {
                    // for synchronous samples, elide runtime frames at top of stack
                    *bt_inner = it;
                }
                found = 1;
                break;
            }
        }

        if (found == 0) {
            // reenter_runtime_frame not found on stack. all frames are runtime frames
            goto clip_base_frames;
        }
        // frames at top of stack elided. continue with the rest
    }


    // FIXME vi3: trouble with master thread when defering
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

//    void *low_sp = (*bt_inner)->cursor.bp;
//    void *high_sp = (*bt_outer)->cursor.bp;


        // if a frame marker is inside the call stack, set its flag to true
        bool exit0_flag =
                interval_contains(low_sp, high_sp, frame0->exit_runtime_frame);

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
                if ((uint64_t) (it->cursor.sp) > (uint64_t) (frame0->exit_runtime_frame)) {
//        if((uint64_t)(it->cursor.bp) > (uint64_t)(frame0->exit_runtime_frame)) {
                    exit0 = it - 1;
                    break;
                }
            }
        }

        if (exit0_flag && task_data && (how_is_eliding_finished == 0 || how_is_eliding_finished == -2)) {
            //TD_GET(omp_task_context) = omp_task_context;
            *bt_outer = exit0 - 1;
            how_is_eliding_finished = 1;
            break;
        }

        frame1 = hpcrun_ompt_get_task_frame(++i);
        if (!frame1) {
            // FIXME vi3: if cct which represents thread root
            // should be shown separately
            // Otherwise, it will be connected with innermost region.
            how_is_eliding_finished = -3;
            break;
        }

        bool reenter1_flag =
                interval_contains(low_sp, high_sp, frame1->reenter_runtime_frame);

#if 0
        ompt_frame_t *help_frame = region_stack[top_index-i+1].parent_frame;
        if (!ompt_eager_context && !reenter1_flag && help_frame) {
            frame1 = help_frame;
            reenter1_flag = interval_contains(low_sp, high_sp, frame1->reenter_runtime_frame);
            // printf("THIS ONLY HAPPENS IN MASTER: %d\n", TD_GET(master));
        }
#endif

        if(reenter1_flag) {
            for (; it <= *bt_outer; it++) {
                if((uint64_t)(it->cursor.sp) > (uint64_t)(frame1->reenter_runtime_frame)) {
                    // if((uint64_t)(it->cursor.bp) > (uint64_t)(frame0->exit_runtime_frame)) {
                    reenter1 = it - 1;
                    break;
                }
            }
        }



        // FIXME vi3: This makes trouble with master thread when defering
        if (exit0 && reenter1) {



            // FIXME: IBM and INTEL need to agree
            // laksono 2014.07.08: hack removing one more frame to avoid redundancy with the parent
            // It seems the last frame of the master is the same as the first frame of the workers thread
            // By eliminating the topmost frame we should avoid the appearance of the same frame twice
            //  in the callpath

            // FIXME vi3: find better way to solve this  "This makes trouble with master thread when defering"
//      if(TD_GET(master)){
//        return;
//      }
//      if(omp_get_thread_num() == 0)
//        return;

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
    }

    bt->trace_pc = (*bt_inner)->cursor.pc_unnorm;

    elide_debug_dump("ELIDED", *bt_inner, *bt_outer, region_id);
    goto return_label;

    clip_base_frames:
    {
        int master = TD_GET(master);
        if (!master) {
            how_is_eliding_finished = -1;
            set_frame(*bt_outer, &ompt_placeholders.ompt_idle);
            *bt_inner = *bt_outer;
            bt->bottom_frame_elided = false;
            bt->partial_unwind = false;
            bt->trace_pc = (*bt_inner)->cursor.pc_unnorm;
            goto return_label;
        }

        /* runtime frames with nothing else; it is harmless to reveal them all */
        uint64_t idle_frame = (uint64_t) hpcrun_ompt_get_idle_frame();

        if (idle_frame) {
            /* clip below the idle frame */
            for (it = *bt_inner; it <= *bt_outer; it++) {
                if ((uint64_t)(it->cursor.sp) >= idle_frame) {
                    // if ((uint64_t)(it->cursor.bp) >= idle_frame) {
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
    }


    return_label:
    {
        return;
    };


}


static void
ompt_elide_runtime_frame_end_region(
        backtrace_info_t *bt,
        uint64_t region_id,
        int isSync
)
{

    how_is_eliding_finished = 0;

    //return;
    frame_t **bt_outer = &bt->last;
    frame_t **bt_inner = &bt->begin;

    frame_t *bt_outer_at_entry = *bt_outer;

    //printf("Eliding\n");
    // eliding only if the thread is an OpenMP initial or worker thread

    switch(ompt_thread_type_get()) {
        case ompt_thread_initial:
            break;
        case ompt_thread_worker:
            break;
//    if (hpcrun_ompt_get_parallel_info_id(0) != ompt_parallel_id_none)
//      break;
//
//    if(!TD_GET(master)){
//      thread_data_t *td = hpcrun_get_thread_data();
//      td->core_profile_trace_data.epoch->csdata.top = main_top_root;
//      td->core_profile_trace_data.epoch->csdata.thread_root = main_top_root;
//      bt->fence = FENCE_MAIN;
//
//
//      hpcrun_cct_insert_node(main_top_root, td->core_profile_trace_data.epoch->csdata.top);
//      printf("\n\n\nThis is parent of top_root   : %p\n", td->core_profile_trace_data.epoch->csdata.top->parent);
//      printf("This is parent of thread_root: %p\n", td->core_profile_trace_data.epoch->csdata.thread_root->parent);
//
//
//    }
//    collapse_callstack(bt, &ompt_placeholders.ompt_barrier_wait);
//    goto return_label;
        case ompt_thread_other:
        case ompt_thread_unknown:
        default:
            goto return_label;
    }

    // collapse callstack if a thread is idle or waiting in a barrier
    switch(check_state()) {
        case omp_state_wait_barrier:
        case omp_state_wait_barrier_implicit:
        case omp_state_wait_barrier_explicit:
            break; // FIXME: skip barrier collapsing until the kinks are worked out.
//    if(!TD_GET(master)){
//
//      collapse_callstack(bt, &ompt_placeholders.ompt_barrier_wait);
//      goto return_label;
//    }

        case omp_state_idle:
//    if (!TD_GET(master)) {
            // FIXME vi3: I think we should delete task context when thread is idle-ing
            TD_GET(omp_task_context) = 0;
            collapse_callstack(bt, &ompt_placeholders.ompt_idle);
            how_is_eliding_finished = -1;
            goto return_label;
//    }
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



    while ((frame0->reenter_runtime_frame == 0) &&
           (frame0->exit_runtime_frame == 0)) {
        // corner case: the top frame has been set up,
        // but not filled in. ignore this frame.
        frame0 = hpcrun_ompt_get_task_frame(++i);

        if (!frame0) {
            // corner case: the innermost task (if any) has no frame info.
            goto clip_base_frames;
        }
    }

    if (frame0->exit_runtime_frame &&
        (((uint64_t) frame0->exit_runtime_frame) <
         //  ((uint64_t) (*bt_inner)->cursor.bp))) {
         ((uint64_t) (*bt_inner)->cursor.sp))) {
        // corner case: the top frame has been set up, exit frame has been filled in;
        // however, exit_runtime_frame points beyond the top of stack. the final call
        // to user code hasn't been made yet. ignore this frame.
        frame0 = hpcrun_ompt_get_task_frame(++i);
    }

    if (!frame0) {
        // corner case: the innermost task (if any) has no frame info.
        goto clip_base_frames;
    }

    if (frame0->reenter_runtime_frame) {
        // the sample was received inside the runtime;
        // elide frames from top of stack down to runtime entry
        int found = 0;
        for (it = *bt_inner; it <= *bt_outer; it++) {
            // FIXME: different from ompt branch which used >
            if ((uint64_t)(it->cursor.sp) >= (uint64_t)frame0->reenter_runtime_frame) {
//      if ((uint64_t)(it->cursor.bp) >= (uint64_t)frame0->reenter_runtime_frame) {
                if (isSync) {
                    // for synchronous samples, elide runtime frames at top of stack
                    *bt_inner = it;
                }
                found = 1;
                break;
            }
        }

        if (found == 0) {
            // reenter_runtime_frame not found on stack. all frames are runtime frames
            goto clip_base_frames;
        }
        // frames at top of stack elided. continue with the rest
    }

    // FIXME vi3: trouble with master thread when defering
    // general case: elide frames between frame1->enter and frame0->exit
    while (true) {
        frame_t *exit0 = NULL, *reenter1 = NULL;
        ompt_frame_t *frame1;

        frame0 = hpcrun_ompt_get_task_frame(i);

        if (!frame0) break;

        ompt_data_t *task_data = hpcrun_ompt_get_task_data(i);
        cct_node_t *omp_task_context = NULL;
        if(task_data)
            omp_task_context = task_data->ptr;

        void *low_sp = (*bt_inner)->cursor.sp;
        void *high_sp = (*bt_outer)->cursor.sp;

//    void *low_sp = (*bt_inner)->cursor.bp;
//    void *high_sp = (*bt_outer)->cursor.bp;


        // if a frame marker is inside the call stack, set its flag to true
        bool exit0_flag =
                interval_contains(low_sp, high_sp, frame0->exit_runtime_frame);

        /* start from the top of the stack (innermost frame).
           find the matching frame in the callstack for each of the markers in the
           stack. look for them in the order in which they should occur.

           optimization note: this always starts at the top of the stack. this can
           lead to quadratic cost. could pick up below where you left off cutting in
           previous iterations.
        */
        it = *bt_inner;
        if(exit0_flag) {
            for (; it <= *bt_outer; it++) {
                if((uint64_t)(it->cursor.sp) > (uint64_t)(frame0->exit_runtime_frame)) {
//        if((uint64_t)(it->cursor.bp) > (uint64_t)(frame0->exit_runtime_frame)) {
                    exit0 = it - 1;
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

        bool reenter1_flag =
                interval_contains(low_sp, high_sp, frame1->reenter_runtime_frame);

#if 0
        ompt_frame_t *help_frame = region_stack[top_index-i+1].parent_frame;
        if (!ompt_eager_context && !reenter1_flag && help_frame) {
            frame1 = help_frame;
            reenter1_flag = interval_contains(low_sp, high_sp, frame1->reenter_runtime_frame);
            // printf("THIS ONLY HAPPENS IN MASTER: %d\n", TD_GET(master));
        }
#endif

        if(reenter1_flag) {
            for (; it <= *bt_outer; it++) {
                if((uint64_t)(it->cursor.sp) > (uint64_t)(frame1->reenter_runtime_frame)) {
                    // if((uint64_t)(it->cursor.bp) > (uint64_t)(frame0->exit_runtime_frame)) {
                    reenter1 = it - 1;
                    break;
                }
            }
        }



        // FIXME vi3: This makes trouble with master thread when defering
        if (exit0 && reenter1) {



            // FIXME: IBM and INTEL need to agree
            // laksono 2014.07.08: hack removing one more frame to avoid redundancy with the parent
            // It seems the last frame of the master is the same as the first frame of the workers thread
            // By eliminating the topmost frame we should avoid the appearance of the same frame twice
            //  in the callpath

            // FIXME vi3: find better way to solve this  "This makes trouble with master thread when defering"
//      if(TD_GET(master)){
//        return;
//      }
//      if(omp_get_thread_num() == 0)
//        return;

            //------------------------------------
            // The prefvous version DON'T DELETE
            memmove(*bt_inner+(reenter1-exit0+1), *bt_inner,
                    (exit0 - *bt_inner)*sizeof(frame_t));

            *bt_inner = *bt_inner + (reenter1 - exit0 + 1);

//            exit0 = reenter1 = NULL;
//            // --------------------------------

            // FIXME vi3: offset 1 is added here, this may not be right in all cases
            //*bt_inner = exit0 - 1;
            *bt_outer = reenter1;
            break;
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
    }

    bt->trace_pc = (*bt_inner)->cursor.pc_unnorm;

    elide_debug_dump("ELIDED", *bt_inner, *bt_outer, region_id);
    goto return_label;

    clip_base_frames:
    {
        int master = TD_GET(master);
        if (!master) {
            set_frame(*bt_outer, &ompt_placeholders.ompt_idle);
            *bt_inner = *bt_outer;
            bt->bottom_frame_elided = false;
            bt->partial_unwind = false;
            bt->trace_pc = (*bt_inner)->cursor.pc_unnorm;
            goto return_label;
        }

        /* runtime frames with nothing else; it is harmless to reveal them all */
        uint64_t idle_frame = (uint64_t) hpcrun_ompt_get_idle_frame();

        if (idle_frame) {
            /* clip below the idle frame */
            for (it = *bt_inner; it <= *bt_outer; it++) {
                if ((uint64_t)(it->cursor.sp) >= idle_frame) {
                    // if ((uint64_t)(it->cursor.bp) >= idle_frame) {
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
    }


    return_label:
    {
        return;
    };


}

static cct_node_t *
memoized_context_get(thread_data_t* td, uint64_t region_id)
{
  return (td->outer_region_id == region_id && td->outer_region_context) ? 
    td->outer_region_context : 
    NULL;
}

static void
memoized_context_set(thread_data_t* td, uint64_t region_id, cct_node_t *result)
{
    td->outer_region_id = region_id;
    td->outer_region_context = result;
}


cct_node_t *
region_root(cct_node_t *_node)
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

// vi3: not used in this file
//static cct_node_t *
//lookup_region_id(uint64_t region_id)
//{
//  thread_data_t* td = hpcrun_get_thread_data();
//  cct_node_t *result = NULL;
//
//  if (hpcrun_trace_isactive()) {
//    result = memoized_context_get(td, region_id);
//    if (result) return result;
//
//    cct_node_t *t0_path = hpcrun_region_lookup(region_id);
//    if (t0_path) {
//      cct_node_t *rroot = region_root(t0_path);
//      result = hpcrun_cct_insert_path_return_leaf(rroot, t0_path);
//      memoized_context_set(td, region_id, result);
//    }
//  }
//
//  return result;
//}


cct_node_t *
ompt_region_context(uint64_t region_id, 
		    ompt_context_type_t ctype, 
		    int levels_to_skip,
                    int adjust_callsite)
{
  cct_node_t *node;
  ucontext_t uc;
  getcontext(&uc);

  // levels to skip will be broken if inlining occurs.
  // FIXME: vi3 Change according new signature of hpcrun_sample_callpath
  // vi3 old version
  // node = hpcrun_sample_callpath(&uc, 0, 0, 0, 1).sample_node;
  // vi3 new version
  hpcrun_metricVal_t blame_metricVal;
  blame_metricVal.i = 0;
  node = hpcrun_sample_callpath(&uc, 0, blame_metricVal, 0, 1, NULL).sample_node;

  TMSG(DEFER_CTXT, "unwind the callstack for region 0x%lx", region_id);

  if (node && adjust_callsite) {
    // extract the load module and offset of the leaf CCT node at the 
    // end of a call path representing a parallel region
    cct_addr_t *n = hpcrun_cct_addr(node);
    cct_node_t *n_parent = hpcrun_cct_parent(node); 
    uint16_t lm_id = n->ip_norm.lm_id; 
    uintptr_t lm_ip = n->ip_norm.lm_ip;
    uintptr_t master_outlined_fn_return_addr;

    // adjust the address to point to return address of the call to 
    // the outlined function in the master
    if (ctype == ompt_context_begin) {
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
    node = sibling;
  }

  return node;
}


// ===========================
void
ompt_region_context_end_region_not_eager(uint64_t region_id,
                    ompt_context_type_t ctype,
                    int levels_to_skip,
                    int adjust_callsite)
{
  cct_node_t *node;
  ucontext_t uc;
  getcontext(&uc);

  // levels to skip will be broken if inlining occurs.
  // FIXME: vi3 Change according new signature of hpcrun_sample_callpath
  // vi3 old version
  // node = hpcrun_sample_callpath(&uc, 0, 0, 0, 1).sample_node;
  // vi3 new version
  hpcrun_metricVal_t blame_metricVal;
  blame_metricVal.i = 0;
  node = hpcrun_sample_callpath(&uc, 0, blame_metricVal, 0, 33, NULL).sample_node;

  TMSG(DEFER_CTXT, "unwind the callstack for region 0x%lx", region_id);

//  if (node && adjust_callsite) {
//    // extract the load module and offset of the leaf CCT node at the
//    // end of a call path representing a parallel region
//    cct_addr_t *n = hpcrun_cct_addr(node);
//    cct_node_t *n_parent = hpcrun_cct_parent(node);
//    uint16_t lm_id = n->ip_norm.lm_id;
//    uintptr_t lm_ip = n->ip_norm.lm_ip;
//    uintptr_t master_outlined_fn_return_addr;
//
//    // adjust the address to point to return address of the call to
//    // the outlined function in the master
//    if (ctype == ompt_context_begin) {
//      void *ip = hpcrun_denormalize_ip(&(n->ip_norm));
//      uint64_t offset = offset_to_pc_after_next_call(ip);
//      master_outlined_fn_return_addr = lm_ip + offset;
//    } else {
//      uint64_t offset = length_of_call_instruction();
//      master_outlined_fn_return_addr = lm_ip - offset;
//    }
//    // ensure that there is a leaf CCT node with the proper return address
//    // to use as the context. when using the GNU API for OpenMP, it will
//    // be a sibling to one returned by sample_callpath.
//    cct_node_t *sibling = hpcrun_cct_insert_addr
//            (n_parent, &(ADDR2(lm_id, master_outlined_fn_return_addr)));
//    node = sibling;
//  }

//  return node;
}

// ===========================



cct_node_t *
ompt_parallel_begin_context(ompt_parallel_id_t region_id, int levels_to_skip, 
                            int adjust_callsite)
{
//  return ompt_region_context(region_id, ompt_context_begin,
//                             ++levels_to_skip, adjust_callsite);
  if (ompt_eager_context) {
    return ompt_region_context(region_id, ompt_context_begin,
                               ++levels_to_skip, adjust_callsite);
  } else {
    return NULL;
  }
}


static void
ompt_backtrace_finalize(
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
    if(!ompt_eager_context)
      resolve_cntxt();
  }
  uint64_t region_id = TD_GET(region_id);

  if (ompt_eager_context) {
    ompt_elide_runtime_frame(bt, region_id, isSync);
  } else {
    if (ending_region) {
      ompt_elide_runtime_frame_end_region(bt, region_id, isSync);
    } else {
      ompt_elide_runtime_frame_inside_region(bt, region_id, isSync);
    }
  }



}



//******************************************************************************
// interface operations
//******************************************************************************

cct_node_t *
top_or_cct_cursor(cct_node_t *cct_cursor)
{
  if (top_index >= 0) {
    return region_stack[top_index].notification->unresolved_cct;
  }
  return cct_cursor;
}



#define STICK_BARRIER_WITH_INNERMOST_REGION 0

// Short remainder of what how_is_eliding_finished value means:
// -4 - ompt_thread_type_get() is equal to omp_state_overhead
//      runtime frames are elided, put ccts either under
//      thread_root (if TD_GET(master) == 1) or under the
//      cct_not_master (if TD_GET(master) == 0)
// -3 - ompt_thread_type_get() == ompt_thread_unknown, elider is stopped, ccts under the thread root
// -2 - thread took a sample at barrier, but bt is collapsed to ompt_idle,
//      so ccts will be put under the thread_root (which is cct_cursor)
// -1 - thread took a sample while idling, bt is collapsed to ompt_idle
//      so ccts will be put under the thread root
//  0 - took a sample in initial task, put ccts under thread root
//      FIXME vi3: I am not sure if this ever happened
//      it is possible that I missed some case is two switch at the beginning of the elider
//  1 - frames above current task are clipped from bt.
//      bt only contains user code frames of current task.
//      ccts should be stick with current parallel region.
//      if there is no parallel region, stick ccts under thread_root
//  2 - thread took a sample on barrier, runtime frames are elided
//      but no frames are clipped (keep fully unwinded stack)
//      ccts are connected to thread_root (if TD_GET(master) == 1)
//      or to cct_not_master (if TD_GET(master) == 0)

cct_node_t *
ompt_cct_cursor_finalize(cct_bundle_t *cct, backtrace_info_t *bt, 
                           cct_node_t *cct_cursor)
{

  if (ompt_eager_context) {
    cct_node_t *omp_task_context = TD_GET(omp_task_context);

    // FIXME: should memoize the resulting task context in a thread-local variable
    //        I think we can just return omp_task_context here. it is already
    //        relative to one root or another.
    if (omp_task_context) {
      cct_node_t *root;
#if 1
      root = region_root(omp_task_context);
#else
    if((is_partial_resolve((cct_node_tt *)omp_task_context) > 0)) {
      root = hpcrun_get_thread_epoch()->csdata.unresolved_root;
    } else {
      root = hpcrun_get_thread_epoch()->csdata.tree_root;
    }
#endif
      // FIXME: vi3 why is this called here??? Makes troubles for worker thread when !ompt_eager_context
      if(ompt_eager_context || TD_GET(master))
        return hpcrun_cct_insert_path_return_leaf(root, omp_task_context);
    }

#if 0
    // FIXME: vi3 consider this when tracing, for now everything works fine

    // if I am not the master thread, full context may not be immediately available.
    // if that is the case, then it will later become available in a deferred fashion.
    if (!TD_GET(master)) { // sub-master thread in nested regions

    //    uint64_t region_id = TD_GET(region_id);
    //    ompt_data_t* current_parallel_data = TD_GET(current_parallel_data);
    //    ompt_region_data_t* region_data = (ompt_region_data_t*)current_parallel_data->ptr;
      // FIXME: check whether bottom frame elided will be right for IBM runtime
      //        without help of get_idle_frame

      if(not_master_region && bt->bottom_frame_elided){
        // it should be enough just to set cursor to unresolved node
        // which corresponds to not_master_region

        // everything is ok with cursos
        cct_cursor = cct_not_master_region;

      }
    }
#endif

    return cct_cursor;

  } else {

    if(ending_region) {
      return cct_cursor;
    }

#if STICK_BARRIER_WITH_INNERMOST_REGION == 1
    if (how_is_eliding_finished == 1  || how_is_eliding_finished == -2) {
#else
    if (how_is_eliding_finished == 1) {
#endif
      return top_or_cct_cursor(cct_cursor);

#if STICK_BARRIER_WITH_INNERMOST_REGION == 1
    } else if (how_is_eliding_finished == -1 || how_is_eliding_finished == -3) {
#else
    } else if (how_is_eliding_finished == -1 || how_is_eliding_finished == -3 || how_is_eliding_finished == -2) {
#endif
      return cct_cursor;
    } else {
      return TD_GET(master) ? cct_cursor : cct_not_master_region;
    }
  }

}

void
ompt_callstack_init_deferred(void)
{
  if (hpcrun_trace_isactive()) ompt_eager_context = 1;
}

void
ompt_callstack_init(void)
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
