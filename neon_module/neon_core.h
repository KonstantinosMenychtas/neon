/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON black-box GPU channel management"
*/
/**************************************************************************/

#ifndef __NEON_CORE_H__
#define __NEON_CORE_H__

#include <linux/list.h>      // lists
#include <linux/spinlock.h>  // spin and rwlocks
#include <linux/kthread.h>   // kthread

/****************************************************************************/
// early declarations
struct _neon_dev_t_;    // forward

/**************************************************************************/
// neon channel abstraction
typedef struct {
  // index of this channel
  unsigned int id;
  // occupying process id
  unsigned int pid;
  // manually-constructed kernel-map of index register
  void *ir_kvaddr;
  // assigned reference counter address (kernel virtual)
  void *refc_kvaddr;
  // assigned reference counter target value
  unsigned long refc_target;
  // tics this channel has been occupied processing
  unsigned long pdt;
  // protect this struct
  spinlock_t lock;
} neon_chan_t;

/**************************************************************************/
// neon device abstraction
typedef struct _neon_dev_t_ {
  // index of associated device
  unsigned int id;
  // base address of range in which to expect index register mappings
  unsigned long reg_base;
  // offset at which to find registers in area starting at reg_base
  unsigned long reg_ofs;
  // device-specific reference-target address cmd offset
  int (*refc_eval)(const unsigned int pid,
                   struct vm_area_struct * vma,
                   const unsigned int workload,
                   const unsigned long * const cmd_tuple,
                   unsigned long * const refc_addr_val);
  // device-specific reference-target value cmd offset
  unsigned int rc_dist_val[2];
  // number of channels this device supports
  unsigned int nchan;
  // channel array 
  neon_chan_t *chan;
  // channel bitmap: [i]=1 to mark currently live channel (rqst-busy)
  long *bmp_sub2comp;
  // protect this struct (essentially "all channels")
  spinlock_t lock;
} neon_dev_t;

/**************************************************************************/
// globals - main point of reference for neon
typedef struct {
  // context id source
  atomic_t ctx_ever;
  // number of live contexts
  atomic_t ctx_live;
  // number of devices in the system
  unsigned int ndev;
  // device arary
  neon_dev_t *dev;
} neon_global_t;
extern neon_global_t neon_global;

/****************************************************************************/
// struct management interface calls
void         neon_chan_print(const neon_chan_t * const chan);
void         neon_dev_print(const neon_dev_t * const dev);

int          neon_global_init(void);
int          neon_global_fini(void);
void         neon_global_print(void);

#endif  // __NEON_CORE_H__
