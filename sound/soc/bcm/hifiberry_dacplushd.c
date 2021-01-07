// SPDX-License-Identifier: GPL-2.0
/*
 * ASoC Driver for HiFiBerry DAC+ HD
 *
 * Author:	Joerg Schambacher, i2Audio GmbH for HiFiBerry
 *		Copyright 2020
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

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/i2c.h>
#include <linux/clk.h>

#include "../codecs/pcm179x.h"

#define DEFAULT_RATE		44100

struct brd_drv_data {
	struct regmap *regmap;
	struct clk *sclk;
};

static struct brd_drv_data drvdata;
static struct gpio_desc *reset_gpio;
static const unsigned int hb_dacplushd_rates[] = {
	192000, 96000, 48000, 176400, 88200, 44100,
};

static struct snd_pcm_hw_constraint_list hb_dacplushd_constraints = {
	.list = hb_dacplushd_rates,
	.count = ARRAY_SIZE(hb_dacplushd_rates),
};

static int snd_rpi_hb_dacplushd_startup(struct snd_pcm_substream *substream)
{
	/* constraints for standard sample rates */
	snd_pcm_hw_constraint_list(substream->runtime, 0,
				SNDRV_PCM_HW_PARAM_RATE,
				&hb_dacplushd_constraints);
	return 0;
}

static void snd_rpi_hifiberry_dacplushd_set_sclk(
		struct snd_soc_component *component,
		int sample_rate)
{
	if (!IS_ERR(drvdata.sclk))
		clk_set_rate(drvdata.sclk, sample_rate);
}

static int snd_rpi_hifiberry_dacplushd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai_link *dai = rtd->dai_link;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	dai->name = "HiFiBerry DAC+ HD";
	dai->stream_name = "HiFiBerry DAC+ HD HiFi";
	dai->dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
		| SND_SOC_DAIFMT_CBM_CFM;

	/* allow only fixed 32 clock counts per channel */
	snd_soc_dai_set_bclk_ratio(cpu_dai, 32*2);

	return 0;
}

static int snd_rpi_hifiberry_dacplushd_hw_params(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;

	struct snd_soc_component *component = rtd->codec_dai->component;

	snd_rpi_hifiberry_dacplushd_set_sclk(component, params_rate(params));
	return ret;
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_hifiberry_dacplushd_ops = {
	.startup = snd_rpi_hb_dacplushd_startup,
	.hw_params = snd_rpi_hifiberry_dacplushd_hw_params,
};

static struct snd_soc_dai_link snd_rpi_hifiberry_dacplushd_dai[] = {
{
	.name		= "HiFiBerry DAC+ HD",
	.stream_name	= "HiFiBerry DAC+ HD HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "pcm179x-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "pcm179x.1-004c",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_hifiberry_dacplushd_ops,
	.init		= snd_rpi_hifiberry_dacplushd_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_hifiberry_dacplushd = {
	.name         = "snd_rpi_hifiberry_dacplushd",
	.driver_name  = "HifiberryDacplusHD",
	.owner        = THIS_MODULE,
	.dai_link     = snd_rpi_hifiberry_dacplushd_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_hifiberry_dacplushd_dai),
};

static int snd_rpi_hifiberry_dacplushd_probe(struct platform_device *pdev)
{
	int ret = 0;
	static int dac_reset_done;
	struct device *dev = &pdev->dev;
	struct device_node *dev_node = dev->of_node;

	snd_rpi_hifiberry_dacplushd.dev = &pdev->dev;

	/* get GPIO and release DAC from RESET */
	if (!dac_reset_done) {
		reset_gpio = gpiod_get(&pdev->dev, "reset", GPIOD_OUT_LOW);
		if (IS_ERR(reset_gpio)) {
			dev_err(&pdev->dev, "gpiod_get() failed\n");
			return -EINVAL;
		}
		dac_reset_done = 1;
	}
	if (!IS_ERR(reset_gpio))
		gpiod_set_value(reset_gpio, 0);
	msleep(1);
	if (!IS_ERR(reset_gpio))
		gpiod_set_value(reset_gpio, 1);
	msleep(1);
	if (!IS_ERR(reset_gpio))
		gpiod_set_value(reset_gpio, 0);

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;

		dai = &snd_rpi_hifiberry_dacplushd_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
			"i2s-controller", 0);

		if (i2s_node) {
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = i2s_node;
			dai->platform_name = NULL;
			dai->platform_of_node = i2s_node;
		} else {
			return -EPROBE_DEFER;
		}

	}

	ret = devm_snd_soc_register_card(&pdev->dev,
			&snd_rpi_hifiberry_dacplushd);
	if (ret && ret != -EPROBE_DEFER) {
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);
		return ret;
	}
	if (ret == -EPROBE_DEFER)
		return ret;

	dev_set_drvdata(dev, &drvdata);
	if (dev_node == NULL) {
		dev_err(&pdev->dev, "Device tree node not found\n");
		return -ENODEV;
	}

	drvdata.sclk = devm_clk_get(dev, NULL);
	if (IS_ERR(drvdata.sclk)) {
		drvdata.sclk = ERR_PTR(-ENOENT);
		return -ENODEV;
	}

	clk_set_rate(drvdata.sclk, DEFAULT_RATE);

	return ret;
}

static int snd_rpi_hifiberry_dacplushd_remove(struct platform_device *pdev)
{
	if (IS_ERR(reset_gpio))
		return -EINVAL;

	/* put DAC into RESET and release GPIO */
	gpiod_set_value(reset_gpio, 0);
	gpiod_put(reset_gpio);

	return 0;
}

static const struct of_device_id snd_rpi_hifiberry_dacplushd_of_match[] = {
	{ .compatible = "hifiberry,hifiberry-dacplushd", },
	{},
};

MODULE_DEVICE_TABLE(of, snd_rpi_hifiberry_dacplushd_of_match);

static struct platform_driver snd_rpi_hifiberry_dacplushd_driver = {
	.driver = {
		.name   = "snd-rpi-hifiberry-dacplushd",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_hifiberry_dacplushd_of_match,
	},
	.probe          = snd_rpi_hifiberry_dacplushd_probe,
	.remove		= snd_rpi_hifiberry_dacplushd_remove,
};

module_platform_driver(snd_rpi_hifiberry_dacplushd_driver);

MODULE_AUTHOR("Joerg Schambacher <joerg@i2audio.com>");
MODULE_DESCRIPTION("ASoC Driver for HiFiBerry DAC+ HD");
MODULE_LICENSE("GPL v2");
