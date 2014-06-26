/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief   Token-based timeslice scheduling (exclusive GPU access)
*/
/**************************************************************************/

#include <linux/sysctl.h>  // sysctl
#include <linux/delay.h>   // msleep
#include "neon_policy.h"   // policy interface
#include "neon_help.h"     // print wrappers
#include "neon_sched.h"    // disengage var

/**************************************************************************/
#define TS(x) ps.tslc.x

// hrtimer time descriptor
ktime_t               timeslice_interval;

static unsigned int _timeslice_T_ = NEON_TIMESLICE_T_DEFAULT;
unsigned int timeslice_T          = NEON_TIMESLICE_T_DEFAULT;

// control dis/en-gaging the access-tracking mechanism after a fault
static unsigned int _disengage_ = NEON_DISENGAGE_DEFAULT;
static unsigned int disengage   = NEON_DISENGAGE_DEFAULT;

// sysctl/proc options
ctl_table neon_knob_timeslice_options [] = {
  {
    .procname = "timeslice_T",
    .data = &_timeslice_T_,
    .maxlen = sizeof(int),
    .mode = 0666,
    .proc_handler = &proc_dointvec,
  },
  {
    .procname = "disengage",
    .data = &_disengage_,
    .maxlen = sizeof(int),
    .mode = 0666,
    .proc_handler = &proc_dointvec,
  },
  { 0 }
};

/****************************************************************************/

// no-interference (timeslice) policy interface
static int  init_timeslice(void);
static void fini_timeslice(void);
static void reset_timeslice(unsigned int nctx);
static int  create_timeslice(sched_task_t *sched_task);
static void destroy_timeslice(sched_task_t *sched_task);
static void start_timeslice(sched_dev_t  * const sched_dev,
                            sched_work_t * const sched_work,
                            sched_task_t * const sched_task);
static void stop_timeslice(sched_dev_t  * const sched_dev,
                           sched_work_t * const sched_work,
                           sched_task_t * const sched_task);
static void submit_timeslice(sched_dev_t  * const sched_dev,
                             sched_work_t * const sched_work,
                             sched_task_t * const sched_task);
static void issue_timeslice(sched_dev_t  * const sched_dev,
                            sched_work_t * const sched_work,
                            sched_task_t * const sched_task,
                            unsigned int had_blocked);
static void complete_timeslice(sched_dev_t  * const sched_dev,
                               sched_work_t * const sched_work,
                               sched_task_t * const sched_task);
static void event_timeslice(void);
static int  reengage_map_timeslice(const neon_map_t * const map);

neon_policy_face_t neon_policy_timeslice = {
  .init = init_timeslice,
  .fini = fini_timeslice,
  .reset = reset_timeslice,
  .create = create_timeslice,
  .destroy = destroy_timeslice,
  .start = start_timeslice,
  .stop = stop_timeslice,
  .submit = submit_timeslice,
  .issue = issue_timeslice,
  .complete = complete_timeslice,
  .event = event_timeslice,
  .reengage_map = reengage_map_timeslice
};

/****************************************************************************/
// dev_status_print
/****************************************************************************/
#ifdef NEON_DEBUG_LEVEL_2
void
dev_status_print(sched_dev_t *sched_dev)
{
  sched_task_t *stask    = NULL;

  list_for_each_entry(stask, &sched_dev->stask_list.entry, entry) {
    neon_debug("pid %5d : [ %c -- sem = %d ] : dev %d",
               stask->pid,
               ((sched_dev->TS(token_holder) == stask) ? 'H' : ' '),
               stask->TS(sem_count), sched_dev->id);


  }

  return;
}
#else // ! NEON_DEBUG_LEVEL_2
#define dev_status_print(a) while(0)
#endif // NEON_DEBUG_LEVEL_2

