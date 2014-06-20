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
#include <mach/irqs.h>

	.macro  arch_ret_to_user, tmp1, tmp2
	.endm

	.macro	get_irqnr_and_base, irqnr, irqstat, base, tmp

	/* get core number */
	mrc     p15, 0, \base, c0, c0, 5
	ubfx    \base, \base, #0, #2

	/* get core's local interrupt controller */
	ldr	\irqstat, = __io_address(ARM_LOCAL_IRQ_PENDING0)	@ local interrupt source
	add	\irqstat, \irqstat, \base, lsl #2
	ldr	\tmp, [\irqstat]
#ifdef CONFIG_SMP
	/* test for mailbox0 (IPI) interrupt */
	tst	\tmp, #0x10
	beq	1030f

	/* get core's mailbox interrupt control */
	ldr	\irqstat, = __io_address(ARM_LOCAL_MAILBOX0_CLR0)	@ mbox_clr
	add	\irqstat, \irqstat, \base, lsl #4
	ldr	\tmp, [\irqstat]
	clz	\tmp, \tmp
	rsb	\irqnr, \tmp, #31
	mov	\tmp, #1
	lsl	\tmp, \irqnr
	str	\tmp, [\irqstat]  @ clear interrupt source
	dsb
	mov	r1, sp
	badr	lr, 1b
	b	do_IPI
#endif
1030:
	/* check gpu interrupt */
	tst	\tmp, #0x100
	beq	1040f

	ldr	\base, =IO_ADDRESS(ARMCTRL_IC_BASE)
	/* get masked status */
	ldr	\irqstat, [\base, #(ARM_IRQ_PEND0 - ARMCTRL_IC_BASE)]
	mov	\irqnr, #(ARM_IRQ0_BASE + 31)
	and	\tmp, \irqstat, #0x300		 @ save bits 8 and 9
	/* clear bits 8 and 9, and test */
	bics	\irqstat, \irqstat, #0x300
	bne	1010f

	tst	\tmp, #0x100
	ldrne	\irqstat, [\base, #(ARM_IRQ_PEND1 - ARMCTRL_IC_BASE)]
	movne	\irqnr, #(ARM_IRQ1_BASE + 31)
	@ Mask out the interrupts also present in PEND0 - see SW-5809
	bicne	\irqstat, #((1<<7) | (1<<9) | (1<<10))
	bicne	\irqstat, #((1<<18) | (1<<19))
	bne	1010f

	tst	\tmp, #0x200
	ldrne	\irqstat, [\base, #(ARM_IRQ_PEND2 - ARMCTRL_IC_BASE)]
	movne	\irqnr, #(ARM_IRQ2_BASE + 31)
	@ Mask out the interrupts also present in PEND0 - see SW-5809
	bicne	\irqstat, #((1<<21) | (1<<22) | (1<<23) | (1<<24) | (1<<25))
	bicne	\irqstat, #((1<<30))
	beq	1020f
1010:
	@ For non-zero x, LSB(x) = 31 - CLZ(x^(x-1))
	sub	\tmp, \irqstat, #1
	eor	\irqstat, \irqstat, \tmp
	clz	\tmp, \irqstat
	sub	\irqnr, \tmp
	b	1050f
1040:
	cmp	\tmp, #0
	beq	1020f

	/* handle local (e.g. timer) interrupts */
	@ For non-zero x, LSB(x) = 31 - CLZ(x^(x-1))
	mov	\irqnr, #(ARM_IRQ_LOCAL_BASE + 31)
	sub	\irqstat, \tmp, #1
	eor	\irqstat, \irqstat, \tmp
	clz	\tmp, \irqstat
	sub	\irqnr, \tmp
1050:
	mov	r1, sp
	@
	@ routine called with r0 = irq number, r1 = struct pt_regs *
	@
	badr	lr, 1b
	b	asm_do_IRQ

1020:	@ EQ will be set if no irqs pending
	.endm

/*
 * Interrupt handling.  Preserves r7, r8, r9
 */
	.macro	arch_irq_handler_default
1:	get_irqnr_and_base r0, r2, r6, lr
	.endm
