/*
 * Copyright 2017 Tadeusz Kijkowski
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
 */

#include <linux/linkage.h>

#include "bcm2836-reparkcpu.h"

	.text

ENTRY(bcm2836_repark_loop)
bcm2836_repark_loop:
	/* Disable IRQs and FIQs */
	cpsid i
	cpsid f

	/* Calculate physical to current addressing mode offset */
	adr r0, .L_base_ptr
	ldr r1, [r0]
	subs r0, r0, r1		@ r0 = phy - virt, Z = MMU enabled

	/* Address to data from C */
	ldr r1, .L_repark_data_ptr
	add r1, r1, r0		@ convert address to current mode
	ldrne r2, [r1, #BCM2836_REPARK_PHYS_BASE_OFFSET]
	movne r3, #CPU_REPARK_STATUS_NOMMU
	ldreq r2, [r1, #BCM2836_REPARK_VIRT_BASE_OFFSET]
	moveq r3, #CPU_REPARK_STATUS_MMU
	add r1, r1, #BCM2836_REPARK_CPU_STATUS_OFFSET

	/* Calculate address to our MAILBOX3 read and clear register */
	mrc p15, 0, r0, c0, c0, 5
	and r0, r0, #3		@ r0 = CPU number
	add r2, r2, r0, lsl #4	@ r2 = mailbox3 read & clear reg
	add r1, r1, r0, lsl #2	@ r1 = &repark_data.cpu_status[cpunr]

	/* Set CPU status */
	str r3, [r1]

	/* Wait for data in mailbox 3 */
1:	wfe
	ldr r0, [r2]
	cmp r0, #0
	beq 1b			@ early wake

	/* Clear mailbox */
	str r0, [r2]

	/* Set CPU status - online */
	mov r3, #CPU_REPARK_STATUS_ONLINE
	str r3, [r1]

	/* Jump to start address read from mailbox */
	bx r0
ENDPROC(bcm2836_repark_loop)

	.ltorg
	.align
.L_base_ptr:
	.word .
.L_repark_data_ptr:
	.word bcm2836_repark_data

