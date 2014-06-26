/***************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON black-box GPU channel management"
*/
/***************************************************************************/

#ifndef __NEON_HELP_H__
#define __NEON_HELP_H__

#include <linux/printk.h>      // pr_
#include <stdarg.h>            // va_list

/***************************************************************************/
// Macros and declarations
#define NOTE_LEN 240
#define NAME_LEN 10

#define ABS(a)            (((a) < 0) ? (-(a)) : (a))
#define ROUND_DIV(a, b)   ((a) % (b) == 0 ?                     \
                           ((a)/(b)) : ((((a)+(b))/(b))))
#define MULTIPLE_OF(a, b) ((a) % (b) == 0 ?                     \
                           ((a)/(b)) : ((((a)+(b))/(b))*(b)))

#define MASK_MAP_KEY(k) ((k) & 0xfff)
#define MASK_MAP_OFS(o)  (((o) & 0xff000000) >> 24)

// NOTE: use neon_note_ts's DEBUG-enabled wrappers
int neon_note(const char *fmt, ...); // a neon_help function

/***************************************************************************/
// Debugging verbosity control
// Level 0 --> 5 : errors only --> verbose
// Control through Makefile : 
// #define NEON_TRACE_REPORT  // to define trace collection
// #define NEON_LTTRACE       // to collect using lttng

#ifndef NEON_DEBUG_LEVEL_0
#ifndef NEON_DEBUG_LEVEL_1
#ifndef NEON_DEBUG_LEVEL_2
#ifndef NEON_DEBUG_LEVEL_3
#ifndef NEON_DEBUG_LEVEL_4
#ifdef NEON_DEBUG_LEVEL_5 // <--- enable levels 0, 1, 2, 3, 4, 5
#define NEON_DEBUG_LEVEL_4
#define NEON_DEBUG_LEVEL_3
#define NEON_DEBUG_LEVEL_2
#define NEON_DEBUG_LEVEL_1
#define NEON_DEBUG_LEVEL_0
#endif // NEON_DEBUG_LEVEL_5
#endif // NEON_DEBUG_LEVEL_4
#endif // !NEON_DEBUG_LEVEL_3
#endif // !NEON_DEBUG_LEVEL_2
#endif // !NEON_DEBUG_LEVEL_1
#endif // !NEON_DEBUG_LEVEL_0

#ifndef NEON_DEBUG_LEVEL_0
#ifndef NEON_DEBUG_LEVEL_1
#ifndef NEON_DEBUG_LEVEL_2
#ifndef NEON_DEBUG_LEVEL_3
#ifdef NEON_DEBUG_LEVEL_4 // <--- enable levels 0, 1, 2, 3, 4
#define NEON_DEBUG_LEVEL_3
#define NEON_DEBUG_LEVEL_2
#define NEON_DEBUG_LEVEL_1
#define NEON_DEBUG_LEVEL_0
#endif // NEON_DEBUG_LEVEL_4
#endif // !NEON_DEBUG_LEVEL_3
#endif // !NEON_DEBUG_LEVEL_2
#endif // !NEON_DEBUG_LEVEL_1
#endif // !NEON_DEBUG_LEVEL_0

#ifndef NEON_DEBUG_LEVEL_0
#ifndef NEON_DEBUG_LEVEL_1
#ifndef NEON_DEBUG_LEVEL_2
#ifdef NEON_DEBUG_LEVEL_3 // <--- enable levels 0, 1, 2, 3
#define NEON_DEBUG_LEVEL_2
#define NEON_DEBUG_LEVEL_1
#define NEON_DEBUG_LEVEL_0
#endif // NEON_DEBUG_LEVEL_3
#endif // !NEON_DEBUG_LEVEL_2
#endif // !NEON_DEBUG_LEVEL_1
#endif // !NEON_DEBUG_LEVEL_0

