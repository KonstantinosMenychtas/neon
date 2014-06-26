/**************************************************************************/
/*!
  \author  Kontantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief   Sampling-based Fair-Queueing scheduling algorithm; routines
*/
/**************************************************************************/

#include <linux/kernel.h>   // BUILD_BUG_ON_ZERO
#include <linux/slab.h>     // kmalloc/kzalloc
#include <linux/sysctl.h>   // sysctl
#include <linux/ktime.h>    // ktime
#include "neon_policy.h"    // policy interface
#include "neon_help.h"      // print wrappers

/***************************************************************************/
#define DFQ(x) ps.smpl.x

static char season_name[DFQ_TASK_NOFSEASONS+1][NAME_LEN] = {
  {'B', 'A', 'R', 'R', 'I', 'E', 'R', '\0',  0 ,   0 },
  {'D', 'R', 'A', 'I', 'N', 'I', 'N', 'G',  '\0',  0 },
  {'S', 'A', 'M', 'P', 'L', 'I', 'N', 'G',  '\0',  0 },
  {'F', 'R', 'E', 'E', 'R', 'U', 'N', '\0', '\0',  0 }
};

// sampling period
static unsigned int _sampling_T_ = NEON_SAMPLING_T_DEFAULT;
static unsigned int  sampling_T  = NEON_SAMPLING_T_DEFAULT;
ktime_t sampling_interval;

// all samples should be collected by cut-off time
static unsigned int _sampling_X_ = NEON_SAMPLING_X_DEFAULT;
static unsigned int  sampling_X  = NEON_SAMPLING_X_DEFAULT;

// sysctl/proc options
ctl_table neon_knob_sampling_options [] = {
  {
    .procname = "sampling_T",
    .data = &_sampling_T_,
    .maxlen = sizeof(int),
    .mode = 0666,
    .proc_handler = &proc_dointvec,
  },
  {
    .procname = "sampling_X",
    .data = &_sampling_X_,
    .maxlen = sizeof(int),
    .mode = 0666,
    .proc_handler = &proc_dointvec,
  },
  { 0 }
};

/**************************************************************************/
// no-interference (sampling) policy interface
static int  init_sampling(void);
static void fini_sampling(void);
static void reset_sampling(unsigned int onoff);
static int  create_sampling(sched_task_t *sched_task);
static void destroy_sampling(sched_task_t *sched_task);
static void start_sampling(sched_dev_t  * const sched_dev,
                           sched_work_t * const sched_work,
                           sched_task_t * const sched_task);
static void stop_sampling(sched_dev_t  * const sched_dev,
                          sched_work_t * const sched_work,
                          sched_task_t * const sched_task);
static void submit_sampling(sched_dev_t  * const sched_dev,
                            sched_work_t * const sched_work,
                            sched_task_t * const sched_task);
static void issue_sampling(sched_dev_t  * const sched_dev,
                           sched_work_t * const sched_work,
                           sched_task_t * const sched_task,
                           unsigned int had_blocked);
static void complete_sampling(sched_dev_t  * const sched_dev,
                              sched_work_t * const sched_work,
                              sched_task_t * const sched_task);
static void event_sampling(void);
static int  reengage_map_sampling(const neon_map_t * const neon_map);

neon_policy_face_t neon_policy_sampling = {
  .init = init_sampling,
  .fini = fini_sampling,
  .create = create_sampling,
  .destroy = destroy_sampling,
  .reset = reset_sampling,
  .start = start_sampling,
  .stop = stop_sampling,
  .submit = submit_sampling,
  .issue = issue_sampling,
  .complete = complete_sampling,
  .event = event_sampling,
  .reengage_map = reengage_map_sampling
};

