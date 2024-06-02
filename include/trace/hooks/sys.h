/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM sys
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_SYS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_SYS_H
#include <trace/hooks/vendor_hooks.h>

struct task_struct;
DECLARE_HOOK(android_vh_syscall_prctl_finished,
	TP_PROTO(int option, struct task_struct *task),
	TP_ARGS(option, task));

struct mm_struct;
struct anon_vma_name;
DECLARE_HOOK(android_vh_anon_vma_name_recog,
	TP_PROTO(struct mm_struct *mm, struct anon_vma_name *anon_name),
	TP_ARGS(mm, anon_name));
DECLARE_HOOK(android_vh_restore_mm_flags,
	TP_PROTO(struct mm_struct *mm),
	TP_ARGS(mm));
#endif

#include <trace/define_trace.h>
