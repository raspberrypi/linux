// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Raspberry Pi Ltd.
 *
 * Based on rpi-panel-attiny-regulator.c by Marek Vasut <marex@denx.de>
 */

#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

/* I2C registers of the microcontroller. */
#define REG_ID		0x01
#define REG_POWERON	0x02
#define REG_PWM		0x03

// bits for poweron register
#define LCD_RESET_BIT	BIT(0)
#define CTP_RESET_BIT	BIT(1)

//bits for the PWM register
#define PWM_BL_ENABLE	BIT(7)
#define PWM_VALUE	GENMASK(4, 0)

#define NUM_GPIO	2	/* Treat LCD_RESET and CTP_RESET as GPIOs */

struct rpi_panel_v2_lcd {
	/* lock to serialise overall accesses to the Atmel */
	struct mutex	lock;
	struct regmap	*regmap;
	u8 poweron_state;

	struct gpio_chip gc;
};

static const struct regmap_config rpi_panel_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_PWM,
};

static int rpi_panel_v2_gpio_get_direction(struct gpio_chip *gc, unsigned int off)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static void rpi_panel_v2_gpio_set(struct gpio_chip *gc, unsigned int off, int val)
{
	struct rpi_panel_v2_lcd *state = gpiochip_get_data(gc);
	u8 last_val;

	if (off >= NUM_GPIO)
		return;

	mutex_lock(&state->lock);

	last_val = state->poweron_state;
	if (val)
		last_val |= (1 << off);
	else
		last_val &= ~(1 << off);

	state->poweron_state = last_val;

	regmap_write(state->regmap, REG_POWERON, last_val);

	mutex_unlock(&state->lock);
}

static int rpi_panel_v2_update_status(struct backlight_device *bl)
{
	struct regmap *regmap = bl_get_data(bl);
	int brightness = bl->props.brightness;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK)
		brightness = 0;

	return regmap_write(regmap, REG_PWM, brightness | PWM_BL_ENABLE);
}

static const struct backlight_ops rpi_panel_v2_bl = {
	.update_status	= rpi_panel_v2_update_status,
};

/*
 * I2C driver interface functions
 */
static int rpi_panel_v2_i2c_probe(struct i2c_client *i2c)
{
	struct backlight_properties props = { };
	struct backlight_device *bl;
	struct rpi_panel_v2_lcd *state;
	struct regmap *regmap;
	unsigned int data;
	int ret;

	state = devm_kzalloc(&i2c->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	mutex_init(&state->lock);
	i2c_set_clientdata(i2c, state);

	regmap = devm_regmap_init_i2c(i2c, &rpi_panel_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		goto error;
	}

	ret = regmap_read(regmap, REG_ID, &data);
	if (ret < 0) {
		dev_err(&i2c->dev, "Failed to read REG_ID reg: %d\n", ret);
		goto error;
	}

	switch (data & 0x0f) {
	case 0x01: /* 7 inch */
	case 0x04: /* 7 inch - old */
	case 0x08: /* 5 inch - old */
	case 0x09: /* 5 inch */
		break;
	default:
		dev_err(&i2c->dev, "Unknown revision: 0x%02x\n",
			data & 0x0f);
		ret = -ENODEV;
		goto error;
	}

	regmap_write(regmap, REG_POWERON, 0);

	state->regmap = regmap;
	state->gc.parent = &i2c->dev;
	state->gc.label = i2c->name;
	state->gc.owner = THIS_MODULE;
	state->gc.base = -1;
	state->gc.ngpio = NUM_GPIO;

	state->gc.set = rpi_panel_v2_gpio_set;
	state->gc.get_direction = rpi_panel_v2_gpio_get_direction;
	state->gc.can_sleep = true;

	ret = devm_gpiochip_add_data(&i2c->dev, &state->gc, state);
	if (ret) {
		dev_err(&i2c->dev, "Failed to create gpiochip: %d\n", ret);
		goto error;
	}

	props.type = BACKLIGHT_RAW;
	props.max_brightness = PWM_VALUE;
	bl = devm_backlight_device_register(&i2c->dev, dev_name(&i2c->dev),
					    &i2c->dev, regmap, &rpi_panel_v2_bl,
					    &props);
	if (IS_ERR(bl))
		return PTR_ERR(bl);

	bl->props.brightness = PWM_VALUE;

	return 0;

error:
	mutex_destroy(&state->lock);
	return ret;
}

static void rpi_panel_v2_i2c_remove(struct i2c_client *client)
{
	struct rpi_panel_v2_lcd *state = i2c_get_clientdata(client);

	mutex_destroy(&state->lock);
}

static void rpi_panel_v2_i2c_shutdown(struct i2c_client *client)
{
	struct rpi_panel_v2_lcd *state = i2c_get_clientdata(client);

	regmap_write(state->regmap, REG_PWM, 0);
	regmap_write(state->regmap, REG_POWERON, 0);
}

static const struct of_device_id rpi_panel_v2_dt_ids[] = {
	{ .compatible = "raspberrypi,v2-touchscreen-panel-regulator" },
	{},
};
MODULE_DEVICE_TABLE(of, rpi_panel_v2_dt_ids);

static struct i2c_driver rpi_panel_v2_regulator_driver = {
	.driver = {
		.name = "rpi_touchscreen_v2",
		.of_match_table = of_match_ptr(rpi_panel_v2_dt_ids),
	},
	.probe = rpi_panel_v2_i2c_probe,
	.remove	= rpi_panel_v2_i2c_remove,
	.shutdown = rpi_panel_v2_i2c_shutdown,
};

module_i2c_driver(rpi_panel_v2_regulator_driver);

MODULE_AUTHOR("Dave Stevenson <dave.stevenson@raspberrypi.com>");
MODULE_DESCRIPTION("Regulator device driver for Raspberry Pi 7-inch V2 touchscreen");
MODULE_LICENSE("GPL");
