/*
 *  linux/arch/arm/mach-bcm2708/bcm2708_gpio.c
 *
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <mach/gpio.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <mach/platform.h>

#define BCM_GPIO_DRIVER_NAME "bcm2708_gpio"
#define DRIVER_NAME BCM_GPIO_DRIVER_NAME
#define BCM_GPIO_USE_IRQ 1

#define GPIOFSEL(x)  (0x00+(x)*4)
#define GPIOSET(x)   (0x1c+(x)*4)
#define GPIOCLR(x)   (0x28+(x)*4)
#define GPIOLEV(x)   (0x34+(x)*4)
#define GPIOEDS(x)   (0x40+(x)*4)
#define GPIOREN(x)   (0x4c+(x)*4)
#define GPIOFEN(x)   (0x58+(x)*4)
#define GPIOHEN(x)   (0x64+(x)*4)
#define GPIOLEN(x)   (0x70+(x)*4)
#define GPIOAREN(x)  (0x7c+(x)*4)
#define GPIOAFEN(x)  (0x88+(x)*4)
#define GPIOUD(x)    (0x94+(x)*4)
#define GPIOUDCLK(x) (0x98+(x)*4)

enum { GPIO_FSEL_INPUT, GPIO_FSEL_OUTPUT,
	GPIO_FSEL_ALT5, GPIO_FSEL_ALT_4,
	GPIO_FSEL_ALT0, GPIO_FSEL_ALT1,
	GPIO_FSEL_ALT2, GPIO_FSEL_ALT3,
};

	/* Each of the two spinlocks protects a different set of hardware
	 * regiters and data structurs. This decouples the code of the IRQ from
	 * the GPIO code. This also makes the case of a GPIO routine call from
	 * the IRQ code simpler.
	 */
static DEFINE_SPINLOCK(lock);	/* GPIO registers */

struct bcm2708_gpio {
	struct list_head list;
	void __iomem *base;
	struct gpio_chip gc;
	unsigned long rising;
	unsigned long falling;
	unsigned long high;
	unsigned long low;
};

static int bcm2708_set_function(struct gpio_chip *gc, unsigned offset,
				int function)
{
	struct bcm2708_gpio *gpio = container_of(gc, struct bcm2708_gpio, gc);
	unsigned long flags;
	unsigned gpiodir;
	unsigned gpio_bank = offset / 10;
	unsigned gpio_field_offset = (offset - 10 * gpio_bank) * 3;

//printk(KERN_ERR DRIVER_NAME ": bcm2708_gpio_set_function %p (%d,%d)\n", gc, offset, function);
	if (offset >= BCM2708_NR_GPIOS)
		return -EINVAL;

	spin_lock_irqsave(&lock, flags);

	gpiodir = readl(gpio->base + GPIOFSEL(gpio_bank));
	gpiodir &= ~(7 << gpio_field_offset);
	gpiodir |= function << gpio_field_offset;
	writel(gpiodir, gpio->base + GPIOFSEL(gpio_bank));
	spin_unlock_irqrestore(&lock, flags);
	gpiodir = readl(gpio->base + GPIOFSEL(gpio_bank));

	return 0;
}

static int bcm2708_gpio_dir_in(struct gpio_chip *gc, unsigned offset)
{
	return bcm2708_set_function(gc, offset, GPIO_FSEL_INPUT);
}

static void bcm2708_gpio_set(struct gpio_chip *gc, unsigned offset, int value);
static int bcm2708_gpio_dir_out(struct gpio_chip *gc, unsigned offset,
				int value)
{
	int ret;
	ret = bcm2708_set_function(gc, offset, GPIO_FSEL_OUTPUT);
	if (ret >= 0)
		bcm2708_gpio_set(gc, offset, value);
	return ret;
}

static int bcm2708_gpio_get(struct gpio_chip *gc, unsigned offset)
{
	struct bcm2708_gpio *gpio = container_of(gc, struct bcm2708_gpio, gc);
	unsigned gpio_bank = offset / 32;
	unsigned gpio_field_offset = (offset - 32 * gpio_bank);
	unsigned lev;

	if (offset >= BCM2708_NR_GPIOS)
		return 0;
	lev = readl(gpio->base + GPIOLEV(gpio_bank));
//printk(KERN_ERR DRIVER_NAME ": bcm2708_gpio_get %p (%d)=%d\n", gc, offset, 0x1 & (lev>>gpio_field_offset));
	return 0x1 & (lev >> gpio_field_offset);
}

