/*
 *  linux/arch/arm/mach-bcm2708/power.h
 *
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This device provides a shared mechanism for controlling the power to
 * VideoCore subsystems.
 */

#ifndef _MACH_BCM2708_POWER_H
#define _MACH_BCM2708_POWER_H

#include <linux/types.h>
#include <mach/arm_power.h>

typedef unsigned int BCM_POWER_HANDLE_T;

extern int bcm_power_open(BCM_POWER_HANDLE_T *handle);
extern int bcm_power_request(BCM_POWER_HANDLE_T handle, uint32_t request);
extern int bcm_power_close(BCM_POWER_HANDLE_T handle);

#endif
