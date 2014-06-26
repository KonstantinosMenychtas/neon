/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/**************************************************************************/

#include <linux/spinlock.h>  // required for pte_*#
#include <linux/kdebug.h>    // DIE_NOTIFY
#include <linux/slab.h>      // kfree
#include <asm/atomic.h>      // atomics
#include <asm/pgtable.h>     // pte_* and friends
#include <asm/tlbflush.h>    // __flush_tlb_one
#include <asm/debugreg.h>    // DR_STEP
#include <asm/pf_in.h>       // fault decoder
#include "neon_sched.h"
#include "neon_control.h"
#include "neon_track.h"
#include "neon_help.h"
// #include "neon_sched.h"

/****************************************************************************/
// Early declarations

// fault->step handler
static int neon_trap_handler(unsigned long condition, struct pt_regs *regs);

// stepping entry point
static int neon_die_notifier(struct notifier_block *nb,
                             unsigned long val,
                             void *args);

/**************************************************************************/
// Globals

// standard callback to stepping entry point
struct notifier_block nb_die = {
  .notifier_call = neon_die_notifier
};

/**************************************************************************/
// neon_die_notifier
/**************************************************************************/
// single-stepping entry point
static int
neon_die_notifier(struct notifier_block *nb,
                  unsigned long val,
                  void *args)
{
  struct die_args *arg = args;
  unsigned long* dr6_p = (unsigned long *) ERR_PTR(arg->err);

  if (val == DIE_DEBUG && (*dr6_p & DR_STEP)) {
    if (neon_trap_handler(*dr6_p, arg->regs) == 0) {
      // From kmmio: reset the BS bit in dr6 (pointed by args->err)
      // to denote completion of processing
      *dr6_p &= ~DR_STEP;
      return NOTIFY_STOP;
    }
  }

  return NOTIFY_DONE;
}

/****************************************************************************/
// neon_fault_save_decode
/****************************************************************************/
// decode instruction to operands and data and save to fault
void
neon_fault_save_decode(struct pt_regs * regs,
                       unsigned long addr,
                       neon_map_t * const map,
                       unsigned long page_num,
                       neon_fault_t * const fault)
{
  const unsigned long instptr = instruction_pointer(regs);
  const enum reason_type type = get_ins_type(instptr);

  fault->map = map;
  fault->page_num = page_num;
  fault->flags = regs->flags; // moved outside , at fault handler func
  fault->addr = addr;
  fault->ip = instptr;
  switch (type) {
  case REG_READ:
    fault->op = 'R';
    fault->val = 0; // updated @ trap
    break;
  case REG_WRITE:
    fault->op = 'W';
    fault->val = get_ins_reg_val(instptr, regs);
    break;
  case IMM_WRITE:
    fault->op = 'W';
    fault->val= get_ins_imm_val(instptr);
    break;
  default:
    {
      unsigned char *ip = (unsigned char *) instptr;
      fault->op = 'U';
      fault->val = (*ip) << 16 | *(ip + 1) << 8 | *(ip + 2);
    }
  }

  return;
}

/**************************************************************************/
// neon_page_arming
/**************************************************************************/
// arm page --- manually induced fault on every access
void
neon_page_arming(unsigned int arm,
                 neon_page_t *page)
{
  pteval_t ptev = 0;

  neon_debug("page 0x%p : %s", page, arm == 0 ? "disarmed" : "---armed");

  if(arm == 1) {
    if(page->armed == 1) {
      neon_warning("page 0x%p : pte 0x%p : saved ptev 0x%x : armed already",
                   page, page->pte, !!page->saved_ptev);
      return;
    }
    ptev = pte_val(*page->pte);
    page->saved_ptev = ptev & _PAGE_PRESENT;
    ptev &= ~_PAGE_PRESENT;
    page->armed = 1;
  } else { // disarm
    if(page->armed == 0) {
      neon_warning("page 0x%p : pte 0x%p : saved ptev 0x%x : disarmed already",
                   page, page->pte, !!page->saved_ptev);
      return;
    }
    ptev = pte_val(*page->pte);
    ptev |= page->saved_ptev;
    page->armed = 0;
  }

  set_pte_atomic(page->pte, __pte(ptev));
  __flush_tlb_one(page->addr);

  return;
}

