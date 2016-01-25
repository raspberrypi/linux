// SPDX-License-Identifier: GPL-2.0
/*
 * Clock Driver for HiFiBerry DAC+ HD
 *
 * Author: Joerg Schambacher, i2Audio GmbH for HiFiBerry
 *         Copyright 2020
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

#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/regmap.h>

#define NO_PLL_RESET			0
#define PLL_RESET			1
#define HIFIBERRY_PLL_MAX_REGISTER	256
#define DEFAULT_RATE			44100

static struct reg_default hifiberry_pll_reg_defaults[] = {
	{0x02, 0x53}, {0x03, 0x00}, {0x07, 0x20}, {0x0F, 0x00},
	{0x10, 0x0D}, {0x11, 0x1D}, {0x12, 0x0D}, {0x13, 0x8C},
	{0x14, 0x8C}, {0x15, 0x8C}, {0x16, 0x8C}, {0x17, 0x8C},
	{0x18, 0x2A}, {0x1C, 0x00}, {0x1D, 0x0F}, {0x1F, 0x00},
	{0x2A, 0x00}, {0x2C, 0x00}, {0x2F, 0x00}, {0x30, 0x00},
	{0x31, 0x00}, {0x32, 0x00}, {0x34, 0x00}, {0x37, 0x00},
	{0x38, 0x00}, {0x39, 0x00}, {0x3A, 0x00}, {0x3B, 0x01},
	{0x3E, 0x00}, {0x3F, 0x00}, {0x40, 0x00}, {0x41, 0x00},
	{0x5A, 0x00}, {0x5B, 0x00}, {0x95, 0x00}, {0x96, 0x00},
	{0x97, 0x00}, {0x98, 0x00}, {0x99, 0x00}, {0x9A, 0x00},
	{0x9B, 0x00}, {0xA2, 0x00}, {0xA3, 0x00}, {0xA4, 0x00},
	{0xB7, 0x92},
	{0x1A, 0x3D}, {0x1B, 0x09}, {0x1E, 0xF3}, {0x20, 0x13},
	{0x21, 0x75}, {0x2B, 0x04}, {0x2D, 0x11}, {0x2E, 0xE0},
	{0x3D, 0x7A},
	{0x35, 0x9D}, {0x36, 0x00}, {0x3C, 0x42},
	{ 177, 0xAC},
};
static struct reg_default common_pll_regs[HIFIBERRY_PLL_MAX_REGISTER];
static int num_common_pll_regs;
static struct reg_default dedicated_192k_pll_regs[HIFIBERRY_PLL_MAX_REGISTER];
static int num_dedicated_192k_pll_regs;
static struct reg_default dedicated_96k_pll_regs[HIFIBERRY_PLL_MAX_REGISTER];
static int num_dedicated_96k_pll_regs;
static struct reg_default dedicated_48k_pll_regs[HIFIBERRY_PLL_MAX_REGISTER];
static int num_dedicated_48k_pll_regs;
static struct reg_default dedicated_176k4_pll_regs[HIFIBERRY_PLL_MAX_REGISTER];
static int num_dedicated_176k4_pll_regs;
static struct reg_default dedicated_88k2_pll_regs[HIFIBERRY_PLL_MAX_REGISTER];
static int num_dedicated_88k2_pll_regs;
static struct reg_default dedicated_44k1_pll_regs[HIFIBERRY_PLL_MAX_REGISTER];
static int num_dedicated_44k1_pll_regs;

/**
 * struct clk_hifiberry_drvdata - Common struct to the HiFiBerry DAC HD Clk
 * @hw: clk_hw for the common clk framework
 */
struct clk_hifiberry_drvdata {
	struct regmap *regmap;
	struct clk *clk;
	struct clk_hw hw;
	unsigned long rate;
};

#define to_hifiberry_clk(_hw) \
	container_of(_hw, struct clk_hifiberry_drvdata, hw)

static int clk_hifiberry_dachd_write_pll_regs(struct regmap *regmap,
				struct reg_default *regs,
				int num, int do_pll_reset)
{
	int i;
	int ret = 0;
	char pll_soft_reset[] = { 177, 0xAC, };

	for (i = 0; i < num; i++) {
		ret |= regmap_write(regmap, regs[i].reg, regs[i].def);
		if (ret)
			return ret;
	}
	if (do_pll_reset) {
		ret |= regmap_write(regmap, pll_soft_reset[0],
						pll_soft_reset[1]);
		mdelay(10);
	}
	return ret;
}

