// SPDX-License-Identifier: GPL-2.0
/*
 * I2C backlight for VIEWE DSI panels (MCU @0x45, reg 0x86, 0..255).
 * Brightness is queued; the touch driver flushes the bus.
 */
#include <linux/backlight.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "viewe_dsi_i2c_bus.h"

#define VIEWE_BL_MAX		255
#define VIEWE_BL_DEFAULT	200

static bool bl_debug;
module_param_named(debug, bl_debug, bool, 0644);
MODULE_PARM_DESC(debug, "Log every brightness update");

struct viewe_bl {
	struct i2c_client *client;
	struct backlight_device *bd;
};

static int viewe_bl_update_status(struct backlight_device *bd)
{
	struct viewe_bl *bl = bl_get_data(bd);
	int br = bd->props.brightness;
	u8 val;

	if (br < 0)
		br = 0;
	if (br > VIEWE_BL_MAX)
		br = VIEWE_BL_MAX;
	val = (u8)br;

	if (backlight_is_blank(bd))
		val = 0;

	if (bl_debug)
		dev_info(&bl->client->dev,
			 "brightness req=%d power=%d blank=%d -> MCU 0x86=%u\n",
			 bd->props.brightness, bd->props.power,
			 backlight_is_blank(bd), val);

	viewe_dsi_bl_request(val);
	return 0;
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
	viewe_dsi_bl_request(VIEWE_BL_DEFAULT);
	return 0;
}

static void viewe_bl_remove(struct i2c_client *client)
{
}

static const struct of_device_id viewe_bl_of_match[] = {
	{ .compatible = "viewe,i2c-backlight" },
	{ .compatible = "hkc,i2c-backlight" },
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
	.remove = viewe_bl_remove,
	.id_table = viewe_bl_id,
};
module_i2c_driver(viewe_bl_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VIEWE I2C backlight");
MODULE_SOFTDEP("pre: viewe_dsi_i2c_bus");