#ifndef NEON_DEBUG_LEVEL_0
#ifndef NEON_DEBUG_LEVEL_1
#ifdef NEON_DEBUG_LEVEL_2  // <--- enable levels 0, 1, 2
#define NEON_DEBUG_LEVEL_1
#define NEON_DEBUG_LEVEL_0
#endif // NEON_DEBUG_LEVEL_2
#endif // !NEON_DEBUG_LEVEL_1
#endif // !NEON_DEBUG_LEVEL_0

#ifndef NEON_DEBUG_LEVEL_0
#ifdef NEON_DEBUG_LEVEL_1 // // <--- enable levels 0, 1
#define NEON_DEBUG_LEVEL_0
#endif // NEON_DEBUG_LEVEL_1
#endif // NEON_DEBUG_LEVEL_0

// Standard messages

#ifdef NEON_DEBUG_LEVEL_0
#define neon_error(fmt, ...)                                    \
  neon_note(KERN_ERR pr_fmt("NEON ERR : " fmt), ##__VA_ARGS__)
#else // !NEON_DEBUG_LEVEL_0
#define neon_error(fmt, ...)                    \
  while(0)
#endif // NEON_DEBUG_LEVEL_0

#ifdef NEON_DEBUG_LEVEL_1
#define neon_warning(fmt, ...)                                  \
  neon_note(KERN_ERR pr_fmt("NEON WRN : " fmt), ##__VA_ARGS__)
#else // !NEON_DEBUG_LEVEL_1
#define neon_warning(fmt, ...)                  \
  while(0)
#endif // NEON_DEBUG_LEVEL_1

#ifdef NEON_DEBUG_LEVEL_2
#define neon_notice(fmt, ...)                                   \
  neon_note(KERN_ERR pr_fmt("NEON NTC : " fmt), ##__VA_ARGS__)
#else // !NEON_DEBUG_LEVEL_2
#define neon_notice(fmt, ...)                   \
  while(0)
#endif // NEON_DEBUG_LEVEL_2

#ifdef NEON_DEBUG_LEVEL_3
#define neon_info(fmt, ...)                                     \
  neon_note(KERN_ERR pr_fmt("NEON NFO : " fmt), ##__VA_ARGS__)
#else // !NEON_DEBUG_LEVEL_3
#define neon_info(fmt, ...)                     \
  while(0)
#endif // NEON_DEBUG_LEVEL_3

#ifdef NEON_DEBUG_LEVEL_4
#define neon_debug(fmt, ...)                                    \
  neon_note(KERN_ERR pr_fmt("NEON DBG : " fmt), ##__VA_ARGS__)
#else // !NEON_DEBUG_LEVEL_4
#define neon_debug(fmt, ...)                    \
  while(0)
#endif // NEON_DEBUG_LEVEL_4

#ifdef NEON_DEBUG_LEVEL_5

#define neon_verbose(fmt, ...)                                  \
  neon_note(KERN_ERR pr_fmt("NEON VRB : " fmt), ##__VA_ARGS__)
#else // !NEON_DEBUG_LEVEL_5
#define neon_verbose(fmt, ...)                    \
  while(0)
#endif // NEON_DEBUG_LEVEL_5

#ifdef NEON_DEBUG_LEVEL_1
#define neon_account(fmt, ...)                                  \
  neon_note(KERN_ERR pr_fmt("NEON CNT : " fmt), ##__VA_ARGS__)
#else // !NEON_DEBUG_LEVEL_1
#define neon_account(fmt, ...)                  \
  while(0)
#endif // NEON_DEBUG_LEVEL_1

#ifdef NEON_DEBUG_LEVEL_1
#define neon_report(fmt, ...)                                   \
  neon_note(KERN_ERR pr_fmt("NEON RPT : " fmt), ##__VA_ARGS__)
#else // !NEON_DEBUG_LEVEL_1
#define neon_report(fmt, ...)                   \
  while(0)
#endif // NEON_DEBUG_LEVEL_1

// Temporary checks --- always on
#define neon_urgent(fmt, ...)                                      \
  neon_note(KERN_ERR pr_fmt("NEON URG : " fmt), ##__VA_ARGS__)

#endif  // __NEON_HELP_H__
