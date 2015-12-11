/* arch/arm/include/debug/pl01x.S
 *
 * Debugging macro include header
 *
 *  Copyright (C) 1994-1999 Russell King
 *  Moved from linux/arch/arm/kernel/debug.S by Ben Dooks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
*/
#include <linux/amba/serial.h>

#ifdef CONFIG_DEBUG_ZTE_ZX
#undef UART01x_DR
#undef UART01x_FR
#define UART01x_DR     0x04
#define UART01x_FR     0x14
#endif

#define DEBUG_UART_P1_PHYS	0x20201000
#define DEBUG_UART_P2_PHYS	0x3f201000

#ifdef CONFIG_DEBUG_UART_PHYS
		.macro	addruart, rp, rv, tmp
		ldr     \rp, =0x410fb767	@ ARM1176JZF-S r0p7
		mrc	p15, 0, \tmp, c0, c0, 0	@ processor id
		cmp     \tmp, \rp		@ Is it a Pi 1?
		ldreq	\rp, =DEBUG_UART_P1_PHYS
		ldrne	\rp, =DEBUG_UART_P2_PHYS
		ldr	\rv, =CONFIG_DEBUG_UART_VIRT
		.endm
#endif

		.macro	senduart,rd,rx
		strb	\rd, [\rx, #UART01x_DR]
		.endm

		.macro	waituart,rd,rx
1001:		ldr	\rd, [\rx, #UART01x_FR]
 ARM_BE8(	rev	\rd, \rd )
		tst	\rd, #UART01x_FR_TXFF
		bne	1001b
		.endm

		.macro	busyuart,rd,rx
1001:		ldr	\rd, [\rx, #UART01x_FR]
 ARM_BE8(	rev	\rd, \rd )
		tst	\rd, #UART01x_FR_BUSY
		bne	1001b
		.endm
