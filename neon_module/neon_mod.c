/***************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/***************************************************************************/

#include <linux/mm.h>        // VM_DONTCOPY
#include <linux/slab.h>      // kalloc
#include <linux/sched.h>     // current
#include <linux/module.h>    // module
#include <linux/kdebug.h>    // unregister_die_notifier
#include <linux/semaphore.h> // down
#include <linux/wait.h>      // waitqueue
#include <neon/neon_face.h>  // neon interface
#include "neon_help.h"
#include "neon_core.h"
#include "neon_control.h"
#include "neon_sys.h"
#include "neon_track.h"
#include "neon_sched.h"
#include "neon_ui.h"

/****************************************************************************/
// Early declarations
static void neon_unmap_vma(struct vm_area_struct *vma);

/****************************************************************************/
// globally accessible neon struct containing device list and
// some simple global statistics
neon_global_t neon_global;

/***************************************************************************/
// neon_pre_ioctl
/***************************************************************************/
// Interface: Neon/Driver
// nvidia-driver/neon-module interface : ioctl call
static int
neon_ioctl(int   cmd_nr,
           void *pre_cmd_val,
           void *post_cmd_val)
{
  int retval = 0;
  void *cmd_val = NULL;

  if ( post_cmd_val == NULL )
    cmd_val = pre_cmd_val;
  else
    cmd_val = post_cmd_val;

#ifdef NEON_TRACE_REPORT
  if (cmd_nr == NEON_RQST_CTX  || cmd_nr == NEON_RQST_UPDT ||
      cmd_nr == NEON_RQST_MMAP || cmd_nr == NEON_RQST_MAPIN || cmd_nr == 0x52)
    neon_info("%s-IOCTL id:0x%x "
              "[0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, "
              "0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x]",
              (post_cmd_val == NULL) ? "PRE--" : "POST-",
              cmd_nr,
              ((unsigned int *) cmd_val)[0],
              ((unsigned int *) cmd_val)[1],
              ((unsigned int *) cmd_val)[2],
              ((unsigned int *) cmd_val)[3],
              ((unsigned int *) cmd_val)[4],
              ((unsigned int *) cmd_val)[5],
              ((unsigned int *) cmd_val)[6],
              ((unsigned int *) cmd_val)[7],
              ((unsigned int *) cmd_val)[8],
              ((unsigned int *) cmd_val)[9],
              ((unsigned int *) cmd_val)[10],
              ((unsigned int *) cmd_val)[11]);
  else
    neon_info("%s-IOCTL id:0x%x [0x%x, 0x%x, 0x%x]",
              (post_cmd_val == NULL) ? "PRE--" : "POST-",
              cmd_nr,
              ((unsigned int *) cmd_val)[0],
              ((unsigned int *) cmd_val)[1],
              ((unsigned int *) cmd_val)[2]);
#endif // NEON_TRACE_REPORT

  // Relative sequence of operations of interest in the trace:
  // 1. create context
  // 2. map buffer containing reference counter
  // 3. ring buffer [entries = (start, size) touples point to cb]
  // Tracking accesses in virtual memory areas, identified to be of
  // two kinds : pinned user pages and mmapped pages. Mark them using :
  // a) ioctl calls right before the mmap/pin requests,
  // b) the actual calls to mmap/get_user_pages and
  // c) ioctl requests right after the calls.

  if (post_cmd_val == NULL) {
    // reached by the nvidia module before the ioctl
    // request is passed to the blob and handled

    // new context request
    if (cmd_nr == NEON_RQST_CTX)
      retval = neon_rqst_pre_context(pre_cmd_val);

    // build a new neon-map for mmapped or pinned pages
    if (cmd_nr == NEON_RQST_MAPIN)
      retval = neon_rqst_pre_mapin(cmd_nr, pre_cmd_val);
  } else {
    // post_cmd_val contains values set by the blob
    // i.e. value-return entries set by the callee (system) not the caller
    // (library/user-level)
    // we reach here when after the nvidia module ioctl request is handled

    // update an existing neon-map for pinned or mmapped pages
    if (cmd_nr == NEON_RQST_MAPIN)
      retval = neon_rqst_post_mapin(cmd_nr, pre_cmd_val, post_cmd_val);

    // build a neon-map for mmapped pages
    if (cmd_nr == NEON_RQST_MMAP)
      retval = neon_rqst_post_mmap(cmd_nr, pre_cmd_val, post_cmd_val);

    // update vma's gpu-view ("mmio_gpu")
    if (cmd_nr == NEON_RQST_UPDT)
      retval = neon_rqst_post_gpuview(cmd_nr, pre_cmd_val, post_cmd_val);
  }

  return retval;
}