/**************************************************************************/
// update_token_holder
/**************************************************************************/
// safely pass the token to the next requesting task in the queue
// CAREFUL : called with sched-dev write lock held
static unsigned int
update_token_holder(sched_dev_t *sched_dev)
{
  sched_task_t *last_holder = NULL;
  sched_task_t *new_holder  = NULL;
  sched_task_t *sched_task  = NULL;
  unsigned int  count       = 0;
  unsigned int  repeat      = 0;

  if(list_empty(&sched_dev->stask_list.entry))
    return 0;

  do {
    // count efforts to set next token holder
    // save current token holder
    last_holder = sched_dev->TS(token_holder);
    // find next token holder
    if(likely(last_holder != NULL)) {
      // a token holder has been assigned before --- find next in list
      // and pass them the token
      struct list_head *pos = last_holder->entry.next;
      // point to first entry in list (rather than the head!)
      if(pos == &sched_dev->stask_list.entry)
        pos = pos->next;
      new_holder = list_entry(pos, sched_task_t, entry);
    } else {
      // no token_holder has been assigned before, if the
      // task list is not empty, pass token to the first task
      new_holder = list_first_entry(&sched_dev->stask_list.entry,
                                    sched_task_t, entry);
    }    
    sched_dev->TS(token_holder) = new_holder;

    // first check whether any penalty needs to be applied;
    // if new holder has indeed overused the device, notify
    // the alarm to check again (return 0) and subtract
    // T from the would-be-holder's penalty
    if(new_holder->TS(overuse) > (timeslice_T * USEC_PER_MSEC)) {
      if(last_holder == new_holder) {
        // going solo after having been sharing the GPU
        neon_info("did %d : pid %d : overuse %ld usec reset --> going solo",
                  sched_dev->id, new_holder->pid, new_holder->TS(overuse));
        new_holder->TS(overuse) = 0;
      } else {
        // Penalizing means when the overuse exceeds a timeslice,
        // skip turns until overuse < T again
        neon_info("did %d : pid %d : overuse %ld uSec > T %ld uSec --> skip turn",
                  sched_dev->id, new_holder->pid,
                  new_holder->TS(overuse), timeslice_T * USEC_PER_MSEC);
        new_holder->TS(overuse) -= (timeslice_T * USEC_PER_MSEC);
        repeat = 1;
      }
    } else
      repeat = 0;
  } while(repeat == 1);

  // the application will only be allowed to submit if it holds
  // the token; else, it will block itself at its semaphore
  list_for_each_entry(sched_task, &sched_dev->stask_list.entry, entry) {
    if(sched_task == new_holder) {
      if(disengage != 0)
          neon_policy_reengage_task(sched_dev, sched_task, 0);
      if(sched_task->TS(sem_count) < 0) {
        sched_task->TS(sem_count)++;
        up(&sched_task->TS(sem));
      }
    } else
      if(disengage != 0)
        neon_policy_reengage_task(sched_dev, sched_task, 1);
  }

#ifdef NEON_DEBUG_LEVEL_3  
  do{
    struct timespec now_ts = { 0 };
    getnstimeofday(&now_ts);    
    neon_info("did %d : UPDATE_HOLDER %d (overuse %ld)  --> %d (overuse %ld) @ %ld",
              sched_dev->id,
              last_holder == NULL ? 0 : last_holder->pid,
              last_holder == NULL ? 0 : last_holder->TS(overuse),
              new_holder == NULL ? 0 : new_holder->pid,
              new_holder == NULL ? 0 : new_holder->TS(overuse),
              (unsigned long) (timespec_to_ns(&now_ts) / NSEC_PER_USEC));
  } while(0);
#endif // NEON_DEBUG_LEVEL_3
  
  return count;
}

