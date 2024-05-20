/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM cpuinfo

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_CPUINFO_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_CPUINFO_H

#include <trace/hooks/vendor_hooks.h>

DECLARE_RESTRICTED_HOOK(android_rvh_cpuinfo_c_show,
	TP_PROTO(struct seq_file *m),
	TP_ARGS(m), 1);

#endif /* _TRACE_HOOK_CPUINFO_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