static void bcm2708_gpio_set(struct gpio_chip *gc, unsigned offset, int value)
{
	struct bcm2708_gpio *gpio = container_of(gc, struct bcm2708_gpio, gc);
	unsigned gpio_bank = offset / 32;
	unsigned gpio_field_offset = (offset - 32 * gpio_bank);
//printk(KERN_ERR DRIVER_NAME ": bcm2708_gpio_set %p (%d=%d)\n", gc, offset, value);
	if (offset >= BCM2708_NR_GPIOS)
		return;
	if (value)
		writel(1 << gpio_field_offset, gpio->base + GPIOSET(gpio_bank));
	else
		writel(1 << gpio_field_offset, gpio->base + GPIOCLR(gpio_bank));
}

/*************************************************************************************************************************
 * bcm2708 GPIO IRQ
 */

#if BCM_GPIO_USE_IRQ

static int bcm2708_gpio_to_irq(struct gpio_chip *chip, unsigned gpio)
{
	return gpio_to_irq(gpio);
}

static int bcm2708_gpio_irq_set_type(struct irq_data *d, unsigned type)
{
	unsigned irq = d->irq;
	struct bcm2708_gpio *gpio = irq_get_chip_data(irq);

	gpio->rising  &= ~(1 << irq_to_gpio(irq));
	gpio->falling &= ~(1 << irq_to_gpio(irq));
	gpio->high    &= ~(1 << irq_to_gpio(irq));
	gpio->low     &= ~(1 << irq_to_gpio(irq));

	if (type & ~(IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING | IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH))
		return -EINVAL;

	if (type & IRQ_TYPE_EDGE_RISING)
		gpio->rising |= (1 << irq_to_gpio(irq));
	if (type & IRQ_TYPE_EDGE_FALLING)
		gpio->falling |= (1 << irq_to_gpio(irq));
	if (type & IRQ_TYPE_LEVEL_HIGH)
		gpio->high |= (1 << irq_to_gpio(irq));
	if (type & IRQ_TYPE_LEVEL_LOW)
		gpio->low |= (1 << irq_to_gpio(irq));
	return 0;
}

static void bcm2708_gpio_irq_mask(struct irq_data *d)
{
	unsigned irq = d->irq;
	struct bcm2708_gpio *gpio = irq_get_chip_data(irq);
	unsigned gn = irq_to_gpio(irq);
	unsigned gb = gn / 32;
	unsigned long rising  = readl(gpio->base + GPIOREN(gb));
	unsigned long falling = readl(gpio->base + GPIOFEN(gb));
	unsigned long high    = readl(gpio->base + GPIOHEN(gb));
	unsigned long low     = readl(gpio->base + GPIOLEN(gb));

	gn = gn % 32;

	writel(rising  & ~(1 << gn), gpio->base + GPIOREN(gb));
	writel(falling & ~(1 << gn), gpio->base + GPIOFEN(gb));
	writel(high    & ~(1 << gn), gpio->base + GPIOHEN(gb));
	writel(low     & ~(1 << gn), gpio->base + GPIOLEN(gb));
}

static void bcm2708_gpio_irq_unmask(struct irq_data *d)
{
	unsigned irq = d->irq;
	struct bcm2708_gpio *gpio = irq_get_chip_data(irq);
	unsigned gn = irq_to_gpio(irq);
	unsigned gb = gn / 32;
	unsigned long rising  = readl(gpio->base + GPIOREN(gb));
	unsigned long falling = readl(gpio->base + GPIOFEN(gb));
	unsigned long high    = readl(gpio->base + GPIOHEN(gb));
	unsigned long low     = readl(gpio->base + GPIOLEN(gb));

	gn = gn % 32;

	writel(1 << gn, gpio->base + GPIOEDS(gb));

	if (gpio->rising & (1 << gn)) {
		writel(rising |  (1 << gn), gpio->base + GPIOREN(gb));
	} else {
		writel(rising & ~(1 << gn), gpio->base + GPIOREN(gb));
	}

	if (gpio->falling & (1 << gn)) {
		writel(falling |  (1 << gn), gpio->base + GPIOFEN(gb));
	} else {
		writel(falling & ~(1 << gn), gpio->base + GPIOFEN(gb));
	}

	if (gpio->high & (1 << gn)) {
		writel(high |  (1 << gn), gpio->base + GPIOHEN(gb));
	} else {
		writel(high & ~(1 << gn), gpio->base + GPIOHEN(gb));
	}

	if (gpio->low & (1 << gn)) {
		writel(low |  (1 << gn), gpio->base + GPIOLEN(gb));
	} else {
		writel(low & ~(1 << gn), gpio->base + GPIOLEN(gb));
	}
}

