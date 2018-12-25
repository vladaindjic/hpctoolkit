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
#include <unresolved.h>


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

  ompt_data_t *parent_region_info = NULL;
  int team_size = 0;
  // FIXED: if we put 0 as previous, it would return the current parallel_data which is inside this function always different than NULL
  hpcrun_ompt_get_parallel_info(1, &parent_region_info, &team_size);
  if (parent_region_info == NULL) {
    // mark the master thread in the outermost region
    // (the one that unwinds to FENCE_MAIN)
    td->master = 1;
  }

  if(ompt_eager_context){
     // FIXME vi3: should this point to the parent of the region, see task.c
     region_data->call_path =
     ompt_parallel_begin_context(region_id, ++levels_to_skip,
                                 invoker == ompt_invoker_program);
  }

  hpcrun_safe_exit();

}


static void
ompt_parallel_end_internal(
    ompt_data_t *parallel_data,    /* data of parallel region       */
    int levels_to_skip,
    ompt_invoker_t invoker
)
{

  // vi3: We insert unresolved cct for each region, which means that even master
  // needs to resolve region call path. Master can do it immediately after providing region callpath

  // FIXME vi3: check if we have to delete unresolved cct
  // for the region is sample are not taken by the master thread
  // and all workers


  // vi3: Note - Implicit tasks end happens before here
  // That means that the current region has been remove from the stack
  // top_index points to its parent
  // that's why we used top_index+1 to get the region that's ending

  // printf("Passed to internal. \n");
  hpcrun_safe_enter();

  ompt_region_data_t* region_data = (ompt_region_data_t*)parallel_data->ptr;

  // check if we took sample in this region
  region_stack_el_t *top_el = &region_stack[top_index+1];

  if(!ompt_eager_context){
    // check if there is any thread registered that should be notified that region call path is available
    ompt_notification_t* to_notify = (ompt_notification_t*) wfq_dequeue_public(&region_data->queue);
    if(to_notify){
      if(region_data->call_path == NULL){

        // FIXME vi3: this is one big hack
        ending_region = region_data;
        // need to provide call path, because master did not take a sample inside region
        ompt_region_context_end_region_not_eager(region_data->region_id, ompt_context_end,
                                     ++levels_to_skip, invoker == ompt_invoker_program);
        // I think that is enough to call previous function, which call hpcrun_sample_callpath
        // FIXME: consider this
        //printf("This is the region data: %p\n", region_data->call_path);
        ending_region = NULL;
      }

      //region_data->call_path = ompt_region_context(region_data->region_id, ompt_context_end,
      //                             ++levels_to_skip, invoker == ompt_invoker_program);

      // notify next thread
      wfq_enqueue(OMPT_BASE_T_STAR(to_notify), to_notify->threads_queue);

      // FIXME vi3: check if this is thread safe
      if (top_el->took_sample) {
        resolve_one_region(top_el->notification, region_data);
      }
    }else{
      // FIXME vi3: check if this is thread safe
      if (top_el->took_sample) {

        cct_node_t *tmp_cct = ompt_region_context_end_region_not_eager(region_data->region_id, ompt_context_end,
                                                   ++levels_to_skip, invoker == ompt_invoker_program);

        // FIXME vi3: this should be resolved better, but because of ompt-defer.c:1258
        // I made this hack. I don't have mechanism to get the right call path segment
        // of the current region. That's why we found the full call path, and clip the
        // segment of interest
        // I am not sure that this works in every case.
        if (!region_data->call_path) {
          cct_node_t *bottom_prefix = tmp_cct;
          cct_node_t *top_prefix = NULL;
          if (top_index == -1) {
            top_prefix = top_cct(tmp_cct);
          } else {
            cct_node_t *current = tmp_cct;
            cct_node_t *parent = hpcrun_cct_parent(current);
            cct_addr_t *parent_addr;
            top_el = &region_stack[top_index];
            cct_addr_t *top_addr = hpcrun_cct_addr(top_el->notification->unresolved_cct);

            while (current && parent) {
              parent_addr = hpcrun_cct_addr(parent);
              if (parent_addr->ip_norm.lm_id == top_addr->ip_norm.lm_id && parent_addr->ip_norm.lm_ip == top_addr->ip_norm.lm_ip) {
                break;
              }
              current = parent;
              parent = hpcrun_cct_parent(parent);
            }
            top_prefix = current;
          }

          region_data->call_path = copy_prefix(top_prefix, bottom_prefix);

          printf("RID: %lx, LM_ID: %d, LM_IP: %lx\n", region_data->region_id,
                 hpcrun_cct_addr(region_data->call_path)->ip_norm.lm_id,
                 hpcrun_cct_addr(region_data->call_path)->ip_norm.lm_ip);

        }
        top_el = &region_stack[top_index+1];

        resolve_one_region(top_el->notification, region_data);
      }


      // if none, you can reuse region
      // this thread is region creator, so it could add to private region's list
      // FIXME vi3: check if you are right
      freelist_add_first(OMPT_BASE_T_STAR(region_data), OMPT_BASE_T_STAR_STAR(private_region_freelist_head));
      // or should use this
      // wfq_enqueue((ompt_base_t*)region_data, &public_region_freelist);
    }
  }

  // FIXME: vi3 can we remove this
  // FIXME: not using team_master but use another routine to
  // resolve team_master's tbd. Only with tasking, a team_master
  // need to resolve itself
//  if (ompt_task_full_context) {
//    TD_GET(team_master) = 1;
//    thread_data_t* td = hpcrun_get_thread_data();
//    resolve_cntxt_fini(td);
//    TD_GET(team_master) = 0;
//  }


//  printf("PARALLEL END: %d\n", TD_GET(master));


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
//  printf("Parallel end...\n");

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

  // FIXME vi3: check if this is fine
  add_region_and_ancestors_to_stack(region_data, thread_num==0);

  // FIXME vi3: move this to add_region_and_ancestors_to_stack
  // Memoization process vi3:
  if(thread_num != 0){
    not_master_region = region_data;
  }

  // mark that we have task data, but call path is not available
  if (task_data->ptr == NULL) {
    task_data->ptr = cct_node_invalid;
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
  pop_region_stack();
//  printf("IMPLICIT TASK END: master: %d, thread: %p\n", TD_GET(master), &threads_queue);
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




