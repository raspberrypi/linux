/*
 * Clock Driver for HiFiBerry DAC Pro
 *
 * Author: Stuart MacLean
 *         Copyright 2015
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
#include <linux/clkdev.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/platform_device.h>

/* Clock rate of CLK44EN attached to GPIO6 pin */
#define CLK_44EN_RATE 22579200UL
/* Clock rate of CLK48EN attached to GPIO3 pin */
#define CLK_48EN_RATE 24576000UL

/**
 * struct hifiberry_dacpro_clk - Common struct to the HiFiBerry DAC Pro
 * @hw: clk_hw for the common clk framework
 * @mode: 0 => CLK44EN, 1 => CLK48EN
 */
struct clk_hifiberry_hw {
	struct clk_hw hw;
	uint8_t mode;
};

#define to_hifiberry_clk(_hw) container_of(_hw, struct clk_hifiberry_hw, hw)

static const struct of_device_id clk_hifiberry_dacpro_dt_ids[] = {
	{ .compatible = "hifiberry,dacpro-clk",},
	{ }
};
MODULE_DEVICE_TABLE(of, clk_hifiberry_dacpro_dt_ids);

static unsigned long clk_hifiberry_dacpro_recalc_rate(struct clk_hw *hw,
	unsigned long parent_rate)
{
	return (to_hifiberry_clk(hw)->mode == 0) ? CLK_44EN_RATE :
		CLK_48EN_RATE;
}

static long clk_hifiberry_dacpro_round_rate(struct clk_hw *hw,
	unsigned long rate, unsigned long *parent_rate)
{
	long actual_rate;

	if (rate <= CLK_44EN_RATE) {
		actual_rate = (long)CLK_44EN_RATE;
	} else if (rate >= CLK_48EN_RATE) {
		actual_rate = (long)CLK_48EN_RATE;
	} else {
		long diff44Rate = (long)(rate - CLK_44EN_RATE);
		long diff48Rate = (long)(CLK_48EN_RATE - rate);

		if (diff44Rate < diff48Rate)
			actual_rate = (long)CLK_44EN_RATE;
		else
			actual_rate = (long)CLK_48EN_RATE;
	}
	return actual_rate;
}


static int clk_hifiberry_dacpro_set_rate(struct clk_hw *hw,
	unsigned long rate, unsigned long parent_rate)
{
	unsigned long actual_rate;
	struct clk_hifiberry_hw *clk = to_hifiberry_clk(hw);

	actual_rate = (unsigned long)clk_hifiberry_dacpro_round_rate(hw, rate,
		&parent_rate);
	clk->mode = (actual_rate == CLK_44EN_RATE) ? 0 : 1;
	return 0;
}


const struct clk_ops clk_hifiberry_dacpro_rate_ops = {
	.recalc_rate = clk_hifiberry_dacpro_recalc_rate,
	.round_rate = clk_hifiberry_dacpro_round_rate,
	.set_rate = clk_hifiberry_dacpro_set_rate,
};

static int clk_hifiberry_dacpro_probe(struct platform_device *pdev)
{
	int ret;
	struct clk_hifiberry_hw *proclk;
	struct clk *clk;
	struct device *dev;
	struct clk_init_data init;

	dev = &pdev->dev;

	proclk = kzalloc(sizeof(struct clk_hifiberry_hw), GFP_KERNEL);
	if (!proclk)
		return -ENOMEM;

	init.name = "clk-hifiberry-dacpro";
	init.ops = &clk_hifiberry_dacpro_rate_ops;
	init.flags = CLK_IS_BASIC;
	init.parent_names = NULL;
	init.num_parents = 0;

	proclk->mode = 0;
	proclk->hw.init = &init;

	clk = devm_clk_register(dev, &proclk->hw);
	if (!IS_ERR(clk)) {
		ret = of_clk_add_provider(dev->of_node, of_clk_src_simple_get,
			clk);
	} else {
		dev_err(dev, "Fail to register clock driver\n");
		kfree(proclk);
		ret = PTR_ERR(clk);
	}
	return ret;
}

static int clk_hifiberry_dacpro_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);
	return 0;
}

static struct platform_driver clk_hifiberry_dacpro_driver = {
	.probe = clk_hifiberry_dacpro_probe,
	.remove = clk_hifiberry_dacpro_remove,
	.driver = {
		.name = "clk-hifiberry-dacpro",
		.of_match_table = clk_hifiberry_dacpro_dt_ids,
	},
};

static int __init clk_hifiberry_dacpro_init(void)
{
	return platform_driver_register(&clk_hifiberry_dacpro_driver);
}
core_initcall(clk_hifiberry_dacpro_init);

static void __exit clk_hifiberry_dacpro_exit(void)
{
	platform_driver_unregister(&clk_hifiberry_dacpro_driver);
}
module_exit(clk_hifiberry_dacpro_exit);

MODULE_DESCRIPTION("HiFiBerry DAC Pro clock driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:clk-hifiberry-dacpro");
