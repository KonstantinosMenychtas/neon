/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/**************************************************************************/

#include <linux/list.h>      // lists
#include <linux/slab.h>      // kmalloc/kzalloc
#include <linux/sysctl.h>    // sysctl
#include <linux/delay.h>     // sleep at exit
#include <linux/semaphore.h> // sched-control semaphores
#include <linux/spinlock.h>  // locks
#include <asm/io.h>          // readl
#include <linux/string.h>    // strncpy
#include "neon_core.h"
#include "neon_policy.h"
#include "neon_fcfs.h"
#include "neon_timeslice.h"
#include "neon_help.h"

/**************************************************************************/
// APPEND MORE POLICIES HERE

static neon_policy_face_t *policy_face[NEON_POLICIES] = {
  &neon_policy_fcfs,       // NEON_POLICY_FCFS
  &neon_policy_timeslice,  // NEON_POLICY_TIMESLICE
  &neon_policy_sampling    // NEON_POLICY_SAMPLING
};

static char neon_policy_name[NEON_POLICIES+1][NAME_LEN] = {
  {'f', 'c', 'f', 's', '\0',  0,   0,   0 ,   0,    0 },
  {'t', 'i', 'm', 'e', 's',  'l', 'i', 'c',   'e', '\0'},
  {'s', 'a', 'm', 'p', 'l',  'i', 'n', 'g',   '\0', 0}
};

/***************************************************************************/

// policy selection
static unsigned int policy_id = NEON_DEFAULT_POLICY;
static neon_policy_face_t *select_policy = NULL;
char _policy_name_[NAME_LEN] = { 0 };

// GPU devices scheduling abstraction
sched_dev_t *sched_dev_array;

/**************************************************************************/
// create_sched_task
/**************************************************************************/
// create new sched-task
static inline sched_task_t *
create_sched_task(unsigned int did,
                  unsigned int pid)
{
  unsigned long nchan      = neon_global.dev[did].nchan;
  sched_task_t *sched_task = NULL;

  sched_task = (sched_task_t *) kzalloc(sizeof(sched_task_t), GFP_KERNEL);
  if(sched_task == NULL) {
    neon_error("%s : sched-task kalloc failed ", __func__);
    return NULL;
  }

  sched_task->pid = pid;
  sched_task->bmp_start2stop = (long *) kzalloc(BITS_TO_LONGS(nchan) *  \
                                                sizeof(long), GFP_KERNEL);
  sched_task->bmp_issue2comp = (long *) kzalloc(BITS_TO_LONGS(nchan) *      \
                                                sizeof(long), GFP_KERNEL);
  if(sched_task->bmp_start2stop == NULL || \
     sched_task->bmp_issue2comp == NULL) {
    neon_error("%s : pid %d : kalloc sched-task bmp failed", pid);
    return NULL;
  }

  INIT_LIST_HEAD(&sched_task->entry);

  select_policy->create(sched_task);

  return sched_task;
}

/**************************************************************************/
// destroy_sched_task
/**************************************************************************/
// create new sched-task
static inline void
destroy_sched_task(sched_task_t *sched_task)
{
  select_policy->destroy(sched_task);

  kfree(sched_task->bmp_issue2comp);
  kfree(sched_task->bmp_start2stop);

  return;
}

