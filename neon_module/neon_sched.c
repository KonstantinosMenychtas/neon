/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/**************************************************************************/
#include <linux/sysctl.h>  // sysctl
#include <linux/mm.h>      // neon_follow_pte
#include <linux/vmalloc.h> // vm_map/unmap_ram
#include <linux/delay.h>   // msleep
#include <linux/timer.h>   // polling timer
#include <linux/slab.h>    // kalloc
#include <linux/highmem.h> // kmap
#include <linux/pid.h>     // get_pid_task
#include <linux/signal.h>  // kill_pgrp
#include "neon_core.h"
#include "neon_control.h"
#include "neon_sys.h"
#include "neon_sched.h"
#include "neon_policy.h"
#include "neon_help.h"

/***************************************************************************/
// period presets
unsigned int _polling_T_     = NEON_POLLING_T_DEFAULT;
unsigned int polling_T       = NEON_POLLING_T_DEFAULT;
unsigned int _malicious_T_   = NEON_MALICIOUS_T_DEFAULT;
unsigned int malicious_T     = NEON_MALICIOUS_T_DEFAULT;

// requests queue for scheduling purposes
wait_queue_head_t neon_kthread_event_wait_queue;
// kernel-thread exit flag
static unsigned int      kthread_repeat = 0;
// hrtimer time descriptor
ktime_t                  polling_interval;
// polling high rez timer
static struct hrtimer    polling_timer;

/****************************************************************************/
// polling_timer_callback
/****************************************************************************/
// called by the polling timer, this alarm will wake-up the sleeping
// polling thread to poll every polling_T periods
static enum hrtimer_restart
polling_timer_callback( struct hrtimer *timer )
{
  if(likely(kthread_repeat)) {
    if(atomic_read(&neon_global.ctx_live) > 0)
      wake_up_interruptible(&neon_kthread_event_wait_queue);
    hrtimer_forward(timer, timer->base->get_time(),polling_interval);
    return HRTIMER_RESTART;
  }

  return HRTIMER_NORESTART;
}

#ifdef NEON_MALICIOUS_TERMINATOR
/****************************************************************************/
// kill_malicious
/****************************************************************************/
static void
kill_malicious(unsigned int pidnum)
{
  struct pid *pid = find_get_pid(pidnum);
  struct task_struct *cpu_task = get_pid_task(pid, PIDTYPE_PID);
  neon_task_t *neon_task = NULL;
  unsigned int marked_malicious = 0;

  
  write_lock(&cpu_task->neon_task_rwlock);
  neon_task = (neon_task_t *) cpu_task->neon_task;
  if(neon_task != NULL) {
    if (neon_task->malicious == 0) {
      neon_task->malicious = 1;
      marked_malicious = 1;
    }
  }
  write_unlock(&cpu_task->neon_task_rwlock);
  
  // making sure we only attempt to kill the process once
  if (marked_malicious == 1) {
    neon_info("PID %d is likely malicious; will be killed", pidnum);
    kill_pgrp(pid, SIGKILL, 1);
  }

  return;
}
#endif // NEON_MALICIOUS_TERMINATOR

