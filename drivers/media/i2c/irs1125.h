/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A V4L2 driver for Infineon IRS1125 TOF cameras.
 * Copyright (C) 2018, pieye GmbH
 *
 * Based on V4L2 OmniVision OV5647 Image Sensor driver
 * Copyright (C) 2016 Ramiro Oliveira <roliveir@synopsys.com>
 *
 * DT / fwnode changes, and GPIO control taken from ov5640.c
 * Copyright (C) 2011-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (C) 2014-2017 Mentor Graphics Inc.
 *
 */

#ifndef IRS1125_H
#define IRS1125_H

#include <linux/v4l2-controls.h>
#include <linux/types.h>

#define IRS1125_NUM_SEQ_ENTRIES 20
#define IRS1125_NUM_MOD_PLLS 4

#define IRS1125_CID_CUSTOM_BASE        (V4L2_CID_USER_BASE | 0xf000)
#define IRS1125_CID_SAFE_RECONFIG      (IRS1125_CID_CUSTOM_BASE + 0)
#define IRS1125_CID_CONTINUOUS_TRIG    (IRS1125_CID_CUSTOM_BASE + 1)
#define IRS1125_CID_TRIGGER            (IRS1125_CID_CUSTOM_BASE + 2)
#define IRS1125_CID_RECONFIG           (IRS1125_CID_CUSTOM_BASE + 3)
#define IRS1125_CID_ILLU_ON            (IRS1125_CID_CUSTOM_BASE + 4)
#define IRS1125_CID_NUM_SEQS           (IRS1125_CID_CUSTOM_BASE + 5)
#define IRS1125_CID_MOD_PLL            (IRS1125_CID_CUSTOM_BASE + 6)
#define IRS1125_CID_SEQ_CONFIG         (IRS1125_CID_CUSTOM_BASE + 7)
#define IRS1125_CID_IDENT0             (IRS1125_CID_CUSTOM_BASE + 8)
#define IRS1125_CID_IDENT1             (IRS1125_CID_CUSTOM_BASE + 9)
#define IRS1125_CID_IDENT2             (IRS1125_CID_CUSTOM_BASE + 10)

struct irs1125_seq_cfg {
	__u16 exposure;
	__u16 framerate;
	__u16 ps;
	__u16 pll;
};

struct irs1125_illu {
	__u16 exposure;
	__u16 framerate;
};

struct irs1125_mod_pll {
	__u16 pllcfg1;
	__u16 pllcfg2;
	__u16 pllcfg3;
	__u16 pllcfg4;
	__u16 pllcfg5;
	__u16 pllcfg6;
	__u16 pllcfg7;
	__u16 pllcfg8;
};

#endif /* IRS1125 */

