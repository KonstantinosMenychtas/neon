/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON black-box GPU channel management"
*/
/**************************************************************************/

#ifndef __NEON_CONTROL_H__
#define __NEON_CONTROL_H__

#include <asm/atomic.h>       // atomics
#include <linux/mm_types.h>   // vm_area_struct
#include <linux/list.h>       // lists
#include <linux/wait.h>       // waitqueue
#include <linux/spinlock.h>   // spin and rwlocks
#include "neon_core.h"        // dev, chan
#include "neon_track.h"       // page_t, fault_t
#include "neon_sched.h"       // work_t

/****************************************************************************/
// forward declarations
struct _neon_ctx_t_;    // forward
struct _neon_task_t_;   // forward

/**************************************************************************/
// identifier struct for mapped areas
typedef struct _neon_map_t_ {
  // mmapped object identifier (ioctl cmd val entry)
  unsigned int key;
  // containing context identifier
  unsigned int ctx_key;
  // associated device identifier
  unsigned int dev_key;
  // mmap size
  unsigned long size;
  // mmap offset --- cpu perspective
  unsigned long offset;
  // mmio address --- gpu perspective
  unsigned long mmio_gpu;
  // associated vma
  struct vm_area_struct *vma;
  // start of locked user pages array (if any)
  struct page **pinned_pages;
  // array of tracked page data
  neon_page_t *page;
  // info for pending fault at page in this map
  neon_fault_t *fault;
  // entry in ctx's list of maps
  struct list_head entry;
} neon_map_t;

/**************************************************************************/
// context control struct
typedef struct _neon_ctx_t_ {
  // context id
  unsigned int id;
  // context key (ioctl cmd val)
  unsigned int key;
  // memory maps in use by this context
  neon_map_t map_list;
  // list of fault->trap transiting mmaps
  neon_fault_t fault_list;
  // channel instances (works) in use by this context
  struct _neon_work_t_ work_list;
  // entry in task-struct context list
  struct list_head entry;
} neon_ctx_t;

/**************************************************************************/
// neon-task
// protected by neon_task_rwlock in struct task
typedef struct _neon_task_t_ {
  // father (primary cpu-task) pid
  int pid;
  // count of processes (task_struct) sharing this neon-struct
  unsigned long sharers;
  //  if characterized malicious
  unsigned int malicious;
  // number of contexts
  unsigned long nctx;
  // list of contexts
  neon_ctx_t ctx_list;
} neon_task_t;

/****************************************************************************/
// to define search approach
typedef enum {
  FOR_KEY,
  FOR_VMA,
  FOR_OFFSET_PRECISE,
  FOR_OFFSET_ALIGNED,
  FOR_PINNED_PAGES,
  UNDEFINED
} neon_map_search_t;

// struct management interface calls
neon_map_t*   neon_map_init(unsigned int ctx_key,
                            unsigned int dev_key,
                            unsigned int map_key);
int           neon_map_fini(neon_ctx_t *ctx,
                            neon_map_t * const map);
void          neon_map_print(const neon_map_t * const map);

neon_ctx_t*   neon_ctx_init(unsigned int id, unsigned int ctx_key);
int           neon_ctx_fini(neon_ctx_t * const ctx);
void          neon_ctx_print(const neon_ctx_t * const ctx);
neon_map_t*   neon_ctx_search_map(neon_ctx_t *ctx,
                                  unsigned long arg,
                                  neon_map_search_t type);

neon_task_t*  neon_task_init(unsigned int pid);
int           neon_task_fini(neon_task_t * const task);
void          neon_task_print(const neon_task_t * const neon_task);
neon_ctx_t*   neon_task_search_ctx(neon_task_t *task,
                                   unsigned int ctx_key);

#endif  // __NEON_CONTROL_H__
