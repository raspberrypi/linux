/*
 *  arch/arm/mach-bcm2708/include/mach/irqs.h
 *
 *  Copyright (C) 2010 Broadcom
 *  Copyright (C) 2003 ARM Limited
 *  Copyright (C) 2000 Deep Blue Solutions Ltd.
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

#ifndef _BCM2708_IRQS_H_
#define _BCM2708_IRQS_H_

#include <mach/platform.h>

/*
 *  IRQ interrupts definitions are the same as the INT definitions
 *  held within platform.h
 */
#define IRQ_ARMCTRL_START     0
#define IRQ_TIMER0            (IRQ_ARMCTRL_START + INTERRUPT_TIMER0)
#define IRQ_TIMER1            (IRQ_ARMCTRL_START + INTERRUPT_TIMER1)
#define IRQ_TIMER2            (IRQ_ARMCTRL_START + INTERRUPT_TIMER2)
#define IRQ_TIMER3            (IRQ_ARMCTRL_START + INTERRUPT_TIMER3)
#define IRQ_CODEC0            (IRQ_ARMCTRL_START + INTERRUPT_CODEC0)
#define IRQ_CODEC1            (IRQ_ARMCTRL_START + INTERRUPT_CODEC1)
#define IRQ_CODEC2            (IRQ_ARMCTRL_START + INTERRUPT_CODEC2)
#define IRQ_JPEG              (IRQ_ARMCTRL_START + INTERRUPT_JPEG)
#define IRQ_ISP               (IRQ_ARMCTRL_START + INTERRUPT_ISP)
#define IRQ_USB               (IRQ_ARMCTRL_START + INTERRUPT_USB)
#define IRQ_3D                (IRQ_ARMCTRL_START + INTERRUPT_3D)
#define IRQ_TRANSPOSER        (IRQ_ARMCTRL_START + INTERRUPT_TRANSPOSER)
#define IRQ_MULTICORESYNC0    (IRQ_ARMCTRL_START + INTERRUPT_MULTICORESYNC0)
#define IRQ_MULTICORESYNC1    (IRQ_ARMCTRL_START + INTERRUPT_MULTICORESYNC1)
#define IRQ_MULTICORESYNC2    (IRQ_ARMCTRL_START + INTERRUPT_MULTICORESYNC2)
#define IRQ_MULTICORESYNC3    (IRQ_ARMCTRL_START + INTERRUPT_MULTICORESYNC3)
#define IRQ_DMA0              (IRQ_ARMCTRL_START + INTERRUPT_DMA0)
#define IRQ_DMA1              (IRQ_ARMCTRL_START + INTERRUPT_DMA1)
#define IRQ_DMA2              (IRQ_ARMCTRL_START + INTERRUPT_DMA2)
#define IRQ_DMA3              (IRQ_ARMCTRL_START + INTERRUPT_DMA3)
#define IRQ_DMA4              (IRQ_ARMCTRL_START + INTERRUPT_DMA4)
#define IRQ_DMA5              (IRQ_ARMCTRL_START + INTERRUPT_DMA5)
#define IRQ_DMA6              (IRQ_ARMCTRL_START + INTERRUPT_DMA6)
#define IRQ_DMA7              (IRQ_ARMCTRL_START + INTERRUPT_DMA7)
#define IRQ_DMA8              (IRQ_ARMCTRL_START + INTERRUPT_DMA8)
#define IRQ_DMA9              (IRQ_ARMCTRL_START + INTERRUPT_DMA9)
#define IRQ_DMA10             (IRQ_ARMCTRL_START + INTERRUPT_DMA10)
#define IRQ_DMA11             (IRQ_ARMCTRL_START + INTERRUPT_DMA11)
#define IRQ_DMA12             (IRQ_ARMCTRL_START + INTERRUPT_DMA12)
#define IRQ_AUX               (IRQ_ARMCTRL_START + INTERRUPT_AUX)
#define IRQ_ARM               (IRQ_ARMCTRL_START + INTERRUPT_ARM)
#define IRQ_VPUDMA            (IRQ_ARMCTRL_START + INTERRUPT_VPUDMA)
#define IRQ_HOSTPORT          (IRQ_ARMCTRL_START + INTERRUPT_HOSTPORT)
#define IRQ_VIDEOSCALER       (IRQ_ARMCTRL_START + INTERRUPT_VIDEOSCALER)
#define IRQ_CCP2TX            (IRQ_ARMCTRL_START + INTERRUPT_CCP2TX)
#define IRQ_SDC               (IRQ_ARMCTRL_START + INTERRUPT_SDC)
#define IRQ_DSI0              (IRQ_ARMCTRL_START + INTERRUPT_DSI0)
#define IRQ_AVE               (IRQ_ARMCTRL_START + INTERRUPT_AVE)
#define IRQ_CAM0              (IRQ_ARMCTRL_START + INTERRUPT_CAM0)
#define IRQ_CAM1              (IRQ_ARMCTRL_START + INTERRUPT_CAM1)
#define IRQ_HDMI0             (IRQ_ARMCTRL_START + INTERRUPT_HDMI0)
#define IRQ_HDMI1             (IRQ_ARMCTRL_START + INTERRUPT_HDMI1)
#define IRQ_PIXELVALVE1       (IRQ_ARMCTRL_START + INTERRUPT_PIXELVALVE1)
#define IRQ_I2CSPISLV         (IRQ_ARMCTRL_START + INTERRUPT_I2CSPISLV)
#define IRQ_DSI1              (IRQ_ARMCTRL_START + INTERRUPT_DSI1)
#define IRQ_PWA0              (IRQ_ARMCTRL_START + INTERRUPT_PWA0)
#define IRQ_PWA1              (IRQ_ARMCTRL_START + INTERRUPT_PWA1)
#define IRQ_CPR               (IRQ_ARMCTRL_START + INTERRUPT_CPR)
#define IRQ_SMI               (IRQ_ARMCTRL_START + INTERRUPT_SMI)
#define IRQ_GPIO0             (IRQ_ARMCTRL_START + INTERRUPT_GPIO0)
#define IRQ_GPIO1             (IRQ_ARMCTRL_START + INTERRUPT_GPIO1)
#define IRQ_GPIO2             (IRQ_ARMCTRL_START + INTERRUPT_GPIO2)
#define IRQ_GPIO3             (IRQ_ARMCTRL_START + INTERRUPT_GPIO3)
#define IRQ_I2C               (IRQ_ARMCTRL_START + INTERRUPT_I2C)
#define IRQ_SPI               (IRQ_ARMCTRL_START + INTERRUPT_SPI)
#define IRQ_I2SPCM            (IRQ_ARMCTRL_START + INTERRUPT_I2SPCM)
#define IRQ_SDIO              (IRQ_ARMCTRL_START + INTERRUPT_SDIO)
#define IRQ_UART              (IRQ_ARMCTRL_START + INTERRUPT_UART)
#define IRQ_SLIMBUS           (IRQ_ARMCTRL_START + INTERRUPT_SLIMBUS)
#define IRQ_VEC               (IRQ_ARMCTRL_START + INTERRUPT_VEC)
#define IRQ_CPG               (IRQ_ARMCTRL_START + INTERRUPT_CPG)
#define IRQ_RNG               (IRQ_ARMCTRL_START + INTERRUPT_RNG)
#define IRQ_ARASANSDIO        (IRQ_ARMCTRL_START + INTERRUPT_ARASANSDIO)
#define IRQ_AVSPMON           (IRQ_ARMCTRL_START + INTERRUPT_AVSPMON)

