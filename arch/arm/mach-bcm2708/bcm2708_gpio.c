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
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <mach/platform.h>
#include <mach/gpio.h>

#define BCM_GPIO_DRIVER_NAME "bcm2708_gpio"
#define DRIVER_NAME BCM_GPIO_DRIVER_NAME
#define BCM_GPIO_USE_IRQ 0

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
       GPIO_FSEL_ALT2, GPIO_FSEL_ALT3, };

	/* Each of the two spinlocks protects a different set of hardware
	 * regiters and data structurs. This decouples the code of the IRQ from
	 * the GPIO code. This also makes the case of a GPIO routine call from
	 * the IRQ code simpler.
	 */
static DEFINE_SPINLOCK(lock);		/* GPIO registers */
static DEFINE_SPINLOCK(irq_lock);	/* IRQ registers */


struct bcm2708_gpio {
	/* We use a list of bcm2708_gpio structs for each trigger IRQ in the main
	 * interrupts controller of the system. We need this to support systems
	 * in which more that one bcm2708s are connected to the same IRQ. The ISR
	 * interates through this list to find the source of the interrupt.
	 */
	struct list_head	list;

	void __iomem		*base;
	unsigned		irq_base;
        struct gpio_chip        gc;
};

static int bcm2708_set_function(struct gpio_chip *gc, unsigned offset, int function)
{
	struct bcm2708_gpio *gpio = container_of(gc, struct bcm2708_gpio, gc);
	unsigned long flags;
	unsigned gpiodir;
        unsigned gpio_bank = offset/10;
        unsigned gpio_field_offset = (offset - 10*gpio_bank) * 3;

//printk(KERN_ERR DRIVER_NAME ": bcm2708_gpio_set_function %p (%d,%d)\n", gc, offset, function);
	if (offset >= ARCH_NR_GPIOS)
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
static int bcm2708_gpio_dir_out(struct gpio_chip *gc, unsigned offset, int value)
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
        unsigned gpio_bank = offset/32;
	unsigned gpio_field_offset = (offset - 32*gpio_bank);
        unsigned lev;

	if (offset >= ARCH_NR_GPIOS)
		return 0;
        lev = readl(gpio->base + GPIOLEV(gpio_bank));
//printk(KERN_ERR DRIVER_NAME ": bcm2708_gpio_get %p (%d)=%d\n", gc, offset, 0x1 & (lev>>gpio_field_offset));
	return 0x1 & (lev>>gpio_field_offset);
}

static void bcm2708_gpio_set(struct gpio_chip *gc, unsigned offset, int value)
{
	struct bcm2708_gpio *gpio = container_of(gc, struct bcm2708_gpio, gc);
        unsigned gpio_bank = offset/32;
	unsigned gpio_field_offset = (offset - 32*gpio_bank);
//printk(KERN_ERR DRIVER_NAME ": bcm2708_gpio_set %p (%d=%d)\n", gc, offset, value);
	if (offset >= ARCH_NR_GPIOS)
		return;
	if (value)
	        writel(1<<gpio_field_offset, gpio->base + GPIOSET(gpio_bank));
	else
	        writel(1<<gpio_field_offset, gpio->base + GPIOCLR(gpio_bank));
}

/*
 * bcm2708 GPIO IRQ
 */

#if BCM_GPIO_USE_IRQ
static void bcm2708_irq_disable(unsigned irq)
{
	struct bcm2708_gpio *chip = get_irq_chip_data(irq);
	//int offset = irq - gpio->irq_base;
	unsigned long flags;

	spin_lock_irqsave(&chip->irq_lock, flags);
        // disable gpio interrupts here
	spin_unlock_irqrestore(&chip->irq_lock, flags);
}

static void bcm2708_irq_enable(unsigned irq)
{
	struct bcm2708_gpio *chip = get_irq_chip_data(irq);
	//int offset = irq - chip->irq_base;
	unsigned long flags;

	spin_lock_irqsave(&chip->irq_lock, flags);
        // enable gpio interrupts here
	spin_unlock_irqrestore(&chip->irq_lock, flags);
}

