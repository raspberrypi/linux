// SPDX-License-Identifier: GPL-2.0
/*
 * pwm-rp1.c
 *
 * Raspberry Pi RP1 PWM.
 *
 * Copyright © 2023 Raspberry Pi Ltd.
 *
 * Author: Naushir Patuck (naush@raspberrypi.com)
 *
 * Based on the pwm-bcm2835 driver by:
 * Bart Tanghe <bart.tanghe@thomasmore.be>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define PWM_GLOBAL_CTRL		0x000
#define PWM_CHANNEL_CTRL(x)	(0x014 + ((x) * 16))
#define PWM_RANGE(x)		(0x018 + ((x) * 16))
#define PWM_DUTY(x)		(0x020 + ((x) * 16))

/* 8:FIFO_POP_MASK + 0:Trailing edge M/S modulation */
#define PWM_CHANNEL_DEFAULT	(BIT(8) + BIT(0))
#define PWM_CHANNEL_ENABLE(x)	BIT(x)
#define PWM_POLARITY		BIT(3)
#define SET_UPDATE		BIT(31)
#define PWM_MODE_MASK		GENMASK(1, 0)

struct rp1_pwm {
	void __iomem *base;
	struct clk *clk;
};

static inline struct rp1_pwm *to_rp1_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static void rp1_pwm_apply_config(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rp1_pwm *pc = to_rp1_pwm(chip);
	u32 value;

	value = readl(pc->base + PWM_GLOBAL_CTRL);
	value |= SET_UPDATE;
	writel(value, pc->base + PWM_GLOBAL_CTRL);
}

static int rp1_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rp1_pwm *pc = to_rp1_pwm(chip);

	writel(PWM_CHANNEL_DEFAULT, pc->base + PWM_CHANNEL_CTRL(pwm->hwpwm));
	return 0;
}

static void rp1_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rp1_pwm *pc = to_rp1_pwm(chip);
	u32 value;

	value = readl(pc->base + PWM_CHANNEL_CTRL(pwm->hwpwm));
	value &= ~PWM_MODE_MASK;
	writel(value, pc->base + PWM_CHANNEL_CTRL(pwm->hwpwm));
	rp1_pwm_apply_config(chip, pwm);
}

static int rp1_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			 const struct pwm_state *state)
{
	struct rp1_pwm *pc = to_rp1_pwm(chip);
	unsigned long clk_rate = clk_get_rate(pc->clk);
	unsigned long clk_period;
	u32 value;

	if (!clk_rate) {
		dev_err(&chip->dev, "failed to get clock rate\n");
		return -EINVAL;
	}

	/* set period */
	clk_period = DIV_ROUND_CLOSEST(NSEC_PER_SEC, clk_rate);

	writel(DIV_ROUND_CLOSEST(state->duty_cycle, clk_period),
	       pc->base + PWM_DUTY(pwm->hwpwm));

	/* set duty cycle */
	writel(DIV_ROUND_CLOSEST(state->period, clk_period),
	       pc->base + PWM_RANGE(pwm->hwpwm));

	/* set polarity */
	value = readl(pc->base + PWM_CHANNEL_CTRL(pwm->hwpwm));
	if (state->polarity == PWM_POLARITY_NORMAL)
		value &= ~PWM_POLARITY;
	else
		value |= PWM_POLARITY;
	writel(value, pc->base + PWM_CHANNEL_CTRL(pwm->hwpwm));

	/* enable/disable */
	value = readl(pc->base + PWM_GLOBAL_CTRL);
	if (state->enabled)
		value |= PWM_CHANNEL_ENABLE(pwm->hwpwm);
	else
		value &= ~PWM_CHANNEL_ENABLE(pwm->hwpwm);
	writel(value, pc->base + PWM_GLOBAL_CTRL);

	rp1_pwm_apply_config(chip, pwm);

	return 0;
}

static const struct pwm_ops rp1_pwm_ops = {
	.request = rp1_pwm_request,
	.free = rp1_pwm_free,
	.apply = rp1_pwm_apply,
};

static int rp1_pwm_probe(struct platform_device *pdev)
{
	struct pwm_chip *chip;
	struct rp1_pwm *pc;
	int ret;

	chip = devm_pwmchip_alloc(&pdev->dev, 4, sizeof(*pc));

	if (IS_ERR(chip))
		return PTR_ERR(chip);

	pc = to_rp1_pwm(chip);

	pc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pc->base))
		return PTR_ERR(pc->base);

	pc->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(pc->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(pc->clk),
				     "clock not found\n");

	chip->ops = &rp1_pwm_ops;
	chip->of_xlate = of_pwm_xlate_with_flags;

	ret = devm_pwmchip_add(&pdev->dev, chip);
	if (ret < 0)
		goto add_fail;

	return 0;

add_fail:
	clk_disable_unprepare(pc->clk);
	return ret;
}

static void rp1_pwm_remove(struct platform_device *pdev)
{
	struct rp1_pwm *pc = platform_get_drvdata(pdev);

	clk_disable_unprepare(pc->clk);
}

static const struct of_device_id rp1_pwm_of_match[] = {
	{ .compatible = "raspberrypi,rp1-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rp1_pwm_of_match);

static struct platform_driver rp1_pwm_driver = {
	.driver = {
		.name = "rpi-pwm",
		.of_match_table = rp1_pwm_of_match,
	},
	.probe = rp1_pwm_probe,
	.remove = rp1_pwm_remove,
};
module_platform_driver(rp1_pwm_driver);

MODULE_AUTHOR("Naushir Patuck <naush@raspberrypi.com");
MODULE_DESCRIPTION("RP1 PWM driver");
MODULE_LICENSE("GPL");
