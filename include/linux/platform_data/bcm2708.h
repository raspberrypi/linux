/*
 * include/linux/platform_data/bcm2708.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * (C) 2014 Julian Scheel <julian@jusst.de>
 *
 */
#ifndef __BCM2708_H_
#define __BCM2708_H_

typedef enum {
	BCM2708_PULL_OFF,
	BCM2708_PULL_UP,
	BCM2708_PULL_DOWN
} bcm2708_gpio_pull_t;

extern int bcm2708_gpio_setpull(struct gpio_chip *gc, unsigned offset,
		bcm2708_gpio_pull_t value);

#endif