/**************************************************************************/
// neon_policy_init
/**************************************************************************/
// Initialize scheduling abstractions, policies
int
neon_policy_init(void)
{
  unsigned int i = 0;

  // init sched devs
  sched_dev_array = kzalloc(neon_global.ndev * sizeof(sched_dev_t),
                            GFP_KERNEL);
  if(sched_dev_array == NULL) {
    neon_error("%s : sched-dev alloc failed! ",  __func__);
    goto policy_init_fail;
  }

  // init sched devices and sched channels (sched-work array)
  for(i = 0 ; i < neon_global.ndev; i++) {
    unsigned long  nchan     =  neon_global.dev[i].nchan;
    sched_dev_t   *sched_dev = &sched_dev_array[i];
    sched_dev->id = i;
    sched_dev->swork_array = kzalloc(nchan * sizeof(sched_work_t), GFP_KERNEL);
    if(sched_dev->swork_array == NULL) {
      neon_error("%s : sched chan-array alloc failed! ",  __func__);
      goto policy_init_fail;
    } // sched-work ids will be reset at sched-work start time
    INIT_LIST_HEAD(&sched_dev->stask_list.entry);
    rwlock_init(&sched_dev->lock);
  }

  // select and init policy
  // (might have to re-init at reset, per-policy behavior applies)
  strncpy(_policy_name_, neon_policy_name[policy_id], NAME_LEN);
  select_policy = policy_face[policy_id];
  select_policy->init();
  select_policy->reset(0);

  neon_debug("policy_init");

  return 0;

 policy_init_fail:

  if(sched_dev_array != NULL) {
    for(i = 0; i < neon_global.ndev; i++) {
      sched_dev_t *sched_dev = &sched_dev_array[i];
      if(sched_dev->swork_array != NULL)
        kfree(sched_dev->swork_array);
    }
  }
  kfree(sched_dev_array);

  return -1;
}

/**************************************************************************/
// neon_policy_fini
/**************************************************************************/
// finalize and destroy scheduling structs
int
neon_policy_fini(void)
{
  unsigned int i   = 0;
  int          ret = 0;

  select_policy->fini();

  // this function is only reachable at a successfuly module-exit call
  // and task-exit routines gurantee that no dormant tasks should be
  // found in any device task-list
  for(i = 0 ; i < neon_global.ndev ; i++) {
    sched_dev_t *sched_dev = &sched_dev_array[i];
    if(unlikely(!list_empty(&sched_dev->stask_list.entry))) {
      struct list_head *pos = NULL;
      struct list_head *q   = NULL;
      neon_error("%s : did %d : task list not empty at policy fini",
                 __func__, i);
      list_for_each_safe(pos, q, &sched_dev->stask_list.entry) {
        sched_task_t *sched_task = list_entry(pos, sched_task_t, entry);
        list_del_init(pos);
        destroy_sched_task(sched_task);
        kfree(sched_task);
      }
      ret = -1;
    }
    kfree(sched_dev->swork_array);
  }
  kfree(sched_dev_array);
  sched_dev_array = NULL;

  neon_debug("policy_fini");

  return ret;
}

/**************************************************************************/
// neon_policy_reset
/**************************************************************************/
// Reset policy structs
void
neon_policy_reset(unsigned int nctx)
{
  if(nctx == 0 || nctx == 1) {
    unsigned int i = 0;

    for(i = 0; i < NEON_POLICIES; i++) {
      if(strncmp(_policy_name_, neon_policy_name[i], NAME_LEN) == 0) {
        policy_id = i;
        break;
      }
    }
    if(i == NEON_POLICIES) {
      neon_info("Select policy \"%s\" is not valid --- switching to default %s",
                _policy_name_, neon_policy_name[NEON_DEFAULT_POLICY]);
      policy_id = NEON_DEFAULT_POLICY;
    }

    strncpy(_policy_name_, neon_policy_name[policy_id], NAME_LEN);
    if(select_policy != policy_face[policy_id] && nctx == 1) {
      if(select_policy != NULL)
        select_policy->fini();
      select_policy = policy_face[policy_id];
      select_policy->init();
      neon_info("policy reset: new policy is \"%s\", nctx = %d",
                _policy_name_, nctx);
    }
    neon_info("policy reset: policy set to \"%s\", nctx = %d",
              _policy_name_, nctx);
  }

  select_policy->reset(nctx);

  return;
}

