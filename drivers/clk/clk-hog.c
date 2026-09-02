// SPDX-License-Identifier: GPL-2.0
/*
 * clk-hog.c - a trivial clock "hog" driver
 *
 * Modelled on the gpio-hog concept in gpiolib-of.c: a device tree node
 * whose sole purpose is to claim, configure (via the standard
 * assigned-clocks/assigned-clock-rates/assigned-clock-parents
 * properties) and enable one or more clocks, and keep them running for
 * as long as the node is bound - e.g. to hold a GPCLK output enabled
 * with no other consumer driver involved.
 */

#include <linux/clk.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

struct clk_hog {
	struct clk_bulk_data *clks;
	int num_clks;
};

static int clk_hog_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hog *hog;
	int ret;

	hog = devm_kzalloc(dev, sizeof(*hog), GFP_KERNEL);
	if (!hog)
		return -ENOMEM;

	ret = devm_clk_bulk_get_all(dev, &hog->clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	if (!ret)
		return dev_err_probe(dev, -EINVAL, "no clocks specified\n");

	hog->num_clks = ret;

	ret = clk_bulk_prepare_enable(hog->num_clks, hog->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	platform_set_drvdata(pdev, hog);

	return 0;
}

static void clk_hog_remove(struct platform_device *pdev)
{
	struct clk_hog *hog = platform_get_drvdata(pdev);

	clk_bulk_disable_unprepare(hog->num_clks, hog->clks);
}

static const struct of_device_id clk_hog_of_match[] = {
	{ .compatible = "clk-hog" },
	{ }
};
MODULE_DEVICE_TABLE(of, clk_hog_of_match);

static struct platform_driver clk_hog_driver = {
	.probe = clk_hog_probe,
	.remove = clk_hog_remove,
	.driver = {
		.name = "clk-hog",
		.of_match_table = clk_hog_of_match,
	},
};
module_platform_driver(clk_hog_driver);

MODULE_DESCRIPTION("Clock hog driver");
MODULE_LICENSE("GPL");