static unsigned long clk_hifiberry_dachd_recalc_rate(struct clk_hw *hw,
	unsigned long parent_rate)
{
	return to_hifiberry_clk(hw)->rate;
}

static long clk_hifiberry_dachd_round_rate(struct clk_hw *hw,
	unsigned long rate, unsigned long *parent_rate)
{
	return rate;
}

static int clk_hifiberry_dachd_set_rate(struct clk_hw *hw,
	unsigned long rate, unsigned long parent_rate)
{
	int ret;
	struct clk_hifiberry_drvdata *drvdata = to_hifiberry_clk(hw);

	switch (rate) {
	case 44100:
		ret = clk_hifiberry_dachd_write_pll_regs(drvdata->regmap,
			dedicated_44k1_pll_regs, num_dedicated_44k1_pll_regs,
			PLL_RESET);
		break;
	case 88200:
		ret = clk_hifiberry_dachd_write_pll_regs(drvdata->regmap,
			dedicated_88k2_pll_regs, num_dedicated_88k2_pll_regs,
			PLL_RESET);
		break;
	case 176400:
		ret = clk_hifiberry_dachd_write_pll_regs(drvdata->regmap,
			dedicated_176k4_pll_regs, num_dedicated_176k4_pll_regs,
			PLL_RESET);
		break;
	case 48000:
		ret = clk_hifiberry_dachd_write_pll_regs(drvdata->regmap,
			dedicated_48k_pll_regs,	num_dedicated_48k_pll_regs,
			PLL_RESET);
		break;
	case 96000:
		ret = clk_hifiberry_dachd_write_pll_regs(drvdata->regmap,
			dedicated_96k_pll_regs,	num_dedicated_96k_pll_regs,
			PLL_RESET);
		break;
	case 192000:
		ret = clk_hifiberry_dachd_write_pll_regs(drvdata->regmap,
			dedicated_192k_pll_regs, num_dedicated_192k_pll_regs,
			PLL_RESET);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	to_hifiberry_clk(hw)->rate = rate;

	return ret;
}

const struct clk_ops clk_hifiberry_dachd_rate_ops = {
	.recalc_rate = clk_hifiberry_dachd_recalc_rate,
	.round_rate = clk_hifiberry_dachd_round_rate,
	.set_rate = clk_hifiberry_dachd_set_rate,
};

static int clk_hifiberry_get_prop_values(struct device *dev,
					char *prop_name,
					struct reg_default *regs)
{
	int ret;
	int i;
	u8 tmp[2 * HIFIBERRY_PLL_MAX_REGISTER];

	ret = of_property_read_variable_u8_array(dev->of_node, prop_name,
			tmp, 0, 2 * HIFIBERRY_PLL_MAX_REGISTER);
	if (ret < 0)
		return ret;
	if (ret & 1) {
		dev_err(dev,
			"%s <%s> -> #%i odd number of bytes for reg/val pairs!",
			__func__,
			prop_name,
			ret);
		return -EINVAL;
	}
	ret /= 2;
	for (i = 0; i < ret; i++) {
		regs[i].reg = (u32)tmp[2 * i];
		regs[i].def = (u32)tmp[2 * i + 1];
	}
	return ret;
}


static int clk_hifiberry_dachd_dt_parse(struct device *dev)
{
	num_common_pll_regs = clk_hifiberry_get_prop_values(dev,
				"common_pll_regs", common_pll_regs);
	num_dedicated_44k1_pll_regs = clk_hifiberry_get_prop_values(dev,
				"44k1_pll_regs", dedicated_44k1_pll_regs);
	num_dedicated_88k2_pll_regs = clk_hifiberry_get_prop_values(dev,
				"88k2_pll_regs", dedicated_88k2_pll_regs);
	num_dedicated_176k4_pll_regs = clk_hifiberry_get_prop_values(dev,
				"176k4_pll_regs", dedicated_176k4_pll_regs);
	num_dedicated_48k_pll_regs = clk_hifiberry_get_prop_values(dev,
				"48k_pll_regs", dedicated_48k_pll_regs);
	num_dedicated_96k_pll_regs = clk_hifiberry_get_prop_values(dev,
				"96k_pll_regs", dedicated_96k_pll_regs);
	num_dedicated_192k_pll_regs = clk_hifiberry_get_prop_values(dev,
				"192k_pll_regs", dedicated_192k_pll_regs);
	return 0;
}


static int clk_hifiberry_dachd_remove(struct device *dev)
{
	of_clk_del_provider(dev->of_node);
	return 0;
}

const struct regmap_config hifiberry_pll_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = HIFIBERRY_PLL_MAX_REGISTER,
	.reg_defaults = hifiberry_pll_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(hifiberry_pll_reg_defaults),
	.cache_type = REGCACHE_RBTREE,
};
EXPORT_SYMBOL_GPL(hifiberry_pll_regmap);


