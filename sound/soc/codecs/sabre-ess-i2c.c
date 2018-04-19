/*
 * Driver for the SABRE ESS CODECs
 *
 * Author: Jaikumar <jaikumar@cem-solutions.net>	
 *		Copyright 2018
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>

#include "sabre-ess.h"

static int sabre_ess_i2c_probe(struct i2c_client *i2c,
			     const struct i2c_device_id *id)
{
	struct regmap *regmap;
	struct regmap_config config = sabre_ess_regmap;

	regmap = devm_regmap_init_i2c(i2c, &config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return sabre_ess_probe(&i2c->dev, regmap);
}

static int sabre_ess_i2c_remove(struct i2c_client *i2c)
{
	sabre_ess_remove(&i2c->dev);
	return 0;
}

static const struct i2c_device_id sabre_ess_i2c_id[] = {
	{ "sabre-ess", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sabre_ess_i2c_id);

static const struct of_device_id sabre_ess_of_match[] = {
	{ .compatible = "saber,sabre-ess", },
	{ }
};
MODULE_DEVICE_TABLE(of, sabre_ess_of_match);

static struct i2c_driver sabre_ess_i2c_driver = {
	.probe 		= sabre_ess_i2c_probe,
	.remove 	= sabre_ess_i2c_remove,
	.id_table	= sabre_ess_i2c_id,
	.driver		= {
		.name	= "sabre-ess",
		.of_match_table = sabre_ess_of_match,
	},
};

module_i2c_driver(sabre_ess_i2c_driver);

MODULE_DESCRIPTION("ASoC SABRE ESS codec driver - I2C");
MODULE_AUTHOR("Jaikumar <jaikumar@cem-solutions.net>");
MODULE_LICENSE("GPL v2");