#define IRQ_ARM_TIMER         (IRQ_ARMCTRL_START + INTERRUPT_ARM_TIMER)
#define IRQ_ARM_MAILBOX       (IRQ_ARMCTRL_START + INTERRUPT_ARM_MAILBOX)
#define IRQ_ARM_DOORBELL_0    (IRQ_ARMCTRL_START + INTERRUPT_ARM_DOORBELL_0)
#define IRQ_ARM_DOORBELL_1    (IRQ_ARMCTRL_START + INTERRUPT_ARM_DOORBELL_1)
#define IRQ_VPU0_HALTED       (IRQ_ARMCTRL_START + INTERRUPT_VPU0_HALTED)
#define IRQ_VPU1_HALTED       (IRQ_ARMCTRL_START + INTERRUPT_VPU1_HALTED)
#define IRQ_ILLEGAL_TYPE0     (IRQ_ARMCTRL_START + INTERRUPT_ILLEGAL_TYPE0)
#define IRQ_ILLEGAL_TYPE1     (IRQ_ARMCTRL_START + INTERRUPT_ILLEGAL_TYPE1)
#define IRQ_PENDING1          (IRQ_ARMCTRL_START + INTERRUPT_PENDING1)
#define IRQ_PENDING2          (IRQ_ARMCTRL_START + INTERRUPT_PENDING2)

/*
 *  FIQ interrupts definitions are the same as the INT definitions.
 */