/**************************************************************************/
// update_vtimes
/**************************************************************************/
// update virtual times for all tasks in device
// CAREFUL : sched_dev write lock held
static void
update_vtimes(sched_dev_t  * const sched_dev)
{
  sched_task_t  *stask             = NULL;
  unsigned long  min_vtime         = (unsigned long) (-1);
  unsigned long  total_avg_exe_dt  = 0;
  unsigned long  epoch_dt          = 0;
  unsigned long  nrqst_per_call    = 0;
  unsigned int   activity          = 0;

  list_for_each_entry(stask, &sched_dev->stask_list.entry, entry) {
    unsigned long avg_exe_dt = 0;
    // adjust average execution time, as expected by sampling,
    // to ensure that blocked tasks appear as not having run at all
    if(stask->DFQ(held_back) != 0) {
      activity = 1;
      continue;
    }
    if(stask->DFQ(nrqst_sampled) == 0)
      continue;
    activity = 1;
    if(stask->DFQ(ncall_sampled) > 0) {
      unsigned long nr = stask->DFQ(nrqst_sampled);
      unsigned long nc = stask->DFQ(ncall_sampled);
      nrqst_per_call = (nr + (nr%nc)) / nc;
    } else
      nrqst_per_call = 1;
    avg_exe_dt = nrqst_per_call * stask->DFQ(exe_dt_sampled)/   \
      stask->DFQ(nrqst_sampled);
    total_avg_exe_dt += avg_exe_dt;
  }
  if(activity == 0)
    goto just_decide;
  // This is possible in combined opencl/gl situations
  if(unlikely(total_avg_exe_dt == 0))
    goto just_decide;

  epoch_dt = sched_dev->DFQ(sampling_season_dt) * sampling_X;

  list_for_each_entry(stask, &sched_dev->stask_list.entry, entry) {
    unsigned long avg_exe_dt = 0;
    unsigned long vt         = 0;
    if(stask->DFQ(held_back) != 0) {
      if(stask->DFQ(vtime) < min_vtime)
        min_vtime = stask->DFQ(vtime);
      neon_report("DFQ : don't account pid %d (nrqst %ld, held_back %d) "
                  "but consider vtp %ld as dev-vtime %ld",
                  stask->pid, stask->DFQ(nrqst_sampled),
                  stask->DFQ(held_back), stask->DFQ(vtime),
                  sched_dev->DFQ(vtime));
      continue;
    }
    if(stask->DFQ(nrqst_sampled) == 0) {
      // idle tasks are not considered as candidates to
      // set device vtime
      neon_report("DFQ : don't account pid %d (nrqst %ld, held_back %d)",
                  stask->pid, stask->DFQ(nrqst_sampled),
                  stask->DFQ(held_back));
      continue;
    }

    if(stask->DFQ(ncall_sampled) > 0) {
      unsigned long nr = stask->DFQ(nrqst_sampled);
      unsigned long nc = stask->DFQ(ncall_sampled);
      nrqst_per_call = (nr + (nr%nc)) / nc;
    } else
      nrqst_per_call = 1;

    avg_exe_dt = nrqst_per_call * stask->DFQ(exe_dt_sampled)/   \
      stask->DFQ(nrqst_sampled);
    neon_report("DFQ : did %d : pid %d : exe_dt_sampled %ld : "
                "nrqst %ld : %ld calls/request : avg_exe_dt %lu ",
                sched_dev->id, stask->pid,  stask->DFQ(exe_dt_sampled),
                stask->DFQ(nrqst_sampled), nrqst_per_call, avg_exe_dt);

    vt = (avg_exe_dt * epoch_dt) / total_avg_exe_dt;
    stask->DFQ(vtime) += vt;
    if(stask->DFQ(vtime) < min_vtime)
      min_vtime = stask->DFQ(vtime);
    neon_account("DFQ : did %d : pid %d : vtd = %ld : "
                 "vtp +=  %ld/%ld (exe avg/total) * "
                 "%ld (epoch_dt) = %ld -> vtp = %ld ",
                 sched_dev->id, stask->pid, sched_dev->DFQ(vtime),
                 avg_exe_dt, total_avg_exe_dt,
                 epoch_dt, vt, stask->DFQ(vtime));
  }
  if(min_vtime < (unsigned long) -1) {
    sched_dev->DFQ(vtime) = min_vtime;
    list_for_each_entry(stask, &sched_dev->stask_list.entry, entry) {
      if(stask->DFQ(vtime) < sched_dev->DFQ(vtime)) {
        neon_report("DFQ : process %-6d : vtime %-15lu <  dev vtime %-15lu "
                    "---> ___MOVED fwd to match",
                    stask->pid, stask->DFQ(vtime), sched_dev->DFQ(vtime));
        stask->DFQ(vtime) = sched_dev->DFQ(vtime);
      } else {
        neon_report("DFQ : process %-6d : vtime %-15lu >= dev vtime %-15lu ---> "
                    "NOT_MOVED fwd",
                    stask->pid, stask->DFQ(vtime), sched_dev->DFQ(vtime));
      }
    }
  }

 just_decide:
  sched_dev->DFQ(active) = activity;
  
  list_for_each_entry(stask, &sched_dev->stask_list.entry, entry) {
    // update vtime, decide to block advanced tasks if
    // the whole system is not idle
    if(stask->DFQ(vtime) > (sched_dev->DFQ(vtime) + epoch_dt))
      stask->DFQ(held_back) = 1;
    else
      stask->DFQ(held_back) = 0;
    // reset/task counters
    stask->DFQ(exe_dt_sampled) = 0;
    stask->DFQ(nrqst_sampled) = 0;
    stask->DFQ(ncall_sampled) = 0;
    neon_report("DFQ : did %d : pid %d : vtp %ld : vtd %ld + epoch_dt %d : "
                "held_back %d : dev-activity %d : update_vtime",
                sched_dev->id, stask->pid, stask->DFQ(vtime),
                sched_dev->DFQ(vtime), epoch_dt, stask->DFQ(held_back),
                sched_dev->DFQ(active));
  }
  // reset dev stats
  sched_dev->DFQ(sampling_season_dt) = 0;
  sched_dev->DFQ(sampled_task) = NULL;

  return;
}

/****************************************************************************/
// update_sampled_task
/****************************************************************************/
// called after a full device draining or previous sample, a sched_task
// will be picked to be sampled for sampling_T by this routine
// CAREFUL : sched_dev write lock held
static void
update_sampled_task(sched_dev_t *sched_dev)
{
  //  unsigned int  nchan        = neon_global.dev[sched_dev->id].nchan;
  sched_task_t     *last_sampled = NULL;
  sched_task_t     *now_sampled  = NULL;
  struct list_head *pos          = NULL;

  if(list_empty(&sched_dev->stask_list.entry)) {
    // This should not happen, unless a timer is not cancelled at exit
    neon_warning("%s : did %d : empty task list @ next sample pick",
               __func__, sched_dev->id);
    sched_dev->DFQ(sampled_task) = NULL;
    // not a BUG() but needs to be handled
    goto just_pick;
  }

  last_sampled = sched_dev->DFQ(sampled_task);
  if(last_sampled == NULL) {
    // no task has been sampled before in this epoch,
    // start with first entry
    pos = sched_dev->stask_list.entry.next;
    neon_report("DFQ : did %d : last sampled 0, start at head _SAMPLING",
                sched_dev->id);
  } else {
    // this is not the first task to be sampled in this epoch,
    // pick up where we left off with last sampled task and
    // reset if we reached the head again
    pos = last_sampled->entry.next;
    if(pos == &sched_dev->stask_list.entry ) {
      neon_report("DFQ : did %d : last sampled %d, fully circled _SAMPLING",
                  sched_dev->id, last_sampled->pid);
      goto just_pick;
    } else
      neon_report("DFQ : did %d : last sampled %d, start at next _SAMPLING",
                  sched_dev->id, last_sampled->pid);
  }
  while(1) {
    now_sampled = list_entry(pos, sched_task_t, entry);
    //    if(now_sampled->DFQ(mng_chans) > 0 && now_sampled->DFQ(held_back) == 0) {
    if(now_sampled->DFQ(held_back) == 0) { // || sched_dev->DFQ(active) == 0) {
      neon_report("DFQ : did %d : pid %d : held-back %d : sem %d : "
                  "dev-active %d : DONT_SKIP_SAMPLING",
                  sched_dev->id, now_sampled->pid, now_sampled->DFQ(held_back),
                  now_sampled->DFQ(sem_count), sched_dev->DFQ(active));
      break;
    } else {
      // fake-increase time spent in sampling season or else
      // freerun will be unnecessarily short
      sched_dev->DFQ(sampling_season_dt) += sampling_T * USEC_PER_MSEC;
      neon_report("DFQ : did %d : pid %d : held-back %d : sem %d : "
                  "dev-active %d : DO___SKIP_SAMPLING",
                  sched_dev->id, now_sampled->pid, now_sampled->DFQ(held_back),
                  now_sampled->DFQ(sem_count), sched_dev->DFQ(active));
    }
    pos = now_sampled->entry.next;
    if(pos == &sched_dev->stask_list.entry) {
      now_sampled = NULL;
      break;
    }
  }

 just_pick:

  // update dev indicator of last sampled task
  sched_dev->DFQ(sampled_task) = now_sampled;

  neon_report("DFQ : picked %d (sem-count %d) for samplng",
              now_sampled == NULL ? 0 : now_sampled->pid,
              now_sampled == NULL ? 0 : now_sampled->DFQ(sem_count));

  return;
}



