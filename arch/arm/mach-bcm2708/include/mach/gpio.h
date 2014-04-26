/*
 * arch/arm/mach-bcm2708/include/mach/gpio.h
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __ASM_ARCH_GPIO_H
#define __ASM_ARCH_GPIO_H

#define BCM2708_NR_GPIOS 54 // number of gpio lines

#define gpio_to_irq(x)	((x) + GPIO_IRQ_START)
#define irq_to_gpio(x)	((x) - GPIO_IRQ_START)

#endif

