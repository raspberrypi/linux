/*
 * ASoC driver for WM8750 AudioCODEC (with a WM8750)
 * connected to a Raspberry Pi
 *
 * Author:      Guillaume Trannoy, <guillaume.trannoy@gmail.com>
 *	      Copyright 2015
 *
 * based on rpi-proto.c
 * Author:      Florian Meier, <koalo@koalo.de>
 *	      Copyright 2013
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 * 
 * Initially based on sound/soc/bcm/rpi-proto.c
 * 
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/jack.h>

#include "../codecs/wm8750.h"

static const unsigned int wm8750_rates_12288000[] = {
	8000, 12000, 16000, 24000, 32000, 48000, 96000,
};

static struct snd_pcm_hw_constraint_list wm8750_constraints_12288000 = {
	.list = wm8750_rates_12288000,
	.count = ARRAY_SIZE(wm8750_rates_12288000),
};

static int snd_rpi_wm8750_startup(struct snd_pcm_substream *substream)
{
	/* Setup constraints, because there is a 12.288 MHz XTAL on the board */
	snd_pcm_hw_constraint_list(substream->runtime, 0,
				SNDRV_PCM_HW_PARAM_RATE,
				&wm8750_constraints_12288000);
	return 0;
}

static int snd_rpi_wm8750_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int sysclk = 12288000; /* This is fixed on this board */

	/* Set proto bclk */
	int ret = snd_soc_dai_set_bclk_ratio(cpu_dai,32*2);
	if (ret < 0){
		dev_err(substream->pcm->dev,
				"Failed to set BCLK ratio %d\n", ret);
		return ret;
	}

	/* Set proto sysclk */
	ret = snd_soc_dai_set_sysclk(codec_dai, WM8750_SYSCLK_XTAL,
			sysclk, SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(substream->pcm->dev,
				"Failed to set WM8750 SYSCLK: %d\n", ret);
		return ret;
	}

	return 0;
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_wm8750_ops = {
	.startup = snd_rpi_wm8750_startup,
	.hw_params = snd_rpi_wm8750_hw_params,
};

static struct snd_soc_dai_link snd_rpi_wm8750_dai[] = {
{
	.name		= "WM8750",
	.stream_name	= "WM8750 HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "wm8750-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "wm8750-codec",
	.dai_fmt	= SND_SOC_DAIFMT_I2S
				| SND_SOC_DAIFMT_NB_NF
				| SND_SOC_DAIFMT_CBM_CFM,
	.ops		= &snd_rpi_wm8750_ops,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_wm8750 = {
	.name		= "snd_rpi_wm8750",
	.dai_link	= snd_rpi_wm8750_dai,
	.num_links	= ARRAY_SIZE(snd_rpi_wm8750_dai),
};

static int snd_rpi_wm8750_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_wm8750.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai = &snd_rpi_wm8750_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
				            "i2s-controller", 0);

		if (i2s_node) {
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = i2s_node;
			dai->platform_name = NULL;
			dai->platform_of_node = i2s_node;
		}
	}

	ret = snd_soc_register_card(&snd_rpi_wm8750);
	if (ret) {
		dev_err(&pdev->dev,
				"snd_soc_register_card() failed: %d\n", ret);
	}

	return ret;
}


static int snd_rpi_wm8750_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_wm8750);
}

static const struct of_device_id snd_rpi_wm8750_of_match[] = {
	{ .compatible = "rpi,rpi-wm8750", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_wm8750_of_match);

static struct platform_driver snd_rpi_wm8750_driver = {
	.driver = {
		.name   = "snd-rpi-wm8750",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_wm8750_of_match,
	},
	.probe	  = snd_rpi_wm8750_probe,
	.remove	 = snd_rpi_wm8750_remove,
};

module_platform_driver(snd_rpi_wm8750_driver);

MODULE_AUTHOR("Guillaume Trannoy");
MODULE_DESCRIPTION("ASoC Driver for Raspberry Pi connected to WM8750");
MODULE_LICENSE("GPL");