/***************************************************************************/
// neon_map_pages
/***************************************************************************/
// Interface: Neon/Driver
// map pages:
static int
neon_map_pages(struct vm_area_struct *vma,
               unsigned long addr,
               unsigned long offset,
               unsigned long size,
               neon_map_area_t area)
{
  struct task_struct *cpu_task  = NULL;
  neon_task_t        *neon_task = NULL;
  neon_ctx_t         *ctx       = NULL;
  neon_map_t         *map       = NULL;
  neon_work_t        *work      = NULL;
  int                 ret       = 0;

  cpu_task = current;
  neon_task = cpu_task->neon_task;
  if(unlikely(neon_task == NULL)) {
    neon_error("%s : pid %d has no neon task", __func__, cpu_task->pid);
    return -1;
  }

  neon_debug("TRY map_vma : vma 0x%p addr 0x%lx "
             "offset 0x%lx size 0x%lx (%d pages)",
             vma, addr, offset, size, (size/PAGE_SIZE));

  // find map entry to update
  list_for_each_entry(ctx, &neon_task->ctx_list.entry, entry) {
    map = neon_ctx_search_map(ctx, offset, FOR_OFFSET_ALIGNED);
    if(map != NULL)
      break;
  }
  if(map == NULL) {
    neon_error("%s : ARGH! trace misunderstood, can't find map after mmap",
               __func__);
    //    BUG();
    return -1;
  }

#ifndef NEON_TRACE_REPORT
  // if this is an index register, we 're going to set up a new work
  // to use for scheduling
  work = neon_work_init(neon_task, ctx, map);
#endif // !NEON_TRACE_REPORT

  // page manipulation is easier if we don't have to worry about
  // copying pages around; this has proved safe
  vma->vm_flags |= VM_DONTCOPY;

  // update map
  map->vma = vma;
  map->size = size;

#ifndef NEON_TRACE_REPORT
  // track acceses only to index registers ; enough for scheduling
  if(work != NULL) {
#endif // !NEON_TRACE_REPORT
    if(neon_track_init(map) != 0) {
      neon_error("%s : cannot init tracking for map 0x%x",
                 __func__, map->key);
      return -1;
    } else
      ret = neon_track_start(map);
#ifndef NEON_TRACE_REPORT
  }
#endif // !NEON_TRACE_REPORT
  if(ret != 0) {
    neon_error("%s : cannot start tracking on map 0x%x",
               __func__, map->key);
    return -1;
  }

  if(work != NULL) {
    list_add(&work->entry, &ctx->work_list.entry);
    if(neon_work_start(work) != 0) {
      neon_error("%s : cannot start work related to map 0x%x",
                 __func__, map->key);
      return -1;
    }
  }

  neon_info("pid %d : ctx 0x%x : dev 0x%x : map 0x%x : area 0x%x : "
            "ofs : 0x%x vm_start 0x%lx : "
            "mmio_gpu 0x%lx : size 0x%x : mmapped",
            cpu_task->pid, map->ctx_key, map->dev_key, map->key,
            area, map->offset, map->vma->vm_start,
            map->mmio_gpu, map->size);

  return ret;
}

