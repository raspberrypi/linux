/*
 *  linux/arch/arm/mach-bcm2708/include/mach/arm_power.h
 *
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _ARM_POWER_H
#define _ARM_POWER_H

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

#endif
