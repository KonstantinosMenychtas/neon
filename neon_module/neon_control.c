/***************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON black-box GPU channel management"
*/
/***************************************************************************/

#include <linux/list.h>     // lists
#include <asm/atomic.h>     // atomics
#include <linux/slab.h>     // kmalloc/kzalloc
#include <linux/sched.h>    // current
#include <linux/vmalloc.h>  // vmalloc
#include "neon_help.h"
#include "neon_core.h"
#include "neon_control.h"
#include "neon_sys.h"
#include "neon_track.h"
#include "neon_sched.h"

/**************************************************************************/
// neon_map_init
/**************************************************************************/
// init a new memory mapping struct (not ready for tracking)
neon_map_t *
neon_map_init(unsigned int  ctx_key,
              unsigned int  dev_key,
              unsigned int  map_key)
{
  neon_map_t *map = NULL;

  might_sleep();

  neon_info("ctx 0x%lx : dev 0x%lx : map 0%lx : init",
            ctx_key, dev_key, map_key);
   
  map = (neon_map_t *) kzalloc(sizeof(neon_map_t), GFP_KERNEL);
  if(map == NULL) {
    neon_error("%s : failed to init map : "
               "key 0x%lx : ctx 0x%lx dev : 0x%lx",
               map_key, ctx_key, dev_key);
    return NULL;
  }

  // In order to support multiple contexts, we need to make sure
  // we properly associate contexts to ioctl and other system calls
  // that might refer to them. We use unique identifiers from the ioctl
  // vals to associate any identifiable GPU object (memory area) with
  // a context, device and memory-map.
  map->key     = map_key;
  map->ctx_key = ctx_key;
  map->dev_key = dev_key;
  INIT_LIST_HEAD(&map->entry);

  // the rest of the map struct will be updated progressively,
  // by appropriate ioctl and pin-pages/mmap and fault, pages
  // will be alloced for tracking only if necessary

  return map;
}

/**************************************************************************/
// neon_map_fini
/**************************************************************************/
// finalize and cleanup a neon map struct in specified context
int
neon_map_fini(neon_ctx_t * const ctx,
              neon_map_t * const map)
{
  int ret = 0;

  neon_info("ctx 0x%x : map 0x%x : fini", map->ctx_key, map->key);
  
  // stop memory access tracking, if not already
  if(map->fault != NULL) {
    if(neon_track_stop(map) != 0) {
      neon_warning("%s:  map_key 0x%x : tracking in progress",
                   __func__, map->key);
      ret = -1;
    }
  }

  // withdraw any related work from scheduling and clean up related entries
  if(!list_empty(&ctx->work_list.entry)) {
    struct list_head *pos  = NULL;
    struct list_head *q    = NULL;
    neon_work_t      *work = NULL;
    list_for_each_safe(pos, q, &ctx->work_list.entry) {
      work = list_entry(pos, neon_work_t, entry);
      if(map == work->ir ||
         map == work->rb ||
         //         map == work->cb ||
         map == work->rc) {
        /* neon_map_print(map); */
        /* neon_map_print(work->ir); */
        /* neon_map_print(work->rb); */
        /* neon_map_print(work->cb); */
        /* neon_map_print(work->rc); */
        neon_work_stop(work);
        if (neon_work_fini(work) != 0) {
          neon_warning("map 0x%lx : work @ did %d chan %d : "
                       "pending completion notification",
                       map->key, work->did, work->cid);
          ret = -1;
        }
        list_del_init(pos);
        kfree(work);
      }
    }
  }

  //  free fault/page entries
  if(map->fault != NULL) {
    list_del_init(&map->fault->entry);
    neon_track_fini(map);
    kfree(map->fault);
  }

  return ret;
}

/**************************************************************************/
// neon_map_print
/**************************************************************************/
// print a neon map struct
void
neon_map_print(const neon_map_t * const map)
{
  if(map == NULL) {
    neon_error("map key 0x0 : cannot print NULL map");
    return;
  }
    
  neon_info("map key 0x%lx : ctx 0x%lx : dev 0x%lx : "
            "sz 0x%lx : ofs 0x%lx : gpu 0x%lx : vma @ 0x%p : fault...",
            map->key, map->ctx_key, map->dev_key,
            map->size, map->offset, map->mmio_gpu, map->vma);
  if(map->fault != NULL) 
    neon_fault_print(map->fault);
  
  return;
}

/**************************************************************************/
// neon_ctx_init
/**************************************************************************/
// create and initialize a new context
neon_ctx_t *
neon_ctx_init(unsigned int id, unsigned int ctx_key)
{
  neon_ctx_t *ctx = NULL;

  neon_info("ctx 0x%x : init", ctx_key);

  ctx = (neon_ctx_t *) kzalloc(sizeof(neon_ctx_t), GFP_KERNEL);
  if(ctx == NULL) {
    neon_error("%s: ctx init failed", __func__);
    return NULL;
  }

  ctx->id = id;
  ctx->key = ctx_key;
  INIT_LIST_HEAD(&ctx->map_list.entry);
  INIT_LIST_HEAD(&ctx->work_list.entry);
  INIT_LIST_HEAD(&ctx->fault_list.entry);
  INIT_LIST_HEAD(&ctx->entry);

  return ctx;
}

