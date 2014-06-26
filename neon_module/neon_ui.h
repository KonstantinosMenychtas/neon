/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/**************************************************************************/

#ifndef __NEON_UI_H__
#define __NEON_UI_H__

#define TWITTER_DEV_NAME "twitter"
#define TWITTER_MAGIC    0x74776974 // twit
#define TWEET_LENGTH     256

int neon_ui_init(struct module *owner);
int neon_ui_fini(void);

#endif // __NEON_UI_H__
