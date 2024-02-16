/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM thermal

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_THERMAL_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_THERMAL_H

#include <trace/hooks/vendor_hooks.h>

struct thermal_cooling_device;
DECLARE_HOOK(android_vh_disable_thermal_cooling_stats,
	TP_PROTO(struct thermal_cooling_device *cdev, bool *disable_stats),
	TP_ARGS(cdev, disable_stats));

#endif /* _TRACE_HOOK_THERMAL_H */
/* This part must be outside protection */
#include <trace/define_trace.h>

