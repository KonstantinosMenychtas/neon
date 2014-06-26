/***************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON black-box GPU kernel-level management"
*/
/***************************************************************************/
#include <linux/module.h>  // module
#include <linux/slab.h>    // kmalloc
#include <linux/sched.h>   // current
#include <linux/kdebug.h>  // register_die_notifier
#include <linux/mm.h>      // neon_follow_page
#include <linux/vmalloc.h> // vm_map/unmap_ram
#include "neon_help.h"
#include "neon_control.h"
#include "neon_sys.h"

typedef enum {
  RQST_PRE_MAPIN,
  RQST_POST_MMAP,
  RQST_POST_MAPIN,
  RQST_POST_GPUVIEW,
  RQST_UNDEFINED
} rqst_action_t;

/***************************************************************************/
// rqst_safe
/***************************************************************************/
// safely apply ctx/map accessing requests
static int
rqst_safe(unsigned int ctx_key,
          unsigned int map_key,
          rqst_action_t action,
          unsigned long arg)
{
  struct task_struct *cpu_task  = NULL;
  neon_task_t        *neon_task = NULL;
  neon_ctx_t         *ctx       = NULL;
  neon_map_t         *map       = NULL;
  int                 ret       = 0;

  // It is possible that what appears as a legit request
  // does not really belong to the events that describe the
  // state-machine we try to decode; the following points of
  // failure thus are not errors, they just signify requests
  // (ioctl calls) that should indeed not be managed

  cpu_task = current;
  neon_task = cpu_task->neon_task;
  if(unlikely( (neon_task = cpu_task->neon_task) == NULL)) {
    neon_debug("%s : pid %d has no neon task",
               __func__, cpu_task->pid);
    return -1;
  }

  ctx = neon_task_search_ctx(neon_task, ctx_key);
  if(unlikely (ctx == NULL)) {
    neon_debug("%s : ctx 0x%lx not in task %d",
               __func__, ctx_key, neon_task->pid);
    return -1;
  }

  if(map_key != 0) {
    map = neon_ctx_search_map(ctx, map_key, FOR_KEY);
    if(map == NULL) {
      neon_debug("%s : map 0x%lx not in ctx 0x%x",
                 __func__, map_key, ctx_key);
      return -1;
    }
  }

  switch(action) {
  case RQST_PRE_MAPIN:
  case RQST_POST_MMAP:
    map = (neon_map_t *) arg;
    list_add(&map->entry, &ctx->map_list.entry);
    neon_debug("ctx key 0x%x : dev key 0x%x : map key 0x%x : "
               "map \"offset\" 0x%lx : map enlisted",
               map->ctx_key, map->dev_key, map->key, map->offset);
    ret = 0;
    break;
  case RQST_POST_MAPIN:
    map->offset = arg;
    neon_debug("map 0x%x : offset 0x%lx  now set",
               map_key, arg);
    break;
  case RQST_POST_GPUVIEW:
    map->mmio_gpu = arg;
    neon_debug("map 0x%x : mmio_gpu 0x%lx now set",
               map_key, arg);
    ret = 0;
    break;
  default:
    neon_error("%s : unknown rqstaction", __func__);
    ret = -1;
    break;
  }

  return ret;
}