/****************************************************************************/
// neon_pin_pages
/****************************************************************************/
// Interface: Neon/Driver
// pin pages: lock user pages in memory (--> pci_map_page)
static int
neon_pin_pages(void *user_address,
               struct page **pinned_pages,
               unsigned long long *pte_array,
               unsigned long long nr_pages)
{
  struct task_struct    *cpu_task  = NULL;
  neon_task_t           *neon_task = NULL;
  neon_ctx_t            *ctx       = NULL;
  neon_map_t            *map       = NULL;
  struct vm_area_struct *vma       = NULL;
  unsigned long          vmaofs    = 0;

  cpu_task = current;
  neon_task = cpu_task->neon_task;
  if(unlikely(neon_task == NULL)) {
    neon_error("%s : pid %d has no neon task", __func__, cpu_task->pid);
    return -1;
  }

  neon_debug("TRY pin %d pages, pin-array @ 0x%p, user-addr @0x%p",
             nr_pages, pinned_pages, user_address);

  vma = find_vma(cpu_task->mm, (unsigned long) user_address);
  // page manipulation is easier if we don't have to worry about
  // copying pages around; this has proved safe
  vma->vm_flags |= VM_DONTCOPY;

  // find map entry to update
  list_for_each_entry(ctx, &neon_task->ctx_list.entry, entry) {
    map = neon_ctx_search_map(ctx, (unsigned long) user_address,
                              FOR_OFFSET_PRECISE);
    if(map != NULL)
      break;
  }
  if(map == NULL) {
    neon_error("%s : cannot find map for pinned vma @ 0x%lx",
               __func__, vma->vm_start);
    return -1;
  }

  map->vma = vma;
  map->size = nr_pages * PAGE_SIZE;
  map->pinned_pages = pinned_pages;
  map->offset = 0; // tells pinned areas from mmapped areas

  // Pinned vmas might be mapped in chunks --- 5 pages has been observed to
  // be a common sub-vma-size requested to be pinned. Tracking R/W to these areas,
  // which are not always starting at vma->vm_start, requires adding extra
  // contextual information in the map_t. Since we have only observed 0-value R/Ws
  // to this areas, and they are not critical for request submit/complete events,
  // we will skip tracking them. Note that vmaofs could be passed to track_start
  // and with a proper modification of map_t, these areas can be tracked too.
  vmaofs = (((unsigned long) user_address) - vma->vm_start) / PAGE_SIZE;
  vmaofs *= PAGE_SIZE;
  if(vma->vm_start + vmaofs + map->size > vma->vm_end) {
    neon_error("%s : wrong assumption about pinned vma tracking with offset",
               __func__);
    return -1;
  }

#ifdef NEON_TRACE_REPORT
  // trace collecting mode
  // track of accesses to all maps can generate massive traces;
  if(vmaofs == 0 && neon_track_init(map) != 0) {
    neon_error("%s : cannot init tracking for map 0x%lx", map->key);
    return -1;
  }
  // don't track non-vm_start-aligned areas (extra work for mapping accesses
  // to those deemed unnecessary, given that values have been observed to be
  // always 0 when tracked [observed only 5-page maps]).
  if(vmaofs == 0 && neon_track_start(map) != 0) {
    neon_error("%s : cannot start tracking on map 0x%lx", map->key);
    return -1;
  }
#endif // NEON_TRACE_REPORT

  neon_info("ctx 0x%x : dev 0x%x : map 0x%x : "
            "ofs 0x%x : vm_start 0x%lx : vm_end 0x%lx "
            "mmio_gpu 0x%lx : size 0x%x : vmaofs 0x%lx : pinned",
            map->ctx_key, map->dev_key, map->key,
            map->offset, map->vma->vm_start, map->vma->vm_end,
            map->mmio_gpu, map->size, vmaofs);

  return 0;
}

