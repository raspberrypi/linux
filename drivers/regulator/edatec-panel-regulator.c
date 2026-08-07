// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 EDATEC
 *
 */

#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/regmap.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>

#define REG_PWM				0x01
#define REG_PWR				0x03
#define REG_OUTPUT			0x0A
#define REG_BRIDGE_INIT		0x11
#define REG_BACKLIGHT_EN	0x12

#define CMD_FW_VERSION		0xE1

enum gpio_signals {
	PIN_LCD_BL_EN,
	PIN_LCD_BL_PWM,
	PIN_LCD_RST,
	PIN_TP_RST,
	PIN_LCD_VDD_EN,
	NUM_GPIO
};

struct ed_regulator {
	struct regmap	*regmap;
	int bl_power_last;
};

static bool ed_readable_reg(struct device *dev, unsigned int reg)
{
	/* No registers are readable via the regmap. Use cached values */
	return false;
}

static const struct regmap_config ed_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_BACKLIGHT_EN,
	.can_sleep = true,
	.cache_type = REGCACHE_RBTREE,
	.readable_reg = ed_readable_reg,
};

static int ed_update_status(struct backlight_device *bl)
{
	struct backlight_properties *props = &(bl->props);
	struct ed_regulator *regulator = bl_get_data(bl);
	struct regmap *regmap = regulator->regmap;
	int brightness = backlight_get_brightness(bl);
	int bl_power = props->power;
	int ret;

	if (bl_power != regulator->bl_power_last)
		ret = regmap_write(regmap, REG_PWR, bl_power);
	else
		ret = regmap_write(regmap, REG_PWM, brightness);

	regulator->bl_power_last = bl_power;

	return ret;
}

static const struct backlight_ops ed_bl = {
	.update_status = ed_update_status,
};

static int ed_firmware_version(struct i2c_client *i2c)
{
	struct i2c_msg msgs[1];
	u8 addr_buf[1] = { CMD_FW_VERSION };
	u8 data_buf[16] = { 0 };
	int ret;

	msgs[0].addr = i2c->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = addr_buf;

	ret = i2c_transfer(i2c->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	usleep_range(200, 300);

	/* Read data from register */
	msgs[0].addr = i2c->addr;
	msgs[0].flags = I2C_M_RD;
	msgs[0].len = 16;
	msgs[0].buf = data_buf;

	ret = i2c_transfer(i2c->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	dev_info(&i2c->dev, "Firmware version: %s\n", data_buf);

	return 0;
}

/*
 * I2C driver interface functions
 */
static int ed_i2c_probe(struct i2c_client *i2c)
{
	struct gpio_regmap_config gconfig = {
		.ngpio		= NUM_GPIO,
		.ngpio_per_reg	= NUM_GPIO,
		.parent		= &i2c->dev,
		.reg_set_base	= REG_OUTPUT,
	};
	struct backlight_properties props = { };
	struct backlight_device *bl;
	struct ed_regulator *regulator;
	struct regmap *regmap;
	int ret;

	regulator = devm_kzalloc(&i2c->dev, sizeof(*regulator), GFP_KERNEL);
	if (!regulator)
		return -ENOMEM;

	ed_firmware_version(i2c);

	i2c_set_clientdata(i2c, regulator);

	regmap = devm_regmap_init_i2c(i2c, &ed_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	props.type = BACKLIGHT_RAW;
	props.max_brightness = 0xff;
	props.brightness = 0xff;

	regulator->bl_power_last = 0;
	regulator->regmap = regmap;
	bl = devm_backlight_device_register(&i2c->dev, dev_name(&i2c->dev),
					    &i2c->dev, regulator, &ed_bl, &props);
	if (IS_ERR(bl))
		return PTR_ERR(bl);

	bl->props.brightness = 0xff;

	ret = regmap_write(regmap, REG_OUTPUT, 0x00);
	if (ret) {
		dev_err(&i2c->dev, "Failed to initialise regmap values: %d\n", ret);
		return ret;
	}

	ret = regmap_write(regmap, REG_BRIDGE_INIT, 0x02);
	if (ret) {
		dev_err(&i2c->dev, "Failed to initialise panel bridge: %d\n", ret);
		return ret;
	}

	ret = regmap_write(regmap, REG_BACKLIGHT_EN, 0x01);
	if (ret) {
		dev_err(&i2c->dev, "Failed to enable backlight: %d\n", ret);
		return ret;
	}

	msleep(100);

	gconfig.regmap = regmap;
	ret = PTR_ERR_OR_ZERO(devm_gpio_regmap_register(&i2c->dev, &gconfig));
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "Failed to create gpiochip\n");

	return 0;
}

static void ed_i2c_shutdown(struct i2c_client *client)
{
	struct ed_regulator *regulator = i2c_get_clientdata(client);

	regmap_write(regulator->regmap, REG_OUTPUT, 0);
	regmap_write(regulator->regmap, REG_BACKLIGHT_EN, 0);
}

static const struct of_device_id ed_dt_ids[] = {
	{ .compatible = "edatec,disp-regulator" },
	{},
};
MODULE_DEVICE_TABLE(of, ed_dt_ids);

static struct i2c_driver ed_regulator_driver = {
	.driver = {
		.name = "edatec_panel_regulator",
		.of_match_table = of_match_ptr(ed_dt_ids),
	},
	.probe = ed_i2c_probe,
	.shutdown = ed_i2c_shutdown,
};
module_i2c_driver(ed_regulator_driver);

MODULE_AUTHOR("EDATEC <support@edatec.cn>");
MODULE_DESCRIPTION("EDATEC TFT LCD panel regulator driver");
MODULE_LICENSE("GPL");
