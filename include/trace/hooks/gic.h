/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM gic

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_GIC_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_GIC_H

#include <trace/hooks/vendor_hooks.h>

struct irq_data;
DECLARE_HOOK(android_vh_gic_set_affinity,
	TP_PROTO(struct irq_data *d, const struct cpumask *mask_val,
		 bool force, u8 *gic_cpu_map, void __iomem *reg),
	TP_ARGS(d, mask_val, force, gic_cpu_map, reg));

DECLARE_RESTRICTED_HOOK(android_rvh_fiq_dump,
	TP_PROTO(struct pt_regs *regs),
	TP_ARGS(regs), 1);

#endif /* _TRACE_HOOK_GIC_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