/****************************************************************************/
// update_now
/****************************************************************************/
// Pick task to sample (if any) and update all tasks virtual times
// CAREFUL : sched_dev write lock held
static ktime_t
update_now(sched_dev_t *sched_dev)
{
  sched_task_t *last_sampled = NULL;
  sched_task_t *now_sampled  = NULL;
  season_t      last_season  = DFQ_TASK_NOFSEASONS;
  ktime_t       interval     = sampling_interval;
  sched_task_t *stask        = NULL;

  // update sampled task
  last_season = sched_dev->DFQ(season);
  last_sampled = sched_dev->DFQ(sampled_task);
  update_sampled_task(sched_dev);
  now_sampled  = sched_dev->DFQ(sampled_task);

  // if a full sampling season has finished, then update vtimes
  if(sched_dev->DFQ(sampled_task) != NULL) {
    // account time to be spent sampling
    sched_dev->DFQ(sampling_season_dt) += sampling_T * USEC_PER_MSEC;
  } else {
    // all tasks have been sampled, move to FREERUN season
    if(sched_dev->DFQ(sampling_season_dt) == 0) {
      // if we had not witnessed any new requests in the last sampling
      // interval, make sure to still set a limit to the freerun season
      sched_dev->DFQ(sampling_season_dt) = sampling_T * USEC_PER_MSEC;
    }
    interval = ktime_set(0, sampling_X *
                         sched_dev->DFQ(sampling_season_dt) * NSEC_PER_USEC);
    // NOTE:
    // To test season correctness without virtual-time enforcement,
    // one can do this
    // sched_dev->DFQ(season) = DFQ_TASK_FREERUN; // OR DFQ_TASK_BARRIER;
    // sched_dev->DFQ(sampling_season_dt) = 0;
    // rather than
    sched_dev->DFQ(season) = DFQ_TASK_FREERUN;
    update_vtimes(sched_dev);
  }

  // unblock those who should be unblocked
  list_for_each_entry(stask, &sched_dev->stask_list.entry, entry) {
    if(now_sampled == NULL || stask == now_sampled) {
      neon_report("DFQ : did %d : pid %d : held-back %d : sem %d : "
                  "unblock %s", sched_dev->id, stask->pid,
                  stask->DFQ(held_back), stask->DFQ(sem_count),
                  now_sampled == NULL ? "all not held-back" : "sampled");
      if(stask->DFQ(sem_count) < 0 && stask->DFQ(held_back) == 0) {
        stask->DFQ(sem_count)++;
        up(&stask->DFQ(sem));
      }
    }
  }
  
  neon_report("DFQ : %s->%s : did %d : update_now %d -> %d (%s will proceed)",
              season_name[last_season], season_name[sched_dev->DFQ(season)],
              sched_dev->id, last_sampled == NULL ? 0 : last_sampled->pid,
              now_sampled == NULL ? 0 : now_sampled->pid,
              now_sampled == NULL ? "everybody" : "select only");

  return interval;
}

/****************************************************************************/
// season_timer_callback
/****************************************************************************/
// alarm signifying standard season or sampling period transition
static enum hrtimer_restart
season_timer_callback(struct hrtimer *timer)
{
  sampling_dev_t *sampling_dev = container_of(timer, sampling_dev_t,
                                              season_timer);
  policy_dev_t *policy_dev    = (policy_dev_t *) sampling_dev;
  sched_dev_t  *sched_dev     = container_of(policy_dev, sched_dev_t, ps);

#ifdef NEON_DEBUG_LEVEL_3
  if(sched_dev->id == NEON_MAIN_GPU_DID) {
    struct timespec now_ts      = { 0 };
    unsigned long   ts          = 0;
    getnstimeofday(&now_ts);
    ts = (unsigned long) (timespec_to_ns(&now_ts) / NSEC_PER_USEC);
    neon_report("DFQ : did %d : nctx %d : alarm timer callback @ %ld",
                sched_dev->id, atomic_read(&neon_global.ctx_live), ts);
  }
#endif // NEON_DEBUG_LEVEL_3
  
  if(atomic_read(&neon_global.ctx_live) > 0) {
    read_lock(&sched_dev->lock);
    if(sched_dev->DFQ(update_ts) == 0) {
      atomic_set(&sampling_dev->action, 1);
      wake_up_interruptible(&neon_kthread_event_wait_queue);
    }
    read_unlock(&sched_dev->lock);
  }

  return HRTIMER_NORESTART;
}

/**************************************************************************/
// init_sampling
/**************************************************************************/
// initialize DFQ specific structs
static int
init_sampling(void)
{
  unsigned int i = 0;

  for(i = 0; i < neon_global.ndev; i++) {
    sched_dev_t *sched_dev = &sched_dev_array[i];
#ifdef NEON_SAMPLING_COMP0_ONLY
    if(i > 0)
      break;
#endif // NEON_SAMPLING_COMP0_ONLY
    sched_dev->DFQ(season) = DFQ_TASK_BARRIER;
    atomic_set(&sched_dev->DFQ(action), 0);
    hrtimer_init(&sched_dev->DFQ(season_timer), CLOCK_MONOTONIC,
                 HRTIMER_MODE_REL);
    sched_dev->DFQ(season_timer).function = &season_timer_callback;
  }

  neon_info("DFQ : init");

  return 0;
}

/**************************************************************************/
// fini_sampling
/**************************************************************************/
// finalize and destroy DFQ-specific structs
static void
fini_sampling(void)
{
  unsigned int i = 0;

  // cancel any live season timers; reset(0) must have already stopped them
  // but force a stop otherwise
  for(i = 0; i < neon_global.ndev; i++) {
    sched_dev_t *sched_dev = &sched_dev_array[i];
#ifdef NEON_SAMPLING_COMP0_ONLY
    if(i > 0)
      break;
#endif // NEON_SAMPLING_COMP0_ONLY
    atomic_set(&sched_dev->DFQ(action), 0);
    if(hrtimer_cancel(&sched_dev->DFQ(season_timer)) != 0)
      neon_error("%s : did %d : Sampling timer was busy at fini",
                 __func__, i);
  }

  neon_info("DFQ : fini");

  return;
}

