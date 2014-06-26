/***************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/***************************************************************************/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/semaphore.h>
#include <linux/wait.h>
#include <neon/neon_face.h>

/****************************************************************************/
// Neon TracePoint

#define  CREATE_TRACE_POINTS
#include <trace/events/neon.h>
EXPORT_TRACEPOINT_SYMBOL(neon_record);

/****************************************************************************/
// Interface: Neon/Driver

/****************************************************************************/
// neon_ioctl
/****************************************************************************/
int
neon_face_ioctl(int cmd_nr,
                void *pre_cmd_val,
                void *post_cmd_val)
{
  // pass ioctl information to state machine 
  // stub; to be overloaded by driver
  return 0;
}

/****************************************************************************/
// neon_map_pages
/****************************************************************************/
int
neon_face_map_pages(struct vm_area_struct *vma,
                    unsigned long addr,
                    unsigned long offset,
                    unsigned long size,
                    unsigned int area,
                    neon_map_area_t pages)
{
  // pass mmap requests to state machine
  // stub; to be overloaded by driver
  return 0;
}

/****************************************************************************/
// neon_pin_pages
/****************************************************************************/
int
neon_face_pin_pages(void *user_address,
                    struct page **pinned_pages,
                    unsigned long long *pte_array,
                    unsigned long long nr_pages)
{
  // pass lock_user_pages requests to state machine
  // stub; to be overloaded by driver
  return 0;
}

/****************************************************************************/
// neon_unpin_pages
/****************************************************************************/
int
neon_face_unpin_pages(struct page **pinned_pages,
                      unsigned long long *pte_array,
                      unsigned long long nr_pages)
{
  // pass unlock_user_pages requests to state machine
  // stub; to be overloaded by driver
  return 0;
}

/****************************************************************************/
// Interface: Neon/Linux

/****************************************************************************/
// neon_copy_task
/****************************************************************************/
int
neon_face_copy_task(unsigned long clone_flags,
                    struct task_struct *tsk)
{
  // "copy" neon-related task-struct during new thread creation (do_fork)
  // stub; to be overloaded by neon module
  return 0;
}

/****************************************************************************/
// neon_exit_task
/****************************************************************************/
void
neon_face_exit_task(struct task_struct *tsk)
{
  // clean-up function for task holding a context
  // stub; to be overloaded by neon module
  return;
}

/****************************************************************************/
// neon_fault_handler
/****************************************************************************/
int
neon_face_fault_handler(struct pt_regs *regs,
                        unsigned long addr)
{
  // handle fault at guarded (channel-register-mapped) page
  // stub; to be overloaded by neon module

  // default return 1 to signify that fault does not concern a GPU
  // memory area being tracked, so it has to be managed by the
  // normal handler
  return 1;
}

/****************************************************************************/
// neon_unmap_vma
/****************************************************************************/
void
neon_face_unmap_vma(struct vm_area_struct *vma)
{
  // properly clean up as related vma gets unmapped
  // stub; to be overloaded by neon module
  return;
}

/****************************************************************************/
// neon_tweet
/****************************************************************************/
void
neon_tweet(const char *str)
{
  // associate a tracepoint to str, allowing str to appear inline
  // with the rest of neon's trace
  // stub; to be overloaded by neon module
  return;
}

/****************************************************************************/
// Default = none

neon_face_t neon_face_none = {
  .ioctl          = neon_face_ioctl,
  .map_pages      = neon_face_map_pages,
  .pin_pages      = neon_face_pin_pages,
  .unpin_pages    = neon_face_unpin_pages,
  .unmap_vma      = neon_face_unmap_vma,
  .fault_handler  = neon_face_fault_handler,
  .copy_task      = neon_face_copy_task,
  .exit_task      = neon_face_exit_task,
  .tweet          = neon_tweet,
};

neon_face_t *neon_face = &neon_face_none;
EXPORT_SYMBOL(neon_face);

DEFINE_SEMAPHORE(neon_face_sem);

/****************************************************************************/
// neon_register
/****************************************************************************/
int
neon_face_register(neon_face_t *face)
{
  if(down_trylock(&neon_face_sem) == 0) {
    //    pr_info("NEON: register neon face [none 0x%p] 0x%p ---> 0x%p",
    //           &neon_face_none, neon_face, face);
    if(face != NULL)
      neon_face = face;
    else
      neon_face = &neon_face_none;
    up(&neon_face_sem);
    return 0;
  }

  return 1;
}
EXPORT_SYMBOL(neon_face_register);

/****************************************************************************/
// neon_face_init
/****************************************************************************/
static int
__init neon_face_init(void)
{
  if(neon_face_register(&neon_face_none) == 0) {
    pr_info("NEON: interface loaded.\n");
    return 0;
  } else
    pr_info("NEON: interface could not be loaded.\n");

  return 1;
}

/****************************************************************************/
// neon_face_exit
/****************************************************************************/
static void
__exit neon_face_exit(void)
{
  if(neon_face_register(NULL) == 0)
    pr_info("NEON: interface unloaded.\n");
  else
    pr_warning("NEON: interface could not be unloaded.\n");

  return;
}

module_init(neon_face_init);
module_exit(neon_face_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Konstantinos Menychtas");
MODULE_DESCRIPTION("NEON Interface for Black-Box"
                   "GPU kernel-level management");
