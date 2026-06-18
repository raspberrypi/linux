/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_RAS_H_
#define _XE_RAS_H_

#include <linux/types.h>

struct xe_device;
struct xe_sysctrl_event_response;

void xe_ras_counter_threshold_crossed(struct xe_device *xe,
				      struct xe_sysctrl_event_response *response);
int xe_ras_get_counter(struct xe_device *xe, u8 severity, u8 component, u32 *value);

#endif