/**************************************************************************/
// neon_policy_start
/**************************************************************************/
// new GPU work request: intialize and start a new job
int
neon_policy_start(neon_work_t * const neon_work)
{
  const unsigned int did = neon_work->did;
  const unsigned int cid = neon_work->cid;
  const unsigned int pid = neon_work->neon_task->pid;

  unsigned long nchan      = neon_global.dev[did].nchan;
  sched_dev_t  *sched_dev  = &sched_dev_array[did];
  sched_work_t *sched_work = &sched_dev->swork_array[cid];
  sched_task_t *sched_task = NULL;
  sched_task_t *stask      = NULL;

  // check whether this task has started works before; if not, create
  // a new sched-task
  read_lock(&sched_dev->lock);
  list_for_each_entry(stask, &sched_dev->stask_list.entry, entry) {
    if(stask->pid == pid) {
      sched_task = stask;
      break;
    }
  }
  read_unlock(&sched_dev->lock);
  if(sched_task == NULL) {
    // create new entry to consider for scheduling
    sched_task = create_sched_task(did, pid);
    if(sched_task == NULL) {
      neon_error("%s : pid %d ; kalloc sched-task during"
                 "policy start failed", pid);
      return -1;
    }
  }

  memset(sched_work, 0, sizeof(sched_work_t));

  write_lock(&sched_dev->lock);
  sched_work->neon_work = neon_work;
  sched_work->id = cid;
  sched_work->pid = pid;
  if(bitmap_empty(sched_task->bmp_start2stop, nchan))
    list_add_tail(&sched_task->entry, &sched_dev->stask_list.entry);
  select_policy->start(sched_dev, sched_work, sched_task);
  // mark work as started
  set_bit(cid, sched_task->bmp_start2stop);
  neon_info("did %d : cid %d : pid %d : policy start", did, cid, pid);
  write_unlock(&sched_dev->lock);

  return 0;
}

/**************************************************************************/
// neon_policy_stop
/**************************************************************************/
// carefully exit work
int
neon_policy_stop(const neon_work_t * const neon_work)
{
  const unsigned int did = neon_work->did;
  const unsigned int cid = neon_work->cid;
  const unsigned int pid = neon_work->neon_task->pid;

  unsigned long nchan      = neon_global.dev[did].nchan;
  sched_dev_t  *sched_dev  = &sched_dev_array[did];
  sched_work_t *sched_work = &sched_dev->swork_array[cid];
  sched_task_t *sched_task = NULL;
  sched_task_t *stask      = NULL;

  read_lock(&sched_dev->lock);
  list_for_each_entry(stask, &sched_dev->stask_list.entry, entry) {
    if(stask->pid == pid) {
      sched_task = stask;
      break;
    }
  }
  read_unlock(&sched_dev->lock);
  if(sched_task == NULL) {
    neon_error("%s : did %d : cid %d : pid %d ; no sched-task found",
               did , cid, pid);
    return -1;
  }

  // mark work as stopped
  clear_bit(cid, sched_task->bmp_start2stop);

  neon_account("did %2d : cid %2d : pid %6d : nrqst %10ld : "
               "exe %10ld (%10ld/rqst): wait %10ld (%10ld/rqst) : "
               "work stats @ work stop",
               sched_dev->id, sched_work->id, sched_task->pid,
               sched_work->nrqst,
               sched_work->exe_dt, sched_work->nrqst > 0 ?        \
               sched_work->exe_dt/sched_work->nrqst : 0,
               sched_work->wait_dt, sched_work->nrqst > 0 ?       \
               sched_work->wait_dt/sched_work->nrqst : 0);

  write_lock(&sched_dev->lock);
  // notify scheduling policy this work in this task is stopping
  select_policy->stop(sched_dev, sched_work, sched_task);
  // reset sched-work to avoid misunderstandings by concurrently
  // accessing sched-"threads" (e.g. timeslice alarm, sampling alarm)
  memset(sched_work, 0, sizeof(sched_work_t));

  if(bitmap_empty(sched_task->bmp_start2stop, nchan)) {
    // task is not accessible by anyone at this point can be removed
    list_del_init(&sched_task->entry);
    neon_account("did %2d : cid %2s : pid %6d : nrqst %10ld : "
                 "exe %10ld (%10ld/rqst): wait %10ld (%10ld/rqst) : "
                 "task stats @ task stop",
                 sched_dev->id, "", sched_task->pid,
                 sched_task->nrqst,
                 sched_task->exe_dt, sched_task->nrqst > 0 ?    \
                 sched_task->exe_dt/sched_task->nrqst : 0,
                 sched_task->wait_dt, sched_task->nrqst > 0 ?   \
                 sched_task->wait_dt/sched_task->nrqst : 0);
    destroy_sched_task(sched_task);
    kfree(sched_task);
  }

  write_unlock(&sched_dev->lock);

  return 0;
}

