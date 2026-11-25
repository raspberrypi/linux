// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 VIEWE
 *
 * I2C backlight for VIEWE 7KF82 DSI modules (MCU @0x45, register 0x86, 0..255).
 */

#include <linux/backlight.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/property.h>

#define VIEWE_BL_REG		0x86
#define VIEWE_BL_MAX		255
#define VIEWE_BL_DEFAULT	200

static int viewe_bl_update_status(struct backlight_device *bd)
{
	struct i2c_client *client = bl_get_data(bd);

	return i2c_smbus_write_byte_data(client, VIEWE_BL_REG,
					 backlight_get_brightness(bd));
}

static const struct backlight_ops viewe_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = viewe_bl_update_status,
};

static int viewe_bl_probe(struct i2c_client *client)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = VIEWE_BL_MAX,
	};
	struct backlight_device *bd;
	u32 brightness = VIEWE_BL_DEFAULT;
	int error;

	device_property_read_u32(&client->dev, "default-brightness", &brightness);
	if (brightness > VIEWE_BL_MAX)
		brightness = VIEWE_BL_MAX;
	props.brightness = brightness;

	bd = devm_backlight_device_register(&client->dev, dev_name(&client->dev),
					   &client->dev, client, &viewe_bl_ops,
					   &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);

	error = backlight_update_status(bd);
	if (error)
		return error;

	return 0;
}

static const struct of_device_id viewe_bl_of_match[] = {
	{ .compatible = "viewe,7kf82-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, viewe_bl_of_match);

static const struct i2c_device_id viewe_bl_id[] = {
	{ .name = "7kf82-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, viewe_bl_id);

static struct i2c_driver viewe_bl_driver = {
	.driver = {
		.name = "viewe_bl_i2c",
		.of_match_table = viewe_bl_of_match,
	},
	.probe = viewe_bl_probe,
	.id_table = viewe_bl_id,
};
module_i2c_driver(viewe_bl_driver);

MODULE_AUTHOR("Hefei <3066883572@qq.com>");
MODULE_DESCRIPTION("VIEWE 7KF82 I2C backlight");
MODULE_LICENSE("GPL");
