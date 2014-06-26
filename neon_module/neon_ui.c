/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/**************************************************************************/
#include <linux/sysctl.h>    // sysctl
#include <linux/fs.h>        // write
#include <asm/uaccess.h>     // user copy
#include <neon/neon_face.h>  // neon interface
#include <linux/sched.h>     // current
#include "neon_ui.h"
#include "neon_help.h"
#include "neon_sched.h"
#include "neon_policy.h"
#include "neon_fcfs.h"
#include "neon_timeslice.h"

/***************************************************************************/

static ctl_table knob_neon_options[] = {
  NEON_POLLING_KNOB,
  NEON_MALICIOUS_KNOB,
  NEON_POLICY_KNOB,
  NEON_POLICY_TIMESLICE_KNOB,
  NEON_POLICY_FCFS_KNOB,
  NEON_POLICY_SAMPLING_KNOB,
  { 0 }
};

// main sysctl knob
static ctl_table knob_root[] = {
  {
    .procname = "neon",
    .mode = 0555,
    .child = knob_neon_options,
  },
  { 0 }
};

static struct ctl_table_header *knob_header = NULL;

static int  dev_major;
static char tweet_str[TWEET_LENGTH + 1];
static struct file_operations fops;

/***************************************************************************/

/***************************************************************************/
// twitter_write
/***************************************************************************/
// write a short message to the log
static ssize_t
twitter_write(struct file *fp,
              const char __user *u_tweet_str,
              size_t len,
              loff_t *off)
{
  if(len > TWEET_LENGTH)
    return -EINVAL;

  copy_from_user(tweet_str, u_tweet_str, len);
  tweet_str[len] = '\0';

  // write a note to the log
  // the log is also accessible through the kernel interface (neon_tweet)
  neon_notice("U_tweet %s", tweet_str);

  return len;
}

/***************************************************************************/
// neon_ui_init
/***************************************************************************/
// init user interface proc options and virtual devices
int
neon_ui_init(struct module *owner)
{
  fops.owner   = owner;
  fops.open    = NULL; // standard open call
  fops.release = NULL; // standard release call
  fops.read    = NULL; // standard read call
  fops.write   = twitter_write;

  // register twitter device
  dev_major = register_chrdev(0, TWITTER_DEV_NAME, &fops);
  if(dev_major < 0) {
    neon_error("Error %d : Registering twitter chardev failed\n",
               dev_major);
  } else {
    neon_info("Twitter chardev assigned major 0x%x \n", dev_major);
    neon_info("Use mknod to create the device if necessary. \n");
  }

  // register proc/sys options
  // update proc fs with our sysctl entries
  knob_header = register_sysctl_table(knob_root);
  if(!knob_header)
    return -ENOMEM;

  return 0;
}

/***************************************************************************/
// neon_ui_fini
/***************************************************************************/
// destroy user-interface proc options and virtual devices
int
neon_ui_fini(void)
{
  // unregister proc/sys options
  unregister_sysctl_table(knob_header);

  // unregister twitter virtual device
  unregister_chrdev(dev_major, TWITTER_DEV_NAME);

  return 0;
}
