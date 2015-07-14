/*
 * Raspberry Pi Sense HAT core driver
 * http://raspberrypi.org
 *
 * Copyright (C) 2015 Raspberry Pi
 *
 * Author: Serge Schneider
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  This driver is based on wm8350 implementation.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/mfd/rpisense/core.h>
#include <linux/slab.h>

static struct rpisense *rpisense;

static void rpisense_client_dev_register(struct rpisense *rpisense,
					 const char *name,
					 struct platform_device **pdev)
{
	int ret;

	*pdev = platform_device_alloc(name, -1);
	if (*pdev == NULL) {
		dev_err(rpisense->dev, "Failed to allocate %s\n", name);
		return;
	}

	(*pdev)->dev.parent = rpisense->dev;
	platform_set_drvdata(*pdev, rpisense);
	ret = platform_device_add(*pdev);
	if (ret != 0) {
		dev_err(rpisense->dev, "Failed to register %s: %d\n",
			name, ret);
		platform_device_put(*pdev);
		*pdev = NULL;
	}
}

static int rpisense_probe(struct i2c_client *i2c,
			       const struct i2c_device_id *id)
{
	int ret;
	struct rpisense_js *rpisense_js;

	rpisense = devm_kzalloc(&i2c->dev, sizeof(struct rpisense), GFP_KERNEL);
	if (rpisense == NULL)
		return -ENOMEM;

	i2c_set_clientdata(i2c, rpisense);
	rpisense->dev = &i2c->dev;
	rpisense->i2c_client = i2c;

	ret = rpisense_reg_read(rpisense, RPISENSE_WAI);
	if (ret > 0) {
		if (ret != 's')
			return -EINVAL;
	} else {
		return ret;
	}
	ret = rpisense_reg_read(rpisense, RPISENSE_VER);
	if (ret < 0)
		return ret;

	dev_info(rpisense->dev,
		 "Raspberry Pi Sense HAT firmware version %i\n", ret);

	rpisense_js = &rpisense->joystick;
	rpisense_js->keys_desc = devm_gpiod_get(&i2c->dev,
						"keys-int", GPIOD_IN);
	if (IS_ERR(rpisense_js->keys_desc)) {
		dev_warn(&i2c->dev, "Failed to get keys-int descriptor.\n");
		rpisense_js->keys_desc = gpio_to_desc(23);
		if (rpisense_js->keys_desc == NULL) {
			dev_err(&i2c->dev, "GPIO23 fallback failed.\n");
			return PTR_ERR(rpisense_js->keys_desc);
		}
	}
	rpisense_client_dev_register(rpisense, "rpi-sense-js",
				     &(rpisense->joystick.pdev));
	rpisense_client_dev_register(rpisense, "rpi-sense-fb",
				     &(rpisense->framebuffer.pdev));

	return 0;
}

static void rpisense_remove(struct i2c_client *i2c)
{
	struct rpisense *rpisense = i2c_get_clientdata(i2c);

	platform_device_unregister(rpisense->joystick.pdev);
}

struct rpisense *rpisense_get_dev(void)
{
	return rpisense;
}
EXPORT_SYMBOL_GPL(rpisense_get_dev);

s32 rpisense_reg_read(struct rpisense *rpisense, int reg)
{
	int ret = i2c_smbus_read_byte_data(rpisense->i2c_client, reg);

	if (ret < 0)
		dev_err(rpisense->dev, "Read from reg %d failed\n", reg);
	/* Due to the BCM270x I2C clock stretching bug, some values
	 * may have MSB set. Clear it to avoid incorrect values.
	 * */
	return ret & 0x7F;
}
EXPORT_SYMBOL_GPL(rpisense_reg_read);

int rpisense_block_write(struct rpisense *rpisense, const char *buf, int count)
{
	int ret = i2c_master_send(rpisense->i2c_client, buf, count);

	if (ret < 0)
		dev_err(rpisense->dev, "Block write failed\n");
	return ret;
}
EXPORT_SYMBOL_GPL(rpisense_block_write);

static const struct i2c_device_id rpisense_i2c_id[] = {
	{ "rpi-sense", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rpisense_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id rpisense_core_id[] = {
	{ .compatible = "rpi,rpi-sense" },
	{ },
};
MODULE_DEVICE_TABLE(of, rpisense_core_id);
#endif


static struct i2c_driver rpisense_driver = {
	.driver = {
		   .name = "rpi-sense",
		   .owner = THIS_MODULE,
	},
	.probe = rpisense_probe,
	.remove = rpisense_remove,
	.id_table = rpisense_i2c_id,
};

module_i2c_driver(rpisense_driver);

MODULE_DESCRIPTION("Raspberry Pi Sense HAT core driver");
MODULE_AUTHOR("Serge Schneider <serge@raspberrypi.org>");
MODULE_LICENSE("GPL");

