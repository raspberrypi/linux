/*
 *  linux/arch/arm/lib/delay.S
 *
 *  Copyright (C) 1995, 1996 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/param.h>

		.text
.align 3	@ 8 byte alignment seems to be needed to avoid fetching stalls
@ Delay routine
ENTRY(bcm2708_delay)
		subs	r0, r0, #1
		bhi	bcm2708_delay
		mov	pc, lr
ENDPROC(bcm2708_delay)