/**************************************************************************/
// reset_sampling
/**************************************************************************/
// Reset Sampling-based Fair Queuing scheduling structs (checkpoint)
static void
reset_sampling(unsigned int nctx)
{
  unsigned int i = 0 ;

  if(nctx == 1) {
    sampling_T = _sampling_T_;
    sampling_X = _sampling_X_;

    if(sampling_T < NEON_POLLING_T_MIN) {
      neon_warning("Adjusting sampling T %u to implicit min = "
                   "min_polling %d T",
                   sampling_T, NEON_POLLING_T_MIN);
      sampling_T = NEON_POLLING_T_MIN;
    }
    if(sampling_T > NEON_SAMPLING_T_MAX) {
      neon_warning("Adjusting sampling T %u to max default %d T",
                   sampling_T, NEON_SAMPLING_T_MAX);
      sampling_T = NEON_SAMPLING_T_MAX;
    }
    if(sampling_X == 0) {
      neon_warning("Adjusting free-run to default %d*sampling_T",
                   sampling_X, NEON_SAMPLING_X_DEFAULT);
      sampling_X = NEON_SAMPLING_X_DEFAULT;
    }
    sampling_interval = ktime_set(0, sampling_T * NSEC_PER_MSEC);

    for(i = 0; i < neon_global.ndev; i++) {
      sched_dev_t *sched_dev = &sched_dev_array[i];
#ifdef NEON_SAMPLING_COMP0_ONLY
      if(i > 0)
        break;
#endif // NEON_SAMPLING_COMP0_ONLY
      sched_dev->DFQ(season) = DFQ_TASK_BARRIER;
      sched_dev->DFQ(vtime) = 0;
      sched_dev->DFQ(sampling_season_dt) = 0;
      sched_dev->DFQ(update_ts) = 0;
      sched_dev->DFQ(sampled_task) = NULL;
      atomic_set(&sched_dev->DFQ(action), 0);
    }
    neon_info("DFQ : Sampling reset; (re)start with T=%d mSec",
              sampling_T);
  }
  if(nctx == 0) {
    for(i = 0; i < neon_global.ndev; i++) {
      sched_dev_t *sched_dev = &sched_dev_array[i];
#ifdef NEON_SAMPLING_COMP0_ONLY
      if(i > 0)
        break;
#endif // NEON_SAMPLING_COMP0_ONLY
      // TODO: try to include sched_dev->DFQ(sampled_task) != NULL ||
      // in the check for status ? currently update_vtimes does
      // this and if it fails to run, i know reset won't happen
      if(atomic_cmpxchg(&sched_dev->DFQ(action), 1, 0) == 0) {
        //      if(atomic_read(&sched_dev->DFQ(action)) != 0) {
        // unclean status at nctx == 0 should be impossible
        // if work-complete/stop worked as they should have been handled
        neon_warning("%s : did %d : unclean status @ nctx == 0",
                     __func__, sched_dev->id);
        //        BUG();
        return;
      }
    }
    neon_info("DFQ : Sampling reset; stop");
  }

  neon_info("DFQ : (re)set");

  return;
}

/**************************************************************************/
// create_sampling
/**************************************************************************/
// build new GPU sched-task sampling scheduler entries
static int
create_sampling(sched_task_t *sched_task)
{
  // policy-specific struct initializer
  memset(&sched_task->ps.smpl, 0, sizeof(sampling_task_t));
  sema_init(&sched_task->DFQ(sem), 0);

  neon_debug("DFQ : pid %d : create sched-task", sched_task->pid);

  return 0;
}

/**************************************************************************/
// destroy_sampling
/**************************************************************************/
// destroy GPU sched-task sampling scheduler entries
// CAREFUL : sched-dev write lock held
static void
destroy_sampling(sched_task_t *sched_task)
{
  // safety-check; preceeding stop/complete must have cleaned up properly
  if(sched_task->DFQ(held_back) != 0) {
    neon_warning("%s : DFQ : pid %d : held back task @ destroy "
                 "unblocked @ destroy", __func__, sched_task->pid);
    if(sched_task->DFQ(sem_count) < 0) {
      sched_task->DFQ(sem_count)++;
      up(&sched_task->DFQ(sem));
    }
    // sched_task->DFQ(held_back) = 0;
  }

  neon_debug("DFQ - pid %d : destroy sched-task", sched_task->pid);

  return;
}

/**************************************************************************/
// start_sampling
/**************************************************************************/
// new work, channel occupied
// CAREFUL : sched-dev write lock held
static void
start_sampling(sched_dev_t  * const sched_dev,
               sched_work_t * const sched_work,
               sched_task_t * const sched_task)
{
  // incoming jobs start engaged; it is only upon a submit attempt
  // that task and system status may change
  ++sched_task->DFQ(occ_chans);

#ifdef NEON_SAMPLING_COMP0_ONLY
  if(sched_task->DFQ(occ_chans) % 2 == 0 ||
     sched_dev->id != NEON_MAIN_GPU_DID) {
    sched_work->DFQ(heed) = 0;
    sched_work->DFQ(engage) = 0;
    goto just_start;
  }
#endif // NEON_SAMPLING_COMP0_ONLY

  sched_work->DFQ(heed) = 1;
  sched_work->DFQ(engage) = 1;
  if(sched_task->DFQ(mng_chans)++ == 0)
    sched_task->DFQ(vtime) = sched_dev->DFQ(vtime);

 just_start :

  if(sched_dev->id == NEON_MAIN_GPU_DID)
    neon_report("DFQ : %s : did %d : cid %d : pid %d : sem %d : "
                "heed %d : engage %d : "
                "mng_chan %d : dma %d : vtime %d : start",
                season_name[sched_dev->DFQ(season)],
                sched_dev->id, sched_work->id, sched_task->pid,
                sched_task->DFQ(sem_count),
                sched_work->DFQ(heed), sched_work->DFQ(engage),
                sched_task->DFQ(mng_chans),
                sched_work->DFQ(heed),  sched_task->DFQ(vtime));

  return;
}

