/******************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON interface for black-box GPU kernel-level management"
*/
/******************************************************************************/

#undef  TRACE_SYSTEM
#define TRACE_SYSTEM neon

#if !defined(__NEON_H__) || defined(TRACE_HEADER_MULTI_READ)
#define __NEON_H__

#include <linux/tracepoint.h>

TRACE_EVENT(neon_record,

	TP_PROTO(const char *in_str),

	TP_ARGS(in_str),

	TP_STRUCT__entry(
		__string(	str,		in_str	)
	),

	TP_fast_assign(
	        __assign_str(str, in_str)
	),

        TP_printk("%s", __get_str(str))
)

#endif // __NEON_H___

#include <trace/define_trace.h>
