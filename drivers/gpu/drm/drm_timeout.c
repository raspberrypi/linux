// SPDX-License-Identifier: MIT
/*
 * Timeout conversion helpers for wait ioctls.
 *
 * Copyright 2017 Red Hat
 * Copyright 2016 Advanced Micro Devices, Inc.
 */

#include <linux/export.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/sched.h>

#include <drm/drm_timeout.h>

/**
 * drm_timeout_abs_to_jiffies - calculate jiffies timeout from absolute value
 * @timeout_nsec: timeout nsec component in ns, 0 for poll
 *
 * Calculate the timeout in jiffies from an absolute time in ns. The result is
 * clamped to MAX_SCHEDULE_TIMEOUT - 1 to avoid infinite timeouts.
 *
 * Returns:
 * The number of jiffies until the given timeout, or the upper limit.
 */
signed long drm_timeout_abs_to_jiffies(s64 timeout_nsec)
{
	ktime_t abs_timeout, now;
	u64 timeout_ns, timeout_jiffies64;

	/* make 0 timeout means poll - absolute 0 doesn't seem valid */
	if (timeout_nsec == 0)
		return 0;

	abs_timeout = ns_to_ktime(timeout_nsec);
	now = ktime_get();

	if (!ktime_after(abs_timeout, now))
		return 0;

	timeout_ns = ktime_to_ns(ktime_sub(abs_timeout, now));

	timeout_jiffies64 = nsecs_to_jiffies64(timeout_ns);
	/*  clamp timeout to avoid infinite timeout */
	if (timeout_jiffies64 >= MAX_SCHEDULE_TIMEOUT - 1)
		return MAX_SCHEDULE_TIMEOUT - 1;

	return timeout_jiffies64 + 1;
}
EXPORT_SYMBOL(drm_timeout_abs_to_jiffies);
