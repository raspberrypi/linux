// SPDX-License-Identifier: GPL-2.0+
/*
 * Raspberry Pi driver for firmware controlled clocks
 *
 * Even though clk-bcm2835 provides an interface to the hardware registers for
 * the system clocks we've had to factor out 'pllb' as the firmware 'owns' it.
 * We're not allowed to change it directly as we might race with the
 * over-temperature and under-voltage protections provided by the firmware.
 *
 * Copyright (C) 2019 Nicolas Saenz Julienne <nsaenzjulienne@suse.de>
 */

#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/bcm2835.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define RPI_FIRMWARE_ARM_CLK_ID		0x00000003
#define RPI_FIRMWARE_V3D_CLK_ID		0x00000005

#define RPI_FIRMWARE_STATE_ENABLE_BIT	BIT(0)
#define RPI_FIRMWARE_STATE_WAIT_BIT	BIT(1)

#define A2W_PLL_FRAC_BITS		20

#define SOC_BCM2835		BIT(0)
#define SOC_BCM2711		BIT(1)
#define SOC_ALL			(SOC_BCM2835 | SOC_BCM2711)

struct raspberrypi_clk {
	struct device *dev;
	struct rpi_firmware *firmware;
	struct platform_device *cpufreq;
};

typedef int (*raspberrypi_clk_register)(struct raspberrypi_clk *rpi,
					      const void *data);


/* assignment helper macros for different clock types */
#define _REGISTER(f, s, ...) { .clk_register = (raspberrypi_clk_register)f, \
			       .supported = s,				\
			       .data = __VA_ARGS__ }
#define REGISTER_PLL(s, ...)	_REGISTER(&raspberrypi_register_pll,	\
					  s,				\
					  &(struct raspberrypi_pll_data)	\
					  {__VA_ARGS__})
#define REGISTER_PLL_DIV(s, ...) _REGISTER(&raspberrypi_register_pll_divider, \
					   s,				  \
					   &(struct raspberrypi_pll_divider_data) \
					   {__VA_ARGS__})
#define REGISTER_CLK(s, ...)	_REGISTER(&raspberrypi_register_clock,	\
					  s,				\
					  &(struct raspberrypi_clock_data)	\
					  {__VA_ARGS__})


struct raspberrypi_pll_data {
	const char *name;
	const char *const *parents;
	int num_parents;
	u32 clock_id;
};

struct raspberrypi_clock_data {
	const char *name;
	const char *const *parents;
	int num_parents;
	u32 flags;
	u32 clock_id;
};

struct raspberrypi_pll_divider_data {
	const char *name;
	const char *divider_name;
	const char *lookup;
	const char *source_pll;

	u32 fixed_divider;
	u32 flags;
	u32 clock_id;
};

struct raspberrypi_clk_desc {
	raspberrypi_clk_register clk_register;
	unsigned int supported;
	const void *data;
};

struct raspberrypi_clock {
	struct clk_hw hw;
	struct raspberrypi_clk *rpi;
	u32 min_rate;
	u32 max_rate;
	const struct raspberrypi_clock_data *data;
};

struct raspberrypi_pll {
	struct clk_hw hw;
	struct raspberrypi_clk *rpi;
	u32 min_rate;
	u32 max_rate;
	const struct raspberrypi_pll_data *data;
};

struct raspberrypi_pll_divider {
	struct clk_divider div;
	struct raspberrypi_clk *rpi;
	u32 min_rate;
	u32 max_rate;
	const struct raspberrypi_pll_divider_data *data;
};

/*
 * Structure of the message passed to Raspberry Pi's firmware in order to
 * change clock rates. The 'disable_turbo' option is only available to the ARM
 * clock (pllb) which we enable by default as turbo mode will alter multiple
 * clocks at once.
 *
 * Even though we're able to access the clock registers directly we're bound to
 * use the firmware interface as the firmware ultimately takes care of
 * mitigating overheating/undervoltage situations and we would be changing
 * frequencies behind his back.
 *
 * For more information on the firmware interface check:
 * https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 */
struct raspberrypi_firmware_prop {
	__le32 id;
	__le32 val;
	__le32 disable_turbo;
} __packed;