/****************************************************************************/
// neon_unpin_pages
/****************************************************************************/
// Interface: Neon/Driver
// unpin pages: unlock previously pinned user pages from memory
static int
neon_unpin_pages(struct page **pinned_pages,
                 unsigned long long *pte_array,
                 unsigned long long nr_pages)
{
  struct task_struct *cpu_task  = NULL;
  neon_task_t        *neon_task = NULL;
  neon_ctx_t         *ctx       = NULL;
  neon_map_t         *map       = NULL;
  int                 ret       = 0;

  cpu_task = current;
  neon_task = cpu_task->neon_task;
  if(neon_task == NULL) {
    // not an error - unmap_vma must have succeeded in removing the vma
    neon_debug("%s : pid %d has no neon task", __func__, cpu_task->pid);
    return 0;
  }

  neon_debug("TRY unpin %d pages, pin-array @ 0x%p",
             nr_pages, pinned_pages);

  // find map entry to remove
  list_for_each_entry(ctx, &neon_task->ctx_list.entry, entry) {
    map = neon_ctx_search_map(ctx, (unsigned long) pinned_pages,
                              FOR_PINNED_PAGES);
    if(map != NULL)
      break;
  }
  if(map == NULL) {
    // unlike map-vma, pinned pages are captured directly from
    // the driver interface, so not finding one is an error
    neon_error("%s : cannot find map for pinned pages @ 0x%lx",
               __func__, pinned_pages);
    return -1;
  }

  // found a map, carefully remove it from all lists it
  // might participate in
  ret = neon_map_fini(ctx, map);
  if(ret != 0) {
    neon_info("ctx 0x%lx : dev 0x%lx : map 0x%lx : fini failed",
              map->ctx_key, map->dev_key, map->key);
    return -1;
  }

  list_del_init(&map->entry);
  kfree(map);

  return 0;
}

/***************************************************************************/
// neon_unmap_vma
/***************************************************************************/
// Interface: Neon/Linux
// vma unmapping --> cleanly remove associated map
static void
neon_unmap_vma(struct vm_area_struct *vma)
{
  struct task_struct *cpu_task  = NULL;
  neon_task_t        *neon_task = NULL;
  neon_ctx_t         *ctx       = NULL;
  neon_map_t         *map       = NULL;
  int                 ret       = 0;

  cpu_task = current;
  neon_task = cpu_task->neon_task;
  if(likely(neon_task == NULL)) {
    // this is not an error : unmap-vma can concern a request
    // to unmap a vma that has been established before the
    // neon task and context were created
    return;
  }

  neon_debug("TRY unmap_vma : vma 0x%p --> start 0x%lx",
             vma, (vma != NULL) ? vma->vm_start : 0);

  // find map entry to remove
  list_for_each_entry(ctx, &neon_task->ctx_list.entry, entry) {
    map = neon_ctx_search_map(ctx, vma->vm_start, FOR_VMA);
    if(map != NULL)
      break;
  }
  if(map == NULL) {
    neon_debug("%s : cannot find map for mmapped vma @ 0x%lx",
               __func__, vma->vm_start);
    return;
  }

  // found a map, carefully remove it from all lists it
  // might participate in
  ret = neon_map_fini(ctx, map);
  if(ret != 0) {
    neon_info("ctx 0x%lx : dev 0x%lx : map 0x%lx : fini failed",
              map->ctx_key, map->dev_key, map->key);
    return;
  } else
    neon_info("ctx 0x%lx : dev 0x%lx : map 0x%lx : unmapined vma",
              map->ctx_key, map->dev_key, map->key);

  list_del_init(&map->entry);
  kfree(map);

  return;
}

