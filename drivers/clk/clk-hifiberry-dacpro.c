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

struct ext_clk_rates {
	/* Clock rate of CLK44EN attached to GPIO6 pin */
	unsigned long clk_44en;
	/* Clock rate of CLK48EN attached to GPIO3 pin */
	unsigned long clk_48en;
};

/**
 * struct hifiberry_dacpro_clk - Common struct to the HiFiBerry DAC Pro
 * @hw: clk_hw for the common clk framework
 * @mode: 0 => CLK44EN, 1 => CLK48EN
 */
struct clk_hifiberry_hw {
	struct clk_hw hw;
	uint8_t mode;
	struct ext_clk_rates clk_rates;
};

#define to_hifiberry_clk(_hw) container_of(_hw, struct clk_hifiberry_hw, hw)

static const struct ext_clk_rates hifiberry_dacpro_clks = {
	.clk_44en = 22579200UL,
	.clk_48en = 24576000UL,
};

static const struct ext_clk_rates allo_dac_clks = {
	.clk_44en = 45158400UL,
	.clk_48en = 49152000UL,
};

static const struct of_device_id clk_hifiberry_dacpro_dt_ids[] = {
	{ .compatible = "hifiberry,dacpro-clk", &hifiberry_dacpro_clks },
	{ .compatible = "allo,dac-clk", &allo_dac_clks },
	{ }
};
MODULE_DEVICE_TABLE(of, clk_hifiberry_dacpro_dt_ids);

static unsigned long clk_hifiberry_dacpro_recalc_rate(struct clk_hw *hw,
	unsigned long parent_rate)
{
	struct clk_hifiberry_hw *clk = to_hifiberry_clk(hw);
	return (clk->mode == 0) ? clk->clk_rates.clk_44en :
		clk->clk_rates.clk_48en;
}

static long clk_hifiberry_dacpro_round_rate(struct clk_hw *hw,
	unsigned long rate, unsigned long *parent_rate)
{
	struct clk_hifiberry_hw *clk = to_hifiberry_clk(hw);
	long actual_rate;

	if (rate <= clk->clk_rates.clk_44en) {
		actual_rate = (long)clk->clk_rates.clk_44en;
	} else if (rate >= clk->clk_rates.clk_48en) {
		actual_rate = (long)clk->clk_rates.clk_48en;
	} else {
		long diff44Rate = (long)(rate - clk->clk_rates.clk_44en);
		long diff48Rate = (long)(clk->clk_rates.clk_48en - rate);

		if (diff44Rate < diff48Rate)
			actual_rate = (long)clk->clk_rates.clk_44en;
		else
			actual_rate = (long)clk->clk_rates.clk_48en;
	}
	return actual_rate;
}


static int clk_hifiberry_dacpro_set_rate(struct clk_hw *hw,
	unsigned long rate, unsigned long parent_rate)
{
	struct clk_hifiberry_hw *clk = to_hifiberry_clk(hw);
	unsigned long actual_rate;

	actual_rate = (unsigned long)clk_hifiberry_dacpro_round_rate(hw, rate,
		&parent_rate);
	clk->mode = (actual_rate == clk->clk_rates.clk_44en) ? 0 : 1;
	return 0;
}


const struct clk_ops clk_hifiberry_dacpro_rate_ops = {
	.recalc_rate = clk_hifiberry_dacpro_recalc_rate,
	.round_rate = clk_hifiberry_dacpro_round_rate,
	.set_rate = clk_hifiberry_dacpro_set_rate,
};

static int clk_hifiberry_dacpro_probe(struct platform_device *pdev)
{
	const struct of_device_id *of_id;
	struct clk_hifiberry_hw *proclk;
	struct clk *clk;
	struct device *dev;
	struct clk_init_data init;
	int ret;

	dev = &pdev->dev;
	of_id = of_match_node(clk_hifiberry_dacpro_dt_ids, dev->of_node);
	if (!of_id)
		return -EINVAL;

	proclk = kzalloc(sizeof(struct clk_hifiberry_hw), GFP_KERNEL);
	if (!proclk)
		return -ENOMEM;

	init.name = "clk-hifiberry-dacpro";
	init.ops = &clk_hifiberry_dacpro_rate_ops;
	init.flags = 0;
	init.parent_names = NULL;
	init.num_parents = 0;

	proclk->mode = 0;
	proclk->hw.init = &init;
	memcpy(&proclk->clk_rates, of_id->data, sizeof(proclk->clk_rates));

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

static void clk_hifiberry_dacpro_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);
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
