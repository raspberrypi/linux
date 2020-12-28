/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM ftrace_dump

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_FTRACE_DUMP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_FTRACE_DUMP_H

#include <trace/hooks/vendor_hooks.h>

DECLARE_HOOK(android_vh_ftrace_format_check,
	TP_PROTO(bool *ftrace_check),
	TP_ARGS(ftrace_check));

#endif /* _TRACE_HOOK_FTRACE_DUMP_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