/***************************************************************************/
// neon_rqst_context
/***************************************************************************/
// identify a new context creation ; called before ioctl is managed
int
neon_rqst_pre_context(void *cmd_val)
{
  struct task_struct *cpu_task  = NULL;
  neon_task_t        *neon_task = NULL;
  neon_ctx_t         *c         = NULL;
  neon_ctx_t         *ctx       = NULL;
  unsigned int        ctx_key   = 0;
  unsigned int        method    = 0;

  might_sleep();

  // ioctl id 0x2a dubbed here as new-context in fact
  // identifies that some GPU-method is being applied on a
  // GPU-recognized memory object; we tell apart and handle the calls
  // that help us uniquely identify that a new context is being built

  // only context objects are handled by this call
  method = ((unsigned int *) cmd_val)[NEON_CMD_IDX_METHOD];
  if(method != NEON_ENABLE_GRAPHICS &&
     method != NEON_ENABLE_COMPUTE &&
     method != NEON_ENABLE_OTHER)
    return 0;

  // A GPU context is associated with the process that created it
  cpu_task  = current;
  // A GPU-accessing process (neon_task) can have more than
  // one context (ctx). The same context can be used to schedule
  // both compute and graphics workloads. The ctx-key is a unique
  // identifier
  ctx_key = ((unsigned int *) cmd_val)[NEON_CMD_IDX_KEY_CTX];

  // Look for existing neon task
  neon_task = (neon_task_t *) cpu_task->neon_task;
  if(unlikely(neon_task == NULL)) {
    neon_debug("create new neon-task for cpu-task pid %d ", cpu_task->pid);
    neon_task = neon_task_init((int) cpu_task->pid);
    if(neon_task == NULL) {
      neon_error("%s  : cannot create new neon-task for cpu-task pid %d",
                 __func__, cpu_task->pid);
      return -1;
    } else
      try_module_get(THIS_MODULE);
  }

  // Though NEON_ENABLE_GRAPHICS (0x214) and NEON_ENABLE_COMPUTE (0x204)
  // values indicate clearly a request for a new context, it has
  // been observed (e.g. X server) that new context might be
  // marked first with a NEON_ENABLE_OTHER (0x201) val.
  // Search and confirm we don't recreate an existing context
  list_for_each_entry(c, &neon_task->ctx_list.entry, entry) {
    if(c->key == ctx_key) {
      // not an error, context already exists
      return 0;
    }
  }

  // no context found, it is necessary to create a new one
  ctx = neon_ctx_init(atomic_inc_return(&neon_global.ctx_ever), ctx_key);
  if(ctx == NULL) {
    neon_error("%s : failed to create new ctx", __func__);
    return -1;
  }

  // save context and task

  list_add(&ctx->entry, &neon_task->ctx_list.entry);

  write_lock(&cpu_task->neon_task_rwlock);
  cpu_task->neon_task = neon_task;
  neon_task->nctx++;
  write_unlock(&cpu_task->neon_task_rwlock);

  if(atomic_inc_return(&neon_global.ctx_live) == 1) {
    neon_sched_reset(1);
    register_die_notifier(&nb_die);
  }

  neon_info("NEW CTX 0x%x added, method 0x%x, %d contexts live!",
            ctx_key, method, atomic_read(&neon_global.ctx_live));

  return 0;
}

/***************************************************************************/
// neon_rqst_pre_mapin
/***************************************************************************/
// new pinned or mmapped area; called before ioctl is managed
int
neon_rqst_pre_mapin(int cmd_nr, void *pre_cmd_val)
{
  neon_map_t    *map     = NULL;
  unsigned int   ctx_key = 0;
  unsigned int   dev_key = 0;
  unsigned int   map_key = 0;
  unsigned long  bottom  = 0;
  unsigned long  top     = 0;
  unsigned long  offset  = 0;
  unsigned int   type    = 0;
  int            ret     = 0;

  might_sleep();

  type = ((unsigned int *)  pre_cmd_val)[NEON_CMD_IDX_MAPIN_TYPE];
  if(type != NEON_PIN_USER_PAGES && type != NEON_MMAP_KERNEL_PAGES) {
    neon_info("skip mapin pre rqst: type 0x%x", type);
    return 0;
  }

  ctx_key  = ((unsigned int *) pre_cmd_val)[NEON_CMD_IDX_KEY_CTX];
  dev_key  = ((unsigned int *) pre_cmd_val)[NEON_CMD_IDX_KEY_DEV_GET];
  map_key  = ((unsigned int *) pre_cmd_val)[NEON_CMD_IDX_KEY_MAP_PREP];

  if(type == NEON_PIN_USER_PAGES) {
    bottom   = (unsigned long)                                  \
      ((unsigned int *)pre_cmd_val)[NEON_CMD_IDX_MAPIN_ADDR];
    top      = (unsigned long)                                  \
      ((unsigned int *)pre_cmd_val)[NEON_CMD_IDX_MAPIN_ADDR+1];
    offset   = (bottom | (top << (8 * sizeof(int))));
  }

  map = neon_map_init(ctx_key, dev_key, map_key);
  if (map == NULL)
    return -1;

  if(type == NEON_PIN_USER_PAGES) {
    // for the neon-map of pinned pages, we temporarilly save in the
    // offset param the virtual address corresponding to
    // the vma start_addr; it is used for verification
    map->offset = offset;
  }

  // add map to ctx's map-list (offset preset if this is pinned pages)
  ret = rqst_safe(ctx_key, 0, RQST_PRE_MAPIN, (unsigned long) map);

  neon_info("RQST MAPIN PRE 0x%x - ctx 0x%x : dev 0x%x : "
            "map 0x%x : offset 0x%lx",
            type, ctx_key, dev_key, map_key, offset);

  return ret;
}

