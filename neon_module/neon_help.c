/**************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/**************************************************************************/

#include <asm/atomic.h>        // atomics
#include <linux/printk.h>      // vprintk
#include <stdarg.h>            // va_list
#include <trace/events/neon.h> // trace event
#include <linux/ktime.h>       // ktime
#include "neon_help.h"
//#include "neon_track.h"

/**************************************************************************/
// neon_note
/**************************************************************************/
// A short trace-related message to appear either in some LTT buffer
// or in the console
int
neon_note(const char *fmt, ...)
{
  int             printed_len = 0;

  va_list args;
#ifdef NEON_LTTRACE
  size_t size = NOTE_LEN;
  char buf[NOTE_LEN] = {0};
#endif // NEON_LTTRACE

  va_start(args, fmt);
#ifdef NEON_LTTRACE
  printed_len = vsnprintf(buf, size, fmt, args);
#else // !NEON_LTTRACE
  printed_len = vprintk(fmt, args);
#endif // NEON_LTTRACE
  va_end(args);

#ifdef NEON_LTTRACE
  while((printed_len > 0) &&
        (buf[printed_len] == '\n')) {
    buf[printed_len--] = '\0';
    break;
  }
  trace_neon_record(buf);
#endif // NEON_LTTRACE
  
  if(printed_len > 0)
    return 0; // success

  return 1;
}