/**************************************************************************/
// neon_trap_handler
/**************************************************************************/
// handle (and report) a manually induced faul-->trap, reset page arm
static int
neon_trap_handler(unsigned long condition, struct pt_regs *regs)
{
  struct task_struct *cpu_task  = current;
  neon_task_t        *neon_task = NULL;
  neon_ctx_t         *ctx       = NULL;
  neon_fault_t       *fault     = NULL;
  neon_map_t         *trap_map  = NULL;
  neon_ctx_t         *trap_ctx  = NULL;
  neon_page_t        *trap_page = NULL;

  // skip if trap not concerning a neon-task (e.g. debug regular app)
  if(unlikely( (neon_task = cpu_task->neon_task) == NULL))
    return 1;

  neon_debug("TRY new trap : ip 0x%lx", instruction_pointer(regs));

  // The scenario of multiple contexts and multiple maps in the same task
  // suffering a fault at the same time has not been confirmed in practice.
  // Hence we actually pick the first context and first fault available
  // to handle.
  list_for_each_entry(ctx, &neon_task->ctx_list.entry, entry) {
    if(!list_empty(&ctx->fault_list.entry)) {
      unsigned long instructions = 0;
      unsigned long ip_next = instruction_pointer(regs);
      fault = list_first_entry(&ctx->fault_list.entry, neon_fault_t, entry);
      trap_ctx = ctx;
      trap_map  = fault->map;
      trap_page = &trap_map->page[fault->page_num];
      instructions = (ip_next >> 8) - (fault->ip >> 8);
      // yes, >1 instruction differnce from faults has been witnessed; legit
      if(instructions > 1)
        neon_debug("ctx fault \"jump\"");
      break;
    }
  }
  if(fault == NULL) {
    // Trap without a fault --- this can be legit as a debugging effort,
    // or happen if we "manage" to exit between a fault and a trap,
    // such a trap will have to be ignored
    neon_warning("trap @ IP 0x%lx : can't find fault in list : ... ",
                 instruction_pointer(regs));
    list_for_each_entry(ctx, &neon_task->ctx_list.entry, entry) {
      list_for_each_entry(fault, &ctx->fault_list.entry, entry) 
        neon_fault_print(fault);
    }
    regs->flags &= ~X86_EFLAGS_TF;
    neon_error("spurious trap : ignoring ... (PS> don't dbg with NEON!)");
    return 0;
  }

#ifdef NEON_TRACE_REPORT 
  if(fault->op == 'R' || fault->op == 'W') {
    unsigned long rw_offset = 0;
    unsigned long value     = 0;
    rw_offset = (fault->addr - trap_map->vma->vm_start) % PAGE_SIZE;
    if(fault->op == 'R')
      fault->val = get_ins_reg_val(fault->ip, regs);
    value = fault->val;
    // full trace report is limited to meaningful r/w values
    // since 0s usually signify some buffer reset operation
    // (including, as witnessed, read-scanning), we lower the
    // trace length requirements by not really printing 0-val r/ws
    // this can be disabled, but be warned that Xorg server traces
    // are otherwise in the order of GB
    if(value != 0) {
      unsigned int did = 0;
      unsigned int cid = 0;
      int isreg = -1;
      if(fault->op == 'W') 
        isreg = neon_hash_map_offset(trap_map->offset, &did, &cid);
      if(isreg == 0) 
        neon_info("ctx 0x%x : dev 0x%x : map 0x%x : addr 0x%lx : page %d : "
                  "offs 0x%lx : op %c : val 0x%x : trap : d %d : c %d : WREG",
                  trap_map->ctx_key, trap_map->dev_key, trap_map->key,
                  fault->addr, fault->page_num, 
                  rw_offset, fault->op, value, did, cid);
      else
        neon_info("ctx 0x%x : dev 0x%x : map 0x%x : addr 0x%lx : page %d : "
                  "offs 0x%lx : op %c : val 0x%x : trap",
                  trap_map->ctx_key, trap_map->dev_key, trap_map->key,
                  fault->addr, fault->page_num, 
                  rw_offset, fault->op, value);
    }
  }
#endif // NEON_TRACE_REPORT 

  // re-arm to expect new fault, following scheduler's suggestion
  // as to whether this is necessary
  if(neon_sched_reengage(trap_map) != 0) { 
    neon_page_arming(1, trap_page);
    if(fault->siamese != 0) {
      neon_warning("rearming siamese pages 0x%lx, 0x%lx",
                   fault->page_num, fault->siamese);
      neon_page_arming(1, &trap_map->page[fault->siamese]);
      fault->siamese = 0;
    }
  }
  
  regs->flags &= ~X86_EFLAGS_TF;
  regs->flags |= (fault->flags & (X86_EFLAGS_TF | X86_EFLAGS_IF));

  // mark fault info as handled and remove it from pending list
  fault->addr = 0;
  list_del_init(&fault->entry);

  neon_debug("pid %d : ctx 0x%x : dev 0x%x : map 0x%x : "
             "addr 0x%lx : page %d : val 0x%x :  trap",
             cpu_task->pid, trap_map->ctx_key, trap_map->dev_key, 
             trap_map->key, fault->addr, fault->page_num, fault->val);
            
  return 0;
}

