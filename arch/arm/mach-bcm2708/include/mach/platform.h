/*
 * arch/arm/mach-bcm2708/include/mach/platform.h
 *
 * Copyright (C) 2010 Broadcom
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

#ifndef _BCM2708_PLATFORM_H
#define _BCM2708_PLATFORM_H


/* macros to get at IO space when running virtually */
#define IO_ADDRESS(x)	(((x) & 0x0fffffff) + (((x) >> 4) & 0x0f000000) + 0xf0000000)

#define __io_address(n)     IOMEM(IO_ADDRESS(n))


/*
 *  SDRAM
 */
#define BCM2708_SDRAM_BASE           0x00000000

/*
 *  Logic expansion modules
 *
 */


/* ------------------------------------------------------------------------
 *  BCM2708 ARMCTRL Registers
 * ------------------------------------------------------------------------
 */

#define HW_REGISTER_RW(addr) (addr)
#define HW_REGISTER_RO(addr) (addr)

/*
 * Definitions and addresses for the ARM CONTROL logic
 * This file is manually generated.
 */

#define BCM2708_PERI_BASE        0x20000000
#define IC0_BASE                 (BCM2708_PERI_BASE + 0x2000)
#define ST_BASE                  (BCM2708_PERI_BASE + 0x3000)   /* System Timer */
#define MPHI_BASE		 (BCM2708_PERI_BASE + 0x6000)	/* Message -based Parallel Host Interface */
#define DMA_BASE		 (BCM2708_PERI_BASE + 0x7000)	/* DMA controller */
#define ARM_BASE                 (BCM2708_PERI_BASE + 0xB000)	 /* BCM2708 ARM control block */
#define PM_BASE			 (BCM2708_PERI_BASE + 0x100000) /* Power Management, Reset controller and Watchdog registers */
#define PCM_CLOCK_BASE           (BCM2708_PERI_BASE + 0x101098) /* PCM Clock */
#define RNG_BASE                 (BCM2708_PERI_BASE + 0x104000) /* Hardware RNG */
#define GPIO_BASE                (BCM2708_PERI_BASE + 0x200000) /* GPIO */
#define UART0_BASE               (BCM2708_PERI_BASE + 0x201000)	/* Uart 0 */
#define MMCI0_BASE               (BCM2708_PERI_BASE + 0x202000) /* MMC interface */
#define I2S_BASE                 (BCM2708_PERI_BASE + 0x203000) /* I2S */
#define SPI0_BASE		 (BCM2708_PERI_BASE + 0x204000) /* SPI0 */
#define BSC0_BASE		 (BCM2708_PERI_BASE + 0x205000) /* BSC0 I2C/TWI */
#define UART1_BASE               (BCM2708_PERI_BASE + 0x215000) /* Uart 1 */
#define EMMC_BASE                (BCM2708_PERI_BASE + 0x300000) /* eMMC interface */
#define SMI_BASE		 (BCM2708_PERI_BASE + 0x600000) /* SMI */
#define BSC1_BASE		 (BCM2708_PERI_BASE + 0x804000) /* BSC1 I2C/TWI */
#define USB_BASE                 (BCM2708_PERI_BASE + 0x980000) /* DTC_OTG USB controller */
#define MCORE_BASE               (BCM2708_PERI_BASE + 0x0000)   /* Fake frame buffer device (actually the multicore sync block*/

#define ARMCTRL_BASE             (ARM_BASE + 0x000)
#define ARMCTRL_IC_BASE          (ARM_BASE + 0x200)           /* ARM interrupt controller */
#define ARMCTRL_TIMER0_1_BASE    (ARM_BASE + 0x400)           /* Timer 0 and 1 */
#define ARMCTRL_0_SBM_BASE       (ARM_BASE + 0x800)           /* User 0 (ARM)'s Semaphores Doorbells and Mailboxes */
#define ARMCTRL_0_BELL_BASE      (ARMCTRL_0_SBM_BASE + 0x40)  /* User 0 (ARM)'s Doorbell */
#define ARMCTRL_0_MAIL0_BASE     (ARMCTRL_0_SBM_BASE + 0x80)  /* User 0 (ARM)'s Mailbox 0 */

/*
 * Watchdog
 */
#define PM_RSTC			       (PM_BASE+0x1c)
#define PM_RSTS			       (PM_BASE+0x20)
#define PM_WDOG			       (PM_BASE+0x24)

#define PM_WDOG_RESET                                         0000000000
#define PM_PASSWORD		       0x5a000000
#define PM_WDOG_TIME_SET	       0x000fffff
#define PM_RSTC_WRCFG_CLR              0xffffffcf
#define PM_RSTC_WRCFG_SET              0x00000030
#define PM_RSTC_WRCFG_FULL_RESET       0x00000020
#define PM_RSTC_RESET                  0x00000102

#define PM_RSTS_HADPOR_SET                                 0x00001000
#define PM_RSTS_HADSRH_SET                                 0x00000400
#define PM_RSTS_HADSRF_SET                                 0x00000200
#define PM_RSTS_HADSRQ_SET                                 0x00000100
#define PM_RSTS_HADWRH_SET                                 0x00000040
#define PM_RSTS_HADWRF_SET                                 0x00000020
#define PM_RSTS_HADWRQ_SET                                 0x00000010
#define PM_RSTS_HADDRH_SET                                 0x00000004
#define PM_RSTS_HADDRF_SET                                 0x00000002
#define PM_RSTS_HADDRQ_SET                                 0x00000001

#define UART0_CLOCK      3000000

#endif

/* END */