/**************************************************************************/
// stop_sampling
/**************************************************************************/
// remove work from channel
// CAREFUL : sched-dev write lock held
static void
stop_sampling(sched_dev_t  * const sched_dev,
              sched_work_t * const sched_work,
              sched_task_t * const sched_task)
{
  season_t      last_season   = sched_dev->DFQ(season);
  sched_task_t *last_sampled  = sched_dev->DFQ(sampled_task);

  --sched_task->DFQ(occ_chans);
  if(sched_work->DFQ(heed) != 0)
    --sched_task->DFQ(mng_chans);

  if(sched_task->DFQ(occ_chans) != 0) {
    if(sched_dev->id == NEON_MAIN_GPU_DID)
      neon_report("DFQ : did %d : cid %d : heed %d : mng/occ %d/%d : "
                  "vtime %d : ignore-work : stop",
                  sched_dev->id, sched_work->id, sched_work->DFQ(heed),
                  sched_task->DFQ(mng_chans), sched_task->DFQ(occ_chans),
                  sched_task->DFQ(vtime));
    goto just_stop;
  } else {
    if(sched_dev->id == NEON_MAIN_GPU_DID)
      neon_report("DFQ : did %d : cid %d : heed %d : mng/occ %d/%d : "
                  "vtime %d : halt-work : stop",
                  sched_dev->id, sched_work->id, sched_work->DFQ(heed),
                  sched_task->DFQ(mng_chans), sched_task->DFQ(occ_chans),
                  sched_task->DFQ(vtime));
  }

  // if exiting task is blocked, for whatever reason, unblock it
  while(sched_task->DFQ(sem_count) < 0) {
    sched_task->DFQ(sem_count)++;
    up(&sched_task->DFQ(sem));
  }

  // completion notification must have preceded
  switch(last_season) {
  case DFQ_TASK_BARRIER:
  case DFQ_TASK_DRAINING:
    // preceding completion events and stop-handling routines manage
    // updates sufficiently at this state - nothing DFQ-specific
    break;
  case DFQ_TASK_SAMPLING:
    // force start over with task selection if selected task is exiting
    if(last_sampled == sched_task)
      sched_dev->DFQ(sampled_task) = NULL;
    break;
  case DFQ_TASK_FREERUN:
    // if the task was being held back, mark as let go
    // (already unblocked a few lines back)
    sched_task->DFQ(held_back) = 0;
    break;
  default :
    neon_error("%s : DFQ Unknown season", __func__);
    BUG();
    break;
  }

  // force a new event to occur
  if(hrtimer_try_to_cancel(&sched_dev->DFQ(season_timer)) != -1) {
    neon_report("%s : canceled timer, set wake up event", __func__);
    if(atomic_read(&neon_global.ctx_live) > 0) {
      atomic_set(&sched_dev->DFQ(action), 1);
      wake_up_interruptible(&neon_kthread_event_wait_queue);
    }
  }

  if(sched_dev->id == NEON_MAIN_GPU_DID)
    neon_report("DFQ : %s : did %d : cid %d : pid %d [%d] : sem %d : "
                "vtime %d : %s held back : stop",
                season_name[last_season],
                sched_dev->id, sched_work->id, sched_task->pid,
                sched_dev->DFQ(sampled_task) == NULL ? 0 :
                sched_dev->DFQ(sampled_task)->pid,
                sched_task->DFQ(sem_count), sched_task->DFQ(vtime),
                sched_task->DFQ(held_back) == 0 ? "not" : "was");

 just_stop:

  sched_work->DFQ(engage) = 0;

  return;
}

/**************************************************************************/
// count_incomplete_rqst
/**************************************************************************/
// count requests issued, but not yet marked complete, in the same task
static unsigned int
count_incomplete_rqst(const sched_dev_t  * const sched_dev,
                       const sched_task_t * const sched_task)
{
  neon_dev_t   *neon_dev = &neon_global.dev[sched_dev->id];
  unsigned int  nchan    = neon_dev->nchan;
  unsigned int  i        = 0;
  unsigned int  ret      = 0;
  
  for_each_set_bit(i, sched_task->bmp_issue2comp, nchan)
    ret++;
  
  return ret;
}

/**************************************************************************/
// submit_sampling
/**************************************************************************/
// submit a request for scheduling consideration under SAMPLING policy
// CAREFUL sched-dev write lock held
static void
submit_sampling(sched_dev_t  * const sched_dev,
                sched_work_t * const sched_work,
                sched_task_t * const sched_task)
{
  season_t      last_season = sched_dev->DFQ(season);
  unsigned int  block       = 0;
  unsigned long exe_dt      = 0;

  if(sched_work->DFQ(heed) == 0 || sched_work->DFQ(engage) == 0)
    goto just_submit;

  switch(last_season) {
  case DFQ_TASK_BARRIER :
    // All requests arriving while in barrier/draining season
    // will be stopped; a new request arriving in BARRIER state
    // will push the system to start draining
    atomic_set(&sched_dev->DFQ(action), 1);
    wake_up_interruptible(&neon_kthread_event_wait_queue);
  case DFQ_TASK_DRAINING :
    block = 1;
    break;
  case DFQ_TASK_SAMPLING :
    //       (sched_dev->DFQ(sampled_task) != NULL &&
    if(sched_dev->DFQ(sampled_task) != sched_task ||
       sched_dev->DFQ(update_ts) != 0 ||
       sched_task->DFQ(nrqst_sampled) >= NEON_SAMPLING_CRITICAL_MASS) {
      block = 1;
      // cancel ongoing sampling period, have enough material;
      // let system know of early completion of sampling period
      if(sched_task->DFQ(nrqst_sampled) >= NEON_SAMPLING_CRITICAL_MASS) {
        if(hrtimer_try_to_cancel(&sched_dev->DFQ(season_timer)) == -1)
          neon_error("%s : could not cancel sampling timer", __func__);
        neon_report("%s : canceled timer, set wake up event", __func__);
        atomic_set(&sched_dev->DFQ(action), 1);
        wake_up_interruptible(&neon_kthread_event_wait_queue);
      }
    } else
      block = 0;
    if(test_bit(sched_work->id, sched_task->bmp_issue2comp) != 0) {
      //      && count_incomplete_rqst(sched_dev, sched_task) == 1) {
      struct timespec complete_ts = sched_work->submit_ts;
      struct timespec dtime       = { 0 };
      dtime = timespec_sub(complete_ts, sched_work->issue_ts);
      exe_dt = (unsigned long) (timespec_to_ns(&dtime) / NSEC_PER_USEC);
      sched_task->DFQ(exe_dt_sampled) += exe_dt;
    }
    break;
  case DFQ_TASK_FREERUN:
    // notice that new tasks starting off at freerun get unobstructed
    // access to the device; make sure to bring them back under control
    // when freerun ends
    if(sched_task->DFQ(held_back) == 0) {
      block = 0;
      sched_work->DFQ(engage) = 0;
    } else {
      block = 1;
      sched_work->DFQ(engage) = 1;
    }
    break;
  default :
    neon_error("%s : DFQ Unknown season", __func__);
  }
  
  neon_info("DFQ : %s : did %d : cid %d : pid %d [%d] : "
            "exe_dt = %ld (added %ld [i2c %d|#%d -> %s]) : "
            "nrqst %ld (+1 on issue) : submit %s @ %ld",
            season_name[sched_dev->DFQ(season)],
            sched_dev->id, sched_work->id,
            sched_task->pid, sched_dev->DFQ(sampled_task) == NULL ? 0 :    \
            sched_dev->DFQ(sampled_task)->pid, sched_task->DFQ(exe_dt_sampled),
            exe_dt, (test_bit(sched_work->id, sched_task->bmp_issue2comp) != 0),
            count_incomplete_rqst(sched_dev, sched_task),
            exe_dt == 0 ? "_new_" : "_b2b_",
            sched_task->DFQ(nrqst_sampled), 
            block == 1 ? "WILL__BLOCK" : "WONT_BLOCK",
            (unsigned long) (timespec_to_ns(&sched_work->submit_ts)));
            
  if(block == 1) {
    // because as I realized update_ts != 0, subsequent request
    // that will block will consider issue-bit set
    clear_bit(sched_work->id, sched_task->bmp_issue2comp);
    sched_task->DFQ(sem_count)--;
    write_unlock(&sched_dev->lock);
    // wait here
    down_interruptible(&sched_task->DFQ(sem));
    write_lock(&sched_dev->lock);
  }

 just_submit:
  neon_policy_issue(sched_dev, sched_work, sched_task, block);

  return;
}

