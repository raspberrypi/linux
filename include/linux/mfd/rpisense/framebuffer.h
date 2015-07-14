/*
 * Raspberry Pi Sense HAT framebuffer driver
 * http://raspberrypi.org
 *
 * Copyright (C) 2015 Raspberry Pi
 *
 * Author: Serge Schneider
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#ifndef __LINUX_RPISENSE_FB_H_
#define __LINUX_RPISENSE_FB_H_

#include <linux/platform_device.h>

struct rpisense;

struct rpisense_fb {
	struct platform_device *pdev;
	struct fb_info *info;
};

#endif