/**************************************************************************/
// neon_track_init
/**************************************************************************/
// follow a vma and and report accesses (R/W) to its pages
int
neon_track_init(neon_map_t * const map)
{
  unsigned int np = 0;

  np = ROUND_DIV(map->size, PAGE_SIZE);

  // init fault entry
  map->fault = (struct _neon_fault_t_ *)                \
    kzalloc(sizeof(struct _neon_fault_t_), GFP_KERNEL);
  if(map->fault == NULL) {
    neon_error("%s: alloc map fault failed \n", __func__);
    return -1;
  }
  INIT_LIST_HEAD(&map->fault->entry);  
  
  // init page structs to follow tracking
  map->page = (struct _neon_page_t_ *)                          \
    kzalloc(np * sizeof(struct _neon_page_t_), GFP_KERNEL);
  if(map->page == NULL) {
    kfree(map->fault);
    neon_error("%s: alloc map->page failed \n", __func__);
    return -1;
  }

  neon_info("ctx 0x%x : dev 0x%x : map 0x%x : size 0x%lx : ofs 0x%lx : "
            "vma->start 0x%lx : track init",
            map->ctx_key, map->dev_key, map->key, map->size,
            map->offset, map->vma->vm_start);

  return 0;
}

/**************************************************************************/
// neon_track_start
/**************************************************************************/
// follow a vma and and report accesses (R/W) to its pages
int
neon_track_start(neon_map_t *const map)
{
  unsigned int i = 0;
  unsigned int np = 0;

  if(map->vma == NULL || map->page == NULL || map->fault == NULL) {
    neon_error("%s : map 0x%lx : not fully initialized at track start",
               __func__, map->key);
    return -1;
  }

  np = ROUND_DIV(map->size, PAGE_SIZE);
  
  for(i = 0; i < np; i++) {
    map->page[i].addr = map->vma->vm_start + i * PAGE_SIZE;
    if(neon_follow_pte(map->vma,
                       map->page[i].addr,
                       &map->page[i].pte) != 0) {
      neon_warning("map key 0x%lx : page %d table entry not found",
                   map->key, i);
      return -1;
    } else {
      neon_page_arming(1, &(map->page[i]));
    }
  }
  
  neon_info("map key 0x%lx : size 0x%lx : ofs 0x%lx : "
            " vma->start 0x%lx : track start",
            map->key, map->size, map->offset, map->vma->vm_start);
  
  return 0;
}

/**************************************************************************/
// neon_track_stop
/**************************************************************************/
// stop tracking accesses to a vma
int
neon_track_stop(neon_map_t * const map)
{
  unsigned int np  = 0;
  unsigned int i   = 0;
  int          ret = 0;

  // disarm map's armed pages
  np = ROUND_DIV(map->size, PAGE_SIZE);
  for(i = 0; i < np; i++) 
    neon_page_arming(0, &(map->page[i]));

  // if ctx has pending fault at fini, trap handler might complain
  // if it gets to finish next instruction (shouldn't)
  if(map->fault->addr != 0) {
    neon_warning("ctx 0x%x : dev 0x%lx : map 0x%x : "
                 "stopping tracking with pending fault ...",
                 map->ctx_key, map->dev_key, map->key);
    neon_fault_print(map->fault);
    map->fault->addr = 0;
    ret = -1;
  }
  
  neon_info("ctx 0x%x : dev 0x%x : map 0x%x : track stop",
            map->ctx_key, map->dev_key, map->key);

  return ret;
}

/**************************************************************************/
// neon_track_fini
/**************************************************************************/
// destroy memory related to access tracking for a map
void
neon_track_fini(neon_map_t * const map)
{
  if(map->fault != NULL)
    kfree(map->fault);
  if(map->page != NULL)
    kfree(map->page);
  map->fault = NULL;
  map->page = NULL;

  neon_info("ctx 0x%x : dev 0x%x : map 0x%x : track fini",
            map->ctx_key, map->dev_key, map->key);

  return;
}

/**************************************************************************/
// neon_track_restart
/**************************************************************************/
// enable/disable tracking on all pages of the mapping
inline void
neon_track_restart(unsigned int arm,
                   neon_map_t *map)
{
  unsigned int i   = 0;
  unsigned int np  = ROUND_DIV(map->size, PAGE_SIZE);

  // arm/disarm all pages of the map
  for(i = 0; i < np; i++) {
    if (map->page[i].armed != arm)
      neon_page_arming(arm, &(map->page[i]));
  }
  
  return;
}

/**************************************************************************/
// neon_fault_print
/**************************************************************************/
// print a neon track-fault struct
inline void
neon_fault_print(const neon_fault_t * const fault)
{
  neon_info("fault : op %c : ip 0x%lx : addr 0x%lx : "
            "val 0x%lx : flags 0x%lx",
            fault->op, fault->ip, fault->addr, fault->val, fault->flags);

  return;
}

/**************************************************************************/
// neon_page_print
/**************************************************************************/
// print a neon track-page struct
inline void
neon_page_print(const neon_page_t * const page)
{
  neon_info("page : pte 0x%p : addr 0x%lx : saved ptv 0x%lx : armed %s",
            page->pte, page->addr, (unsigned long) page->saved_ptev,
            page->armed > 0 ? "YES" : "NO");

  return;
}