/***************************************************************************/
// neon_rqst_post_mapin
/***************************************************************************/
// update mmapped area; called after ioctl is managed
int
neon_rqst_post_mapin(int cmd_nr, void *pre_cmd_val, void *post_cmd_val)
{
  unsigned int   ctx_key = 0;
  unsigned int   dev_key = 0;
  unsigned int   map_key = 0;
  unsigned long  bottom  = 0;
  unsigned long  top     = 0;
  unsigned long  offset  = 0;
  unsigned int   type    = 0;
  int            ret     = 0;

  might_sleep();

  type = ((unsigned int *)  pre_cmd_val)[NEON_CMD_IDX_MAPIN_TYPE];
  if(type != NEON_PIN_USER_PAGES && type != NEON_MMAP_KERNEL_PAGES) {
    neon_info("skip mapin post rqst: type 0x%x", type);
    return 0;
  }

  ctx_key  = ((unsigned int *) pre_cmd_val)[NEON_CMD_IDX_KEY_CTX];
  dev_key  = ((unsigned int *) pre_cmd_val)[NEON_CMD_IDX_KEY_DEV_GET];
  map_key  = ((unsigned int *) pre_cmd_val)[NEON_CMD_IDX_KEY_MAP_PREP];

  bottom   = (unsigned long)                                    \
    ((unsigned int *)post_cmd_val)[NEON_CMD_IDX_MAPIN_ADDR];
  top      = (unsigned long)                                            \
    ((unsigned int *)post_cmd_val)[NEON_CMD_IDX_MAPIN_ADDR+1];
  offset   = (bottom | (top << (8 * sizeof(int))));

  // update saved neon-map's offset
  ret = rqst_safe(ctx_key, map_key, RQST_POST_MAPIN, offset);

  neon_debug("RQST MAPIN POST 0x%x - ctx 0x%x : dev 0x%x : "
             "map 0x%x : offset 0x%lx",
             type, ctx_key, dev_key, map_key, offset);

  return ret;
}

/***************************************************************************/
// neon_rqst_post_mmap
/***************************************************************************/
// new mmapped area ; called after ioctl is managed
int
neon_rqst_post_mmap(int cmd_nr, void *pre_cmd_val, void *post_cmd_val)
{
  unsigned int   ctx_key = 0;
  unsigned int   dev_key = 0;
  unsigned int   map_key = 0;
  unsigned long  bottom  = 0;
  unsigned long  top     = 0;
  unsigned long  offset  = 0;
  neon_map_t    *map     = NULL;
  int            ret     = 0;

  might_sleep();

  // necessary to have pre-mmap call to get dev-key
  ctx_key  = ((unsigned int *) pre_cmd_val)[NEON_CMD_IDX_KEY_CTX];
  dev_key  = ((unsigned int *) pre_cmd_val)[NEON_CMD_IDX_KEY_DEV_GET];
  map_key  = ((unsigned int *) pre_cmd_val)[NEON_CMD_IDX_KEY_MAP_PREP];


  bottom = (unsigned long)                                      \
    ((unsigned int *)post_cmd_val)[NEON_CMD_IDX_MMAP_ADDR];
  top = (unsigned long)                                         \
    ((unsigned int *)post_cmd_val)[NEON_CMD_IDX_MMAP_ADDR+1];
  offset = (bottom | (top << (8 * sizeof(int))));

  map =  neon_map_init(ctx_key, dev_key, map_key);
  if(map == NULL)
    return -1;

  map->offset = offset;

  // add map to ctx's map-list with pre-saved offset
  ret = rqst_safe(ctx_key, 0, RQST_POST_MMAP, (unsigned long) map);

  neon_info("RQST MMAP POST - ctx 0x%x : dev 0x%x : "
            "map 0x%x : offset 0x%lx",
            ctx_key, dev_key, map_key, offset);

  return ret;
}

