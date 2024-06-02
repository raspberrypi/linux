/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM madvise
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_MADVISE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_MADVISE_H
#include <trace/hooks/vendor_hooks.h>

struct vm_area_struct;
DECLARE_HOOK(android_vh_update_vma_flags,
	TP_PROTO(struct vm_area_struct *vma),
	TP_ARGS(vma));

#endif

#include <trace/define_trace.h>