/******************************************************************************
 * system includes
 *****************************************************************************/

#include <assert.h>

/******************************************************************************
 * libmonitor
 *****************************************************************************/

#include <monitor.h>

/******************************************************************************
 * local includes
 *****************************************************************************/

#include "ompt-callback.h"
#include "ompt-callstack.h"
#include "ompt-defer.h"
#include "ompt-interface.h"
#include "ompt-region.h"
#include "ompt-region-map.h"
#include "ompt-task.h"
#include "ompt-thread.h"
#include "ompt.h"



#include "../../../lib/prof-lean/stdatomic.h"
#include "../safe-sampling.h"
#include "../thread_data.h"
#include "../memory/hpcrun-malloc.h"

#include <hpcrun/safe-sampling.h>
#include <hpcrun/thread_data.h>




#include <hpcrun/cct/cct.h>
#include <hpcrun/sample_event.h>
#include <printf.h>


/******************************************************************************
 * macros
 *****************************************************************************/

// FIXME: should use eliding interface rather than skipping frames manually
#define LEVELS_TO_SKIP 2 // skip one level in enclosing OpenMP runtime


/******************************************************************************
 * external declarations 
 *****************************************************************************/

extern int omp_get_level(void);
extern int omp_get_thread_num(void);
extern int omp_get_max_threads(void);


/******************************************************************************
 * private operations
 *****************************************************************************/

ompt_region_data_t *
ompt_region_data_new(uint64_t region_id, cct_node_t *call_path)
{
  // old version of allocating
  // ompt_region_data_t *e;
  // e = (ompt_region_data_t *)hpcrun_malloc(sizeof(ompt_region_data_t));

  // new version
  ompt_region_data_t* e = hpcrun_ompt_region_alloc();

  e->region_id = region_id;
  e->call_path = call_path;
  wfq_init(&e->queue);
  // parts for freelist
  OMPT_BASE_T_GET_NEXT(e) = NULL;
  e->thread_freelist = &public_region_freelist;
  e->depth = 0;
  return e;
}

//bool
//ompt_region_data_refcnt_update(ompt_region_data_t* region_data, int val)
//{
//  bool result = false;
//  spinlock_lock(&region_data->region_lock);
//  if(region_data){
//    region_data->refcnt += val;
//    // FIXME: TODO: Decide when to delete region data
//    result = true;
//  }
//  spinlock_unlock(&region_data->region_lock);
//  return result;
//}
//
//uint64_t
//ompt_region_data_refcnt_get(ompt_region_data_t* region_data){
//  spinlock_lock(&region_data->region_lock);
//  uint64_t refcnt = region_data->refcnt;
//  spinlock_unlock(&region_data->region_lock);
//  return refcnt;
//}


static void 
ompt_parallel_begin_internal(
  ompt_data_t *parallel_data,
  int levels_to_skip,
  ompt_invoker_t invoker
) 
{
  hpcrun_safe_enter();


  ompt_region_data_t* region_data = ompt_region_data_new(hpcrun_ompt_get_unique_id(), NULL);
  parallel_data->ptr = region_data;

  uint64_t region_id = region_data->region_id;
  thread_data_t *td = hpcrun_get_thread_data();

  // old version
//  ompt_data_t *parent_region_info = NULL;
//  int team_size = 0;
//  // FIXED: if we put 0 as previous, it would return the current parallel_data which is inside this function always different than NULL
//  hpcrun_ompt_get_parallel_info(1, &parent_region_info, &team_size);
//  if (parent_region_info == NULL) {
//    // mark the master thread in the outermost region
//    // (the one that unwinds to FENCE_MAIN)
//    td->master = 1;
//  }

  // FIXME vi3: check if this is right
  // the region has not been changed yet
  // that's why we say that the parent region is hpcrun_ompt_get_current_region_data
  ompt_region_data_t *parent_region = hpcrun_ompt_get_current_region_data();
  if (!parent_region) {
    // mark the master thread in the outermost region
    // (the one that unwinds to FENCE_MAIN)
    td->master = 1;
    region_data->depth = 0;
  } else {
    region_data->depth = parent_region->depth + 1;
  }

  if(ompt_eager_context){
     region_data->call_path =
     ompt_parallel_begin_context(region_id, ++levels_to_skip,
                                 invoker == ompt_invoker_program);
  }

  hpcrun_safe_exit();

}

