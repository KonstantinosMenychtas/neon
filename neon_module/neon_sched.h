/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/**************************************************************************/

#ifndef __NEON_SCHED_H__
#define __NEON_SCHED_H__

#include <linux/spinlock.h> // task lock
#include <neon/neon_face.h> // neon interface
#include "neon_core.h"      // neon_chan

/***************************************************************************/
// Enable potentiall malicious task killing
// #define NEON_MALICIOUS_TERMINATOR

/***************************************************************************/
// sys/proc managed options

// polling thresholds
#define NEON_POLLING_T_MIN              1 //    1 mSec
#define NEON_POLLING_T_MAX           1000 //    1  Sec
#define NEON_POLLING_T_DEFAULT          1 //    1 mSec
#define NEON_MALICIOUS_T_DEFAULT    60000 //   60  Sec

extern wait_queue_head_t neon_kthread_event_wait_queue;

extern unsigned int _polling_T_;
extern unsigned int polling_T;

extern unsigned int _malicious_T_;
extern unsigned int malicious_T;

#define NEON_MALICIOUS_KNOB  {                  \
    .procname = "malicious_T",                  \
      .data = &_malicious_T_,                   \
      .maxlen = sizeof(int),                    \
      .mode = 0666,                             \
      .proc_handler = &proc_dointvec,           \
      }
#define NEON_POLLING_KNOB  {                    \
    .procname = "polling_T",                    \
      .data = &_polling_T_,                     \
      .maxlen = sizeof(int),                    \
      .mode = 0666,                             \
      .proc_handler = &proc_dointvec,           \
      }

/**************************************************************************/
// gpu workload type
typedef enum {
  NEON_WORKLOAD_UNDEFINED,
  NEON_WORKLOAD_COMPUTE,
  NEON_WORKLOAD_GRAPHICS
} neon_workload_t;

/**************************************************************************/
// external declarations
struct _neon_task_t_; // control.h
struct _neon_ctx_t_; // control.h
struct _neon_map_t_; // control.h

/**************************************************************************/
// work (channel instance) control struct
typedef struct _neon_work_t_ {
  // associated device id [0, neon_global.ndev)
  unsigned int did;
  // associated channel id [0, dev->nchan)
  unsigned int cid;
  // associated index register map
  struct _neon_map_t_ *ir;
  // associated ring buffer (commands as [gpu_addr, len] tuples) map
  struct _neon_map_t_ *rb;
  // associated command buffer
  struct _neon_map_t_ *cb;
  // associated reference-counter buffer
  struct _neon_map_t_ *rc;
  // back-pointer to containing context
  struct _neon_ctx_t_ *ctx;
  // back-pointer to containing task
  struct _neon_task_t_ *neon_task;
  // saved refc vaddr (user virtual)
  unsigned long refc_vaddr;
  // saved refc vaddr (kernel virtual)
  unsigned long refc_kvaddr;
  // target refc value
  unsigned long refc_target;
  // flag marking request is part of a computational kernel/gfx call (2/3)
  unsigned long part_of_call;
  // workload type
  neon_workload_t workload;
  // entry in ctx's work-list
  struct list_head entry;
} neon_work_t;

/**************************************************************************/
// scheduling events and polling kernel thread interface
int neon_hash_map_offset(unsigned long address,
                         unsigned int *did,
                         unsigned int *cid);
neon_work_t * neon_work_init(struct _neon_task_t_ * const neon_task,
                             struct _neon_ctx_t_ * const ctx,
                             struct _neon_map_t_ * const ir);
int  neon_work_fini(neon_work_t * const work);
int  neon_work_update(struct _neon_ctx_t_ * const ctx,
                      neon_work_t * const work,
                      unsigned long reg_idx);
void neon_work_print(const neon_work_t * const work);

int  neon_work_start(neon_work_t * const work);
int  neon_work_stop(const neon_work_t * const work);
int  neon_work_submit(neon_work_t * const work,
                      unsigned int really);
void neon_work_complete(unsigned int did,
                        unsigned int cid,
                        unsigned int pid);

int  neon_sched_init(void);
int  neon_sched_fini(void);
void neon_sched_reset(unsigned int nctx);
int  neon_sched_reengage(const struct _neon_map_t_ * const map);

#endif // __NEON_SCHED_H__
