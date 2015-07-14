/*
 * Raspberry Pi Sense HAT joystick driver
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

#ifndef __LINUX_RPISENSE_JOYSTICK_H_
#define __LINUX_RPISENSE_JOYSTICK_H_

#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>

struct rpisense;

struct rpisense_js {
	struct platform_device *pdev;
	struct input_dev *keys_dev;
	struct gpio_desc *keys_desc;
	struct work_struct keys_work_s;
	int keys_irq;
};


#endif