/****************************************************************************/
// polling_refc_update
/****************************************************************************/
static void
polling_refc_update(void)
{
  neon_dev_t   *dev      = NULL;
  neon_chan_t  *chan     = NULL;
  unsigned int  did      = 0;
  unsigned int  cid      = 0;
  unsigned int  refc_val = 0;

  for(did = 0; did < neon_global.ndev; did++) {
    unsigned int complete  = 0;
    unsigned int likely_malicious = 0;
    // scan through all active device channels (respective bit is set)
    // update the scheduled work's reference counter value and, if
    // the target value is hit, raise a new scheduling-completion event;
    // if anyone has appeared to be maliciously using the GPU for a
    // predefined number of periods, kill 'em
    dev = &neon_global.dev[did];
    likely_malicious = dev->nchan;
    neon_debug("dev %d : sub2comp 0x%lx", did,
              dev->bmp_sub2comp == NULL ? 0 : dev->bmp_sub2comp[0]);
    if(!__bitmap_empty(dev->bmp_sub2comp, dev->nchan)) {
      for_each_set_bit(cid, dev->bmp_sub2comp, dev->nchan) {
        complete = 0;
        chan = &dev->chan[cid];
        if(spin_trylock(&chan->lock) == 0) {
          neon_info("did %d : cid %d : chan locked",
                      did, cid);
          continue;
        }

        if(unlikely(chan->refc_kvaddr == NULL)) {
          neon_info("did %d, cid %d : pid %d : skip completing work",
                    did, cid, chan->pid);
          spin_unlock(&chan->lock);
          continue;
        }

        refc_val = *((unsigned int *) chan->refc_kvaddr);

        neon_debug("did %d : cid %d : pid %d : "
                   "refc 0x%lx/0x%lx : sched_POLL",
                   did, cid, chan->pid, refc_val,
                   chan->refc_target);

        if(refc_val >= chan->refc_target) {
          neon_debug("did %d : cid %d : pid %d : "
                     "refc [?/0x%p, 0x%lx] : sched_COMPL",
                     did, cid, chan->pid,
                     chan->refc_kvaddr, chan->refc_target);
          complete = 1;
        }
#ifdef NEON_MALICIOUS_TERMINATOR
        else {
          if(malicious_T != 0 && chan->pdt > 0) {
            if(chan->pdt++ > (malicious_T / polling_T))
              likely_malicious = cid;
          }
        }
#endif // NEON_MALICIOUS_TERMINATOR        
        spin_unlock(&chan->lock);
        if(complete == 1)
          neon_work_complete(did, cid, chan->pid);
#ifdef NEON_MALICIOUS_TERMINATOR    
        if(likely_malicious != dev->nchan) {
          kill_malicious(chan->pid);
          break;
        }
#endif // NEON_MALICIOUS_TERMINATOR
      }
    }
    // If a (likely) malicious application has been abusing a
    // channel, make sure to reset the abuse counters
    // for all other channels to avoid killing respective
    // processes by mistake (they should be given a chance
    // to prove they are not malicious also as being queued
    // behind the malicious guy made them look bad)
    if(likely_malicious != dev->nchan &&
       !__bitmap_empty(dev->bmp_sub2comp, dev->nchan)) {
      for_each_set_bit(cid, dev->bmp_sub2comp, dev->nchan) {
        if (cid != likely_malicious) {
          chan = &dev->chan[cid];
          spin_lock(&chan->lock);
          if (chan->pdt > 0) {
            neon_info("2nd chance for PID %d, using chan %d,"
                        "to prove it's not malicious", chan->pid, cid);
            chan->pdt = 1;
          }
          spin_unlock(&chan->lock);
        }
      }
      likely_malicious = dev->nchan;
    }
  }
}

/****************************************************************************/
// event_thread_func
/****************************************************************************/
// the kernel-thread event-handling (callback) function
static int
event_thread_func(void *arg)
{
  DEFINE_WAIT(wait);

  neon_debug("neonkthr starting");

  // detach user resources
  daemonize("neonkthr");
  // killable
  allow_signal(SIGKILL);

  while(1) {
    prepare_to_wait(&neon_kthread_event_wait_queue, &wait,
                    TASK_INTERRUPTIBLE);
    schedule();

    if(kthread_repeat) {
      // update reference counters of live contexts->channels
      polling_refc_update();
      // contact policy and handle service requests
      // (appropriate flags must have been set by policy timers)
      neon_policy_event();
    } else
      break;

    if (signal_pending(current)) {
      neon_debug("SIGKILL pending\n");
      break;
    }
  }

  neon_debug("neonkthr exiting");

  finish_wait(&neon_kthread_event_wait_queue, &wait);
  do_exit(0);

  return 0;
}