static int raspberrypi_clock_property(struct rpi_firmware *firmware, u32 tag,
				      u32 clk, u32 *val)
{
	struct raspberrypi_firmware_prop msg = {
		.id = cpu_to_le32(clk),
		.val = cpu_to_le32(*val),
		.disable_turbo = cpu_to_le32(0),
	};
	int ret;

	ret = rpi_firmware_property(firmware, tag, &msg, sizeof(msg));
	if (ret)
		return ret;

	*val = le32_to_cpu(msg.val);

	return 0;
}

static int raspberrypi_fw_is_on(struct raspberrypi_clk *rpi, u32 clock_id, const char *name)
{
	u32 val = 0;
	int ret;

	ret = raspberrypi_clock_property(rpi->firmware,
					 RPI_FIRMWARE_GET_CLOCK_STATE,
					 clock_id, &val);
	if (ret)
		return 0;

	return !!(val & RPI_FIRMWARE_STATE_ENABLE_BIT);
}

static unsigned long raspberrypi_fw_get_rate(struct raspberrypi_clk *rpi,
						 u32 clock_id, const char *name, unsigned long parent_rate)
{
	u32 val = 0;
	int ret;

	ret = raspberrypi_clock_property(rpi->firmware,
					 RPI_FIRMWARE_GET_CLOCK_RATE,
					 clock_id,
					 &val);
	if (ret)
		dev_err_ratelimited(rpi->dev, "Failed to get %s frequency: %d",
				    name, ret);
	return val;
}

static int raspberrypi_fw_set_rate(struct raspberrypi_clk *rpi,
				   u32 clock_id, const char *name, u32 rate,
				   unsigned long parent_rate)
{
	int ret;

	ret = raspberrypi_clock_property(rpi->firmware,
					 RPI_FIRMWARE_SET_CLOCK_RATE,
					 clock_id,
					 &rate);
	if (ret)
		dev_err_ratelimited(rpi->dev, "Failed to change %s frequency: %d",
				    name, ret);

	return ret;
}

/*
 * Sadly there is no firmware rate rounding interface. We borrowed it from
 * clk-bcm2835.
 */
static int raspberrypi_determine_rate(struct raspberrypi_clk *rpi,
					  u32 clock_id, const char *name, unsigned long min_rate, unsigned long max_rate,
					  struct clk_rate_request *req)
{
#if 1
	req->rate = clamp(req->rate, min_rate, max_rate);
#else
	u64 div, final_rate;
	u32 ndiv, fdiv;

	/* We can't use req->rate directly as it would overflow */
	final_rate = clamp(req->rate, min_rate, max_rate);

	div = (u64)final_rate << A2W_PLL_FRAC_BITS;
	do_div(div, req->best_parent_rate);

	ndiv = div >> A2W_PLL_FRAC_BITS;
	fdiv = div & ((1 << A2W_PLL_FRAC_BITS) - 1);

	final_rate = ((u64)req->best_parent_rate *
					((ndiv << A2W_PLL_FRAC_BITS) + fdiv));

	req->rate = final_rate >> A2W_PLL_FRAC_BITS;

#endif
	return 0;
}

