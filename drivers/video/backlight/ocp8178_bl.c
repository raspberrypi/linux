/*
 * ocp8178_bl.c - ocp8178 backlight driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/gpio.h> /* Only for legacy support */
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_data/gpio_backlight.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/clk.h>

struct ocp8178_backlight {
	struct device *dev;
	struct device *fbdev;

	struct gpio_desc *gpiod;
	int def_value;
	int current_value;
};

#define DETECT_DELAY 200
#define DETECT_TIME 500
#define DETECT_WINDOW_TIME 1000
#define START_TIME 10
#define END_TIME 10
#define SHUTDOWN_TIME 3000
#define LOW_BIT_HIGH_TIME 10
#define LOW_BIT_LOW_TIME 50
#define HIGH_BIT_HIGH_TIME 50
#define HIGH_BIT_LOW_TIME 10
#define MAX_BRIGHTNESS_VALUE 9

static void entry_1wire_mode(struct ocp8178_backlight *gbl)
{
	unsigned long flags = 0;
	local_irq_save(flags);
	gpiod_set_value(gbl->gpiod, 0);
	mdelay(SHUTDOWN_TIME/1000);
	gpiod_set_value(gbl->gpiod, 1);
	udelay(DETECT_DELAY);
	gpiod_set_value(gbl->gpiod, 0);
	udelay(DETECT_TIME);
	gpiod_set_value(gbl->gpiod, 1);
	udelay(DETECT_WINDOW_TIME);
	local_irq_restore(flags);
}

static inline void write_bit(struct ocp8178_backlight *gbl, int bit)
{
	if (bit) {
		gpiod_set_value(gbl->gpiod, 0);
		udelay(HIGH_BIT_LOW_TIME);
		gpiod_set_value(gbl->gpiod, 1);
		udelay(HIGH_BIT_HIGH_TIME);
	} else {
		gpiod_set_value(gbl->gpiod, 0);
		udelay(LOW_BIT_LOW_TIME);
		gpiod_set_value(gbl->gpiod, 1);
		udelay(LOW_BIT_HIGH_TIME);
	}
}

static void write_byte(struct ocp8178_backlight *gbl, int byte)
{
	unsigned long flags = 0;
	unsigned char data = 0x72;
	int i;

	local_irq_save(flags);

	gpiod_set_value(gbl->gpiod, 1);
	udelay(START_TIME);
	for(i = 0; i < 8; i++) {
		if(data & 0x80) {
			write_bit(gbl, 1);
		} else {
			write_bit(gbl, 0);
		}
		data <<= 1;
	}
	gpiod_set_value(gbl->gpiod, 0);
	udelay(END_TIME);

	data = byte & 0x1f;

	gpiod_set_value(gbl->gpiod, 1);
	udelay(START_TIME);
	for(i = 0; i < 8; i++) {
		if(data & 0x80) {
			write_bit(gbl, 1);
		} else {
			write_bit(gbl, 0);
		}
		data <<= 1;
	}
	gpiod_set_value(gbl->gpiod, 0);
	udelay(END_TIME);
	gpiod_set_value(gbl->gpiod, 1);

	local_irq_restore(flags);
}

unsigned char ocp8178_bl_table[MAX_BRIGHTNESS_VALUE+1] = {0, 1, 4, 8, 12, 16, 20, 24, 28, 31};

static int ocp8178_update_status(struct backlight_device *bl)
{
	struct ocp8178_backlight *gbl = bl_get_data(bl);
	int brightness = bl->props.brightness, i;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	if(brightness > MAX_BRIGHTNESS_VALUE)
		brightness = MAX_BRIGHTNESS_VALUE;

	for(i = 0; i < 2; i++) {
		entry_1wire_mode(gbl);
		write_byte(gbl, ocp8178_bl_table[brightness]);
	}
	gbl->current_value = brightness;

	return 0;
}

static int ocp8178_get_brightness(struct backlight_device *bl)
{
	struct ocp8178_backlight *gbl = bl_get_data(bl);
	return gbl->current_value;
}

static int ocp8178_check_fb(struct backlight_device *bl,
				   struct fb_info *info)
{
	struct ocp8178_backlight *gbl = bl_get_data(bl);
	return gbl->fbdev == NULL || gbl->fbdev == info->dev;
}

static const struct backlight_ops ocp8178_backlight_ops = {
	.options	= BL_CORE_SUSPENDRESUME,
	.update_status	= ocp8178_update_status,
	.get_brightness = ocp8178_get_brightness,
	.check_fb	= ocp8178_check_fb,
};

static int ocp8178_probe_dt(struct platform_device *pdev,
				   struct ocp8178_backlight *gbl)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	enum gpiod_flags flags;
	int ret = 0;
	u32 value32;

	of_property_read_u32(np, "default-brightness", &value32);
	if(value32 > MAX_BRIGHTNESS_VALUE)
		gbl->def_value = MAX_BRIGHTNESS_VALUE;
	else
		gbl->def_value = value32;
	flags = gbl->def_value ? GPIOD_OUT_HIGH : GPIOD_OUT_LOW;

	gbl->gpiod = devm_gpiod_get(dev, "backlight-control", flags);
	if (IS_ERR(gbl->gpiod)) {
		ret = PTR_ERR(gbl->gpiod);

		if (ret != -EPROBE_DEFER) {
			dev_err(dev,
				"Error: The gpios parameter is missing or invalid.\n");
		}
	}

	return ret;
}

static struct backlight_device *backlight;

static int ocp8178_probe(struct platform_device *pdev)
{
	struct backlight_properties props;
	struct backlight_device *bl;
	struct ocp8178_backlight *gbl;
	struct device_node *np = pdev->dev.of_node;
	int ret;

	if ( !np) {
		dev_err(&pdev->dev,
			"failed to find platform data or device tree node.\n");
		return -ENODEV;
	}

	gbl = devm_kzalloc(&pdev->dev, sizeof(*gbl), GFP_KERNEL);
	if (gbl == NULL)
		return -ENOMEM;

	gbl->dev = &pdev->dev;

	ret = ocp8178_probe_dt(pdev, gbl);
	if (ret)
		return ret;

	gbl->current_value = gbl->def_value;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = MAX_BRIGHTNESS_VALUE;
	bl = devm_backlight_device_register(&pdev->dev, dev_name(&pdev->dev),
					&pdev->dev, gbl, &ocp8178_backlight_ops,
					&props);
	if (IS_ERR(bl)) {
		dev_err(&pdev->dev, "failed to register backlight\n");
		return PTR_ERR(bl);
	}

//	entry_1wire_mode(gbl);

	bl->props.brightness = gbl->def_value;
	backlight_update_status(bl);

	platform_set_drvdata(pdev, bl);

	backlight = bl;
	return 0;
}

static int ocp8178_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int ocp8178_resume(struct platform_device *pdev)
{
	return 0;
}

static struct of_device_id ocp8178_of_match[] = {
	{ .compatible = "ocp8178-backlight" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, ocp8178_of_match);

static struct platform_driver ocp8178_driver = {
	.driver		= {
		.name		= "ocp8178-backlight",
		.of_match_table = of_match_ptr(ocp8178_of_match),
	},
	.probe		= ocp8178_probe,
	.suspend		= ocp8178_suspend,
	.resume		= ocp8178_resume,
};

module_platform_driver(ocp8178_driver);

MODULE_DESCRIPTION("OCP8178 Driver");
MODULE_LICENSE("GPL");