/***************************************************************************/
// neon_rqst_post_gpuview
/***************************************************************************/
// update a neon-map of a memory-mapped (mmap) or memory-pinned
// (get_user_pages) vma; called after ioctl is managed
int
neon_rqst_post_gpuview(int cmd_nr, void *pre_cmd_val, void *post_cmd_val)
{
  unsigned int   ctx_key  = 0;
  unsigned int   map_key  = 0;
  unsigned long  bottom   = 0;
  unsigned long  top      = 0;
  unsigned long  mmio_gpu = 0;
  int            ret      = 0;

  ctx_key  = ((unsigned int *) pre_cmd_val)[NEON_CMD_IDX_KEY_CTX];
  map_key  = ((unsigned int *) pre_cmd_val)[NEON_CMD_IDX_KEY_MAP_UPDT];

  bottom = (unsigned long)                                      \
    ((unsigned int *)post_cmd_val)[NEON_CMD_IDX_MMIO_ADDR];
  top = (unsigned long)                                         \
    ((unsigned int *)post_cmd_val)[NEON_CMD_IDX_MMIO_ADDR+1];
  mmio_gpu = (bottom | (top << (8 * sizeof(int))));

  neon_info("RQST MAP GPU_VIEW - ctx key 0x%x : map key 0x%x : "
            "mmio-gpu 0x%lx", ctx_key, map_key, mmio_gpu);

  // set GPU's view of map
  ret = rqst_safe(ctx_key, map_key, RQST_POST_GPUVIEW, mmio_gpu);

  return ret;
}

/***************************************************************************/
// neon_uptr_read
/***************************************************************************/
// Read the int value contained in some user-space virtual address
unsigned int
neon_uptr_read(const unsigned int pid,
               struct vm_area_struct * vma,
               const unsigned long ptr)
{
  struct page   *page     = 0;
  unsigned int   page_ofs = 0;
  unsigned long  kvaddr   = 0;
  unsigned int   val      = 0;

  if(current->pid == pid) {
    val = *((unsigned int*) ptr);
    neon_debug("SAFE: CURRENT address space translation "
                "[%d===%d]: val = 0x%x",
                current->pid, pid, val);
    return val;
  }

  page     = neon_follow_page(vma, ptr);
  page_ofs = ptr & ~PAGE_MASK;
  if(page_ofs + sizeof(int) <= PAGE_SIZE) {
    // we choose vm_map_ramp over get_user_pages+kmap only to be on the
    // safe side wrt availability of kernel logival addresses;
    // this is 99% unnecessary but works, so no reason to change it
    kvaddr = (unsigned long) vm_map_ram(&page, 1,-1, PAGE_KERNEL);
    val = *((unsigned int *) (kvaddr + page_ofs));
    neon_debug("SAFE : uv 0x%lx --page-> [0x%lx, 0x%lx] "
                "--kv-> *[0x%lx] = 0x%x",
                ptr, page, page_ofs, kvaddr, val);
    vm_unmap_ram((void *) kvaddr, 1);
  }
  else {
    // assuming that cb values are at least page aligned
    // has proven correct so far
    neon_error("%s : SAFE : uv 0x%lx --page-> [0x%lx, 0x%lx] "
              "+ sizeof(int)=0x%x > 0x%x (PAGE_SIZE)",
              __func__, page_ofs, sizeof(int), PAGE_SIZE);
    BUG();
  }

  neon_debug("SAFE: FOREIGN address space translation "
             "[%d=/=%d]: val = 0x%x", current->pid, pid, val);

  return val;
}

