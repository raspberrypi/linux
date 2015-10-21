/*
 * rpi_bl.c - Backlight controller through VPU
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

struct rpi_backlight {
	struct device *dev;
	struct device *fbdev;
	struct rpi_firmware *fw;
};

static int rpi_backlight_update_status(struct backlight_device *bl)
{
	struct rpi_backlight *gbl = bl_get_data(bl);
	int brightness = bl->props.brightness;
	int ret;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	ret = rpi_firmware_property(gbl->fw,
			RPI_FIRMWARE_FRAMEBUFFER_SET_BACKLIGHT,
			&brightness, sizeof(brightness));
	if (ret) {
		dev_err(gbl->dev, "Failed to set brightness\n");
		return ret;
	}

	if (brightness < 0) {
		dev_err(gbl->dev, "Backlight change failed\n");
		return -EAGAIN;
	}

	return 0;
}

static const struct backlight_ops rpi_backlight_ops = {
	.options	= BL_CORE_SUSPENDRESUME,
	.update_status	= rpi_backlight_update_status,
};

static int rpi_backlight_probe(struct platform_device *pdev)
{
	struct backlight_properties props;
	struct backlight_device *bl;
	struct rpi_backlight *gbl;
	struct device_node *fw_node;

	gbl = devm_kzalloc(&pdev->dev, sizeof(*gbl), GFP_KERNEL);
	if (gbl == NULL)
		return -ENOMEM;

	gbl->dev = &pdev->dev;

	fw_node = of_parse_phandle(pdev->dev.of_node, "firmware", 0);
	if (!fw_node) {
		dev_err(&pdev->dev, "Missing firmware node\n");
		return -ENOENT;
	}

	gbl->fw = rpi_firmware_get(fw_node);
	if (!gbl->fw)
		return -EPROBE_DEFER;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = 255;
	bl = devm_backlight_device_register(&pdev->dev, dev_name(&pdev->dev),
					&pdev->dev, gbl, &rpi_backlight_ops,
					&props);
	if (IS_ERR(bl)) {
		dev_err(&pdev->dev, "failed to register backlight\n");
		return PTR_ERR(bl);
	}

	bl->props.brightness = 255;
	backlight_update_status(bl);

	platform_set_drvdata(pdev, bl);
	return 0;
}

static const struct of_device_id rpi_backlight_of_match[] = {
	{ .compatible = "raspberrypi,rpi-backlight" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rpi_backlight_of_match);

static struct platform_driver rpi_backlight_driver = {
	.driver		= {
		.name		= "rpi-backlight",
		.of_match_table = of_match_ptr(rpi_backlight_of_match),
	},
	.probe		= rpi_backlight_probe,
};

module_platform_driver(rpi_backlight_driver);

MODULE_AUTHOR("Gordon Hollingworth <gordon@raspberrypi.org>");
MODULE_DESCRIPTION("Raspberry Pi mailbox based Backlight Driver");
MODULE_LICENSE("GPL");
