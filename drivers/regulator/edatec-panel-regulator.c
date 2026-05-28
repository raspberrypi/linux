// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Edatec
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

#define REG_PWM		0x01
#define REG_IODIR	0x02
#define REG_PWR		0x03
#define REG_OUTPUT	0x0A

#define CMD_BRIDGE_INIT		0x11
#define CMD_BACKLIGHT_EN 	0x12
#define CMD_FW_VERSION		0xE1

#define PIN_LCD_BL_EN	BIT(0)
#define PIN_LCD_BL_PWM	BIT(1)
#define PIN_LCD_RST	BIT(2)
#define PIN_TP_RST	BIT(3)
#define PIN_LCD_VDD_EN	BIT(4)

#define NUM_GPIO	5

struct ed_lcd {
	struct regmap	*regmap;
};

static bool ed_readable_reg(struct device *dev, unsigned int reg)
{
	/* No registers are readable via the regmap. Use cached values */
	return false;
}

static const struct regmap_config ed_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.disable_locking = 1,
	.max_register = REG_OUTPUT,
	.cache_type = REGCACHE_RBTREE,
	.readable_reg = ed_readable_reg,
};

static int ed_update_status(struct backlight_device *bl)
{
	struct ed_lcd *state = bl_get_data(bl);
	struct regmap *regmap = state->regmap;
	int brightness = backlight_get_brightness(bl);
	int ret;

	ret = regmap_write(regmap, REG_PWR, brightness ? 0 : 4);

	if (!ret)
		ret = regmap_write(regmap, REG_PWM, brightness);

	return ret;
}

static const struct backlight_ops ed_bl = {
	.update_status	= ed_update_status,
};

static int ed_firmware_version(struct i2c_client *i2c)
{
	struct i2c_msg msgs[1];
	u8 addr_buf[1] = { CMD_FW_VERSION };
	u8 data_buf[16] = { 0 };
	int ret;

	/* Write register address */
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
	struct ed_lcd *state;
	struct regmap *regmap;
	int ret;

	state = devm_kzalloc(&i2c->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	
	ed_firmware_version(i2c);
	
	i2c_set_clientdata(i2c, state);

	regmap = devm_regmap_init_i2c(i2c, &ed_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}


	ret = regmap_write(regmap, REG_PWR, 0x00);
	if (!ret)
		ret = regmap_write(regmap, REG_PWM, 0);
	if (!ret)
		ret = regmap_write(regmap, CMD_BRIDGE_INIT, 0x02);
	if (!ret)
		ret = regmap_write(regmap, CMD_BACKLIGHT_EN, 1);

	if (ret) {
		dev_err(&i2c->dev, "Failed to initialise regmap values\n");
		return ret;
	}

	props.type = BACKLIGHT_RAW;
	props.max_brightness = 0xff;
	props.brightness = 0xff;

	state->regmap = regmap;
	bl = devm_backlight_device_register(&i2c->dev, dev_name(&i2c->dev),
					    &i2c->dev, state, &ed_bl, &props);
	if (IS_ERR(bl))
		return PTR_ERR(bl);

	bl->props.brightness = 0xff;

	gconfig.regmap = regmap;
	ret = PTR_ERR_OR_ZERO(devm_gpio_regmap_register(&i2c->dev, &gconfig));
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "Failed to create gpiochip\n");

	return 0;
}

static void ed_i2c_shutdown(struct i2c_client *client)
{
	struct ed_lcd *state = i2c_get_clientdata(client);

	regmap_write(state->regmap, CMD_BACKLIGHT_EN, 0);
	regmap_write(state->regmap, REG_OUTPUT, 0);
}

static const struct of_device_id ed_dt_ids[] = {
	{ .compatible = "edatec,disp-regulator" },
	{},
};
MODULE_DEVICE_TABLE(of, ed_dt_ids);

static struct i2c_driver ed_regulator_driver = {
	.driver = {
		.name = "edatec_disp_101c",
		.of_match_table = of_match_ptr(ed_dt_ids),
	},
	.probe = ed_i2c_probe,
	.shutdown = ed_i2c_shutdown,
};

module_i2c_driver(ed_regulator_driver);

MODULE_AUTHOR("EDATEC <support@edatec.cn>");
MODULE_DESCRIPTION("EDATEC TFT LCD panel regulator driver");
MODULE_LICENSE("GPL v2");