/***************************************************************************/
// tesla_refc_eval
/***************************************************************************/
// Using a pointer to the end of a command set, find the associated
// reference counter's address and value (1) for a tesla device
int
tesla_refc_eval(const unsigned int cb_pid,
                struct vm_area_struct * cb_vma,
                const unsigned int workload,
                const unsigned long * const cmd_tuple,
                unsigned long * const refc_addr_val)
{
  const unsigned long cmd_start = cmd_tuple[0];
  const unsigned long cmd_size  = cmd_tuple[1];
  unsigned long cmd_end = cmd_start + cmd_size - 0x6;
  unsigned long ptr     = 0;
  unsigned int  val     = 0;

  // NOTE: this is a system-sensitive translation and a likely part
  // of the trace analysis to fail in new systems. Address-dependent
  // invariances have been ruled out in our own system.

  if(workload == NEON_WORKLOAD_COMPUTE) {
    if(cmd_size - 0x6 < 4 * sizeof(int)) {
      refc_addr_val[0]=0xB16;
      refc_addr_val[1]=0xB00B1E5;
      return -1;
    }
    ptr = cmd_end - 4 * sizeof(int);
    val = neon_uptr_read(cb_pid, cb_vma, ptr);
    if(val == 0x104310 || val == 0x100010) {
      refc_addr_val[0] = neon_uptr_read(cb_pid, cb_vma,
                                        ptr + 2 * sizeof(int));
      refc_addr_val[1] = neon_uptr_read(cb_pid, cb_vma,
                                        ptr + 3 * sizeof(int));    
    } else {
      if(cmd_size - 0x6 < 8 * sizeof(int)) {
        refc_addr_val[0]=0x2B16;
        refc_addr_val[1]=0xB00B1E5;
        return -1;
      }
      ptr = cmd_end - 8 * sizeof(int);
      val = neon_uptr_read(cb_pid, cb_vma, ptr);
      if(val == 0x100010) {
        refc_addr_val[0] = neon_uptr_read(cb_pid, cb_vma,
                                          ptr + 2 * sizeof(int));
        refc_addr_val[1] = neon_uptr_read(cb_pid, cb_vma,
                                          ptr + 3 * sizeof(int));    
      } else {
        refc_addr_val[0]=0xDEAD;
        refc_addr_val[1]=0xC0DE;
        return -1;
      }
    }
  }

  return 0;
}

