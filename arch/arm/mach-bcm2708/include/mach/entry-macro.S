/*
 * arch/arm/mach-bcm2708/include/mach/entry-macro.S
 *
 * Low-level IRQ helper macros for BCM2708 platforms
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
#include <mach/hardware.h>

		.macro	disable_fiq
		.endm

		.macro  get_irqnr_preamble, base, tmp
		ldr	\base, =IO_ADDRESS(ARMCTRL_IC_BASE)
		.endm

		.macro  arch_ret_to_user, tmp1, tmp2
		.endm

		.macro	get_irqnr_and_base, irqnr, irqstat, base, tmp
		/* get masked status */
		ldr	\irqstat, [\base, #(ARM_IRQ_PEND0 - ARMCTRL_IC_BASE)]
		mov	\irqnr, #(ARM_IRQ0_BASE + 31)
		and	\tmp, \irqstat, #0x300		 @ save bits 8 and 9
		/* clear bits 8 and 9, and test */
		bics	\irqstat, \irqstat, #0x300
		bne	1010f

		tst	\tmp, #0x100
		ldrne	\irqstat, [\base, #(ARM_IRQ_PEND1 - ARMCTRL_IC_BASE)]
		movne \irqnr, #(ARM_IRQ1_BASE + 31)
		@ Mask out the interrupts also present in PEND0 - see SW-5809
		bicne \irqstat, #((1<<7) | (1<<9) | (1<<10))
		bicne \irqstat, #((1<<18) | (1<<19))
		bne	1010f

		tst	\tmp, #0x200
		ldrne \irqstat, [\base, #(ARM_IRQ_PEND2 - ARMCTRL_IC_BASE)]
		movne \irqnr, #(ARM_IRQ2_BASE + 31)
		@ Mask out the interrupts also present in PEND0 - see SW-5809
		bicne \irqstat, #((1<<21) | (1<<22) | (1<<23) | (1<<24) | (1<<25))
		bicne \irqstat, #((1<<30))
		beq 1020f

1010:
		@ For non-zero x, LSB(x) = 31 - CLZ(x^(x-1))
		@ N.B. CLZ is an ARM5 instruction.
		sub	\tmp, \irqstat, #1
		eor	\irqstat, \irqstat, \tmp
		clz	\tmp, \irqstat
		sub	\irqnr, \tmp

1020:	@ EQ will be set if no irqs pending

		.endm
