/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief   Scheduling-policy encompassing abstractions
*/
/**************************************************************************/

#ifndef __NEON_POLICY_H__
#define __NEON_POLICY_H__

#include "neon_sched.h"
#include "neon_control.h"
#include "neon_help.h"     // NAME_LEN

/**************************************************************************/
// APPEND MORE POLICIES HERE

#include "neon_fcfs.h"
#include "neon_timeslice.h"
#include "neon_sampling.h"

// policy-specific work abstraction
typedef union {
  fcfs_work_t fcfs;
  timeslice_work_t tslc;
  sampling_work_t smpl;
} policy_work_t;

// policy-specific task abstraction
typedef union {
  fcfs_task_t fcfs;
  timeslice_task_t tslc;
  sampling_task_t smpl;
} policy_task_t;

// policy-specific device abstraction
typedef union {
  fcfs_dev_t fcfs;
  timeslice_dev_t tslc;
  sampling_dev_t smpl;
} policy_dev_t;

// scheduling policy ids
typedef enum {
  NEON_POLICY_FCFS,       // basic, set to DEFAULT
  NEON_POLICY_TIMESLICE,  // token-based timeslice
  NEON_POLICY_SAMPLING,   // sampling-based FQ
  NEON_POLICIES           // # of supported policies
} neon_policy_id_t;

// Set default GPU (for debugging purposes)
#define NEON_MAIN_GPU_DID   0

// Set default policy
#define NEON_DEFAULT_POLICY NEON_POLICY_FCFS

#ifdef NEON_USE_TIMESLICE
#undef NEON_DEFAULT_POLICY
#define NEON_DEFAULT_POLICY NEON_POLICY_TIMESLICE
#endif // NEON_USE_TIMESLICE

#ifdef NEON_USE_SAMPLING
#undef NEON_DEFAULT_POLICY
#define NEON_DEFAULT_POLICY NEON_POLICY_SAMPLING
#endif // NEON_USE_SAMPLING

/**************************************************************************/
/**************************************************************************/

extern char _policy_name_[NAME_LEN];

// sysctl/proc managed options
#define NEON_POLICY_KNOB  {                     \
    .procname = "policy",                       \
      .data = _policy_name_,                    \
      .maxlen = NAME_LEN,                       \
      .mode = 066,                              \
      .proc_handler = &proc_dostring,           \
      }

/**************************************************************************/
// initialized channel abstraction used for scheduling
typedef struct _sched_work_t_ {
  // sched-work id, corresponds to neon-chan id
  unsigned int id;
  // associated process's id
  int pid;
  // clock indication at last submit (per chan)
  struct timespec submit_ts;
  // clock indication at last isuse (per chan)
  struct timespec issue_ts;
  // time spent executing on this channel
  unsigned long exe_dt;
  // time spent waiting on this channel
  unsigned long wait_dt;
  // requests submitted/issued on this channel
  unsigned long nrqst;
  // flag marking request is part of a computational kernel/gfx call (2/3)
  unsigned long part_of_call;
  // work (channel instance) control info
  neon_work_t *neon_work;
  // policy-specific entries
  policy_work_t ps;
} sched_work_t;

// task abstraction used for scheduling
typedef struct _sched_task_t_ {
  // associated process's id
  unsigned int pid;
  // map of channels occupied by this task
  long *bmp_start2stop;
  // channel/work busy (issued but not complete) bitmap
  long *bmp_issue2comp;
  // number of requests issued by this task
  unsigned long nrqst;
  // total time spent executing on GPU
  unsigned long exe_dt;
  // total time spent waiting for GPU
  unsigned long wait_dt;
  // policy-specific entries
  policy_task_t ps;
  // entry in device's list of tasks
  struct list_head entry;
} sched_task_t;

// dev abstraction used for scheduling
typedef struct _sched_dev_t_ {
  // sched-dev id, corresponds to neon-dev id
  unsigned int id;
  // initialized channel (work) runtime info
  sched_work_t *swork_array;
  // list of tasks occupying channels on this device
  sched_task_t stask_list;
  // policy-specific entries
  policy_dev_t ps;
  // protect this struct
  rwlock_t lock;
} sched_dev_t;

// event-based scheduling policy interface shared by all policies
typedef struct _neon_policy_face_t_ {
  int  (*init)(void);
  void (*fini)(void);
  void (*reset)(unsigned int nctx);
  int  (*create)(sched_task_t * const sched_task);
  void (*destroy)(sched_task_t * const sched_task);
  void (*start)(sched_dev_t  * const sched_dev,
                sched_work_t * const sched_work,
                sched_task_t * const sched_task);
  void (*stop)(sched_dev_t  * const sched_dev,
               sched_work_t * const sched_work,
               sched_task_t * const sched_task);
  void (*submit)(sched_dev_t  * const sched_dev,
                 sched_work_t * const sched_work,
                 sched_task_t * const sched_task);
  void (*issue)(sched_dev_t  * const sched_dev,
                sched_work_t * const sched_work,
                sched_task_t * const sched_task,
                unsigned int had_blocked);
  void (*complete)(sched_dev_t  * const sched_dev,
                   sched_work_t * const sched_work,
                   sched_task_t * const sched_task);
  void (*event)(void);
  int  (*reengage_map)(const neon_map_t * const map);
} neon_policy_face_t;

/**************************************************************************/
// list of devices to schedule tasks on;
extern sched_dev_t *sched_dev_array;

// common scheduling-policy interface
// invoked by state-machine scheduling hooks
int neon_policy_init(void);
int neon_policy_fini(void);
void neon_policy_reset(unsigned int nctx);
int neon_policy_start(neon_work_t * const work);
int neon_policy_stop(const neon_work_t * const work);
int neon_policy_submit(const neon_work_t * const work);
void neon_policy_complete(const unsigned int did,
                          const unsigned int cid,
                          const unsigned int pid);
int neon_policy_issue(sched_dev_t  * const sched_dev,
                      sched_work_t * const sched_work,
                      sched_task_t * const sched_task,
                      unsigned int had_blocked);
void neon_policy_event(void);
int neon_policy_reengage_map(const neon_map_t * const map);
void neon_policy_reengage_task(sched_dev_t *sched_dev,
                               sched_task_t *sched_task,
                               unsigned int arm);
void neon_policy_update(const sched_dev_t *const sched_dev,
                        const sched_task_t *const sched_task);

#endif // __NEON_POLICY_H__
