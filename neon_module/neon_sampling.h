/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief   Sampling-based Fair-Queueing scheduling algorithm; interface
*/
/**************************************************************************/

#ifndef __NEON_SAMPLING_H__
#define __NEON_SAMPLING_H__

#include <linux/sysctl.h>  // sysctl

/**************************************************************************/
// sysctl/proc managed options

#define NEON_SAMPLING_COMP0_ONLY          //   account only compute requests on dev0
#define NEON_SAMPLING_CRITICAL_MASS   96  //   can stop sampling after this many
#define NEON_SAMPLING_T_MAX         1000  //   1  Sec
#define NEON_SAMPLING_T_DEFAULT        5  //   5 mSec (per task)
#define NEON_SAMPLING_X_DEFAULT        5  //   5 x sampling season time
// exported sysctl/proc knob
extern ctl_table neon_knob_sampling_options [];

#define NEON_POLICY_SAMPLING_KNOB {             \
    .procname = "sampling",                     \
      .mode = 0555,                             \
      .child = neon_knob_sampling_options       \
      }

/**************************************************************************/
// epoch and seasons
typedef enum {
  DFQ_TASK_BARRIER,
  DFQ_TASK_DRAINING,
  DFQ_TASK_SAMPLING,
  DFQ_TASK_FREERUN,
  DFQ_TASK_NOFSEASONS
} season_t;

// policy-specific work, task and dev sched struct entries
typedef struct {
  // reference counter of last realized work (used for idleness detection)
  unsigned int last_seen;
  // engage-control
  unsigned int engage;
  // flag marking work as ignored for accounting purposes
  unsigned int heed;
} sampling_work_t;

typedef struct {
  // number of channels occupied with works
  unsigned int occ_chans;
  // number of channels with managed works (used when ignoring DMA for sched)
  unsigned int mng_chans;
  // task virtual time
  unsigned long vtime;
  // # of dma/compute requests sampled in the last sample period
  unsigned long nrqst_sampled;
  // # of actuall kernel/gfx calls sampled in last period
  unsigned long ncall_sampled;
  // cumulative runtime of dma/compute rqsts sampled in last period
  unsigned long exe_dt_sampled;
  /* // idleness flag (whether task has submitted at least 1 request) */
  /* unsigned int active; */
  // flag suggesting task did not run in last freerun period
  unsigned int held_back;
  // semaphore state counter
  int sem_count;
  // block at this semaphore
  struct semaphore sem;
} sampling_task_t;

typedef struct {
  // DFQ status
  season_t season;
  // device virtual time
  unsigned long vtime;
  // how long last sampling season lasted
  unsigned long sampling_season_dt;
  // idleness flag (not set if pending requests exist)
  unsigned int active;
  // channel draining countdown
  unsigned int countdown;
  // timestamp marking block-till-completion-update
  unsigned long update_ts;
  // currently sampled task (if any)
  struct _sched_task_t_ *sampled_task;
  // season change event flag (for event handler thread)
  atomic_t action;
  // epoch counter (full cycle)
  struct hrtimer season_timer;
} sampling_dev_t;

/**************************************************************************/
// SAMPLING policy's scheduling interface
struct _neon_policy_face_t_; // early declaration; in neon_policy.h
extern struct _neon_policy_face_t_ neon_policy_sampling;

#endif // __NEON_SAMPLING_H__