/**************************************************************************/
// neon_policy_submit
/**************************************************************************/
// submit a GPU request --- work is enqueued
int
neon_policy_submit(const neon_work_t * const neon_work)
{
  const unsigned int did = neon_work->did;
  const unsigned int cid = neon_work->cid;
  const unsigned int pid = neon_work->neon_task->pid;

  sched_dev_t  *sched_dev   = &sched_dev_array[did];
  sched_work_t *sched_work  = &sched_dev->swork_array[cid];
  sched_task_t *sched_task  = NULL;
  sched_task_t *stask       = NULL;
  struct timespec now_ts    = { 0 };
  unsigned long exe_dt      = 0;

  // find respective sched-task
  read_lock(&sched_dev->lock);
  list_for_each_entry(stask, &sched_dev->stask_list.entry, entry) {
    if(stask->pid == pid) {
      sched_task = stask;
      break;
    }
  }
  read_unlock(&sched_dev->lock);
  if(sched_task == NULL) {
    neon_error("%s : did %d : cid %d : pid %d : submit without task",
               __func__, did, cid, pid);
    return -1;
  }

  write_lock(&sched_dev->lock);

  getnstimeofday(&now_ts);

  // the generic policy handler simply considers all requests the same
  // if this is a back2back call (i.e. new submit on top of previously
  // incomplete submit), mark all time since last issuance as time executing
  if(test_bit(cid, sched_task->bmp_issue2comp) != 0) {
    struct timespec complete_ts = now_ts;
    struct timespec dtime       = { 0 };
    dtime = timespec_sub(complete_ts, sched_work->issue_ts);
    exe_dt = (unsigned long) (timespec_to_ns(&dtime) / NSEC_PER_USEC);
    neon_debug("did %d : cid %d  task-exe %ld (added %ld) : "
               "work-nrqst %ld : task-nrqst %ld : submit b2b ",
               did, cid, sched_task->exe_dt, exe_dt,
               sched_work->nrqst+1, sched_task->nrqst + 1);
  }
  sched_work->exe_dt += exe_dt;
  sched_work->nrqst++;
  sched_task->nrqst++;
  sched_work->submit_ts = now_ts;

  select_policy->submit(sched_dev, sched_work, sched_task);

#ifndef NEON_USE_SAMPLING
#ifndef NEON_USE_TIMESLICE
  neon_info("did %d : cid %d : pid %d : rqst %ld : "
            "refc_target 0x%lx : exe task %ld : exe work %ld : "
            "added %ld : submitted %s",
            did, cid, pid, sched_work->nrqst,
            sched_work->neon_work->refc_target,
            sched_task->exe_dt, sched_work->exe_dt, exe_dt,
            exe_dt == 0 ? "b2b" : "new");
#endif // NEON_USE_TIMESLICE
#endif // NEON_USE_SAMPLING

  write_unlock(&sched_dev->lock);

  return 0;
}

/**************************************************************************/
// neon_policy_issue
/**************************************************************************/
// issue a GPU request --- work is dequeued
// called by select_policy submit function
int
neon_policy_issue(sched_dev_t  * const sched_dev,
                  sched_work_t * const sched_work,
                  sched_task_t * const sched_task,
                  unsigned int         had_blocked)
{
  if(had_blocked != 0){
    unsigned long   wait_dt = 0;
    struct timespec dtime   = { 0 };

    // If this was a previously blocked request, it came here with
    // its issue bit unset; set it again or else we might miss a
    // completion notification;
    set_bit(sched_work->id, sched_task->bmp_issue2comp);
    // and account for waiting time (possibly some of it imposed
    // by algorithm, but wait_dt is the generic counter)
    getnstimeofday(&sched_work->issue_ts);
    dtime = timespec_sub(sched_work->issue_ts, sched_work->submit_ts);
    wait_dt = (unsigned long) (timespec_to_ns(&dtime) / NSEC_PER_USEC);
    sched_work->wait_dt += wait_dt;
  } else
    sched_work->issue_ts = sched_work->submit_ts;

  // a specific policy might choose to consider actual
  // kernel/gfx calls (e.g. NDRangeKernel) for its accounting
  sched_work->part_of_call = (sched_work->neon_work->part_of_call);

  select_policy->issue(sched_dev, sched_work, sched_task, had_blocked);

  set_bit(sched_work->id, sched_task->bmp_issue2comp);

#ifndef NEON_USE_SAMPLING
#ifndef NEON_USE_TIMESLICE
  neon_info("did %d : cid %d  : pid %d : rqst %ld : total exe %ld : "
            "refc_target 0x%lx : issue_ts->now %ld :  issued %s",
            sched_dev->id, sched_work->id, sched_work->pid,
            sched_task->nrqst, sched_task->exe_dt,
            sched_work->neon_work->refc_target,
            (unsigned long) (timespec_to_ns(&sched_work->issue_ts) / NSEC_PER_USEC),
            had_blocked ? "previously_blocked" : "");
#endif // NEON_USE_TIMESLICE
#endif // NEON_USE_SAMPLING

  return 0;
}