#define FIQ_TIMER0            INT_TIMER0
#define FIQ_TIMER1            INT_TIMER1
#define FIQ_TIMER2            INT_TIMER2
#define FIQ_TIMER3            INT_TIMER3
#define FIQ_CODEC0            INT_CODEC0
#define FIQ_CODEC1            INT_CODEC1
#define FIQ_CODEC2            INT_CODEC2
#define FIQ_JPEG              INT_JPEG
#define FIQ_ISP               INT_ISP
#define FIQ_USB               INT_USB
#define FIQ_3D                INT_3D
#define FIQ_TRANSPOSER        INT_TRANSPOSER
#define FIQ_MULTICORESYNC0    INT_MULTICORESYNC0
#define FIQ_MULTICORESYNC1    INT_MULTICORESYNC1
#define FIQ_MULTICORESYNC2    INT_MULTICORESYNC2
#define FIQ_MULTICORESYNC3    INT_MULTICORESYNC3
#define FIQ_DMA0              INT_DMA0
#define FIQ_DMA1              INT_DMA1
#define FIQ_DMA2              INT_DMA2
#define FIQ_DMA3              INT_DMA3
#define FIQ_DMA4              INT_DMA4
#define FIQ_DMA5              INT_DMA5
#define FIQ_DMA6              INT_DMA6
#define FIQ_DMA7              INT_DMA7
#define FIQ_DMA8              INT_DMA8
#define FIQ_DMA9              INT_DMA9
#define FIQ_DMA10             INT_DMA10
#define FIQ_DMA11             INT_DMA11
#define FIQ_DMA12             INT_DMA12
#define FIQ_AUX               INT_AUX
#define FIQ_ARM               INT_ARM
#define FIQ_VPUDMA            INT_VPUDMA
#define FIQ_HOSTPORT          INT_HOSTPORT
#define FIQ_VIDEOSCALER       INT_VIDEOSCALER
#define FIQ_CCP2TX            INT_CCP2TX
#define FIQ_SDC               INT_SDC
#define FIQ_DSI0              INT_DSI0
#define FIQ_AVE               INT_AVE
#define FIQ_CAM0              INT_CAM0
#define FIQ_CAM1              INT_CAM1
#define FIQ_HDMI0             INT_HDMI0
#define FIQ_HDMI1             INT_HDMI1
#define FIQ_PIXELVALVE1       INT_PIXELVALVE1
#define FIQ_I2CSPISLV         INT_I2CSPISLV
#define FIQ_DSI1              INT_DSI1
#define FIQ_PWA0              INT_PWA0
#define FIQ_PWA1              INT_PWA1
#define FIQ_CPR               INT_CPR
#define FIQ_SMI               INT_SMI
#define FIQ_GPIO0             INT_GPIO0
#define FIQ_GPIO1             INT_GPIO1
#define FIQ_GPIO2             INT_GPIO2
#define FIQ_GPIO3             INT_GPIO3
#define FIQ_I2C               INT_I2C
#define FIQ_SPI               INT_SPI
#define FIQ_I2SPCM            INT_I2SPCM
#define FIQ_SDIO              INT_SDIO
#define FIQ_UART              INT_UART
#define FIQ_SLIMBUS           INT_SLIMBUS
#define FIQ_VEC               INT_VEC
#define FIQ_CPG               INT_CPG
#define FIQ_RNG               INT_RNG
#define FIQ_ARASANSDIO        INT_ARASANSDIO
#define FIQ_AVSPMON           INT_AVSPMON

#define FIQ_ARM_TIMER         INT_ARM_TIMER
#define FIQ_ARM_MAILBOX       INT_ARM_MAILBOX
#define FIQ_ARM_DOORBELL_0    INT_ARM_DOORBELL_0
#define FIQ_ARM_DOORBELL_1    INT_ARM_DOORBELL_1
#define FIQ_VPU0_HALTED       INT_VPU0_HALTED
#define FIQ_VPU1_HALTED       INT_VPU1_HALTED
#define FIQ_ILLEGAL_TYPE0     INT_ILLEGAL_TYPE0
#define FIQ_ILLEGAL_TYPE1     INT_ILLEGAL_TYPE1
#define FIQ_PENDING1          INT_PENDING1
#define FIQ_PENDING2          INT_PENDING2

#define HARD_IRQS	      (64 + 21)
#define GPIO_IRQ_START	      HARD_IRQS

#define GPIO_IRQS	      32*5

#define NR_IRQS		      HARD_IRQS+GPIO_IRQS


#endif /* _BCM2708_IRQS_H_ */
