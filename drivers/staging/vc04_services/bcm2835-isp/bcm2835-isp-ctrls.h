/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Broadcom BCM2835 ISP driver
 *
 * Copyright © 2019-2020 Raspberry Pi (Trading) Ltd.
 *
 * Author: Naushir Patuck (naush@raspberrypi.com)
 *
 */

#ifndef BCM2835_ISP_CTRLS
#define BCM2835_ISP_CTRLS

#include <linux/bcm2835-isp.h>

struct bcm2835_isp_custom_ctrl {
	const char *name;
	u32 id;
	u32 size;
	u32 flags;
};

static const struct bcm2835_isp_custom_ctrl custom_ctrls[] = {
	{
		.name	= "Colour Correction Matrix",
		.id	= V4L2_CID_USER_BCM2835_ISP_CC_MATRIX,
		.size	= sizeof(struct bcm2835_isp_custom_ccm),
		.flags  = 0
	}, {
		.name	= "Lens Shading",
		.id	= V4L2_CID_USER_BCM2835_ISP_LENS_SHADING,
		.size	= sizeof(struct bcm2835_isp_lens_shading),
		.flags  = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE
	}, {
		.name	= "Black Level",
		.id	= V4L2_CID_USER_BCM2835_ISP_BLACK_LEVEL,
		.size	= sizeof(struct bcm2835_isp_black_level),
		.flags  = 0
	}, {
		.name	= "Green Equalisation",
		.id	= V4L2_CID_USER_BCM2835_ISP_GEQ,
		.size	= sizeof(struct bcm2835_isp_geq),
		.flags  = 0
	}, {
		.name	= "Gamma",
		.id	= V4L2_CID_USER_BCM2835_ISP_GAMMA,
		.size	= sizeof(struct bcm2835_isp_gamma),
		.flags  = 0
	}, {
		.name	= "Sharpen",
		.id	= V4L2_CID_USER_BCM2835_ISP_SHARPEN,
		.size	= sizeof(struct bcm2835_isp_sharpen),
		.flags  = 0
	}, {
		.name	= "Denoise",
		.id	= V4L2_CID_USER_BCM2835_ISP_DENOISE,
		.size	= sizeof(struct bcm2835_isp_denoise),
		.flags  = 0
	}, {
		.name	= "Colour Denoise",
		.id	= V4L2_CID_USER_BCM2835_ISP_CDN,
		.size	= sizeof(struct bcm2835_isp_cdn),
		.flags  = 0
	}, {
		.name	= "Defective Pixel Correction",
		.id	= V4L2_CID_USER_BCM2835_ISP_DPC,
		.size	= sizeof(struct bcm2835_isp_dpc),
		.flags  = 0
	}
};

#endif