/****************************************************************************/
// timeslice_timer_callback
/****************************************************************************/
// called by the timeslice timer, this alarm will mark the time to
// pass the token to the next requesting thread
static enum hrtimer_restart
timeslice_timer_callback(struct hrtimer *timer)
{
  timeslice_dev_t *timeslice_dev = container_of(timer, timeslice_dev_t,
                                                token_timer);
  policy_dev_t *policy_dev = (policy_dev_t *) timeslice_dev;
  sched_dev_t  *sched_dev = container_of(policy_dev, sched_dev_t, ps);
                                        
  if(atomic_read(&neon_global.ctx_live) > 0) {
    unsigned long update_in_progress = 0;
    read_lock(&sched_dev->lock);      
    update_in_progress = sched_dev->TS(update_ts);
    // TODO : verify this is ok
    if(update_in_progress == 0) {
      struct timespec now_ts = { 0 };
      getnstimeofday(&now_ts);
      if(sched_dev->id == NEON_MAIN_GPU_DID)
        neon_debug("did %d : alarm timer callback @ %ld", sched_dev->id,
                   (unsigned long) (timespec_to_ns(&now_ts) / NSEC_PER_USEC));
      atomic_set(&timeslice_dev->action, 1);
      wake_up_interruptible(&neon_kthread_event_wait_queue);
    }
    read_unlock(&sched_dev->lock);
  }

  return HRTIMER_NORESTART;
}

