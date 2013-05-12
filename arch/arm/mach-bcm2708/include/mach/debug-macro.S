/* arch/arm/mach-bcm2708/include/mach/debug-macro.S
 *
 * Debugging macro include header
 *
 *  Copyright (C) 2010 Broadcom
 *  Copyright (C) 1994-1999 Russell King
 *  Moved from linux/arch/arm/kernel/debug.S by Ben Dooks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
*/

#include <mach/platform.h>

		.macro	addruart, rp, rv, tmp
		ldr	\rp, =UART0_BASE
		ldr	\rv, =IO_ADDRESS(UART0_BASE)
		.endm

#include <asm/hardware/debug-pl01x.S>
