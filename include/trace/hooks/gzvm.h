/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM gzvm
#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_GZVM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_GZVM_H
#include <trace/hooks/vendor_hooks.h>
struct gzvm_vcpu;

DECLARE_HOOK(android_vh_gzvm_vcpu_exit_reason,
	     TP_PROTO(struct gzvm_vcpu *vcpu, bool *userspace),
	     TP_ARGS(vcpu, userspace));

#endif /* _TRACE_HOOK_GZVM_H */
/* This part must be outside protection */
#include <trace/define_trace.h>

