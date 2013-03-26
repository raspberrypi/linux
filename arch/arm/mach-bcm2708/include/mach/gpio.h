/*
 * arch/arm/mach-bcm2708/include/mach/gpio.h
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __ASM_ARCH_GPIO_H
#define __ASM_ARCH_GPIO_H

#define ARCH_NR_GPIOS 54 // number of gpio lines

#include <asm-generic/gpio.h>


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

static inline int gpio_to_irq(unsigned gpio)
{
        WARN_ON(1);
        return -ENOSYS;
}

static inline int irq_to_gpio(unsigned int irq)
{
        WARN_ON(1);
        return -EINVAL;
}

#endif /* CONFIG_GPIOLIB */

#endif