/***************************************************************************/
// neon_fault_handler
/***************************************************************************/
// Interface: Neon/Linux
// handle manually induced fault --> submit @ GPU
// return 1 if faulting process is not using the GPU
// or if the fault is not handled by neon
static int
neon_fault_handler(struct pt_regs *regs,
                   unsigned long addr)
{
  struct task_struct   *cpu_task   = current;
  neon_task_t          *neon_task  = NULL;
  neon_ctx_t           *fault_ctx  = NULL;
  neon_map_t           *fault_map  = NULL;
  unsigned long         fault_pidx = 0;
  neon_page_t          *fault_page = NULL;
  neon_fault_t         *fault      = NULL;
  neon_work_t          *work       = NULL;
  int                   ret        = 0;

  cpu_task = current;
  neon_task = cpu_task->neon_task;
  if(unlikely(neon_task == NULL)) {
    // not an error, just a fault not concerning a  neon-task
    return 1;
  }

  preempt_disable();

  // use the faulting address to find where (ctx, dev, map) it belongs
  if(unlikely(!list_empty(&neon_task->ctx_list.entry))) {
    neon_ctx_t *ctx = NULL;
    list_for_each_entry(ctx, &neon_task->ctx_list.entry, entry) {
      neon_map_t *map = NULL;
      // find the exact map and page concering this fault
      list_for_each_entry(map, &ctx->map_list.entry, entry) {
        // find the exact map and page concering this fault
        // careful : list of maps might contain uncomissioed maps
        if(map->vma != NULL &&
           addr >= map->vma->vm_start &&
           addr <  (map->vma->vm_start + map->size)) {
          fault_ctx  = ctx;
          fault_map  = map;
          fault_pidx = (addr - map->vma->vm_start) / PAGE_SIZE;
          fault_page = &map->page[fault_pidx];
          fault = map->fault;
          break;
        }
      }
    }
  }
  if(fault == NULL) {
    // if no fault info is found, it's because it's not on an addr we track
    // let the regular fault-handler's code path manage this
    ret = 1;
    goto fault_handler_end;
  }

  neon_debug("TRY new fault @ 0x%lx", addr);

  // check whether the faulting address has been seen before
  if(likely(!list_empty(&fault_ctx->fault_list.entry))) {
    neon_fault_t *f = NULL;
    list_for_each_entry(f, &fault_ctx->fault_list.entry, entry) {
      if(f == fault) {
        neon_warning("fault : ctx 0x%lx : map 0x%lx : page %d : "
                     "addr 0x%lx : ip 0x%lx : vs ...",
                     fault_ctx->key, fault_map->key, fault->page_num,
                     addr, instruction_pointer(regs));
        neon_fault_print(fault_map->fault);
        if(fault->addr == addr) {
          // if the exact same address has been seen before, this
          // must be a real fault
          neon_error("%s : fault : ADDR 0x%lx hit ,recursively",
                     __func__, addr);
          ret = 1;
        } else {
          // to our experience, this will happen on values most likely
          // sitting on page boundaries, and tends to be realized only
          // with heavy logging enabled; try to deal with this by
          // skipping one of two faults
          neon_warning("fault : MAP 0x%x hit recursively", fault_map->key);
          neon_page_arming(0, fault_page);
          fault->siamese = fault_pidx;
          ret = 0;
        }
        goto fault_handler_end;
      }
    }
  }

  // decode and save fault info
  neon_fault_save_decode(regs, addr, fault_map, fault_pidx, fault);

  // check whether fault concerns index register access
  // if yes, this will be a new work submit request
  if(fault->op == 'W' && fault_map->offset != 0 && fault_map->mmio_gpu == 0) {
    neon_work_t *w    = NULL;
    // check whether write concerns index register
    list_for_each_entry(w, &fault_ctx->work_list.entry, entry) {
      if(w->ir == fault_map) {
        work = w;
        break;
      }
    }
  }
  // save fault in fault-list
  list_add(&fault->entry, &fault_ctx->fault_list.entry);

  // write-fault is on index register, manage associated work
#ifndef NEON_TRACE_REPORT
  if(work != NULL) {
    ret = neon_work_update(fault_ctx, work, fault->val);
    if(ret != 0) {
      neon_error("%s : work update failure \n", __func__);
      ret = -1;
      goto fault_handler_end;
    }
  }
#endif // !NEON_TRACE_REPORT

  // Flags suggested  by linux kernel's kmmio module with equivalent
  // functionality :
  // Enable single-stepping and disable interrupts for the faulting
  // context. Local interrupts must not get enabled during stepping.
  regs->flags |= X86_EFLAGS_TF;
  regs->flags &= ~X86_EFLAGS_IF;

  if(fault->op == 'R' || fault->op == 'W')
    neon_debug("ctx 0x%lx : dev 0x%lx : map 0x%lx : "
               "addr 0x%lx : page %d : ip 0x%lx : "
               "op %c : val 0x%x : fault",
               fault_map->ctx_key, fault_map->dev_key, fault_map->key,
               fault->addr, fault->page_num, fault->ip,
               fault->op, fault->val);

  // Set present and take a single step on upcoming set trap
  // to re-arm for future writes
  neon_page_arming(0, fault_page);

#ifndef NEON_TRACE_REPORT
  if(work != NULL) {
    preempt_enable_no_resched();
    neon_work_submit(work, 1);
    return 0;
  }
#endif // NEON_TRACE_REPORT

 fault_handler_end:

  preempt_enable_no_resched();

  return ret; // 0 if fault has been handled, 1 to go back to handler
}

