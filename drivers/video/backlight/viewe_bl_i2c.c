// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 VIEWE
 *
 * I2C backlight for VIEWE DSI panels (MCU @0x45, register 0x86, 0..255).
 */

#include <linux/backlight.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/slab.h>

#define VIEWE_BL_REG		0x86
#define VIEWE_BL_MAX		255
#define VIEWE_BL_DEFAULT	200

struct viewe_bl {
	struct i2c_client *client;
	struct backlight_device *bd;
};

static int viewe_bl_update_status(struct backlight_device *bd)
{
	struct viewe_bl *bl = bl_get_data(bd);
	u8 buf[2] = { VIEWE_BL_REG, (u8)backlight_get_brightness(bd) };
	struct i2c_msg msg = {
		.addr = bl->client->addr,
		.flags = 0,
		.len = sizeof(buf),
		.buf = buf,
	};
	int ret;

	ret = i2c_transfer(bl->client->adapter, &msg, 1);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static const struct backlight_ops viewe_bl_ops = {
	.update_status = viewe_bl_update_status,
};

static int viewe_bl_probe(struct i2c_client *client)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = VIEWE_BL_DEFAULT,
		.max_brightness = VIEWE_BL_MAX,
	};
	struct viewe_bl *bl;

	bl = devm_kzalloc(&client->dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;

	bl->client = client;
	i2c_set_clientdata(client, bl);

	bl->bd = devm_backlight_device_register(&client->dev, "viewe_bl",
						&client->dev, bl, &viewe_bl_ops,
						&props);
	if (IS_ERR(bl->bd))
		return PTR_ERR(bl->bd);

	return 0;
}

static const struct of_device_id viewe_bl_of_match[] = {
	{ .compatible = "viewe,i2c-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, viewe_bl_of_match);

static const struct i2c_device_id viewe_bl_id[] = {
	{ "viewe_bl", 0 },
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
MODULE_DESCRIPTION("VIEWE I2C backlight");
MODULE_LICENSE("GPL");