/**************************************************************************/
// neon_ctx_fini
/**************************************************************************/
// finalize and cleanup context
int
neon_ctx_fini(neon_ctx_t * const ctx)
{
  struct list_head *pos = NULL;
  struct list_head *q   = NULL;
  int               ret = 0;

  neon_info("ctx 0x%x : fini", ctx->key);

  // remove all maps in context
  if(!list_empty(&ctx->map_list.entry)) {
    list_for_each_safe(pos, q, &ctx->map_list.entry) {
      neon_map_t *map = NULL;
      map = list_entry(pos, neon_map_t, entry);
      if (neon_map_fini(ctx, map) != 0) {
        ret = -1;
        neon_warning("ctx 0x%lx : map 0x%lx : unclean map fini",
                     ctx->key, map->key);
      }
      list_del_init(pos);
      kfree(map);
    }
  }

  return ret;
}

/***************************************************************************/
// neon_ctx_search_map
/***************************************************************************/
// find map in ctx list
neon_map_t *
neon_ctx_search_map(neon_ctx_t *ctx,
                    unsigned long arg,
                    neon_map_search_t type)
{
  neon_map_t *map = NULL;

  if(unlikely(list_empty(&ctx->map_list.entry))) {
    neon_debug("%s : ctx 0x%x has empty map list", __func__, ctx->key);
    return NULL;
  }

  list_for_each_entry(map, &ctx->map_list.entry, entry) {
    switch(type) {
    case FOR_KEY:
      if((unsigned long) map->key == arg)
        return map;
      break;
    case FOR_VMA:
      if(map->vma != NULL && map->vma->vm_start == arg)
        return map;
      break;
    case FOR_OFFSET_PRECISE:
      if(map->offset == arg)
        return map;
      break;
    case FOR_OFFSET_ALIGNED:
      if((map->offset - (map->offset % PAGE_SIZE)) == arg)
        return map;
      break;
    case FOR_PINNED_PAGES:
      if(((unsigned long) map->pinned_pages) == arg)
        return map;
      break;
    default:
      neon_error("search for map by type %d not supported", type);
      return NULL;
      break;
    }
  }

  return NULL;
}

/**************************************************************************/
// neon_ctx_print
/**************************************************************************/
// print out neon_ctx struct
void
neon_ctx_print(const neon_ctx_t * const ctx)
{
  neon_work_t  *work  = NULL;
  neon_map_t   *map   = NULL;
  neon_fault_t *fault = NULL;

  neon_info("ctx key 0x%x : id %d : faults ...",
            ctx->key, ctx->id, list_empty(&ctx->fault_list.entry));
  list_for_each_entry(fault, &ctx->fault_list.entry, entry)
    neon_fault_print(fault);

  neon_info("ctx key 0x%x : id %d : works ...", ctx->key, ctx->id);
  list_for_each_entry(work, &ctx->work_list.entry, entry)
    neon_work_print(work);

  neon_info("ctx key 0x%x : id %d : maps ... ", ctx->key, ctx->id);
  list_for_each_entry(map, &ctx->map_list.entry, entry)
    neon_map_print(map);

  return;
}

/**************************************************************************/
// neon_task_init
/**************************************************************************/
// create and initialize a neon-task
neon_task_t *
neon_task_init(unsigned int pid)
{
  neon_task_t *task = NULL;

  might_sleep();

  neon_info("neon task @ pid %d init", pid);
  
  task =(neon_task_t *) kzalloc(sizeof(neon_task_t), GFP_ATOMIC);
    
  if(task == NULL) {
    neon_error("%s: task init failed", __func__);    
    return NULL;
  }
  
  task->pid = pid;
  task->sharers = 0;
  task->malicious = 0;
  task->nctx = 0;
  INIT_LIST_HEAD(&task->ctx_list.entry);

  neon_debug("neon init - new GPU-accessing task %d", task->pid);

  return task;
}

/**************************************************************************/
// neon_task_fini
/**************************************************************************/
// finilize and cleanup a neon-process control struct
int
neon_task_fini(neon_task_t * const task)
{
  neon_ctx_t       *ctx = NULL;
  struct list_head *pos = NULL;
  struct list_head *q   = NULL;
  int               ret = 0;

  neon_info("neon task %d accessing GPU fini", task->pid);
  
  // go over the list of contexts and get rid of them
  list_for_each_safe(pos, q, &task->ctx_list.entry) {
    ctx = list_entry(pos, neon_ctx_t, entry);
    ret |= neon_ctx_fini(ctx);
    list_del_init(pos);
    kfree(ctx);
  }

  return ret;
}

/***************************************************************************/
// neon_task_search_ctx
/***************************************************************************/
// find ctx in neon-task's ctx-list
neon_ctx_t *
neon_task_search_ctx(neon_task_t *task,
                     unsigned int ctx_key)
{
  neon_ctx_t *ctx = NULL;

  if(unlikely(list_empty(&task->ctx_list.entry))) {
    neon_warning("%s : pid %d has empty ctx list",
                 __func__, task->pid);
    return NULL;
  }

  list_for_each_entry(ctx, &task->ctx_list.entry, entry) {
    if(ctx->key == ctx_key)
      return ctx;
  }

  return NULL;
}

/**************************************************************************/
// neon_task_print
/**************************************************************************/
// print out neon_task struct
void
neon_task_print(const neon_task_t * const neon_task)
{
  neon_ctx_t *ctx = NULL;

  neon_info("neon task : pid %d : %d sharers : %d ctxs ...",
            neon_task->pid, neon_task->sharers, neon_task->nctx);
  list_for_each_entry(ctx, &neon_task->ctx_list.entry, entry)
    neon_ctx_print(ctx);

  return;
}
