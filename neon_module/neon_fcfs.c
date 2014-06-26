/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/**************************************************************************/

#include <linux/sysctl.h>  // sysctl
#include "neon_policy.h"   // policy interface
#include "neon_help.h"     // print wrappers

/***************************************************************************/
// sysctl/proc options
ctl_table neon_knob_fcfs_options [] = {
  { 0 }
};

/**************************************************************************/
// no-interference (fcfs) policy interface
static int  init_fcfs(void);
static void fini_fcfs(void);
static void reset_fcfs(unsigned int onoff);
static int  create_fcfs(sched_task_t *sched_task);
static void destroy_fcfs(sched_task_t *sched_task);
static void start_fcfs(sched_dev_t  * const sched_dev,
                       sched_work_t * const sched_work,
                       sched_task_t * const sched_task);
static void stop_fcfs(sched_dev_t  * const sched_dev,
                      sched_work_t * const sched_work,
                      sched_task_t * const sched_task);
static void submit_fcfs(sched_dev_t  * const sched_dev,
                        sched_work_t * const sched_work,
                        sched_task_t * const sched_task);
static void issue_fcfs(sched_dev_t  * const sched_dev,
                       sched_work_t * const sched_work,
                       sched_task_t * const sched_task,
                       unsigned int had_blocked);
static void complete_fcfs(sched_dev_t  * const sched_dev,
                          sched_work_t * const sched_work,
                          sched_task_t * const sched_task);
static void event_fcfs(void);
static int  reengage_map_fcfs(const neon_map_t * const neon_map);

neon_policy_face_t neon_policy_fcfs = {
  .init = init_fcfs,
  .fini = fini_fcfs,
  .reset = reset_fcfs,
  .create = create_fcfs,
  .destroy = destroy_fcfs,
  .start = start_fcfs,
  .stop = stop_fcfs,
  .submit = submit_fcfs,
  .issue = issue_fcfs,
  .complete = complete_fcfs,
  .event = event_fcfs,
  .reengage_map = reengage_map_fcfs
};

/**************************************************************************/
// init_fcfs
/**************************************************************************/
// initialize FCFS scheduling structs
static int
init_fcfs(void)
{
  neon_info("init FCFS");

  return 0;
}

/**************************************************************************/
// fini_fcfs
/**************************************************************************/
// finalize and destroy FCFS scheduling structs
static void
fini_fcfs(void)
{
  neon_info("fini FCFS");

  return;
}

/**************************************************************************/
// reset_fcfs
/**************************************************************************/
// Reset FCFS scheduling structs (checkpoint)
static void
reset_fcfs(unsigned int onoff)
{
  neon_debug("FCFS - (re)set");

  return;
}

/**************************************************************************/
// create_fcfs
/**************************************************************************/
// new GPU sched-task
static int
create_fcfs(sched_task_t *sched_task)
{
  neon_debug("FCFS - create sched-task");
  
  return 0;
}

/**************************************************************************/
// destroy_fcfs
/**************************************************************************/
// GPU sched-task exiting
static void
destroy_fcfs(sched_task_t *sched_task)
{
  neon_debug("FCFS - destroy sched-task");
    
  return;
}

/**************************************************************************/
// start_fcfs
/**************************************************************************/
// mark a channel as occupied for a new work
static void
start_fcfs(sched_dev_t  * const sched_dev,
           sched_work_t * const sched_work,
           sched_task_t * const sched_task)
{
  neon_info("did %d : cid %d : pid %d : refc_target 0x%lx : start FCFS",
            sched_dev->id, sched_work->id, sched_task->pid,
            sched_work->neon_work->refc_target);
  
  return ;
}

/**************************************************************************/
// stop_fcfs
/**************************************************************************/
// remove work from channel
static void
stop_fcfs(sched_dev_t  * const sched_dev,
          sched_work_t * const sched_work,
          sched_task_t * const sched_task)
{
  // remove work from channel
  neon_info("did %d : cid %d : pid %d : refc_target 0x%lx : stop FCFS",
            sched_dev->id, sched_work->id, sched_task->pid,
            sched_work->neon_work->refc_target);
  
  return;
}

/**************************************************************************/
// submit_fcfs
/**************************************************************************/
// submit a request for scheduling consideration under FCFS policy
static void
submit_fcfs(sched_dev_t  * const sched_dev,
            sched_work_t * const sched_work,
            sched_task_t * const sched_task)
{
  neon_info("did %d : cid %d : pid %d : refc_target 0x%lx : submit FCFS",
            sched_dev->id, sched_work->id, sched_task->pid,
            sched_work->neon_work->refc_target);

  // issues immediatelly
  neon_policy_issue(sched_dev, sched_work, sched_task, 0);
  
  return;
}

/**************************************************************************/
// issue_fcfs
/**************************************************************************/
// issue request to GPU for processing, scheduled FCFS
static void
issue_fcfs(sched_dev_t  * const sched_dev,
           sched_work_t * const sched_work,
           sched_task_t * const sched_task,
           unsigned int had_blocked)
{
  neon_info("did %d : cid %d : pid %d : refc_target 0x%lx : issue FCFS",
            sched_dev->id, sched_work->id, sched_task->pid,
            sched_work->neon_work->refc_target);

  // CHANNEL COUNT TEST

  /* neon_dev_t  *dev  = NULL; */
  /* list_for_each_entry(dev, &neon_global.dev_list.entry, entry) { */
  /*   if(!__bitmap_empty(dev->bmp_sub2cmp, NEON_MAX_CHANNELS)) { */
  /*     unsigned int idx = 0; */
  /*     unsigned int count = 0; */
  /*     for_each_set_bit(idx, dev->bmp_sub2cmp, NEON_MAX_CHANNELS) { */
  /*       count++; */
  /*     } */
  /*     neon_error("Dev 0x%x channels in use %d", */
  /*                dev->addr, count); */
  /*   } */
  /* } */

  return;
}

/**************************************************************************/
// complete_fcfs
/**************************************************************************/
// mark completion of GPU request
static void
complete_fcfs(sched_dev_t  * const sched_dev,
              sched_work_t * const sched_work,
              sched_task_t * const sched_task)
{
  neon_info("did %d : cid %d : pid %d : refc_target 0x%lx : complete FCFS",
            sched_dev->id, sched_work->id, sched_task->pid,
            sched_work->neon_work->refc_target);
  
  return;
}

/**************************************************************************/
// event_fcfs
/**************************************************************************/
// asynchronous event handler
static void
event_fcfs(void)
{
  // fcfs never creates asynchronous events
  return;
}

/**************************************************************************/
// reengage_map_fcfs
/**************************************************************************/
// control disegnaging after faults
static int
reengage_map_fcfs(const neon_map_t * const neon_map)
{
  // fcfs never disengages, this code is a stub
  return 1;
}