static int bcm2708_irq_type(unsigned irq, unsigned trigger)
{
	struct bcm2708_gpio *chip = get_irq_chip_data(irq);
	int offset = irq - chip->irq_base;
	unsigned long flags;
        unsigned gpio_bank = offset/32;
	unsigned gpio_field_offset = (offset - 32*gpio_bank);
	unsigned gpioren, gpiofen, gpiohen, gpiolen;

	if (offset < 0 || offset >= ARCH_NR_GPIOS)
		return -EINVAL;

	spin_lock_irqsave(&chip->irq_lock, flags);

	gpioren = readl(chip->base + GPIOREN(gpio_bank));
	gpiofen = readl(chip->base + GPIOFEN(gpio_bank));
	gpiohen = readl(chip->base + GPIOHEN(gpio_bank));
	gpiolen = readl(chip->base + GPIOLEN(gpio_bank));

	if (trigger & (IRQ_TYPE_EDGE_RISING))
		gpioren |=  (1<<gpio_field_offset);
	else
		gpioren &= ~(1<<gpio_field_offset);
	if (trigger & (IRQ_TYPE_EDGE_FALLING))
		gpiofen |=  (1<<gpio_field_offset);
	else
		gpiofen &= ~(1<<gpio_field_offset);
	if (trigger & (IRQ_TYPE_LEVEL_HIGH))
		gpiohen |=  (1<<gpio_field_offset);
	else
		gpiohen &= ~(1<<gpio_field_offset);
	if (trigger & (IRQ_TYPE_LEVEL_LOW))
		gpiolen |=  (1<<gpio_field_offset);
	else
		gpiolen &= ~(1<<gpio_field_offset);

	writel(gpioren, chip->base + GPIOREN(gpio_bank));
	writel(gpiofen, chip->base + GPIOFEN(gpio_bank));
	writel(gpiohen, chip->base + GPIOHEN(gpio_bank));
	writel(gpiolen, chip->base + GPIOLEN(gpio_bank));

	spin_unlock_irqrestore(&chip->irq_lock, flags);

	return 0;
}

static struct irq_chip bcm2708_irqchip = {
	.name		= "GPIO",
	.enable		= bcm2708_irq_enable,
	.disable	= bcm2708_irq_disable,
	.set_type	= bcm2708_irq_type,
};

static void bcm2708_irq_handler(unsigned irq, struct irq_desc *desc)
{
	struct list_head *chip_list = get_irq_data(irq);
	struct list_head *ptr;
	struct bcm2708_gpio *chip;
	unsigned gpio_bank;

	desc->chip->ack(irq);
	list_for_each(ptr, chip_list) {
		unsigned long pending;
		int offset;

		chip = list_entry(ptr, struct bcm2708_gpio, list);
		for (gpio_bank = 0; gpio_bank < ARCH_NR_GPIOS/32; gpio_bank++) {
			pending = readl(chip->base + GPIOEDS(gpio_bank));
			writel(pending, chip->base + GPIOEDS(gpio_bank));

			if (pending == 0)
				continue;

			for_each_set_bit(offset, &pending, ARCH_NR_GPIOS)
				generic_handle_irq(gpio_to_irq(offset+32*gpio_bank));
		}
	}
	desc->chip->unmask(irq);
}
#endif /* #if BCM_GPIO_USE_IRQ */

static int bcm2708_gpio_probe(struct platform_device *dev)
{
	struct bcm2708_gpio *ucb;
	struct resource *res;
	int err = 0;

        printk(KERN_ERR DRIVER_NAME ": bcm2708_gpio_probe %p\n", dev);

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
	ucb->gc.ngpio = ARCH_NR_GPIOS;
	ucb->gc.owner = THIS_MODULE;

	ucb->gc.direction_input = bcm2708_gpio_dir_in;
	ucb->gc.direction_output = bcm2708_gpio_dir_out;
	ucb->gc.get = bcm2708_gpio_get;
	ucb->gc.set = bcm2708_gpio_set;
	ucb->gc.can_sleep = 0;

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
	.probe	= bcm2708_gpio_probe,
	.remove	= bcm2708_gpio_remove,
	.driver	= {
		.name	= "bcm2708_gpio"
	},
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