/***************************************************************************/
// neon_copy_task
/***************************************************************************/
// Interface: Neon/Linux
// new thread inherits neon-task from parent
static int
neon_copy_task(unsigned long clone_flags,
               struct task_struct *cpu_task)
{
  neon_task_t *neon_task  = NULL;

  write_lock(&cpu_task->neon_task_rwlock);

  neon_task = (neon_task_t *) current->neon_task;
  // nothing to copy if this is not a context-holding task
  if(neon_task == NULL)
    goto copy_task_end;

  // if this is a CLONE request (i.e. a new thread, but not a new process),
  // then share neon_task
  if(clone_flags & CLONE_VM) {
    // count how many struct-tasks share this neon-task to avoid
    // releasing before all threads in the family exit
    neon_task->sharers++;
    neon_debug("copy task - pid %d, neon-task 0x%p, sharers %d \n",
               (int) cpu_task->pid, neon_task, neon_task->sharers);
    cpu_task->neon_task = (void *) neon_task;
  }

 copy_task_end :
  write_unlock(&cpu_task->neon_task_rwlock);

  return 0;
}

/***************************************************************************/
// neon_exit_task
/***************************************************************************/
// Interface: Neon/Linux
// exit task and neon-task cleaning up behind
static void
neon_exit_task(struct task_struct *cpu_task)
{
  neon_task_t  *neon_task = NULL;
  unsigned int  ctx_live  = 0;

  // if the task does not hold a neon-task, nothing to do here
  neon_task = cpu_task->neon_task;
  if(neon_task == NULL)
    return;

  write_lock(&cpu_task->neon_task_rwlock);

  // one less process in the family sharing this neon-task
  // if this was NOT the last task-struct associated with this
  // neon-task, just return --- task content will be cleaned up
  // by the last thread in the family to exit
  if(neon_task->sharers-- > 0) {
    neon_debug("exit task - pid %d, neon_task 0x%p, sharers %d",
               (int) cpu_task->pid, neon_task, neon_task->sharers);
    write_unlock(&cpu_task->neon_task_rwlock);
    return;
  }

  // clean up this task
  if(neon_task_fini(neon_task) < 0) {
    write_unlock_irq(&cpu_task->neon_task_rwlock);
    neon_error("%s : failed to fini", __func__);
    return;
  }

  cpu_task->neon_task = NULL;
  kfree(neon_task);

  write_unlock_irq(&cpu_task->neon_task_rwlock);

  // main task exiting, sharers == 0;
  // update the (global) value of live contexts appropriately
  ctx_live = atomic_sub_return(neon_task->nctx, &neon_global.ctx_live);
  if(ctx_live == 0) {
    unregister_die_notifier(&nb_die);
    neon_sched_reset(0);
  }

  module_put(THIS_MODULE);

  neon_debug("exit task - %d, neon task 0x%p, ctx live %d",
             (int) cpu_task->pid, neon_task, ctx_live);

  return;
}