/**************************************************************************/
// init_timeslice
/**************************************************************************/
// initialize TIMESLICE scheduler structs
static int
init_timeslice(void)
{
  unsigned int i = 0;

  // nothing to alloc
  for(i = 0; i < neon_global.ndev; i++) {
    sched_dev_t *sched_dev = NULL;
    sched_dev = &sched_dev_array[i];
    atomic_set(&sched_dev->TS(action), 0);
    hrtimer_init(&sched_dev->TS(token_timer), CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    sched_dev->TS(token_timer.)function = &timeslice_timer_callback;
  }
  neon_debug("init - TIMESLICE");

  return 0;
}

/**************************************************************************/
// fini_timeslice
/**************************************************************************/
// finalize and destroy TIMESLICE scheduling structs
static void
fini_timeslice(void)
{
  unsigned int i = 0;
  
  // cancel any live token timers; reset(0) must have already stopped them
  for(i = 0; i < neon_global.ndev; i++) {
    sched_dev_t *sched_dev = &sched_dev_array[i];
    atomic_set(&sched_dev->TS(action), 0);
    if(hrtimer_cancel(&sched_dev->TS(token_timer)) != 0)
      neon_error("%s : Sampling timer was busy at fini", __func__);
  }
  
  return;
}

/**************************************************************************/
// reset_timeslice
/**************************************************************************/
// Reset TIMESLICE structs (safe checkpoint)
static void
reset_timeslice(unsigned int nctx)
{
  unsigned int i = 0;

  if(nctx == 1) {
    timeslice_T = _timeslice_T_;
    disengage   = _disengage_;

    neon_info("%s disengage after %d msec",
              disengage == 0 ? "DO NOT" : "DO ---", timeslice_T);

    if(timeslice_T < NEON_TIMESLICE_T_MIN) {
      neon_error("Adjusting token-passing T %u to min %d T",
                 timeslice_T, NEON_TIMESLICE_T_MIN);
      timeslice_T = NEON_TIMESLICE_T_MIN;
    }
    if(timeslice_T > NEON_TIMESLICE_T_MAX) {
      neon_error("Adjusting token-passing T %u to max %d T",
                 timeslice_T, NEON_TIMESLICE_T_MAX);
      timeslice_T = NEON_TIMESLICE_T_MAX;
    }

    timeslice_interval = ktime_set(0, timeslice_T * NSEC_PER_MSEC);
    for(i = 0; i < neon_global.ndev; i++) {
      sched_dev_t *sched_dev = &sched_dev_array[i];
      sched_dev->TS(token_holder) = NULL;
      sched_dev->TS(update_ts) = 0;
      hrtimer_start(&sched_dev->TS(token_timer),
                    timeslice_interval, HRTIMER_MODE_REL);
    }
    
    neon_info("timeslice reset; (re)start with T=%d mSec", timeslice_T);
  }
  if (nctx == 0) {
    for(i = 0; i < neon_global.ndev; i++) {
      sched_dev_t *sched_dev = &sched_dev_array[i];
      atomic_set(&sched_dev->TS(action), 0);
      sched_dev->TS(token_holder) = NULL;
      sched_dev->TS(update_ts) = 0;
      if(hrtimer_cancel(&sched_dev->TS(token_timer)) != 0)
        neon_debug("did %d : Timeslice timer was busy when stopped", i);
    }

    neon_info("timeslice reset; stop");
  }

  return;
}

/**************************************************************************/
// create_timeslice
/**************************************************************************/
// new GPU sched-task
static int
create_timeslice(sched_task_t *sched_task)
{
  // policy-specific struct initializer
  sema_init(&sched_task->TS(sem), 0);
  sched_task->TS(sem_count) = 0;

  neon_debug("TIMESLICE - create sched-task");

  return 0;
}

/**************************************************************************/
// destroy_timeslice
/**************************************************************************/
// GPU sched-task exiting
static void
destroy_timeslice(sched_task_t *sched_task)
{
  if(sched_task->TS(sem_count) != 0)
    neon_error("Exit with sem_count %d != 0", sched_task->TS(sem_count));

  // nothing should happen in the next few lines

  // TODO : not worrying about disengaging
  // possibly still engaged process - should we?
  if(sched_task->TS(sem_count) < 0) {
    sched_task->TS(sem_count)++;
    up(&sched_task->TS(sem));
  }

  neon_debug("TIMESLICE - destroy sched-task");

  return;
}

/**************************************************************************/
// start_timeslice
/**************************************************************************/
// new GPU task sched request: initialize TIMESLICE specific structs and
// update the token holder prioritizing the incoming task
// CAREFUL : sched-dev write lock held
static void
start_timeslice(sched_dev_t  * const sched_dev,
                sched_work_t * const sched_work,
                sched_task_t * const sched_task)
{
  neon_dev_t    *neon_dev    = &neon_global.dev[sched_dev->id];
  unsigned long  nchan       = neon_dev->nchan;
  sched_task_t  *curr_holder = sched_dev->TS(token_holder);

  // works entering the scheduler roundabout force a token-holder update
  // if there is no current token holder
  if(curr_holder == NULL && bitmap_empty(sched_task->bmp_start2stop, nchan)) {
    update_token_holder(sched_dev);
    curr_holder = sched_dev->TS(token_holder);    
  }

  neon_info("did %d : cid %d : pid %d [H=%d] : "
            "rqst %ld : refc_target 0x%lx : START",
            sched_dev->id, sched_work->id, sched_task->pid,
            curr_holder == NULL ? 0 : curr_holder->pid,
            sched_work->nrqst, sched_work->neon_work->refc_target);

  return;
}

/**************************************************************************/
// stop_timeslice
/**************************************************************************/
// cleanly remove work from channel
// CAREFUL : sched-dev write lock held
static void
stop_timeslice(sched_dev_t  * const sched_dev,
               sched_work_t * const sched_work,
               sched_task_t * const sched_task)
{
  neon_dev_t   *neon_dev    = &neon_global.dev[sched_dev->id];
  unsigned int  nchan       = neon_dev->nchan;
  sched_task_t *last_holder = sched_dev->TS(token_holder);

  // unique works exiting the scheduler roundabout force a token-holder
  // update if found to be holding the token
  if(last_holder == sched_task &&
     bitmap_empty(sched_task->bmp_start2stop, nchan) &&
     !list_empty(&sched_dev->stask_list.entry)) {
    sched_task_t *curr_holder = NULL; 
    unsigned int  retries     = 0;

    retries = update_token_holder(sched_dev);
    curr_holder = sched_dev->TS(token_holder); 
    if(curr_holder == sched_task) {
      sched_dev->TS(token_holder) = NULL;
      curr_holder = NULL;
    }

    if(sched_task->TS(sem_count) < 0) {
      sched_task->TS(sem_count)++;
      up(&sched_task->TS(sem));
    }
  }

  if(hrtimer_try_to_cancel(&sched_dev->TS(token_timer)) != -1) {
    if(atomic_read(&neon_global.ctx_live) > 0) {
      atomic_set(&sched_dev->TS(action), 1);
      wake_up_interruptible(&neon_kthread_event_wait_queue);
    }
  }

  neon_info("did %d : cid %d : pid %d [last H=%d] : "
            "rqst %ld : refc_target 0x%lx : STOP",
            sched_dev->id, sched_work->id, sched_task->pid,
            last_holder == NULL ? 0 : last_holder->pid,
            sched_work->nrqst, sched_work->neon_work->refc_target);

  return;
}

/**************************************************************************/
// submit_timeslice
/**************************************************************************/
// submit a request for scheduling consideration under TIMESLICE
// CAREFUL sched-dev write lock held
static void
submit_timeslice(sched_dev_t  * const sched_dev,
                 sched_work_t * const sched_work,
                 sched_task_t * const sched_task)
{
  sched_task_t  *curr_holder = sched_dev->TS(token_holder);
  unsigned int   block       = 0;

  if(sched_task != curr_holder || sched_dev->TS(update_ts) != 0) {
    // block here if this is not the sched holder or
    // if overuse has been detected
    neon_info("did %d : cid %d : pid %d [H=%d] : "
              "rqst %ld : refc_target 0x%lx : overuse %ld : "
              "SUBMIT _____BLOCK",
              sched_dev->id, sched_work->id, sched_task->pid,
              curr_holder == NULL ? 0 : curr_holder->pid,
              sched_work->nrqst, sched_work->neon_work->refc_target,
              sched_task->TS(overuse));
    block = 1;
  } else {
    // if this is the sched holder, or is just the first task to enter
    // the queue (and thus the sched holder), then let it go through
    neon_info("did %d : cid %d : pid %d [H=%d] "
              "rqst %ld : refc_target 0x%lx : overuse %ld : "
              "SUBMIT DONT_BLOCK",
              sched_dev->id, sched_work->id, sched_task->pid,
              curr_holder == NULL ? 0 : curr_holder->pid,
              sched_work->nrqst, sched_work->neon_work->refc_target,
              sched_task->TS(overuse));
    block = 0;
  }

  dev_status_print(sched_dev);
  if(block == 1) {
    clear_bit(sched_work->id, sched_task->bmp_issue2comp);
    sched_task->TS(sem_count)--;
    write_unlock(&sched_dev->lock);

    // wait till given the token
    down_interruptible(&sched_task->TS(sem));

    write_lock(&sched_dev->lock);
    curr_holder = sched_dev->TS(token_holder);
    neon_info("did %d : cid %d : pid %d [H=%d] : "
              "rqst %ld : refc_target 0x%lx : overuse %ld : "
              "SUBMIT UN___BLOCK",
              sched_dev->id, sched_work->id, sched_task->pid,
              curr_holder == NULL ? 0 : curr_holder->pid,
              sched_work->nrqst, sched_work->neon_work->refc_target,
              sched_task->TS(overuse));
  }

  neon_policy_issue(sched_dev, sched_work, sched_task, block);

  return;
}

/**************************************************************************/
// issue_timeslice
/**************************************************************************/
// issue request to GPU for processing, scheduled under TIMESLICE
// CAREFUL : called from submit, sched dev lock held
static void
issue_timeslice(sched_dev_t  * const sched_dev,
                sched_work_t * const sched_work,
                sched_task_t * const sched_task,
                unsigned int had_blocked)
{
  neon_info("did %d : cid %d : pid %d [H=%d] : "
            "rqst %ld : refc_target 0x%lx : overuse %ld : ISSUE",
            sched_dev->id, sched_work->id, sched_task->pid,
            sched_dev->TS(token_holder) == NULL ? 0 : \
            sched_dev->TS(token_holder)->pid,
            sched_work->nrqst, sched_work->neon_work->refc_target,
            sched_task->TS(overuse));

  return;
}

/**************************************************************************/
// complete_timeslice
/**************************************************************************/
// mark appropriate GPU work as complete, schedule by TIMESLICE
// CAREFUL : sched dev write lock held
static void
complete_timeslice(sched_dev_t  * const sched_dev,
                   sched_work_t * const sched_work,
                   sched_task_t * const sched_task)
{
  unsigned int nchan = neon_global.dev[sched_dev->id].nchan;
  sched_task_t *curr_holder = NULL;

  curr_holder = sched_dev->TS(token_holder);
  if(curr_holder != NULL &&
     curr_holder != sched_task) {
    // this is not a BUG(), can happen if a process gets killed
    // a more careful exit mechanism is necessary
    neon_error("Completing task %d [work %d] != curr_holder %d!",
               sched_task->pid, sched_work->id,
               curr_holder->pid);
    return;
  }

  // overrun, delayed scheduler update
  if(sched_dev->TS(update_ts) != 0 &&
     bitmap_empty(curr_holder->bmp_issue2comp, nchan)) {
    unsigned int retries = 0;
    struct timespec now_ts = { 0 };
    unsigned long dt = 0;
    // account overuse
    getnstimeofday(&now_ts);
    dt = ((unsigned long) (timespec_to_ns(&now_ts) / NSEC_PER_USEC)) -  \
      sched_dev->TS(update_ts);
    neon_info("did %d : cid %d : pid %d [H=%d] : "
              "rqst %ld : refc_target 0x%lx : overuse %ld+%ld isCOMPLTE @ %ld",
              sched_dev->id, sched_work->id, sched_task->pid,
              curr_holder == NULL ? 0 : curr_holder->pid,
              sched_work->nrqst, sched_work->neon_work->refc_target,
              sched_task->TS(overuse), dt,
              ((unsigned long) (timespec_to_ns(&now_ts) / NSEC_PER_USEC)));
    sched_task->TS(overuse) += dt;
    retries = update_token_holder(sched_dev);
    curr_holder = sched_dev->TS(token_holder);
    sched_dev->TS(update_ts) = 0;
    // reset timeslice
    if(hrtimer_try_to_cancel(&sched_dev->TS(token_timer)) != -1)
      hrtimer_start(&sched_dev->TS(token_timer), timeslice_interval,
                    HRTIMER_MODE_REL);
    neon_info("did %d : cid %d : pid %d [H=%d] : "
              "rqst %ld : refc_target 0x%lx : overuse %ld : COMPLT->HOLDR_UPDT",
              sched_dev->id, sched_work->id, sched_task->pid,
              curr_holder == NULL ? 0 : curr_holder->pid,
              sched_work->nrqst, sched_work->neon_work->refc_target,
              sched_task->TS(overuse));
  }

  neon_info("did %d : cid %d : pid %d [H=%d] : "
            "rqst %ld : refc_target 0x%lx : overuse %ld : COMPLETE [bmp 0x%lx]",
            sched_dev->id, sched_work->id, sched_task->pid,
            curr_holder == NULL ? 0 : curr_holder->pid,
            sched_work->nrqst, sched_work->neon_work->refc_target,
            sched_task->TS(overuse),
            curr_holder == NULL ? 0 : curr_holder->bmp_issue2comp[0]);

  neon_info("did %d : cid %d : pid %d : "
            "nrqst %ld : exe %ld : wait %ld : work stats",
            sched_dev->id, sched_work->id, sched_task->pid,
            sched_work->nrqst, sched_work->exe_dt, sched_work->wait_dt);

  return;
}

/**************************************************************************/
// event_timeslice
/**************************************************************************/
// asynchronous event handler --- token alarm raised event handling
static void
event_timeslice(void)
{
  unsigned int i = 0;
  struct timespec now_ts  = { 0 };
  sched_dev_t  *sched_dev = NULL;

  getnstimeofday(&now_ts);
  for(i = 0; i < neon_global.ndev; i++ ) {
    neon_dev_t   *neon_dev    = &neon_global.dev[i];
    unsigned int  nchan       = neon_dev->nchan;
    sched_task_t *curr_holder = NULL;
    sched_task_t *last_holder = NULL;
    unsigned int  retries     = 0;

    sched_dev = &sched_dev_array[i];

    if(atomic_cmpxchg(&sched_dev->TS(action), 1, 0) == 0)
      continue;
    
    /* if(!atomic_read(&sched_dev->TS(action))) */
    /*   continue; */
    /* else */
    /*   atomic_set(&sched_dev->TS(action), 0); */

    write_lock(&sched_dev->lock);
    last_holder = sched_dev->TS(token_holder);
    if(last_holder != NULL &&
       !list_is_singular(&sched_dev->stask_list.entry)) {
      if(disengage != 0 &&
         !bitmap_empty(last_holder->bmp_start2stop, nchan))
        neon_policy_update(sched_dev, last_holder);
      if(!bitmap_empty(last_holder->bmp_issue2comp, nchan)) {
        // there exists pending request from last task-holder,
        // update will happen at the end of pending request
        // but block already to make sure we don't have any request leaks
        if(disengage != 0)
          neon_policy_reengage_task(sched_dev, last_holder, 1);
        sched_dev->TS(update_ts) = (unsigned long)      \
          (timespec_to_ns(&now_ts) / NSEC_PER_USEC);        
        neon_info("did %d : holder %d --- still busy @ alarm %ld",
                  sched_dev->id, last_holder->pid,
                  (unsigned long) (timespec_to_ns(&now_ts) / NSEC_PER_USEC));
        write_unlock(&sched_dev->lock);
        continue;
      }
    }
    // get a task with no significant ( > T ) penalty
    retries = update_token_holder(sched_dev);
    curr_holder = sched_dev->TS(token_holder);
    neon_debug("did %d : retries %d : holder %d --> %d : alarm UPDTd",
               i, retries, last_holder == NULL ? 0 : last_holder->pid,
               curr_holder == NULL ? 0 : curr_holder->pid);
    write_unlock(&sched_dev->lock);

    if(hrtimer_try_to_cancel(&sched_dev->TS(token_timer)) != -1) {
      if(sched_dev->id == NEON_MAIN_GPU_DID)
        neon_debug("did %d : alarm cancel @ %ld and restart", 
                    sched_dev->id, (unsigned long)              \
                    (timespec_to_ns(&now_ts) / NSEC_PER_USEC));
      hrtimer_start(&sched_dev->TS(token_timer), timeslice_interval,
                    HRTIMER_MODE_REL);
    } else
      neon_error("%s : could not cancel timeslice timer", __func__);
  }
    
  return;
}

/**************************************************************************/
// reengage_map_timeslice
/**************************************************************************/
// let policy decide whether to reengage after a fault
static int
reengage_map_timeslice(const neon_map_t * const map)
{
  sched_dev_t *sched_dev = NULL;
  sched_task_t *curr_holder = NULL;
  unsigned int did = 0;
  unsigned int cid = 0;
  int isreg = 0;

  isreg = neon_hash_map_offset(map->offset, &did, &cid);
  if(isreg != 0) {
    neon_debug("map 0x%x : dis-engage unnecessary, not index reg",
               map->key);
    return 1;
  }

  sched_dev = &sched_dev_array[did];
  read_lock(&sched_dev->lock);
  curr_holder = sched_dev->TS(token_holder);
  read_unlock(&sched_dev->lock);

  if(disengage != 0 && curr_holder != NULL) {
    // only reengage if current task is not the token holder
    if(curr_holder->pid == ((int) current->pid)) {
      neon_info("did %d : cid %d : task %d : "
                "dis-engaged --- page",
                did, cid, (int) curr_holder->pid);
      return 0;
    } else
      neon_info("did %d : cid %d : task %d : "
                "___-engaged --- page",
                did, cid, (int) curr_holder->pid);
  }

  // always re-engage when disegange option
  // is not set in proc values
  return 1;
}
