/*
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This device provides a shared mechanism for controlling the power to
 * VideoCore subsystems.
 */

#ifndef _BCM2708_POWER_H
#define _BCM2708_POWER_H

#include <linux/types.h>

/* Use meaningful names on each side */
#ifdef __VIDEOCORE__
#define PREFIX(x) ARM_##x
#else
#define PREFIX(x) BCM_##x
#endif

enum {
	PREFIX(POWER_SDCARD_BIT),
	PREFIX(POWER_UART_BIT),
	PREFIX(POWER_MINIUART_BIT),
	PREFIX(POWER_USB_BIT),
	PREFIX(POWER_I2C0_BIT),
	PREFIX(POWER_I2C1_BIT),
	PREFIX(POWER_I2C2_BIT),
	PREFIX(POWER_SPI_BIT),
	PREFIX(POWER_CCP2TX_BIT),
	PREFIX(POWER_DSI_BIT),

	PREFIX(POWER_MAX)
};

enum {
	PREFIX(POWER_SDCARD) = (1 << PREFIX(POWER_SDCARD_BIT)),
	PREFIX(POWER_UART) = (1 << PREFIX(POWER_UART_BIT)),
	PREFIX(POWER_MINIUART) = (1 << PREFIX(POWER_MINIUART_BIT)),
	PREFIX(POWER_USB) = (1 << PREFIX(POWER_USB_BIT)),
	PREFIX(POWER_I2C0) = (1 << PREFIX(POWER_I2C0_BIT)),
	PREFIX(POWER_I2C1_MASK) = (1 << PREFIX(POWER_I2C1_BIT)),
	PREFIX(POWER_I2C2_MASK) = (1 << PREFIX(POWER_I2C2_BIT)),
	PREFIX(POWER_SPI_MASK) = (1 << PREFIX(POWER_SPI_BIT)),
	PREFIX(POWER_CCP2TX_MASK) = (1 << PREFIX(POWER_CCP2TX_BIT)),
	PREFIX(POWER_DSI) = (1 << PREFIX(POWER_DSI_BIT)),

	PREFIX(POWER_MASK) = (1 << PREFIX(POWER_MAX)) - 1,
	PREFIX(POWER_NONE) = 0
};

typedef unsigned int BCM_POWER_HANDLE_T;

extern int bcm_power_open(BCM_POWER_HANDLE_T *handle);
extern int bcm_power_request(BCM_POWER_HANDLE_T handle, uint32_t request);
extern int bcm_power_close(BCM_POWER_HANDLE_T handle);

#endif
