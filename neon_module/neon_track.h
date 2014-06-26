/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/**************************************************************************/

#ifndef __NEON_TRACK_H__
#define __NEON_TRACK_H__

#include <linux/semaphore.h> // sempahore for multi-fault control

/**************************************************************************/
// external declarations
struct _neon_map_t_; // control.h
extern struct notifier_block nb_die; // track.c

/**************************************************************************/
// page tracking information
typedef struct _neon_page_t_ {
  // protected page table entry
  pte_t *pte;
  // associated vaddr
  unsigned long addr;
  // per-cpu saved presence manipulation
  pteval_t saved_ptev;
  // boolean status (re-entrant)
  unsigned int armed;
  // boolean status (re-entrant)
} neon_page_t;

/**************************************************************************/
// page fault handling information
typedef struct _neon_fault_t_ {
  // instruction mnemonic
  char op;
  // faulting instruction pointer
  unsigned long ip;
  // fault address
  unsigned long addr;
  // R/W value of faulting operation
  unsigned long val;
  // saved flags to restore at mapping
  unsigned long flags;
  // back-pointer to associated page
  unsigned long page_num;
  // 2-fault at page-boundary : rearm after handling
  unsigned long siamese;
  // back-pointer to associated map
  struct _neon_map_t_ *map;
  // entry in ctx's list of faulting maps
  struct list_head entry;
} neon_fault_t;

/**************************************************************************/
// page-access tracking and management calls

int  neon_track_init(struct _neon_map_t_ * const map);
int  neon_track_start(struct _neon_map_t_ * const map);
int  neon_track_stop(struct _neon_map_t_ * const map);
void neon_track_restart(unsigned int arm, struct _neon_map_t_ *map);
void neon_track_fini(struct _neon_map_t_ * const map);

void neon_page_arming(unsigned int arm, neon_page_t *page);
void neon_page_print(const neon_page_t * const page);

void neon_fault_save_decode(struct pt_regs * regs,
                            unsigned long addr,
                            struct _neon_map_t_ * const map,
                            unsigned long page_num,
                            neon_fault_t * const fault);
void neon_fault_print(const neon_fault_t * const fault);

#endif // __NEON_TRACK_H__