static int raspberrypi_fw_clock_is_on(struct clk_hw *hw)
{
	struct raspberrypi_clock *pll = container_of(hw, struct raspberrypi_clock, hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_clock_data *data = pll->data;

	return raspberrypi_fw_is_on(rpi, data->clock_id, data->name);
}

static unsigned long raspberrypi_fw_clock_get_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct raspberrypi_clock *pll = container_of(hw, struct raspberrypi_clock, hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_clock_data *data = pll->data;

	return raspberrypi_fw_get_rate(rpi, data->clock_id, data->name, parent_rate);
}

static int raspberrypi_fw_clock_set_rate(struct clk_hw *hw, unsigned long rate,
				       unsigned long parent_rate)
{
	struct raspberrypi_clock *pll = container_of(hw, struct raspberrypi_clock, hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_clock_data *data = pll->data;

	return raspberrypi_fw_set_rate(rpi, data->clock_id, data->name, rate, parent_rate);
}

static int raspberrypi_clock_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *req)
{
	struct raspberrypi_clock *pll = container_of(hw, struct raspberrypi_clock, hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_clock_data *data = pll->data;

	return raspberrypi_determine_rate(rpi, data->clock_id, data->name, pll->min_rate, pll->max_rate, req);
}

static int raspberrypi_fw_pll_is_on(struct clk_hw *hw)
{
	struct raspberrypi_pll *pll = container_of(hw, struct raspberrypi_pll, hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_pll_data *data = pll->data;

	return raspberrypi_fw_is_on(rpi, data->clock_id, data->name);
}

static unsigned long raspberrypi_fw_pll_get_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct raspberrypi_pll *pll = container_of(hw, struct raspberrypi_pll, hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_pll_data *data = pll->data;

	return raspberrypi_fw_get_rate(rpi, data->clock_id, data->name, parent_rate);
}

static int raspberrypi_fw_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				       unsigned long parent_rate)
{
	struct raspberrypi_pll *pll = container_of(hw, struct raspberrypi_pll, hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_pll_data *data = pll->data;

	return raspberrypi_fw_set_rate(rpi, data->clock_id, data->name, rate, parent_rate);
}

static int raspberrypi_pll_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *req)
{
	struct raspberrypi_pll *pll = container_of(hw, struct raspberrypi_pll, hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_pll_data *data = pll->data;

	return raspberrypi_determine_rate(rpi, data->clock_id, data->name, pll->min_rate, pll->max_rate, req);
}


static int raspberrypi_fw_pll_div_is_on(struct clk_hw *hw)
{
	struct raspberrypi_pll_divider *pll = container_of(hw, struct raspberrypi_pll_divider, div.hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_pll_divider_data *data = pll->data;

	return raspberrypi_fw_is_on(rpi, data->clock_id, data->name);
}

static unsigned long raspberrypi_fw_pll_div_get_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct raspberrypi_pll_divider *pll = container_of(hw, struct raspberrypi_pll_divider, div.hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_pll_divider_data *data = pll->data;

	return raspberrypi_fw_get_rate(rpi, data->clock_id, data->name, parent_rate);
}

static int raspberrypi_fw_pll_div_set_rate(struct clk_hw *hw, unsigned long rate,
				       unsigned long parent_rate)
{
	struct raspberrypi_pll_divider *pll = container_of(hw, struct raspberrypi_pll_divider, div.hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_pll_divider_data *data = pll->data;

	return raspberrypi_fw_set_rate(rpi, data->clock_id, data->name, rate, parent_rate);
}

static int raspberrypi_pll_div_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *req)
{
	struct raspberrypi_pll_divider *pll = container_of(hw, struct raspberrypi_pll_divider, div.hw);
	struct raspberrypi_clk *rpi = pll->rpi;
	const struct raspberrypi_pll_divider_data *data = pll->data;

	return raspberrypi_determine_rate(rpi, data->clock_id, data->name, pll->min_rate, pll->max_rate, req);
}


static const struct clk_ops raspberrypi_firmware_pll_clk_ops = {
	.is_prepared = raspberrypi_fw_pll_is_on,
	.recalc_rate = raspberrypi_fw_pll_get_rate,
	.set_rate = raspberrypi_fw_pll_set_rate,
	.determine_rate = raspberrypi_pll_determine_rate,
};

static const struct clk_ops raspberrypi_firmware_pll_divider_clk_ops = {
	.is_prepared = raspberrypi_fw_pll_div_is_on,
	.recalc_rate = raspberrypi_fw_pll_div_get_rate,
	.set_rate = raspberrypi_fw_pll_div_set_rate,
	.determine_rate = raspberrypi_pll_div_determine_rate,
};

static const struct clk_ops raspberrypi_firmware_clk_ops = {
	.is_prepared = raspberrypi_fw_clock_is_on,
	.recalc_rate = raspberrypi_fw_clock_get_rate,
	.set_rate = raspberrypi_fw_clock_set_rate,
	.determine_rate = raspberrypi_clock_determine_rate,
};


static int raspberrypi_get_clock_range(struct raspberrypi_clk *rpi, u32 clock_id, u32 *min_rate, u32 *max_rate)
{
	int ret;

	/* Get min & max rates set by the firmware */
	ret = raspberrypi_clock_property(rpi->firmware,
					 RPI_FIRMWARE_GET_MIN_CLOCK_RATE,
					 clock_id,
					 min_rate);
	if (ret) {
		dev_err(rpi->dev, "Failed to get clock %d min freq: %d (%d)\n",
			clock_id, *min_rate, ret);
		return ret;
	}

	ret = raspberrypi_clock_property(rpi->firmware,
					 RPI_FIRMWARE_GET_MAX_CLOCK_RATE,
					 clock_id,
					 max_rate);
	if (ret) {
		dev_err(rpi->dev, "Failed to get clock %d max freq: %d (%d)\n",
			clock_id, *max_rate, ret);
		return ret;
	}
	return 0;
}


static int raspberrypi_register_pll(struct raspberrypi_clk *rpi,
					   const struct raspberrypi_pll_data *data)
{
	struct raspberrypi_pll *pll;
	struct clk_init_data init;
	int ret;

	memset(&init, 0, sizeof(init));

	/* All of the PLLs derive from the external oscillator. */
	init.parent_names = data->parents;
	init.num_parents = data->num_parents;
	init.name = data->name;
	init.ops = &raspberrypi_firmware_pll_clk_ops;
	init.flags = CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED;

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return -ENOMEM;
	pll->rpi = rpi;
	pll->data = data;
	pll->hw.init = &init;

	ret = raspberrypi_get_clock_range(rpi, data->clock_id, &pll->min_rate, &pll->max_rate);
	if (ret) {
		dev_err(rpi->dev, "%s: raspberrypi_get_clock_range(%s) failed: %d\n", __func__, init.name, ret);
		return ret;
	}

	ret = devm_clk_hw_register(rpi->dev, &pll->hw);
	if (ret) {
		dev_err(rpi->dev, "%s: devm_clk_hw_register(%s) failed: %d\n", __func__, init.name, ret);
		return ret;
	}
	return 0;
}

static int
raspberrypi_register_pll_divider(struct raspberrypi_clk *rpi,
			     const struct raspberrypi_pll_divider_data *data)
{
	struct raspberrypi_pll_divider *divider;
	struct clk_init_data init;
	int ret;

	memset(&init, 0, sizeof(init));

	init.parent_names = &data->source_pll;
	init.num_parents = 1;
	init.name = data->name;
	init.ops = &raspberrypi_firmware_pll_divider_clk_ops;
	init.flags = data->flags | CLK_IGNORE_UNUSED;

	divider = devm_kzalloc(rpi->dev, sizeof(*divider), GFP_KERNEL);
	if (!divider)
		return -ENOMEM;

	divider->div.hw.init = &init;
	divider->rpi = rpi;
	divider->data = data;

	ret = raspberrypi_get_clock_range(rpi, data->clock_id, &divider->min_rate, &divider->max_rate);
	if (ret) {
		dev_err(rpi->dev, "%s: raspberrypi_get_clock_range(%s) failed: %d\n", __func__, init.name, ret);
		return ret;
	}

	ret = devm_clk_hw_register(rpi->dev, &divider->div.hw);
	if (ret) {
		dev_err(rpi->dev, "%s: devm_clk_hw_register(%s) failed: %d\n", __func__, init.name, ret);
		return ret;
	}

	/*
	 * PLLH's channels have a fixed divide by 10 afterwards, which
	 * is what our consumers are actually using.
	 */
	if (data->fixed_divider != 0) {
		struct clk_lookup *lookup;
		struct clk_hw *clk = clk_hw_register_fixed_factor(rpi->dev,
						    data->divider_name,
						    data->name,
						    CLK_SET_RATE_PARENT,
						    1,
						    data->fixed_divider);
		if (IS_ERR(clk)) {
			dev_err(rpi->dev, "%s: clk_hw_register_fixed_factor(%s) failed: %ld\n", __func__, init.name, PTR_ERR(clk));
			return PTR_ERR(clk);
		}
		if (data->lookup) {
			lookup = clkdev_hw_create(clk, NULL, data->lookup);
			if (IS_ERR(lookup)) {
				dev_err(rpi->dev, "%s: clk_hw_register_fixed_factor(%s) failed: %ld\n", __func__, init.name, PTR_ERR(lookup));
				return PTR_ERR(lookup);
			}
		}
	}

	return 0;
}

static int raspberrypi_register_clock(struct raspberrypi_clk *rpi,
					  const struct raspberrypi_clock_data *data)
{
	struct raspberrypi_clock *clock;
	struct clk_init_data init;
	struct clk *clk;
	int ret;

	memset(&init, 0, sizeof(init));
	init.parent_names = data->parents;
	init.num_parents = data->num_parents;
	init.name = data->name;
	init.flags = data->flags | CLK_IGNORE_UNUSED;

	init.ops = &raspberrypi_firmware_clk_ops;

	clock = devm_kzalloc(rpi->dev, sizeof(*clock), GFP_KERNEL);
	if (!clock)
		return -ENOMEM;

	clock->rpi = rpi;
	clock->data = data;
	clock->hw.init = &init;

	ret = raspberrypi_get_clock_range(rpi, data->clock_id, &clock->min_rate, &clock->max_rate);
	if (ret) {
		dev_err(rpi->dev, "%s: raspberrypi_get_clock_range(%s) failed: %d\n", __func__, init.name, ret);
		return ret;
	}
	clk = devm_clk_register(rpi->dev, &clock->hw);
	if (IS_ERR(clk)) {
		dev_err(rpi->dev, "%s: devm_clk_register(%s) failed: %ld\n", __func__, init.name, PTR_ERR(clk));
		return PTR_ERR(clk);
	}
	ret = clk_register_clkdev(clk, init.name, NULL);
	if (ret) {
		dev_err(rpi->dev, "%s: clk_register_clkdev(%s) failed: %d\n", __func__, init.name, ret);
		return ret;
	}
	return 0;
}


/*
 * the real definition of all the pll, pll_dividers and clocks
 * these make use of the above REGISTER_* macros
 */
static const struct raspberrypi_clk_desc clk_desc_array[] = {
	/* the PLL + PLL dividers */
	[BCM2835_CLOCK_V3D]     = REGISTER_CLK(
		SOC_ALL,
		.name = "v3d",
		.parents = (const char *[]){ "osc" },
		.num_parents = 1,
		.clock_id = RPI_FIRMWARE_V3D_CLK_ID),
	[BCM2835_PLLB_ARM]      = REGISTER_PLL_DIV(
		SOC_ALL,
		.name = "pllb",
		.source_pll = "osc",
		.divider_name = "pllb_arm",
		.lookup = "cpu0",
		.fixed_divider = 1,
		.clock_id = RPI_FIRMWARE_ARM_CLK_ID,
		.flags = CLK_SET_RATE_PARENT),
};

static int raspberrypi_clk_probe(struct platform_device *pdev)
{
	struct device_node *firmware_node;
	struct device *dev = &pdev->dev;
	struct rpi_firmware *firmware;
	struct raspberrypi_clk *rpi;
	const struct raspberrypi_clk_desc *desc;
	const size_t asize = ARRAY_SIZE(clk_desc_array);
	int i;

	firmware_node = of_find_compatible_node(NULL, NULL,
					"raspberrypi,bcm2835-firmware");
	if (!firmware_node) {
		dev_err(dev, "Missing firmware node\n");
		return -ENOENT;
	}

	firmware = rpi_firmware_get(firmware_node);
	of_node_put(firmware_node);
	if (!firmware)
		return -EPROBE_DEFER;

	rpi = devm_kzalloc(dev, sizeof(*rpi), GFP_KERNEL);
	if (!rpi)
		return -ENOMEM;

	rpi->dev = dev;
	rpi->firmware = firmware;
	platform_set_drvdata(pdev, rpi);

	for (i = 0; i < asize; i++) {
		desc = &clk_desc_array[i];
		if (desc->clk_register && desc->data /*&&
		    (desc->supported & pdata->soc)*/) {
			int ret = desc->clk_register(rpi, desc->data);
			if (ret)
				return ret;
		}
	}

	rpi->cpufreq = platform_device_register_data(dev, "raspberrypi-cpufreq",
						     -1, NULL, 0);

	return 0;
}

static int raspberrypi_clk_remove(struct platform_device *pdev)
{
	struct raspberrypi_clk *rpi = platform_get_drvdata(pdev);

	platform_device_unregister(rpi->cpufreq);

	return 0;
}

static struct platform_driver raspberrypi_clk_driver = {
	.driver = {
		.name = "raspberrypi-clk",
	},
	.probe          = raspberrypi_clk_probe,
	.remove		= raspberrypi_clk_remove,
};
module_platform_driver(raspberrypi_clk_driver);

MODULE_AUTHOR("Nicolas Saenz Julienne <nsaenzjulienne@suse.de>");
MODULE_DESCRIPTION("Raspberry Pi firmware clock driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:raspberrypi-clk");
