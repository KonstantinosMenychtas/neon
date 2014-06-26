/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/**************************************************************************/

#ifndef __NEON_FCFS_H__
#define __NEON_FCFS_H__

#include <linux/sysctl.h>  // sysctl

/**************************************************************************/
// exported sysctl/proc knob
extern ctl_table neon_knob_fcfs_options [];

#define NEON_POLICY_FCFS_KNOB {                 \
    .procname = "fcfs",                         \
      .mode = 0555,                             \
      .child = neon_knob_fcfs_options           \
      }

/**************************************************************************/
// policy-specific work, task and dev sched strruct entries

typedef struct {
  unsigned long empty_placeholder;
} fcfs_work_t;

typedef struct {
  unsigned long empty_placeholder;
} fcfs_task_t;

typedef struct {
  unsigned long empty_placeholder;
} fcfs_dev_t;

/**************************************************************************/
// FCFS policy's scheduling interface
struct _neon_policy_face_t_; // early declaration; in neon_policy.h
extern struct _neon_policy_face_t_ neon_policy_fcfs;

#endif // __NEON_FCFS_H__