/***************************************************************************/
// kepler_refc_eval
/***************************************************************************/
// Using a pointer to the end of a command set, find the associated
// reference counter's address and value (1) for a kepler device
int
kepler_refc_eval(const unsigned int cb_pid,
                 struct vm_area_struct * cb_vma,
                 const unsigned int workload,
                 const unsigned long * const cmd_tuple,
                 unsigned long * const refc_addr_val)
{
  const unsigned long cmd_start = cmd_tuple[0];
  const unsigned long cmd_size  = cmd_tuple[1];

  // NOTE: this is a system-sensitive translation and a likely part
  // of the trace analysis to fail in new systems. Address-dependent
  // invariances have been ruled out in our own system by all possible
  // means (swapping, enabling, disabling available gpus)

  unsigned long cmd_end = 0;
  unsigned long ptr     = 0;
  unsigned int  val     = 0;
  unsigned long bottom  = 0;
  unsigned long top     = 0;

  if(workload == NEON_WORKLOAD_COMPUTE) {
    cmd_end = cmd_start + cmd_size - 0x6;
    if(cmd_size - 0x6 < 4 * sizeof(int)) {
      refc_addr_val[0]=0xB16;
      refc_addr_val[1]=0xB00B1E5;
      return -1;
    }
    ptr = cmd_end - 4 * sizeof(int);
    val = neon_uptr_read(cb_pid, cb_vma, ptr);
    if(val == 0x200426c0) {
      top    = neon_uptr_read(cb_pid, cb_vma, ptr + 1 * sizeof(int));
      bottom = neon_uptr_read(cb_pid, cb_vma, ptr + 2 * sizeof(int));
      refc_addr_val[0] = (bottom | (top << (8 * sizeof(int))));
      refc_addr_val[1] = neon_uptr_read(cb_pid, cb_vma,
                                        ptr + 3 * sizeof(int));
#ifdef NEON_KERNEL_CALL_COUNTING
      // this invariance appears to be associated specifically
      // with compute requests ---- they happen in triplets,
      // this invariant appears in the second request (while
      // the first request carries the actual computation)
      val = neon_uptr_read(cb_pid, cb_vma, ptr - 1 * sizeof(int)); 
      if(val == 3)
        return 1;
#endif // NEON_KERNEL_CALL_COUNTING
      return 0;
    } else {
      if(cmd_size - 0x6 < 7 * sizeof(int)) {
        refc_addr_val[0]=0x2B16;
        refc_addr_val[1]=0xB00B1E5;
        return -1;
      }
      ptr = cmd_end - 7 * sizeof(int);
      val = neon_uptr_read(cb_pid, cb_vma, ptr);
      if(val == 0x20018090) {
        top    = neon_uptr_read(cb_pid, cb_vma,
                                ptr + 1 * sizeof(int));
        bottom = neon_uptr_read(cb_pid, cb_vma,
                                ptr + 3 * sizeof(int));
        refc_addr_val[0] = (bottom | (top << (8 * sizeof(int))));
        refc_addr_val[1] = neon_uptr_read(cb_pid, cb_vma,
                                          ptr + 5 * sizeof(int));
      } else {
        if(val == 0x200180c0) {
          if(cmd_size - 0x6 < 13 * sizeof(int)) {
            refc_addr_val[0]=0x22B16;
            refc_addr_val[1]=0xB00B1E5;
            return -1;
          }
          ptr    = cmd_end - 13 * sizeof(int);
          top    = neon_uptr_read(cb_pid, cb_vma,
                                  ptr + 1 * sizeof(int));
          bottom = neon_uptr_read(cb_pid, cb_vma,
                                  ptr + 3 * sizeof(int));
          refc_addr_val[0] = (bottom | (top << (8 * sizeof(int))));
          refc_addr_val[1] = neon_uptr_read(cb_pid, cb_vma,
                                            ptr + 5 * sizeof(int));
        } else {
          refc_addr_val[0]=0xDEAD;
          refc_addr_val[1]=0xC0DE;
          return -1;
        }
      }
    }
  } else if(workload == NEON_WORKLOAD_GRAPHICS) {
    cmd_end = cmd_start + cmd_size - 0x4;
/* #ifdef NEON_DEBUG_LEVEL_5 */
/*     if(cb_pid == current->pid) { */
/*       unsigned int  i = 0; */
/*       for ( i = 0; i  <  0x30 ; i += sizeof(int)) { */
/*         ptr  = cmd_end - i; */
/*         val  = *((unsigned int *) ptr); */
/*         neon_verbose("----> steps 0x%ld : ptr 0x%lx : val 0x%lx ", */
/*                      i, ptr, val); */
/*       } */
/*     } */
/* #endif // NEON_DEBUG_LEVEL_5 */
    if(cmd_size - 0x4 < 4 * sizeof(int)) {
      refc_addr_val[0]=0xB16;
      refc_addr_val[1]=0xB00B1E5;
      return -1;
    }
    ptr = cmd_end - 4 * sizeof(int);
    val = neon_uptr_read(cb_pid, cb_vma, ptr);
    if(val == 0x200406c0) {
      top    = neon_uptr_read(cb_pid, cb_vma, ptr + 1 * sizeof(int));
      bottom = neon_uptr_read(cb_pid, cb_vma, ptr + 2 * sizeof(int));
      refc_addr_val[0] = (bottom | (top << (8 * sizeof(int))));
      refc_addr_val[1] = neon_uptr_read(cb_pid, cb_vma,
                                        ptr + 3 * sizeof(int));
#ifdef NEON_KERNEL_CALL_COUNTING
      // this invariance appears to be associated specifically
      // with compute requests 
      // TODO : verify, has only been verified for some compute requests
      val = neon_uptr_read(cb_pid, cb_vma, ptr - 1 * sizeof(int)); 
      if(val == 3)
        return 1;
#endif // NEON_KERNEL_CALL_COUNTING
    }
  }

  return 0;
}
