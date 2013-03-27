/*
 * arch/arm/mach-bcm2708/include/mach/gpio.h
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __ASM_ARCH_GPIO_H
#define __ASM_ARCH_GPIO_H

#define BCM_NR_GPIOS 54 // number of gpio lines

#include <asm-generic/gpio.h>
#include <mach/platform.h>
#include <mach/irqs.h>

#ifdef CONFIG_GPIOLIB
static inline unsigned irq_to_gpio(unsigned irq) {
	return (irq-GPIO_IRQ_START);
}
#endif /* CONFIG_GPIOLIB */

#endif

