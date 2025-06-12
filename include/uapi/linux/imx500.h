/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A V4L2 driver for Sony IMX500 cameras.
 *
 * Copyright 2025 Ideas on Board Oy
 */

#ifndef _IMX500_H_
#define _IMX500_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#include <linux/videodev2.h>

struct imx500_network_weights {
	size_t size;
	void *data;
};

/* Custom ioctl to allow the passing of network weights to the driver */
#define VIDIOC_IMX500_LOAD_NETWORK _IOW('V', BASE_VIDIOC_PRIVATE, struct imx500_network_weights)

#endif /* _IMX500_H_ */