/**************************************************************************/
// issue_sampling
/**************************************************************************/
// issue request to GPU for processing, scheduled SAMPLING
static void
issue_sampling(sched_dev_t  * const sched_dev,
               sched_work_t * const sched_work,
               sched_task_t * const sched_task,
               unsigned int had_blocked)
{
  season_t last_season = sched_dev->DFQ(season);

  if(sched_work->DFQ(heed) == 0 || sched_work->DFQ(engage) == 0)
    goto just_issue;

  switch(last_season) {
  case DFQ_TASK_BARRIER:
  case DFQ_TASK_DRAINING:
    // in barrier/draining season, all commands should be blocked
    // and never arrive here
    neon_warning("%s : did %d : cid %d : pid %d : issued while in %s ",
                 __func__, sched_dev->id, sched_work->id,
                 sched_task->pid, season_name[sched_dev->DFQ(season)]);
    //    BUG();
    break;
  case DFQ_TASK_SAMPLING:
    // count issued request, except for transitional (crossing periods) requests
    // Note ; The request-size estimation heuristic will give slightly
    // larger request size if possibly) overlapping requests by the same task
    // (the non-blocking requests observed in combo CL/GL apps) 
    // are not accounted for individually; uncomment next line to observe
    // if(count_incomplete_rqst(sched_dev, sched_task) < 2)
    sched_task->DFQ(nrqst_sampled)++;
    // call grouping
    if(sched_work->part_of_call != 0)
      sched_task->DFQ(ncall_sampled)++;
    break;
  case DFQ_TASK_FREERUN:
    // notice that new tasks starting off at freerun get unobstructed
    // access to the device; make sure to bring them back under control
    // when freerun ends
    if(sched_task->DFQ(held_back) == 0)
      sched_work->DFQ(engage) = 0;
    else
      sched_work->DFQ(engage) = 1;
    break;
  default:
    neon_error("%s : DFQ Unknown season", __func__);
  }

  neon_info("DFQ : %s : did %d : cid %d : pid %d [%d] : "
            "engage %d : sem %d : issue... ",
            season_name[sched_dev->DFQ(season)],
            sched_dev->id, sched_work->id, sched_task->pid,
            sched_dev->DFQ(sampled_task) == NULL ? 0 :        \
            sched_dev->DFQ(sampled_task)->pid, sched_work->DFQ(engage), 
            sched_task->DFQ(sem_count));
            
  neon_info("DFQ : held_back %d : refc 0x%x/0x%x :"
            "exe_dt %ld : nrqst %ld [i2c %d|#%d]: %s : ...issue",
            sched_task->DFQ(held_back),
            *((unsigned int *) sched_work->neon_work->refc_kvaddr),
            sched_work->neon_work->refc_target,
            sched_task->DFQ(exe_dt_sampled), sched_task->DFQ(nrqst_sampled),
            (test_bit(sched_work->id, sched_task->bmp_issue2comp) != 0),
            count_incomplete_rqst(sched_dev, sched_task),
            had_blocked == 1 ? "UN__BLOCKED" : "NOT_BLOCKED");

 just_issue:
  return;
}