static int clk_hifiberry_dachd_i2c_probe(struct i2c_client *i2c,
			     const struct i2c_device_id *id)
{
	struct clk_hifiberry_drvdata *hdclk;
	int ret = 0;
	struct clk_init_data init;
	struct device *dev = &i2c->dev;
	struct device_node *dev_node = dev->of_node;
	struct regmap_config config = hifiberry_pll_regmap;

	hdclk = devm_kzalloc(&i2c->dev,
			sizeof(struct clk_hifiberry_drvdata), GFP_KERNEL);
	if (!hdclk)
		return -ENOMEM;

	i2c_set_clientdata(i2c, hdclk);

	hdclk->regmap = devm_regmap_init_i2c(i2c, &config);

	if (IS_ERR(hdclk->regmap))
		return PTR_ERR(hdclk->regmap);

	/* start PLL to allow detection of DAC */
	ret = clk_hifiberry_dachd_write_pll_regs(hdclk->regmap,
				hifiberry_pll_reg_defaults,
				ARRAY_SIZE(hifiberry_pll_reg_defaults),
				PLL_RESET);
	if (ret)
		return ret;

	clk_hifiberry_dachd_dt_parse(dev);

	/* restart PLL with configs from DTB */
	ret = clk_hifiberry_dachd_write_pll_regs(hdclk->regmap, common_pll_regs,
					num_common_pll_regs, PLL_RESET);
	if (ret)
		return ret;

	init.name = "clk-hifiberry-dachd";
	init.ops = &clk_hifiberry_dachd_rate_ops;
	init.flags = 0;
	init.parent_names = NULL;
	init.num_parents = 0;

	hdclk->hw.init = &init;

	hdclk->clk = devm_clk_register(dev, &hdclk->hw);
	if (IS_ERR(hdclk->clk)) {
		dev_err(dev, "unable to register %s\n",	init.name);
		return PTR_ERR(hdclk->clk);
	}

	ret = of_clk_add_provider(dev_node, of_clk_src_simple_get, hdclk->clk);
	if (ret != 0) {
		dev_err(dev, "Cannot of_clk_add_provider");
		return ret;
	}

	ret = clk_set_rate(hdclk->hw.clk, DEFAULT_RATE);
	if (ret != 0) {
		dev_err(dev, "Cannot set rate : %d\n",	ret);
		return -EINVAL;
	}

	return ret;
}

static int clk_hifiberry_dachd_i2c_remove(struct i2c_client *i2c)
{
	clk_hifiberry_dachd_remove(&i2c->dev);
	return 0;
}

static const struct i2c_device_id clk_hifiberry_dachd_i2c_id[] = {
	{ "dachd-clk", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, clk_hifiberry_dachd_i2c_id);

static const struct of_device_id clk_hifiberry_dachd_of_match[] = {
	{ .compatible = "hifiberry,dachd-clk", },
	{ }
};
MODULE_DEVICE_TABLE(of, clk_hifiberry_dachd_of_match);

static struct i2c_driver clk_hifiberry_dachd_i2c_driver = {
	.probe		= clk_hifiberry_dachd_i2c_probe,
	.remove		= clk_hifiberry_dachd_i2c_remove,
	.id_table	= clk_hifiberry_dachd_i2c_id,
	.driver		= {
		.name	= "dachd-clk",
		.of_match_table = of_match_ptr(clk_hifiberry_dachd_of_match),
	},
};

module_i2c_driver(clk_hifiberry_dachd_i2c_driver);


MODULE_DESCRIPTION("HiFiBerry DAC+ HD clock driver");
MODULE_AUTHOR("Joerg Schambacher <joerg@i2audio.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:clk-hifiberry-dachd");
