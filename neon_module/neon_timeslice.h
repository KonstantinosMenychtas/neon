/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief   Token-based timeslice scheduling (exclusive GPU access)
*/
/**************************************************************************/

#ifndef __NEON_TIMESLICE_H__
#define __NEON_TIMESLICE_H__

#include <linux/sysctl.h>  // sysctl

/**************************************************************************/
// sysctl/proc managed options

#define NEON_TIMESLICE_T_MIN         1 //    1 mSec
#define NEON_TIMESLICE_T_MAX      1000 // 1000 mSec
#define NEON_TIMESLICE_T_DEFAULT    30 //   30 mSec
#define NEON_DISENGAGE_DEFAULT       1 //   1=true/0=false

/**************************************************************************/
// exported sysctl/proc knob
extern ctl_table neon_knob_timeslice_options [];

#define NEON_POLICY_TIMESLICE_KNOB {            \
    .procname = "timeslice",                    \
      .mode = 0555,                             \
      .child = neon_knob_timeslice_options      \
      }

// timeslice/token-passing period
extern unsigned int timeslice_T;

/**************************************************************************/
// policy-specific work, task and dev sched strruct entries

typedef struct {
  unsigned long _placeholder_;
} timeslice_work_t;

typedef struct {
  // block at this semaphore
  struct semaphore sem;
  // semaphore state counter
  int sem_count;
  // timeslice over-run 
  long overuse;
} timeslice_task_t;

struct _sched_task_t_; // early declaration; in neon_policy.h
typedef struct {
  // neon-task holding the token (someone in task_list)
  struct _sched_task_t_ *token_holder;
  // timestamp marking block-till-completion-update
  unsigned long update_ts;
  // timeslice event flag
  atomic_t action;
  // timeslice (i.e. token-holder update) high rez timer
  struct hrtimer token_timer;
} timeslice_dev_t;

/**************************************************************************/
// TIMESLICE policy's scheduling interface
struct _neon_policy_face_t_; // early declaration; in neon_policy.h
extern struct _neon_policy_face_t_ neon_policy_timeslice;

#endif // __NEON_TIMESLICE_H__
