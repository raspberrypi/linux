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

static inline int gpio_get_value(unsigned gpio)
{
        return __gpio_get_value(gpio);
}

static inline void gpio_set_value(unsigned gpio, int value)
{
        __gpio_set_value(gpio, value);
}

static inline int gpio_cansleep(unsigned gpio)
{
        return __gpio_cansleep(gpio);
}


static inline unsigned irq_to_gpio(unsigned irq) {
	return (irq-GPIO_IRQ_START);
}

static inline unsigned gpio_to_irq(unsigned gpio) {
	return GPIO_IRQ_START+gpio;
}
#define gpio_to_irq gpio_to_irq

#endif /* CONFIG_GPIOLIB */

#endif