cct_node_t*
copy_prefix(cct_node_t* top, cct_node_t* bottom)
{
    // FIXME: vi3 do we need to copy? find the best way to copy callpath
    // previous implementation
    // return bottom;

    // direct manipulation with cct nodes, which is probably not good way to solve this

    if(!bottom || !top){
        return NULL;
    }

    cct_node_t *prefix_bottom = hpcrun_cct_copy_just_addr(bottom);
    // it is possible that just one node is call path between regions
    if(top == bottom) {
        return prefix_bottom;
    }

    cct_node_t *it = hpcrun_cct_parent(bottom);
    cct_node_t *child = NULL;
    cct_node_t *parent = prefix_bottom;

    while (it) {
        child = parent;
        parent = hpcrun_cct_copy_just_addr(it);
        hpcrun_cct_set_parent(child, parent);
        hpcrun_cct_set_children(parent, child);
        // find the top
        if(it == top) {
            return prefix_bottom;
        }
        it = hpcrun_cct_parent(it);
    }

    return NULL;

}

static void
ompt_parallel_end_internal(
    ompt_data_t *parallel_data,    /* data of parallel region       */
    int levels_to_skip,
    ompt_invoker_t invoker
)
{
  // printf("Passed to internal. \n");
  hpcrun_safe_enter();

  ompt_region_data_t* region_data = (ompt_region_data_t*)parallel_data->ptr;


  if(!ompt_eager_context){
    // check if there is any thread registered that should be notified that region call path is available
    ompt_notification_t* to_notify = (ompt_notification_t*) wfq_dequeue_public(&region_data->queue);



    //printf("TOP_INDEX: %d\n", top_index);
    region_stack_el_t *stack_el = &region_stack[top_index+1];
    if (stack_el->took_sample || to_notify) {
      ending_region = region_data;
      cct_node_t *prefix = ompt_region_context(region_data->region_id, ompt_context_end,
                                               ++levels_to_skip, invoker == ompt_invoker_program);
      cct_node_t *current = prefix;
      if (top_index + 1 > 0) {
        // FIXME vi3: find a better way to remove program_root cct from the beginning of the cct
        cct_node_t *parent = NULL;
        while (current) {
          parent = hpcrun_cct_parent(current);
          if (hpcrun_cct_parent(parent) == NULL) {
            prefix = copy_prefix(current, prefix);
              break;
            }
            current = parent;
          }
      }

      ending_region = NULL;
      region_data->call_path = prefix;
      tmp_end_region_resolve(stack_el->notification);
    }

    if(to_notify){
      // notify next thread
      wfq_enqueue(OMPT_BASE_T_STAR(to_notify), to_notify->threads_queue);
    }else{
      // if none, you can reuse region
      // this thread is region creator, so it could add to private region's list
      // FIXME vi3: check if you are right
      freelist_add_first(OMPT_BASE_T_STAR(region_data), OMPT_BASE_T_STAR_STAR(private_region_freelist_head));
      // or should use this
      // wfq_enqueue((ompt_base_t*)region_data, &public_region_freelist);
    }
  }

  // FIXME: vi3: what is this?
  // FIXME: not using team_master but use another routine to
  // resolve team_master's tbd. Only with tasking, a team_master
  // need to resolve itself
  if (ompt_task_full_context) {
    TD_GET(team_master) = 1;
    thread_data_t* td = hpcrun_get_thread_data();
    resolve_cntxt_fini(td);
    TD_GET(team_master) = 0;
  }


//  // FIXME: vi3 check if this is fine, this should make more sense in implicit task end
//  ompt_notification_t* top = top_region_stack();
//  if(top->region_data == region_data){
//    top_index--;
//  }
  // FIXME: vi3 do we really need to keep this line
  hpcrun_get_thread_data()->region_id = 0;
  hpcrun_safe_exit();

}




/******************************************************************************
 * interface operations
 *****************************************************************************/

#ifdef OMPT_V2013_07 
void 
ompt_parallel_begin(
  ompt_data_t  *parent_task_data,   /* tool data for parent task   */
  ompt_frame_t *parent_task_frame,  /* frame data of parent task   */
  ompt_parallel_id_t parallel_id    /* id of parallel region       */
)
{
  int levels_to_skip = LEVELS_TO_SKIP;
  ompt_parallel_begin_internal(parallel_id, ++level_to_skip, 0);
}
#else

void
ompt_parallel_begin(
  ompt_data_t *parent_task_data,
  const ompt_frame_t *parent_frame,
  ompt_data_t *parallel_data,
  unsigned int requested_team_size,
  ompt_invoker_t invoker,
  const void *codeptr_ra
)
{

#if 0
  hpcrun_safe_enter();
  TMSG(DEFER_CTXT, "team create  id=0x%lx parallel_fn=%p ompt_get_parallel_id(0)=0x%lx", region_id, parallel_fn,
       hpcrun_ompt_get_parallel_info_id(0));
  hpcrun_safe_exit();
#endif
  int levels_to_skip = LEVELS_TO_SKIP;
  ompt_parallel_begin_internal(parallel_data, ++levels_to_skip, invoker);
}

