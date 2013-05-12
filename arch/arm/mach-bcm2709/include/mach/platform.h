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
#define IO_ADDRESS(x)	(((x) & 0x00ffffff) + (((x) >> 4) & 0x0f000000) + 0xf0000000)

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

#include "arm_control.h"
#undef ARM_BASE

/*
 * Definitions and addresses for the ARM CONTROL logic
 * This file is manually generated.
 */

#define BCM2708_PERI_BASE        0x3F000000
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


/*
 * Interrupt assignments
 */

#define ARM_IRQ1_BASE                  0
#define INTERRUPT_TIMER0               (ARM_IRQ1_BASE + 0)
#define INTERRUPT_TIMER1               (ARM_IRQ1_BASE + 1)
#define INTERRUPT_TIMER2               (ARM_IRQ1_BASE + 2)
#define INTERRUPT_TIMER3               (ARM_IRQ1_BASE + 3)
#define INTERRUPT_CODEC0               (ARM_IRQ1_BASE + 4)
#define INTERRUPT_CODEC1               (ARM_IRQ1_BASE + 5)
#define INTERRUPT_CODEC2               (ARM_IRQ1_BASE + 6)
#define INTERRUPT_VC_JPEG              (ARM_IRQ1_BASE + 7)
#define INTERRUPT_ISP                  (ARM_IRQ1_BASE + 8)
#define INTERRUPT_VC_USB               (ARM_IRQ1_BASE + 9)
#define INTERRUPT_VC_3D                (ARM_IRQ1_BASE + 10)
#define INTERRUPT_TRANSPOSER           (ARM_IRQ1_BASE + 11)
#define INTERRUPT_MULTICORESYNC0       (ARM_IRQ1_BASE + 12)
#define INTERRUPT_MULTICORESYNC1       (ARM_IRQ1_BASE + 13)
#define INTERRUPT_MULTICORESYNC2       (ARM_IRQ1_BASE + 14)
#define INTERRUPT_MULTICORESYNC3       (ARM_IRQ1_BASE + 15)
#define INTERRUPT_DMA0                 (ARM_IRQ1_BASE + 16)
#define INTERRUPT_DMA1                 (ARM_IRQ1_BASE + 17)
#define INTERRUPT_VC_DMA2              (ARM_IRQ1_BASE + 18)
#define INTERRUPT_VC_DMA3              (ARM_IRQ1_BASE + 19)
#define INTERRUPT_DMA4                 (ARM_IRQ1_BASE + 20)
#define INTERRUPT_DMA5                 (ARM_IRQ1_BASE + 21)
#define INTERRUPT_DMA6                 (ARM_IRQ1_BASE + 22)
#define INTERRUPT_DMA7                 (ARM_IRQ1_BASE + 23)
#define INTERRUPT_DMA8                 (ARM_IRQ1_BASE + 24)
#define INTERRUPT_DMA9                 (ARM_IRQ1_BASE + 25)
#define INTERRUPT_DMA10                (ARM_IRQ1_BASE + 26)
#define INTERRUPT_DMA11                (ARM_IRQ1_BASE + 27)
#define INTERRUPT_DMA12                (ARM_IRQ1_BASE + 28)
#define INTERRUPT_AUX                (ARM_IRQ1_BASE + 29)
#define INTERRUPT_ARM                  (ARM_IRQ1_BASE + 30)
#define INTERRUPT_VPUDMA               (ARM_IRQ1_BASE + 31)

#define ARM_IRQ2_BASE                  32
#define INTERRUPT_HOSTPORT             (ARM_IRQ2_BASE + 0)
#define INTERRUPT_VIDEOSCALER          (ARM_IRQ2_BASE + 1)
#define INTERRUPT_CCP2TX               (ARM_IRQ2_BASE + 2)
#define INTERRUPT_SDC                  (ARM_IRQ2_BASE + 3)
#define INTERRUPT_DSI0                 (ARM_IRQ2_BASE + 4)
#define INTERRUPT_AVE                  (ARM_IRQ2_BASE + 5)
#define INTERRUPT_CAM0                 (ARM_IRQ2_BASE + 6)
#define INTERRUPT_CAM1                 (ARM_IRQ2_BASE + 7)
#define INTERRUPT_HDMI0                (ARM_IRQ2_BASE + 8)
#define INTERRUPT_HDMI1                (ARM_IRQ2_BASE + 9)
#define INTERRUPT_PIXELVALVE1          (ARM_IRQ2_BASE + 10)
#define INTERRUPT_I2CSPISLV            (ARM_IRQ2_BASE + 11)
#define INTERRUPT_DSI1                 (ARM_IRQ2_BASE + 12)
#define INTERRUPT_PWA0                 (ARM_IRQ2_BASE + 13)
#define INTERRUPT_PWA1                 (ARM_IRQ2_BASE + 14)
#define INTERRUPT_CPR                  (ARM_IRQ2_BASE + 15)
#define INTERRUPT_SMI                  (ARM_IRQ2_BASE + 16)
#define INTERRUPT_GPIO0                (ARM_IRQ2_BASE + 17)
#define INTERRUPT_GPIO1                (ARM_IRQ2_BASE + 18)
#define INTERRUPT_GPIO2                (ARM_IRQ2_BASE + 19)
#define INTERRUPT_GPIO3                (ARM_IRQ2_BASE + 20)
#define INTERRUPT_VC_I2C               (ARM_IRQ2_BASE + 21)
#define INTERRUPT_VC_SPI               (ARM_IRQ2_BASE + 22)
#define INTERRUPT_VC_I2SPCM            (ARM_IRQ2_BASE + 23)
#define INTERRUPT_VC_SDIO              (ARM_IRQ2_BASE + 24)
#define INTERRUPT_VC_UART              (ARM_IRQ2_BASE + 25)
#define INTERRUPT_SLIMBUS              (ARM_IRQ2_BASE + 26)
#define INTERRUPT_VEC                  (ARM_IRQ2_BASE + 27)
#define INTERRUPT_CPG                  (ARM_IRQ2_BASE + 28)
#define INTERRUPT_RNG                  (ARM_IRQ2_BASE + 29)
#define INTERRUPT_VC_ARASANSDIO        (ARM_IRQ2_BASE + 30)
#define INTERRUPT_AVSPMON              (ARM_IRQ2_BASE + 31)