/**************************************************************************/
// neon_policy_complete
/**************************************************************************/
// complete event
void
neon_policy_complete(unsigned int did,
                     unsigned int cid,
                     unsigned int pid)
{
  sched_dev_t     *sched_dev   = &sched_dev_array[did];
  sched_work_t    *sched_work  = &sched_dev->swork_array[cid];
  sched_task_t    *sched_task  = NULL;
  sched_task_t    *stask       = NULL;
  unsigned long    exe_dt      = 0;

  // find respective sched-task
  read_lock(&sched_dev->lock);
  list_for_each_entry(stask, &sched_dev->stask_list.entry, entry) {
    if(stask->pid == pid) {
      sched_task = stask;
      break;
    }
  }
  read_unlock(&sched_dev->lock);
  if(sched_task == NULL) {
    neon_error("%s : did %d : cid %d : pid %d : submit without task",
               __func__, did, cid, pid);
    return;
  }

  if(test_bit(cid, sched_task->bmp_issue2comp) != 0) {
    struct timespec complete_ts = { 0 };
    struct timespec dtime       = { 0 };
    getnstimeofday(&complete_ts);
    dtime = timespec_sub(complete_ts, sched_work->issue_ts);
    exe_dt = (unsigned long) (timespec_to_ns(&dtime) / NSEC_PER_USEC);
    neon_debug("did %d : cid %d : exe %ld : total %ld : "
               "tasknrqst %ld : uninterrupted issue2complete",
               did, cid, exe_dt, sched_task->exe_dt, sched_task->nrqst);
    clear_bit(cid, sched_task->bmp_issue2comp);
  }
  sched_work->exe_dt += exe_dt;
  sched_task->exe_dt += sched_work->exe_dt;
  sched_task->wait_dt += sched_work->wait_dt;

  write_lock(&sched_dev->lock);

  select_policy->complete(sched_dev, sched_work, sched_task);

  neon_info("did %d : cid %d : pid %d : rqst %ld : "
            "exe task %ld : exe work %ld : "
            "added %ld : wait task %ld : work complete",
            sched_dev->id, sched_work->id, sched_task->pid,
            sched_work->nrqst, sched_task->exe_dt,
            sched_work->exe_dt, exe_dt, sched_work->wait_dt);

  write_unlock(&sched_dev->lock);

  return;
}

/**************************************************************************/
// neon_policy_event
/**************************************************************************/
// event thread inquiring policy about event handling
inline void
neon_policy_event(void)
{
  return select_policy->event();
}

/**************************************************************************/
// neon_policy_reengage
/**************************************************************************/
// let policy decide whether to reengage after a fault
inline int
neon_policy_reengage_map(const neon_map_t * const map)
{
  return select_policy->reengage_map(map);
}

