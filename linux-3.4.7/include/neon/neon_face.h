/***************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "Neon" module for black-box GPU kernel-level management
*/
/***************************************************************************/

#ifndef __NEON_FACE_H__
#define __NEON_FACE_H__

struct task_struct;       // linux/sched.h
struct vm_area_struct;    // linux/mm_types.h
struct __wait_queue_head; // linux/wait.h
struct pt_regs;           // asm/ptrace.h

typedef enum {
  NEON_REGISTER,
  NEON_FRAMEBUFFER,
  NEON_SYSTEM,
  NEON_UNKNOWN
} neon_map_area_t;
                     
typedef struct {
  // Interface: Neon/Driver
  /*************************************************************************/
  // pass ioctl information to state machine
  int (*ioctl) (int   cmd_nr,
                void *pre_cmd_val,
                void *post_cmd_val);
  // pass mmap requests to state machine
  int (*map_pages)(struct vm_area_struct *vma,
                   unsigned long addr,
                   unsigned long offset,
                   unsigned long size,
                   neon_map_area_t area);
  // pass lock_user_pages requests to state machine
  int (*pin_pages)(void *user_address,
                   struct page **pinned_pages,
                   unsigned long long *pte_array,
                   unsigned long long nr_pages);
  // pass unlock_user_pages requests to state machine
  int (*unpin_pages)(struct page **pinned_pages,
                     unsigned long long *pte_array,
                     unsigned long long nr_pages);
  // Interface: Neon/Linux
  /*************************************************************************/
  // properly clean up as related vma gets unmapped
  void (*unmap_vma)(struct vm_area_struct *vma);
  // handle fault at guarded (channel-register-mapped) page
  int (*fault_handler)(struct pt_regs *regs,
                       unsigned long addr);
  // "copy" neon-related task-struct during new thread creation (do_fork)
  int (*copy_task)(unsigned long clone_flags,
                   struct task_struct *tsk);
  // clean-up function for task holding a context
  void (*exit_task)(struct task_struct *tsk);
  // Interface: Neon/Linux --- extras
  /*************************************************************************/
  // tracing helper: associate a tracepoint to str
  void (*tweet)(const char *str);
} neon_face_t;

extern neon_face_t *neon_face;
int neon_face_register(neon_face_t *face);

#endif  // __NEON_FACE_H__
