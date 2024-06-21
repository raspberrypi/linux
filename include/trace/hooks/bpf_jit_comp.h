/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM bpf_jit_comp

#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_BPF_JIT_COMP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_BPF_JIT_COMP_H
#include <trace/hooks/vendor_hooks.h>
/*
 * Following tracepoints are not exported in tracefs and provide a
 * mechanism for vendor modules to hook and extend functionality
 */
struct bpf_binary_header;
DECLARE_RESTRICTED_HOOK(android_rvh_bpf_int_jit_compile_ro,
	TP_PROTO(const struct bpf_binary_header *header, u32 size),
	TP_ARGS(header, size), 1);

#endif /* _TRACE_HOOK_BPF_JIT_COMP_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