/***************************************************************************/
// neon_sched_init
/***************************************************************************/
// init proc options, scheduling policies, setup kthread
int
neon_sched_init(void)
{
  int ret = 0;

  // init kthread and sync q; will sleep till requested to poll
  init_waitqueue_head(&neon_kthread_event_wait_queue);

  // prepare polling; init polling kthread daemon
  ret = kernel_thread(event_thread_func, NULL, CLONE_KERNEL);
  if(ret < 0) {
    neon_error("%s polling kthread creation failed \n",
               __func__);
    return ret;
  }
  kthread_repeat = 1;
  hrtimer_init(&polling_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
  polling_timer.function = &polling_timer_callback;

  ret = neon_policy_init();
  if(ret == 0)
    neon_info("sched_init");

  return ret;
}

/***************************************************************************/
// neon_sched_fini
/***************************************************************************/
// stop polling thread, fini scheduling policies, proc options
int
neon_sched_fini(void)
{
  int ret = 0;

  // this should not be possible given module task use count
  // but double checking never hurts
  if (atomic_read(&neon_global.ctx_live) > 0) {
    neon_error("%s : active contexts/devices exist", __func__);
    return -1;
  }

  kthread_repeat = 0;
  wake_up_interruptible(&neon_kthread_event_wait_queue);

  if(hrtimer_cancel(&polling_timer) != 0)
    neon_debug("Polling timer was busy when stopped");

  ret = neon_policy_fini();
  if(ret == 0)
    neon_debug("sched_fini");

  return ret;
}

/***************************************************************************/
// neon_sched_reengage
/***************************************************************************/
// check whether policy wants to keep tracking after a fault
inline int
neon_sched_reengage(const neon_map_t * const map)
{
#ifndef NEON_TRACE_REPORT
  return neon_policy_reengage_map(map);
#endif
  return 1;
}

/***************************************************************************/
// neon_sched_reset
/***************************************************************************/
// update proc-managed options (e.g. mmap access trackings reports,
// scheduling policy, etc) at proper (non-system-interferring) checkpoints
// update the polling intervals, pause/restart the polling thread
// reset scheduling policy structs as necessary
void
neon_sched_reset(unsigned int nctx)
{
  // adjust thread polling period
  if(nctx == 0) {
    if(hrtimer_cancel(&polling_timer) != 0)
      neon_debug("Polling timer was busy when stopped");
  } else if (nctx == 1 ) {
    // proc/sysctl updates
    polling_T = _polling_T_;
    if(polling_T < NEON_POLLING_T_MIN) {
      neon_error("Adjusting polling T %u to min %d T",
                 polling_T, NEON_POLLING_T_MIN);
      polling_T = NEON_POLLING_T_MIN;
    }
    if(polling_T > NEON_POLLING_T_MAX) {
      neon_error("Adjusting polling T %u to max %d T",
                 polling_T, NEON_POLLING_T_MAX);
      polling_T = NEON_POLLING_T_MAX;
    }

    if(_malicious_T_ != 0 && _malicious_T_ <= NEON_POLLING_T_MAX) {
      neon_error("Adjusting malicious T %u to default %u",
                 _malicious_T_, NEON_MALICIOUS_T_DEFAULT);
      malicious_T = NEON_MALICIOUS_T_DEFAULT;
    } else
      malicious_T = _malicious_T_;

    polling_interval = ktime_set(0, polling_T * NSEC_PER_MSEC);
    hrtimer_start(&polling_timer, polling_interval, HRTIMER_MODE_REL);
  } else {
    neon_error("%s : nctx %d : dunno what to do at this checkpoint",
               __func__, nctx);
    return;
  }

  neon_policy_reset(nctx);

  return;
}

/**************************************************************************/
// hash_map_offset
/**************************************************************************/
// map offset to device and channel id
int
neon_hash_map_offset(unsigned long address,
                     unsigned int *did,
                     unsigned int *cid)
{
  neon_dev_t    *dev = NULL;
  unsigned int   i   = 0;
  int            ret = 0;

  // if address concerns an index register, it must lie in predefined
  // register area --- verify this is the case
  ret = -1;
  for(i = 0; i < neon_global.ndev; i++) {
    unsigned long  bottom = 0;
    unsigned long  top    = 0;
    dev = &neon_global.dev[i];
    bottom = dev->reg_base;
    top = dev->reg_base + dev->nchan * dev->reg_ofs;
    if((address > bottom) && (address < top) &&
       (address % dev->reg_ofs == 0)) {
      *cid  = (address - bottom) / dev->reg_ofs;
      *did  = i;
      ret = 0;
      break;
    }
  }

  return ret;
}

/**************************************************************************/
// update_work_cb_cmd
/**************************************************************************/
// get cmd [addr, size] info for work, update work->cb if necessary
static int
update_work_cb_cmd(const struct _neon_ctx_t_ * const ctx,
                   neon_work_t *const work,
                   const unsigned long reg_idx_val,
                   unsigned long * const cmd_tuple)
{
  unsigned long  reg_idx  = 0;
  unsigned long  ptr      = 0;
  unsigned long  bottom   = 0;
  unsigned long  top      = 0;
  unsigned long  cmd_mmio = 0;
  neon_map_t    *map      = NULL;

  if(unlikely(reg_idx_val == 0)) {
    neon_info("rb exhausted - using last entry");
    switch(work->workload) {
    case NEON_WORKLOAD_COMPUTE:
      reg_idx = NEON_RB_SIZE_COMPUTE / (2 * sizeof(int)) - 1;
      break;
    case NEON_WORKLOAD_GRAPHICS:
      reg_idx = NEON_RB_SIZE_GRAPHICS / (2 * sizeof(int)) - 1;
      break;
    default :
      neon_error("%s : unsupported workload type", __func__);
      return -1;
    }
  } else
    reg_idx = reg_idx_val - 1;

  // Get the starting point and size of last written command set
  // by checking at the offset of the index_register; the value
  // read is the mmio-view address. Note that since Fermi,
  // size is in bytes already, the lowest byte of would-be-size
  // acts as some sort of PAE for the cmd_mmio start address
  ptr = ((unsigned long) work->rb->vma->vm_start) +     \
    (2 * reg_idx * sizeof(unsigned int));
  bottom = neon_uptr_read(work->neon_task->pid,
                          work->rb->vma,
                          ptr);
  ptr += sizeof(unsigned int);
  top = neon_uptr_read(work->neon_task->pid,
                          work->rb->vma,
                          ptr);
  cmd_mmio = bottom | ((top & 0xff) << (8 * sizeof(int)));
  cmd_tuple[1] = top >> 8;

  // We need to identify the actual command-buffer address
  // and we 'll use the cmd_mmio, ctx and dev key to searh the
  // map_list for it. The only canonical information we have managed
  // to identify in the trace for the cb is its size. In OpenCL, cb and
  // rb share the same buffer of size 0x402000; the rb takes the lower
  // 2 pages. In OpenGL it looks like the cb has a size of 0x200000,
  // but further command buffers appear to be possible to be added.
  if(unlikely(work->cb == NULL ||
              cmd_mmio <  work->cb->mmio_gpu ||
              cmd_mmio >= work->cb->mmio_gpu + work->cb->size)) {
    work->cb = NULL;
    list_for_each_entry(map, &ctx->map_list.entry, entry) {
      neon_debug("SEARCH_CB work/map : "
                 "map 0x%x/0x%x : ctx 0x%x/0x%x : dev 0x%x/0x%x : "
                 "cmd_mmio 0x%lx  E [0x%lx, 0x%lx]/[0x%lx, 0x%lx]",
                 work->cb == NULL ? 0 : work->cb->key, map->key,
                 work->rb->ctx_key, map->ctx_key,
                 work->rb->dev_key, map->dev_key,
                 cmd_mmio,
                 work->cb == NULL ? 0 : work->cb->mmio_gpu,
                 work->cb == NULL ? 0 : work->cb->size,
                 map->mmio_gpu, map->size);
      if(map->mmio_gpu != 0 &&
         map->ctx_key == work->rb->ctx_key &&
         map->dev_key == work->rb->dev_key &&
         cmd_mmio >= map->mmio_gpu &&
         cmd_mmio <  map->mmio_gpu + map->size) {
        work->cb = map;
        neon_debug("UPDATE_CB : ctx 0x%x : dev 0x%x : did %d : cid %d : "
                   "NEW cb == 0x%lx [0x%lx, 0x%lx]",
                   work->rb->ctx_key, work->rb->dev_key, work->did, work->cid,
                   work->cb->key, work->cb->mmio_gpu, work->cb->size);
        break;
      }
    }
    if(work->cb == NULL) {
      neon_error("%s : ctx 0x%x : dev 0x%x : did %d : cid %d : "
                 "idx-val %d : tuple [0x%lx, 0x%lx] : can't find cb",
                 __func__, work->rb->ctx_key, work->rb->dev_key,
                 work->did, work->cid, reg_idx,
                 cmd_tuple[0], cmd_tuple[1]);
      return -1;
    }
  }
  // The command-set starting point is given wrt the mmio-view,
  // so translate it back to a proper (cpu-view) virtual address.
  cmd_tuple[0] = work->cb->vma->vm_start + cmd_mmio - work->cb->mmio_gpu;

  neon_debug("ctx 0x%x : dev 0x%x : did %d : cid %d : "
             "idx 0x%x : tuple [0x%lx, 0x%lx] : work-update",
             work->rb->ctx_key, work->rb->dev_key,
             work->did, work->cid, reg_idx,
             cmd_tuple[0], cmd_tuple[1]);

  return 0;
}

/**************************************************************************/
// update_work_rc
/**************************************************************************/
// update work->rc if necessary
static inline int
update_work_rc(const struct _neon_ctx_t_ * const ctx,
               neon_work_t * const work,
               const unsigned long * const refc_tuple)
{
  neon_map_t *map = NULL;

  // Get the virtual addess (CPU view) of the reference counter
  if(unlikely(work->rc == NULL ||
              refc_tuple[0] <  work->rc->mmio_gpu ||
              refc_tuple[0] >= work->rc->mmio_gpu + work->rc->size)) {
    // update rc
    work->rc = NULL;
    list_for_each_entry(map, &ctx->map_list.entry, entry) {
      neon_debug("work ctx 0x%x : dev 0x%x : mmio 0x%x : "
                 "in map(0x%x, 0x%x)->[0x%lx, 0x%lx] ? SEARCH ",
                 work->rb->ctx_key, work->rb->dev_key, refc_tuple[0],
                 map->ctx_key, map->dev_key, map->mmio_gpu,
                 map->mmio_gpu + map->size);
      if(map->mmio_gpu != 0 &&
         map->ctx_key == work->rb->ctx_key &&
         map->dev_key == work->rb->dev_key &&
         refc_tuple[0] >= map->mmio_gpu &&
         refc_tuple[0] <  map->mmio_gpu + map->size) {
        work->rc = map;
        break;
      }
    }
    if(work->rc == NULL)
      return -1;
  }

  return 0;
}

/**************************************************************************/
// neon_work_init
/**************************************************************************/
// create and initialize new work
neon_work_t *
neon_work_init(neon_task_t * const neon_task,
               neon_ctx_t * const ctx,
               neon_map_t * const ir)
{
  neon_work_t  *work = NULL;
  unsigned int cid   = 0;
  unsigned int did   = 0;
  neon_map_t   *m    = NULL;
  neon_map_t   *rb   = NULL;

  // check whether incoming map is an index register
  if(neon_hash_map_offset(ir->offset, &did, &cid) != 0)
    return NULL;

  // find last enqueued ring-buffer --- it is the one to which
  // register-map in question must be referring to
  list_for_each_entry(m, &ctx->map_list.entry, entry) {
    if(m->size == NEON_RB_SIZE_GRAPHICS ||
       m->size == NEON_RCB_SIZE_COMPUTE) {
      rb = m;
      break;
    }
  }
  if(rb == NULL) {
    neon_error("%s : ARGH! ctx 0x%x : dev 0x%x : ir 0x%x"
               "did %d : cid %d : no associated rb!",
               __func__, ir->ctx_key, ir->dev_key, ir->key, did, cid);
    // this is a trace misundersunding problem
    // we had assumed there'd always be a ring-buffer to be found
    //    BUG();
    return NULL;
  }

  // create work struct
  work = (neon_work_t *) kzalloc(sizeof(neon_work_t), GFP_KERNEL);
  if(work == NULL) {
    neon_error("%s : alloc work struct failed \n", __func__);
    return NULL;
  }

  work->did         = did;
  work->cid         = cid;
  work->ir          = ir;
  work->rb          = rb;
  work->cb          = NULL; // updated at work submit
  work->rc          = NULL;
  work->ctx         = ctx;
  work->neon_task   = neon_task;
  work->refc_vaddr  = 0;
  work->refc_target = 0;
  switch(rb->size) {
  case NEON_RB_SIZE_GRAPHICS:
    work->workload = NEON_WORKLOAD_GRAPHICS;
    break;
  case NEON_RCB_SIZE_COMPUTE:
    work->workload = NEON_WORKLOAD_COMPUTE;
    break;
  default :
    work->workload = NEON_WORKLOAD_UNDEFINED;
  }
  INIT_LIST_HEAD(&(work->entry));

  neon_info("task %d : ir 0x%x : rb 0x%x : "
            "did %d : cid %d : rb 0x%x : %d work",
            neon_task->pid, ir->key, rb->key, did, cid,
            rb->key, work->workload);

  return work;
}

/**************************************************************************/
// neon_work_fini
/**************************************************************************/
// finish work
inline int
neon_work_fini(neon_work_t * const work)
{
  neon_dev_t  *dev         = &neon_global.dev[work->did];
  neon_chan_t *chan        = &dev->chan[work->cid];
  unsigned int refc_target = 0;

  if(test_bit(work->cid, dev->bmp_sub2comp) != 0) {
    spin_lock(&chan->lock);
    refc_target = chan->refc_target;
    spin_unlock(&chan->lock);
  }

  if(refc_target != 0) {
    // this should not happen if work_stop has run before
    neon_warning("did %d : cid %d : rc [0x%lx/0x%lx, 0x%lx] : "
                 "incomplete at fini", work->did, work->cid,
                 work->refc_vaddr, work->refc_kvaddr,
                 work->refc_target);
    return -1;
  }

  neon_info("did %d : cid %d : pid %d : work fini",
            work->did, work->cid, work->neon_task->pid);

  return 0;
}

/**************************************************************************/
// neon_work_update
/**************************************************************************/
// Prepare work-info for scheduling (e.g. refc addr, target)
inline int
neon_work_update(struct _neon_ctx_t_ * const ctx,
                 neon_work_t * const work,
                 unsigned long reg_idx)
{
  neon_dev_t    *dev           = &neon_global.dev[work->did];
  unsigned long  cmd_tuple[2]  = {0, 0}; // [address, size]
  unsigned long  refc_tuple[2] = {0, 0}; // [address, target]
  unsigned long  refc_vaddr    = 0;
  int            ret           = 0;

  // Get the address and size of the last command on this index reg
  // updating the work_cb if necessary
  ret = update_work_cb_cmd(ctx, work, reg_idx, cmd_tuple);
  if(ret != 0) {
    neon_error("%s : did %d : cid %d : idx %ld : cannot find cmd",
               __func__, work->did, work->cid, reg_idx);
    return ret;
  }

  // Read the last few entries of the command-set and find the mmio
  // addresses (GPU view) of the reference counter and its value.
  // Device type and workload type dependent, this part of the
  // trace analysis is the most sensitive.
  ret = (*dev->refc_eval)(work->neon_task->pid, work->cb->vma,
                          work->workload, cmd_tuple, refc_tuple);
  if(ret < 0) {
    neon_error("%s : did %d : cid %d : idx %ld : cmd [0x%lx, 0x%lx] : "
               "cannot get refc addr/size (GPU view)", __func__,
               work->did, work->cid, reg_idx, cmd_tuple[0], cmd_tuple[1]);
    return ret;
  } else 
    work->part_of_call = ret;

  // validate/update refc counter map
  ret = update_work_rc(ctx, work, refc_tuple);
  if(unlikely(ret != 0)) {
    neon_error("%s : did %d : cid %d : idx %ld : cmd [0x%lx, 0x%lx] : "
               "cannot find reference counter's buffer", __func__,
               work->did, work->cid, reg_idx, cmd_tuple[0], cmd_tuple[1]);
    return -1;
  }

  // save the refc addr, target tuple @ work
  refc_vaddr = work->rc->vma->vm_start + refc_tuple[0] - work->rc->mmio_gpu;
  if(unlikely(work->refc_vaddr != refc_vaddr)) {
    struct page *refc_page = NULL;
    refc_page = neon_follow_page(work->rc->vma, refc_vaddr);
    work->refc_kvaddr = (unsigned long) vm_map_ram(&refc_page, 1,
                                                   -1, PAGE_KERNEL);
    work->refc_kvaddr += (refc_vaddr & ~PAGE_MASK);
    work->refc_vaddr  = refc_vaddr;
    neon_info("did %d  : cid %d : pid %d :"
              "rc [0x%lx/0x%lx, 0x%lx] : refc addr update",
              work->did, work->cid, work->neon_task->pid,
              work->refc_vaddr, work->refc_kvaddr, work->refc_target);
  }
  work->refc_target = refc_tuple[1];

  return 0;
}

/**************************************************************************/
// neon_work_print
/**************************************************************************/
// print out neon_work struct
inline void
neon_work_print(const neon_work_t * const work)
{
  neon_info("did %d : cid %d : pid %d : ir 0x%x : rb 0x%x : "
            "refc [0x%lx/0x%lx, t 0x%lx] : type %d : work",
            work->did, work->cid, work->neon_task->pid,
            work->ir != NULL ? work->ir->key : 0,
            work->rb != NULL ? work->rb->key : 0,
            work->refc_vaddr, work->refc_kvaddr,
            work->refc_target, work->workload);

  return;
}

/**************************************************************************/
// neon_work_submit
/**************************************************************************/
// Submit incoming GPU access request
int
neon_work_submit(neon_work_t * const work,
                 unsigned int really)
{
  neon_dev_t    *dev         = &neon_global.dev[work->did];
  neon_chan_t   *chan        = &dev->chan[work->cid];
  int            ret         = 0;

  if(likely(really == 1)) {
    // reset request processing time; this channel is
    // assumed to be empty because previous request
    // is either already complete or new request is
    // being submitted "back-to-back" (b2b)
    spin_lock(&chan->lock);
    chan->pdt = 0;
    spin_unlock(&chan->lock);
    
    // submit request --- might block here until
    // scheduler allows us to proceed (request
    // will be issued as the specific policy decides)
    ret = neon_policy_submit(work);
  } else 
    work->part_of_call = 0;

  // TODO: CAREFUL --- work was previously saved
  // before policy-submit ; verify that everything
  // is OK with saving it to channel AFTER

  // start counting request processing time; if submit
  // has returned, it means request has been scheduled
  // save work in channel
  spin_lock(&chan->lock);
  chan->pid = work->neon_task->pid;
  chan->refc_kvaddr = (void *) work->refc_kvaddr;
  chan->refc_target = work->refc_target;
  chan->pdt = 1;

  // mark channel as "live" for the kthread to know to query
  set_bit(work->cid, dev->bmp_sub2comp);
  
  spin_unlock(&chan->lock);
  
  neon_debug("did %d : cid %d : pid %d : refc=0x%lx work submitted %s",
             work->did, work->cid, work->neon_task->pid,
             work->refc_target, really == 1 ? "really" : "fake");

  return ret;
}

/**************************************************************************/
// neon_work_complete
/**************************************************************************/
// Completion notification raised by the polling thread or the app
// at stop time 
inline void
neon_work_complete(unsigned int did,
                   unsigned int cid,
                   unsigned int pid)
{
  neon_dev_t  *dev    = &neon_global.dev[did];
  neon_chan_t *chan   = &dev->chan[cid];
  unsigned long check = 0;

  // mark work as complete in dev's live channel bitmap
  if(test_and_clear_bit(cid, dev->bmp_sub2comp) == 0) {
    neon_debug("did %d : cid %d : pid %d : work already completed",
               did, cid, pid);
    return;
  } else {
    spin_lock(&chan->lock);    
    check = chan->refc_target;
    spin_unlock(&chan->lock);
  }

  // notify scheduling policy of completion event
  neon_policy_complete(did, cid, pid);

  // remove from channel
  // ignore if new request has been submitted
  spin_lock(&chan->lock);
  if(test_bit(cid, dev->bmp_sub2comp) == 0){
    chan->pid         = 0;
    chan->refc_kvaddr = NULL;
    chan->refc_target = 0;
    chan->pdt = 0;
  }
  spin_unlock(&chan->lock);

  neon_debug("did %d : cid %d : pid %d : refc=0x%lx -> rqst completed",
             did, cid, pid, check);

  return;
}

/**************************************************************************/
// neon_work_start
/**************************************************************************/
// prepare a new work for scheduling consideration
inline int
neon_work_start(neon_work_t * const work)
{
  int ret = 0;

  ret = neon_policy_start(work);
  neon_info("did %d : cid %d : pid %d : work sched start",
            work->did, work->cid, work->neon_task->pid);

  return ret;
}

/**************************************************************************/
// neon_work_stop
/**************************************************************************/
// cleanly remove work from scheduling consideration
inline int
neon_work_stop(const neon_work_t * const work)
{
  int ret = 0;

  neon_work_complete(work->did, work->cid, work->neon_task->pid);
  ret = neon_policy_stop(work);

  neon_info("did %d : cid %d : pid %d : work sched stop",
            work->did, work->cid, work->neon_task->pid);

  return ret;
}