/**************************************************************************/
// complete_sampling
/**************************************************************************/
// mark completion of GPU request
// CAREFUL : sched_dev write lock held
static void
complete_sampling(sched_dev_t  * const sched_dev,
                  sched_work_t * const sched_work,
                  sched_task_t * const sched_task)
{
  neon_dev_t      *neon_dev     = &neon_global.dev[sched_dev->id];
  unsigned int     nchan        = neon_dev->nchan;
  season_t         last_season  = sched_dev->DFQ(season);
  struct timespec  now_ts       = { 0 };
  struct timespec  dtime        = { 0 };
  unsigned long    ts           = 0;
  unsigned long    exe_dt       = 0;
  unsigned int     account      = 0;

  if(sched_work->DFQ(heed) == 0 || sched_work->DFQ(engage) == 0)
    goto just_complete;

  getnstimeofday(&now_ts);
  ts = (unsigned long) (timespec_to_ns(&now_ts) / NSEC_PER_USEC);

  switch(last_season) {
  case DFQ_TASK_BARRIER :
    // requests can (legally) appear to complete while in barrier state
    // (e.g. if the only request issued during free-run period was
    // the first request to be submitted in that season),
    // but this event is not special
    break;
  case DFQ_TASK_DRAINING :
    // check whether all tasks have been drained; it is possible
    // never to reach here if the channel queues were empty when
    // the barrier was hit.
    if(--sched_dev->DFQ(countdown) <= 0) {
      // TODO : CHECK --- NEEDS TO  RESET TIMER ?
      atomic_set(&sched_dev->DFQ(action), 1);
      wake_up_interruptible(&neon_kthread_event_wait_queue);
    }
    break;
  case DFQ_TASK_SAMPLING :
    if(unlikely(sched_dev->DFQ(sampled_task) != sched_task)) {
      // no task outside the scheduled sampled task should see
      // its tasks completing in sampling season
      // (still possible after re-engaging if the last blocked
      // task has not had the chance to be acknowledged as complete)
      neon_warning("%s : DFQ : %s : did %d : cid %d : pid %d [%d] : "
                   "refc 0x%x/0x%x : vtime %ld : complete != sampled ",
                   __func__, season_name[sched_dev->DFQ(season)],
                   sched_dev->id, sched_work->id, sched_task->pid,
                   sched_dev->DFQ(sampled_task) == NULL ? 0 :     \
                   sched_dev->DFQ(sampled_task)->pid,
                   *((unsigned int *) sched_work->neon_work->refc_kvaddr),
                   sched_work->neon_work->refc_target, sched_task->DFQ(vtime));
      break;
    }
    // account if this was not overuse
    if(sched_dev->DFQ(update_ts) == 0) {
      // count only last of (possibly) overlapping requests
      //      if(count_incomplete_rqst(sched_dev, sched_task) == 0) {
        struct timespec complete_ts = now_ts;
        struct timespec dtime       = { 0 };
        dtime = timespec_sub(complete_ts, sched_work->issue_ts);
        exe_dt = (unsigned long) (timespec_to_ns(&dtime) / NSEC_PER_USEC);
        sched_task->DFQ(exe_dt_sampled) += exe_dt;
        account = 1;
        //      }
    } else {
      // overusing request
      // mark completion and transition to next sampled task
      // or freerun period (handled by kthread)
      if(bitmap_empty(sched_task->bmp_issue2comp, nchan)) {
        // only include overuse requests in the resource usage estimation
        // when the critical mass of sampled requests has not been met
        if(sched_task->DFQ(nrqst_sampled) <= NEON_SAMPLING_CRITICAL_MASS) {
          dtime = timespec_sub(now_ts, sched_work->issue_ts);
          exe_dt = (unsigned long) (timespec_to_ns(&dtime) / NSEC_PER_USEC);
          sched_task->DFQ(exe_dt_sampled) += exe_dt;
          sched_dev->DFQ(sampling_season_dt) += (ts - sched_dev->DFQ(update_ts));
          account = 1;
        } else
          sched_task->DFQ(nrqst_sampled)--;
        sched_dev->DFQ(update_ts) = 0;
        atomic_set(&sched_dev->DFQ(action), 1);
        wake_up_interruptible(&neon_kthread_event_wait_queue);
      }
      neon_info("DFQ : %s : did %d : cid %d : pid %d [%d] : "
                "nrqst %ld : smpl_season_dt %ld : wake-up kthread for complete @ %ld",
                season_name[sched_dev->DFQ(season)],
                sched_dev->id, sched_work->id, sched_task->pid,
                sched_dev->DFQ(sampled_task) == NULL ? 0 :      \
                sched_dev->DFQ(sampled_task)->pid,
                sched_task->DFQ(nrqst_sampled), sched_dev->DFQ(sampling_season_dt),ts);
    }
    break;
  case DFQ_TASK_FREERUN :
    // completion events can (legally) arrive at freerun period
    // (the work issued immediately after re-engaging)
    if(sched_task->DFQ(held_back) == 0)
      sched_work->DFQ(engage) = 0;
    else
      sched_work->DFQ(engage) = 1;
    break;
  default:
    neon_error("%s : DFQ Unknown season", __func__);
  }

  neon_info("DFQ : %s : did %d : cid %d : pid %d [%d] : eng %d : "
            "held_back %d : refc 0x%x/0x%x : exe_dt = %ld (added %ld) : "
            "nrqst %ld [i2c %d|#%d] : complete @ %ld",
            season_name[sched_dev->DFQ(season)],
            sched_dev->id, sched_work->id, sched_task->pid,
            sched_dev->DFQ(sampled_task) == NULL ? 0 :
            sched_dev->DFQ(sampled_task)->pid,
            sched_work->DFQ(engage), sched_task->DFQ(held_back),
            *((unsigned int *) sched_work->neon_work->refc_kvaddr),
            sched_work->neon_work->refc_target,
            sched_task->DFQ(exe_dt_sampled), account == 1 ? exe_dt : 0,
            sched_task->DFQ(nrqst_sampled),
            (test_bit(sched_work->id, sched_task->bmp_issue2comp) != 0),
            count_incomplete_rqst(sched_dev, sched_task), ts);

 just_complete:
  return;
}