#define ARM_IRQ0_BASE                  64
#define INTERRUPT_ARM_TIMER            (ARM_IRQ0_BASE + 0)
#define INTERRUPT_ARM_MAILBOX          (ARM_IRQ0_BASE + 1)
#define INTERRUPT_ARM_DOORBELL_0       (ARM_IRQ0_BASE + 2)
#define INTERRUPT_ARM_DOORBELL_1       (ARM_IRQ0_BASE + 3)
#define INTERRUPT_VPU0_HALTED          (ARM_IRQ0_BASE + 4)
#define INTERRUPT_VPU1_HALTED          (ARM_IRQ0_BASE + 5)
#define INTERRUPT_ILLEGAL_TYPE0        (ARM_IRQ0_BASE + 6)
#define INTERRUPT_ILLEGAL_TYPE1        (ARM_IRQ0_BASE + 7)
#define INTERRUPT_PENDING1             (ARM_IRQ0_BASE + 8)
#define INTERRUPT_PENDING2             (ARM_IRQ0_BASE + 9)
#define INTERRUPT_JPEG                 (ARM_IRQ0_BASE + 10)
#define INTERRUPT_USB                  (ARM_IRQ0_BASE + 11)
#define INTERRUPT_3D                   (ARM_IRQ0_BASE + 12)
#define INTERRUPT_DMA2                 (ARM_IRQ0_BASE + 13)
#define INTERRUPT_DMA3                 (ARM_IRQ0_BASE + 14)
#define INTERRUPT_I2C                  (ARM_IRQ0_BASE + 15)
#define INTERRUPT_SPI                  (ARM_IRQ0_BASE + 16)
#define INTERRUPT_I2SPCM               (ARM_IRQ0_BASE + 17)
#define INTERRUPT_SDIO                 (ARM_IRQ0_BASE + 18)
#define INTERRUPT_UART                 (ARM_IRQ0_BASE + 19)
#define INTERRUPT_ARASANSDIO           (ARM_IRQ0_BASE + 20)

#define ARM_IRQ_LOCAL_BASE             96
#define INTERRUPT_ARM_LOCAL_CNTPSIRQ   (ARM_IRQ_LOCAL_BASE + 0)
#define INTERRUPT_ARM_LOCAL_CNTPNSIRQ  (ARM_IRQ_LOCAL_BASE + 1)
#define INTERRUPT_ARM_LOCAL_CNTHPIRQ   (ARM_IRQ_LOCAL_BASE + 2)
#define INTERRUPT_ARM_LOCAL_CNTVIRQ    (ARM_IRQ_LOCAL_BASE + 3)
#define INTERRUPT_ARM_LOCAL_MAILBOX0   (ARM_IRQ_LOCAL_BASE + 4)
#define INTERRUPT_ARM_LOCAL_MAILBOX1   (ARM_IRQ_LOCAL_BASE + 5)
#define INTERRUPT_ARM_LOCAL_MAILBOX2   (ARM_IRQ_LOCAL_BASE + 6)
#define INTERRUPT_ARM_LOCAL_MAILBOX3   (ARM_IRQ_LOCAL_BASE + 7)
#define INTERRUPT_ARM_LOCAL_GPU_FAST   (ARM_IRQ_LOCAL_BASE + 8)
#define INTERRUPT_ARM_LOCAL_PMU_FAST   (ARM_IRQ_LOCAL_BASE + 9)
#define INTERRUPT_ARM_LOCAL_ZERO       (ARM_IRQ_LOCAL_BASE + 10)
#define INTERRUPT_ARM_LOCAL_TIMER      (ARM_IRQ_LOCAL_BASE + 11)

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