/***************************************************************************/
// neon_tweet
/***************************************************************************/
static void
neon_tweet(const char *str)
{
  // write a note to the log
  // the log is also accessible through the ui (virtual "twitter" device)
  //  neon_urgent("K_tweet %s", str);
  neon_notice("K_tweet %s", str);
  
  return;
}

/***************************************************************************/
// The neon-interface calls implemented by this module replace the
// kernel-initialized dummy calls once the module is properly loaded

neon_face_t neon_face_minimal = {
  .ioctl           = neon_ioctl,
  .map_pages       = neon_map_pages,
  .pin_pages       = neon_pin_pages,
  .unpin_pages     = neon_unpin_pages,
  .unmap_vma       = neon_unmap_vma,
  .fault_handler   = neon_fault_handler,
  .copy_task       = neon_copy_task,
  .exit_task       = neon_exit_task,
  .tweet           = neon_tweet,
};

/***************************************************************************/
// neon_init
/***************************************************************************/
static int
__init neon_init(void)
{
  // initialize structs
  if(neon_global_init() != 0) {
    neon_error("%s: module init - failed to init global control",
               __func__);
    return -1;
  }

  // register NEON interface (replace kernel-resident dummy calls
  // with current module's calls)
  if(neon_face_register(&neon_face_minimal) != 0) {
    neon_warning("module init - failed to register neon interface \n");
    return -1;
  }

  // prepare user interface
  if(neon_ui_init(THIS_MODULE) != 0) {
    neon_warning("module init - failed to init the user interface "
                 "(proc, dev) \n");
    return -1;
  }

  // must come after global init
  if(neon_sched_init() < 0) {
    neon_warning("module init - failed to register "
                 "scheduling frontend");
    return -1;
  }

  // full-access tracing/reporting and scheduling are mutualy exclusive
  // (system descign requirement)
#ifdef NEON_TRACE_REPORT
  neon_info("Buffer access tracing reports FULL ---> Scheduling OFF");
#else // !NEON_TRACE_REPORT
  neon_info("Index register access tracing ONLY ---> Scheduling ON");
#endif // NEON_TRACE_REPORT

  neon_global_print();

  neon_info("module init - ready!");
  return 0;
}

/***************************************************************************/
// neon_exit
/***************************************************************************/
static void
__exit neon_exit(void)
{
  // stop accepting any more input from the driver
  if(neon_face_register(NULL) != 0) {
    neon_error("failed to unregister neon interface", __func__);
    goto neon_exit_fail;
  }

  // unregister scheduling component
  if(neon_sched_fini() != 0) {
    neon_error("failed to fini scheduling infustructure", __func__);
    goto neon_exit_fail;
  }

  // destroy sysctl/proc entries and virtual devs
  if(neon_ui_fini() != 0) {
    neon_warning("%s : failed to fini the user interfce", __func__);
    goto neon_exit_fail;
  }

  // finilize and free basic structs
  if(neon_global_fini() != 0) {
    neon_error("%s : failed to fini/cleanup global data", __func__);
    goto neon_exit_fail;
  }

  neon_info("module exit - module unloaded successfully");
  return;

 neon_exit_fail:
  neon_error("module exit - failed");

  return;
}

module_init(neon_init);
module_exit(neon_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Konstantinos Menychtas");
MODULE_DESCRIPTION("NEON module for Black-Box GPU kernel-level management");
