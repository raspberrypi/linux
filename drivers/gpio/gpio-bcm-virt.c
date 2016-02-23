/*
 *  brcmvirt GPIO driver
 *
 *  Copyright (C) 2012,2013 Dom Cobley <popcornmix@gmail.com>
 *  Based on gpio-clps711x.c by Alexander Shiyan <shc_work@mail.ru>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define MODULE_NAME "brcmvirt-gpio"
#define NUM_GPIO 2

struct brcmvirt_gpio {
	struct gpio_chip	gc;
	u32 __iomem		*ts_base;
	/* two packed 16-bit counts of enabled and disables
           Allows host to detect a brief enable that was missed */
	u32			enables_disables[NUM_GPIO];
};

static int brcmvirt_gpio_dir_in(struct gpio_chip *gc, unsigned off)
{
	struct brcmvirt_gpio *gpio;
	gpio = container_of(gc, struct brcmvirt_gpio, gc);
	return -EINVAL;
}

static int brcmvirt_gpio_dir_out(struct gpio_chip *gc, unsigned off, int val)
{
	struct brcmvirt_gpio *gpio;
	gpio = container_of(gc, struct brcmvirt_gpio, gc);
	return 0;
}

static int brcmvirt_gpio_get(struct gpio_chip *gc, unsigned off)
{
	struct brcmvirt_gpio *gpio;
	unsigned v;
	gpio = container_of(gc, struct brcmvirt_gpio, gc);
	v = readl(gpio->ts_base + off);
	return (v >> off) & 1;
}

static void brcmvirt_gpio_set(struct gpio_chip *gc, unsigned off, int val)
{
	struct brcmvirt_gpio *gpio;
	u16 enables, disables;
	s16 diff;
	bool lit;
	gpio = container_of(gc, struct brcmvirt_gpio, gc);
	enables  = gpio->enables_disables[off] >> 16;
	disables = gpio->enables_disables[off] >>  0;
	diff = (s16)(enables - disables);
	lit = diff > 0;
	if ((val && lit) || (!val && !lit))
		return;
	if (val)
		enables++;
	else
		disables++;
	diff = (s16)(enables - disables);
	BUG_ON(diff != 0 && diff != 1);
	gpio->enables_disables[off] = (enables << 16) | (disables << 0);
	writel(gpio->enables_disables[off], gpio->ts_base + off);
}

static int brcmvirt_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *fw_node;
	struct rpi_firmware *fw;
	struct brcmvirt_gpio *ucb;
	u32 gpiovirtbuf;
	int err = 0;

	fw_node = of_parse_phandle(np, "firmware", 0);
	if (!fw_node) {
		dev_err(dev, "Missing firmware node\n");
		return -ENOENT;
	}

	fw = rpi_firmware_get(fw_node);
	if (!fw)
		return -EPROBE_DEFER;

	err = rpi_firmware_property(fw, RPI_FIRMWARE_FRAMEBUFFER_GET_GPIOVIRTBUF,
				    &gpiovirtbuf, sizeof(gpiovirtbuf));

	if (err) {
		dev_err(dev, "Failed to get gpiovirtbuf\n");
		goto err;
	}

	if (!gpiovirtbuf) {
		dev_err(dev, "No virtgpio buffer\n");
		err = -ENOENT;
		goto err;
	}

	ucb = devm_kzalloc(dev, sizeof *ucb, GFP_KERNEL);
	if (!ucb) {
		err = -EINVAL;
		goto err;
	}

	// mmap the physical memory
	gpiovirtbuf &= ~0xc0000000;
	ucb->ts_base = ioremap(gpiovirtbuf, 4096);
	if (ucb->ts_base == NULL) {
		dev_err(dev, "Failed to map physical address\n");
		err = -ENOENT;
		goto err;
	}

	ucb->gc.label = MODULE_NAME;
	ucb->gc.owner = THIS_MODULE;
	//ucb->gc.dev = dev;
	ucb->gc.of_node = np;
	ucb->gc.base = 100;
	ucb->gc.ngpio = NUM_GPIO;

	ucb->gc.direction_input = brcmvirt_gpio_dir_in;
	ucb->gc.direction_output = brcmvirt_gpio_dir_out;
	ucb->gc.get = brcmvirt_gpio_get;
	ucb->gc.set = brcmvirt_gpio_set;
	ucb->gc.can_sleep = true;

	err = gpiochip_add(&ucb->gc);
	if (err)
		goto err;

	platform_set_drvdata(pdev, ucb);

err:
	return err;

}

static int brcmvirt_gpio_remove(struct platform_device *pdev)
{
	int err = 0;
	struct brcmvirt_gpio *ucb = platform_get_drvdata(pdev);

	gpiochip_remove(&ucb->gc);
	iounmap(ucb->ts_base);
	return err;
}

static const struct of_device_id __maybe_unused brcmvirt_gpio_ids[] = {
	{ .compatible = "brcm,bcm2835-virtgpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, brcmvirt_gpio_ids);

static struct platform_driver brcmvirt_gpio_driver = {
	.driver	= {
		.name		= MODULE_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= of_match_ptr(brcmvirt_gpio_ids),
	},
	.probe	= brcmvirt_gpio_probe,
	.remove	= brcmvirt_gpio_remove,
};
module_platform_driver(brcmvirt_gpio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dom Cobley <popcornmix@gmail.com>");
MODULE_DESCRIPTION("brcmvirt GPIO driver");
MODULE_ALIAS("platform:brcmvirt-gpio");
