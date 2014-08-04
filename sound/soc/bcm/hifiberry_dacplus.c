/*
 * ASoC Driver for HiFiBerry DAC+
 *
 * Author:	Daniel Matuschek
 *		Copyright 2014
 *		based on code by Florian Meier <florian.meier@koalo.de>
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

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>

#include "../codecs/pcm512x.h"

static int snd_rpi_hifiberry_dacplus_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	snd_soc_update_bits(codec, PCM512x_GPIO_EN, 0x08, 0x08);
	snd_soc_update_bits(codec, PCM512x_GPIO_OUTPUT_4, 0xf, 0x02);
	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08,0x08);
	return 0;
}

static int snd_rpi_hifiberry_dacplus_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	return snd_soc_dai_set_bclk_ratio(cpu_dai, 64);
}

static int snd_rpi_hifiberry_dacplus_startup(struct snd_pcm_substream *substream) {
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08,0x08);
	return 0;
}

static void snd_rpi_hifiberry_dacplus_shutdown(struct snd_pcm_substream *substream) {
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08,0x00);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_hifiberry_dacplus_ops = {
	.hw_params = snd_rpi_hifiberry_dacplus_hw_params,
	.startup = snd_rpi_hifiberry_dacplus_startup,
	.shutdown = snd_rpi_hifiberry_dacplus_shutdown,
};

static struct snd_soc_dai_link snd_rpi_hifiberry_dacplus_dai[] = {
{
	.name		= "HiFiBerry DAC+",
	.stream_name	= "HiFiBerry DAC+ HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "pcm512x-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "pcm512x.1-004d",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_hifiberry_dacplus_ops,
	.init		= snd_rpi_hifiberry_dacplus_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_hifiberry_dacplus = {
	.name         = "snd_rpi_hifiberry_dacplus",
	.dai_link     = snd_rpi_hifiberry_dacplus_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_hifiberry_dacplus_dai),
};

static int snd_rpi_hifiberry_dacplus_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_hifiberry_dacplus.dev = &pdev->dev;

	if (pdev->dev.of_node) {
	    struct device_node *i2s_node;
	    struct snd_soc_dai_link *dai = &snd_rpi_hifiberry_dacplus_dai[0];
	    i2s_node = of_parse_phandle(pdev->dev.of_node,
					"i2s-controller", 0);

	    if (i2s_node) {
		dai->cpu_dai_name = NULL;
		dai->cpu_of_node = i2s_node;
		dai->platform_name = NULL;
		dai->platform_of_node = i2s_node;
	    }
	}

	ret = snd_soc_register_card(&snd_rpi_hifiberry_dacplus);
	if (ret)
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static int snd_rpi_hifiberry_dacplus_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_hifiberry_dacplus);
}

static const struct of_device_id snd_rpi_hifiberry_dacplus_of_match[] = {
	{ .compatible = "hifiberry,hifiberry-dacplus", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_hifiberry_dacplus_of_match);

static struct platform_driver snd_rpi_hifiberry_dacplus_driver = {
	.driver = {
		.name   = "snd-rpi-hifiberry-dacplus",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_hifiberry_dacplus_of_match,
	},
	.probe          = snd_rpi_hifiberry_dacplus_probe,
	.remove         = snd_rpi_hifiberry_dacplus_remove,
};

module_platform_driver(snd_rpi_hifiberry_dacplus_driver);

MODULE_AUTHOR("Daniel Matuschek <daniel@hifiberry.com>");
MODULE_DESCRIPTION("ASoC Driver for HiFiBerry DAC+");
MODULE_LICENSE("GPL v2");