static struct irq_chip bcm2708_irqchip = {
	.name = "GPIO",
	.irq_enable = bcm2708_gpio_irq_unmask,
	.irq_disable = bcm2708_gpio_irq_mask,
	.irq_unmask = bcm2708_gpio_irq_unmask,
	.irq_mask = bcm2708_gpio_irq_mask,
	.irq_set_type = bcm2708_gpio_irq_set_type,
};

static irqreturn_t bcm2708_gpio_interrupt(int irq, void *dev_id)
{
	unsigned long edsr;
	unsigned bank;
	int i;
	unsigned gpio;
	for (bank = 0; bank <= 1; bank++) {
		edsr = readl(__io_address(GPIO_BASE) + GPIOEDS(bank));
		for_each_set_bit(i, &edsr, 32) {
			gpio = i + bank * 32;
			generic_handle_irq(gpio_to_irq(gpio));
		}
		writel(0xffffffff, __io_address(GPIO_BASE) + GPIOEDS(bank));
	}
	return IRQ_HANDLED;
}

static struct irqaction bcm2708_gpio_irq = {
	.name = "BCM2708 GPIO catchall handler",
	.flags = IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL,
	.handler = bcm2708_gpio_interrupt,
};

static void bcm2708_gpio_irq_init(struct bcm2708_gpio *ucb)
{
	unsigned irq;

	ucb->gc.to_irq = bcm2708_gpio_to_irq;

	for (irq = GPIO_IRQ_START; irq < (GPIO_IRQ_START + GPIO_IRQS); irq++) {
		irq_set_chip_data(irq, ucb);
		irq_set_chip(irq, &bcm2708_irqchip);
		set_irq_flags(irq, IRQF_VALID);
	}
	setup_irq(IRQ_GPIO3, &bcm2708_gpio_irq);
}

#else

static void bcm2708_gpio_irq_init(struct bcm2708_gpio *ucb)
{
}

#endif /* #if BCM_GPIO_USE_IRQ ***************************************************************************************************************** */

static int bcm2708_gpio_probe(struct platform_device *dev)
{
	struct bcm2708_gpio *ucb;
	struct resource *res;
	int err = 0;

	printk(KERN_INFO DRIVER_NAME ": bcm2708_gpio_probe %p\n", dev);

	ucb = kzalloc(sizeof(*ucb), GFP_KERNEL);
	if (NULL == ucb) {
		printk(KERN_ERR DRIVER_NAME ": failed to allocate "
		       "mailbox memory\n");
		err = -ENOMEM;
		goto err;
	}

	res = platform_get_resource(dev, IORESOURCE_MEM, 0);

	platform_set_drvdata(dev, ucb);
	ucb->base = __io_address(GPIO_BASE);

	ucb->gc.label = "bcm2708_gpio";
	ucb->gc.base = 0;
	ucb->gc.ngpio = BCM2708_NR_GPIOS;
	ucb->gc.owner = THIS_MODULE;

	ucb->gc.direction_input = bcm2708_gpio_dir_in;
	ucb->gc.direction_output = bcm2708_gpio_dir_out;
	ucb->gc.get = bcm2708_gpio_get;
	ucb->gc.set = bcm2708_gpio_set;
	ucb->gc.can_sleep = 0;

	bcm2708_gpio_irq_init(ucb);

	err = gpiochip_add(&ucb->gc);
	if (err)
		goto err;

err:
	return err;

}

static int bcm2708_gpio_remove(struct platform_device *dev)
{
	int err = 0;
	struct bcm2708_gpio *ucb = platform_get_drvdata(dev);

	printk(KERN_ERR DRIVER_NAME ": bcm2708_gpio_remove %p\n", dev);

	err = gpiochip_remove(&ucb->gc);

	platform_set_drvdata(dev, NULL);
	kfree(ucb);

	return err;
}

static struct platform_driver bcm2708_gpio_driver = {
	.probe = bcm2708_gpio_probe,
	.remove = bcm2708_gpio_remove,
	.driver = {
		   .name = "bcm2708_gpio"},
};

static int __init bcm2708_gpio_init(void)
{
	return platform_driver_register(&bcm2708_gpio_driver);
}

static void __exit bcm2708_gpio_exit(void)
{
	platform_driver_unregister(&bcm2708_gpio_driver);
}

module_init(bcm2708_gpio_init);
module_exit(bcm2708_gpio_exit);

MODULE_DESCRIPTION("Broadcom BCM2708 GPIO driver");
MODULE_LICENSE("GPL");