/**************************************************************************/
// event_sampling
/**************************************************************************/
// asynchronous event handler
static void
event_sampling(void)
{
  unsigned int i = 0;
  unsigned int j = 0;

  for(i = 0; i < neon_global.ndev; i++) {
    sched_dev_t     *sched_dev     = &sched_dev_array[i];
    unsigned int     nchan         = neon_global.dev[i].nchan;
    season_t         last_season   = DFQ_TASK_NOFSEASONS;
    sched_task_t    *last_sampled  = NULL;
    sched_task_t    *stask         = NULL;
    struct timespec  now_ts        = { 0 };
    unsigned long    ts            = 0;
    ktime_t          interval      = { .tv64 = 0 };

    if(atomic_cmpxchg(&sched_dev->DFQ(action), 1, 0) == 0)
      continue;

#ifdef NEON_SAMPLING_COMP0_ONLY
    if(i > 0)
      break;
#endif // NEON_SAMPLING_COMP0_ONLY

    getnstimeofday(&now_ts);
    ts = (unsigned long) (timespec_to_ns(&now_ts) / NSEC_PER_USEC);

    write_lock(&sched_dev->lock);

    last_season = sched_dev->DFQ(season);

    neon_debug("DFQ sampling_event: season %s : did %d",
               season_name[last_season], sched_dev->id);
    
    switch(last_season) {
    case DFQ_TASK_FREERUN :
      // reengage freeruners
      for(j = 0; j < nchan; j++) {
        sched_work_t *swork = &sched_dev->swork_array[j];
        if(swork->DFQ(heed) != 0) {
          if(swork->DFQ(engage) == 0) {
            swork->DFQ(engage) = 1;
            neon_track_restart(1, swork->neon_work->ir);
            neon_report("DFQ : did %d : cid %d : pid %d : re_-engaged",
                        i, j, swork->pid);
          } else
            neon_report("DFQ : did %d : cid %d : pid %d : was-engaged",
                        i, j, swork->pid);
        }
      }
      sched_dev->DFQ(season) = DFQ_TASK_BARRIER;
      last_season = sched_dev->DFQ(season);
      neon_report("DFQ : freerun season over %s @ %ld - alarm",
                  sched_dev->DFQ(active) != 0 ? "enter_BARRIER" : \
                  "set___BARRIER", ts);
      if(sched_dev->DFQ(active) == 0) {
        // We can stay in BARRIER state if there exist no active tasks
        // currently; the system will enter BARRIER immediately upon
        // 1st request with a new event. The system's information is
        // up to date until right before FREERUN begun --- by moving
        // to BARRIER it will become up to date until now (FREERUN end)
        break;
      }
    case DFQ_TASK_BARRIER :
      // count pending work
      sched_dev->DFQ(countdown) = 0;
      list_for_each_entry(stask, &sched_dev->stask_list.entry, entry) {
        unsigned int j = 0;
        if(stask->DFQ(held_back) == 0)
          neon_policy_update(sched_dev, stask);
        for_each_set_bit(j, stask->bmp_issue2comp, nchan) {
          sched_work_t *swork = &sched_dev->swork_array[j];
          if(swork->DFQ(heed) == 0)
            continue;
          if(unlikely(swork->DFQ(engage) == 0)) {
            // at this point, being in the barrier, all channels
            // should have been re-engaged, or we did something stupid
            neon_error("DFQ : %s : %s : did %d : pid % d : cid %d : channel "
                       "should have been engaged", __func__,
                       season_name[sched_dev->DFQ(season)],
                       sched_dev->id, stask->pid, j);
            BUG();
          }
          sched_dev->DFQ(countdown++);
        }
      }
      if(sched_dev->DFQ(countdown) > 0) {
        // drain pending work; completion notifications will move
        // us from draining to sampling
        sched_dev->DFQ(season) = DFQ_TASK_DRAINING;
        neon_info("DFQ : %s->%s : did %d : countdown %d - alarm",
                  season_name[last_season],
                  season_name[sched_dev->DFQ(season)],
                  sched_dev->id, sched_dev->DFQ(countdown), ts);
        break;
      }
      // there is no pending work so skip draining phase entirely
      // and go to sampling directly
      neon_info("DFQ : %s : did %d : device totally empty @ %ld - alarm",
                season_name[last_season], sched_dev->id, ts);
    case DFQ_TASK_DRAINING :
      sched_dev->DFQ(season) = DFQ_TASK_SAMPLING;
      last_season = DFQ_TASK_SAMPLING;
      neon_info("DFQ : %s->%s : did %d : countdown %d : "
                "drained @ %ld - alarm", season_name[last_season],
                season_name[sched_dev->DFQ(season)],
                sched_dev->id, sched_dev->DFQ(countdown), ts);
    case DFQ_TASK_SAMPLING :
      // As a sampling period finishes, check whether sampled task has
      // ongoing work (overuse of sampling timeslice)
      last_sampled = sched_dev->DFQ(sampled_task);
      if(last_sampled != NULL &&
         !bitmap_empty(last_sampled->bmp_issue2comp, nchan)) {
        unsigned int false_alarm = 0;
        unsigned int j           = 0;
        // We wait for the completion of ongoing work at the end of a sampling
        // period only if there exist more tasks waiting to be sampled
        for_each_set_bit(j, last_sampled->bmp_issue2comp, nchan) {
          sched_work_t *swork = &sched_dev->swork_array[j];
          neon_report("DFQ : did %d : cid %d : pid %d : %s "
                      "at sampling end @ %ld - alarm",
                      sched_dev->id, j, last_sampled->pid,
                      swork->DFQ(heed) == 0 ? "ignore" : "manage", ts);
          if(swork->DFQ(heed) == 0) {
            // fake-issued requests need to be ignored for
            // completion notification if they are coming from
            // an unmanaged channel --- reset the issued bit
            clear_bit(swork->id, last_sampled->bmp_issue2comp);
            false_alarm = 1;
          }
        }
        if(false_alarm == 1)
          break;
        sched_dev->DFQ(update_ts) = ts;
        neon_report("DFQ : did %d : last %d : busy on sampling end "
                    "@ %ld - alarm", sched_dev->id, last_sampled->pid, ts);
        break;
      } else {
        // if there is no ongoing work, then update the sampled task
        // and, if this is the end of a samplign season, enter freerun
        interval = update_now(sched_dev);
        neon_report("DFQ : %s -> %s : did %d : pid  %d->%d : "
                    "%s @ %ld - alarm (next_in %ld)",
                    season_name[last_season],
                    season_name[sched_dev->DFQ(season)], sched_dev->id,
                    last_sampled == NULL ? 0 : last_sampled->pid,
                    sched_dev->DFQ(sampled_task) == NULL ? 0 :
                    sched_dev->DFQ(sampled_task)->pid,
                    interval.tv64 == sampling_interval.tv64 ? "sample" : \
                    "circled-all", ts, interval.tv64/1000);
      }
      break;
    default :
      neon_error("Unknown season");
    }

    if(interval.tv64 != 0) {
      if(hrtimer_try_to_cancel(&sched_dev->DFQ(season_timer)) != -1) {
        ktime_t next_in = { 0  };
        hrtimer_start(&sched_dev->DFQ(season_timer), interval,
                      HRTIMER_MODE_REL);
        next_in = hrtimer_expires_remaining(&sched_dev->DFQ(season_timer));
        neon_report("%s : canceled timer, restart, next expires in %ld",
                    __func__, next_in.tv64/1000);
      } else
        neon_error("%s : could not cancel sampling timer", __func__);
    }

    write_unlock(&sched_dev->lock);
  }

  return;
}

/**************************************************************************/
// reengage_map_sampling
/**************************************************************************/
// let sampling policy control disegnaging after faults
static int
reengage_map_sampling(const neon_map_t * const neon_map)
{
  sched_dev_t  *sched_dev  = NULL;
  sched_work_t *sched_work = NULL;
  unsigned int  did        = 0;
  unsigned int  cid        = 0;
  int           isreg      = 0;
  int           reengage   = 0;

  isreg = neon_hash_map_offset(neon_map->offset, &did, &cid);
  if(isreg != 0) {
    neon_error("%s : map 0x%x : dis-engage unnecessary, not index reg",
               __func__, neon_map->key);
    return 1;
  }

  sched_dev = &sched_dev_array[did];
  read_lock(&sched_dev->lock);
  sched_work = &sched_dev->swork_array[cid];
  reengage = (sched_work->DFQ(heed) != 0) && (sched_work->DFQ(engage) != 0);
  read_unlock(&sched_dev->lock);

  if(sched_work->DFQ(heed) != 0)
    neon_debug("did %d : cid %d : %s-engaged", did, cid,
               reengage == 0 ? "dis" : "___");

  return reengage;
}