#endif

#ifdef OMPT_V2013_07 
void 
ompt_parallel_end(
  ompt_data_t  *parent_task_data,   /* tool data for parent task   */
  ompt_frame_t *parent_task_frame,  /* frame data of parent task   */
  ompt_parallel_id_t parallel_id    /* id of parallel region       */
  )
{
  int levels_to_skip = LEVELS_TO_SKIP;
  ompt_parallel_end_internal(parallel_id, ++levels_to_skip);
}

#else



void
ompt_parallel_end(
  ompt_data_t *parallel_data,
  ompt_data_t *task_data,
  ompt_invoker_t invoker,
  const void *codeptr_ra
)
{
  //printf("Parallel end... MASTER: %d\n", TD_GET(master));

  uint64_t parallel_id = parallel_data->value;
  //printf("Parallel end... region id = %lx\n", parallel_id);
  uint64_t task_id = task_data->value;

  ompt_data_t *parent_region_info = NULL;
  int team_size = 0;
  hpcrun_ompt_get_parallel_info(0, &parent_region_info, &team_size);
  uint64_t parent_region_id = parent_region_info->value;

  hpcrun_safe_enter();
  TMSG(DEFER_CTXT, "team end   id=0x%lx task_id=%x ompt_get_parallel_id(0)=0x%lx", parallel_id, task_id,
       parent_region_id);
  hpcrun_safe_exit();
  int levels_to_skip = LEVELS_TO_SKIP;
  ompt_parallel_end_internal(parallel_data, ++levels_to_skip, invoker);
}


#endif






void
ompt_implicit_task_internal_begin(
  ompt_data_t *parallel_data,
  ompt_data_t *task_data,
  unsigned int team_size,
  unsigned int thread_num
)
{

  task_data->ptr = NULL;

  ompt_region_data_t* region_data = (ompt_region_data_t*)parallel_data->ptr;
  cct_node_t *prefix = region_data->call_path;

  task_data->ptr = prefix;

  if (!ompt_eager_context) {
    // FIXME vi3: check if this is fine
    // add current region
    add_region_and_ancestors_to_stack(region_data, thread_num==0);
    //printf("______%p->%p=%p\n", hpcrun_ompt_get_parent_region_data(), hpcrun_ompt_get_current_region_data(), region_data);

    // FIXME vi3: move this to add_region_and_ancestors_to_stack
    // Memoization process vi3:
    if(thread_num != 0){
      not_master_region = region_data;
    }

#if 0
    region_stack[top_index].parent_frame = hpcrun_ompt_get_task_frame(1);
#endif
  }


}




void
ompt_implicit_task_internal_end(
  ompt_data_t *parallel_data,
  ompt_data_t *task_data,
  unsigned int team_size,
  unsigned int thread_num
)
{

  if (!ompt_eager_context) {
    //printf("IMPLICIT END REGION_ID: %lx, MASTER: %d\n", region_stack[top_index].notification->region_data->region_id, TD_GET(master));
    // the only thing we could do (certainly) here is to pop element from the stack
    // pop element from the stack
    pop_region_stack();
  }

  //printf("---------%p->%p\n", hpcrun_ompt_get_parent_region_data(), hpcrun_ompt_get_current_region_data());

}


void
ompt_implicit_task(
  ompt_scope_endpoint_t endpoint,
  ompt_data_t *parallel_data,
  ompt_data_t *task_data,
  unsigned int team_size,
  unsigned int thread_num)
{
//    printf("---%p, %d\n", parallel_data, endpoint == ompt_scope_end);
 if(endpoint == ompt_scope_begin)
   ompt_implicit_task_internal_begin(parallel_data,task_data,team_size,thread_num);
 else if (endpoint == ompt_scope_end)
   ompt_implicit_task_internal_end(parallel_data,task_data,team_size,thread_num);
 else
   assert(0);
}


void 
ompt_parallel_region_register_callbacks(
  ompt_set_callback_t ompt_set_callback_fn
)
{
  int retval;
  retval = ompt_set_callback_fn(ompt_callback_parallel_begin,
                    (ompt_callback_t)ompt_parallel_begin);
  assert(ompt_event_may_occur(retval));

  retval = ompt_set_callback_fn(ompt_callback_parallel_end,
                    (ompt_callback_t)ompt_parallel_end);
  assert(ompt_event_may_occur(retval));

  retval = ompt_set_callback_fn(ompt_callback_implicit_task,
                                (ompt_callback_t)ompt_implicit_task);
  assert(ompt_event_may_occur(retval));
}