/**************************************************************************/
// neon_policy_reengage_task
/**************************************************************************/
// re-engage or dis-engage whole task (not policy specific)
// CAREFUL : called with sched-dev write lock held
void
neon_policy_reengage_task(sched_dev_t *sched_dev,
                          sched_task_t *sched_task,
                          unsigned int arm)
{
  unsigned long  nchan    = neon_global.dev[sched_dev->id].nchan;
  unsigned int   i        = 0;

  for_each_set_bit(i, sched_task->bmp_start2stop, nchan) {
    sched_work_t *sched_work = NULL;
    neon_map_t   *map   = NULL;
    sched_work = &sched_dev->swork_array[i];
    if(sched_work->neon_work == NULL) {
      neon_error("neon_work is NULL for set start2stop "
                 "work pid %d, cid %d", sched_task->pid, i);
      BUG();
      // this should never happen because map-fini->track-stop will
      // reach work-stop which will call work-complete (so policy-complete)
      // and then policy-stop
      continue;
    }
    map = sched_work->neon_work->ir;
    neon_track_restart(arm, map);
    neon_info("did %d : cid %d : task %d : %s-engaged --- task ",
              sched_dev->id, i, sched_task->pid,
              ((arm == 0) ? "dis" : "___"));

  }

  return;
}

/**************************************************************************/
// neon_policy_update
/**************************************************************************/
// check whether a task has jobs pending (not policy specific)
// CAREFUL : called with sched-dev write lock held
void
neon_policy_update(const sched_dev_t *const sched_dev,
                   const sched_task_t *const sched_task)
{
  neon_dev_t   *neon_dev    = &neon_global.dev[sched_dev->id];
  unsigned int  nchan       = neon_global.dev[sched_dev->id].nchan;
  unsigned int  i           = 0;
  /* struct timespec start_ts  = { 0 }; */
  /* struct timespec stop_ts   = { 0 }; */
  /* getnstimeofday(&start_ts); */

  neon_debug("did %d : task %d : engage, check if busy",
             sched_dev->id, sched_task == NULL ? 0 : sched_task->pid);

  for_each_set_bit(i, sched_task->bmp_start2stop, nchan) {
    sched_work_t  *sched_work = &sched_dev->swork_array[i];
    neon_work_t   *neon_work  = sched_work->neon_work;
    neon_ctx_t    *neon_ctx   = neon_work->ctx;
    neon_chan_t   *neon_chan  = &neon_dev->chan[i];
    unsigned long  index_reg  = readl(neon_chan->ir_kvaddr);
    // The first 1-2 requests arriving are ignored, as it looks like
    // they are some kind of initialization requests. Notice that this
    // will mean ignoring requests when the index-reg loops back to 0.
    // This condition is not critical for performance/correctness,
    // but has been helpful in numerous debugging sessions, so it
    // stays on by default.
    if(index_reg > 1) {
      unsigned int refc_val = 0;
      neon_report("did %d : cid %d : pid %d : index %d :"
                  "task check if busy post re-eng",
                  sched_dev->id, i, sched_task->pid, index_reg);
      // since no request might have been witnessed, refc_kvaddr
      // might not have been built yet
      if(neon_work->refc_kvaddr != 0)
        refc_val = *((unsigned int *) neon_work->refc_kvaddr);
      if(refc_val != neon_work->refc_target) {
        neon_work_update(neon_ctx, neon_work, index_reg);
        refc_val = *((unsigned int *) neon_work->refc_kvaddr);
        if(refc_val < neon_work->refc_target) {
          neon_report("did %d : cid %d : pid %d : task found busy "
                      "(refc 0x%x, target 0x%x) --- fake-SUBMIT+ISSUE",
                      sched_dev->id, i, sched_task->pid,
                      refc_val, neon_work->refc_target);
          neon_work_submit(neon_work, 0);
          // Note: the work will not really be submitted so we have
          // to manually set the issued bit for the policy
          // to handle
          set_bit(sched_work->id, sched_task->bmp_issue2comp);
        } else
          neon_report("did %d : cid %d : pid %d : task found complete "
                      "(refc 0x%x, target 0x%x)",
                      sched_dev->id, i, sched_task->pid,
                      refc_val, neon_work->refc_target);
      }
    }
  }

  /* getnstimeofday(&stop_ts); */
  /* neon_report("did %d : update_disengaged took %ld usec", */
  /*             sched_dev->id, */
  /*             ((unsigned long) (timespec_to_ns(&stop_ts) / NSEC_PER_USEC)) - \ */
  /*             ((unsigned long) (timespec_to_ns(&start_ts) / NSEC_PER_USEC))); */

  return;
}
