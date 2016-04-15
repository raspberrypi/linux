/*
 * ASoC Driver for BoomBerry DAC Raspberry Pi HAT Sound Card
 *
 * Author:	Milan Neskovic
 *		Copyright 2016
 *		based on code by Daniel Matuschek <info@crazy-audio.com>
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

static bool digital_gain_0db_limit = true;

static int snd_rpi_boomberry_dac_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	snd_soc_update_bits(codec, PCM512x_GPIO_EN, 0x08, 0x08);
	snd_soc_update_bits(codec, PCM512x_GPIO_OUTPUT_4, 0xf, 0x02);
	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08,0x08);

	if (digital_gain_0db_limit)
	{
		int ret;
		struct snd_soc_card *card = rtd->card;

		ret = snd_soc_limit_volume(card, "Digital Playback Volume", 207);
		if (ret < 0)
			dev_warn(card->dev, "Failed to set volume limit: %d\n", ret);
	}

	return 0;
}

static int snd_rpi_boomberry_dac_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	/*return snd_soc_dai_set_bclk_ratio(cpu_dai, 64);*/
	unsigned int sample_bits =
		snd_pcm_format_physical_width(params_format(params));
	return snd_soc_dai_set_bclk_ratio(cpu_dai, sample_bits * 2);
}

static int snd_rpi_boomberry_dac_startup(struct snd_pcm_substream *substream) {
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08,0x08);
	return 0;
}

static void snd_rpi_boomberry_dac_shutdown(struct snd_pcm_substream *substream) {
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08,0x00);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_boomberry_dac_ops = {
	.hw_params = snd_rpi_boomberry_dac_hw_params,
	.startup = snd_rpi_boomberry_dac_startup,
	.shutdown = snd_rpi_boomberry_dac_shutdown,
};

static struct snd_soc_dai_link snd_rpi_boomberry_dac_dai[] = {
{
	.name		= "BoomBerry DAC",
	.stream_name	= "BoomBerry DAC HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "pcm512x-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "pcm512x.1-004d",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_boomberry_dac_ops,
	.init		= snd_rpi_boomberry_dac_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_boomberry_dac = {
	.name         = "snd_rpi_boomberry_dac",
	.owner        = THIS_MODULE,
	.dai_link     = snd_rpi_boomberry_dac_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_boomberry_dac_dai),
};

static int snd_rpi_boomberry_dac_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_boomberry_dac.dev = &pdev->dev;

	if (pdev->dev.of_node) {
	    struct device_node *i2s_node;
	    struct snd_soc_dai_link *dai = &snd_rpi_boomberry_dac_dai[0];
	    i2s_node = of_parse_phandle(pdev->dev.of_node,
					"i2s-controller", 0);

	    if (i2s_node) {
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = i2s_node;
			dai->platform_name = NULL;
			dai->platform_of_node = i2s_node;
	    }

	    digital_gain_0db_limit = !of_property_read_bool(
			pdev->dev.of_node, "boomberry,24db_digital_gain");
	}

	ret = snd_soc_register_card(&snd_rpi_boomberry_dac);
	if (ret)
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static int snd_rpi_boomberry_dac_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_boomberry_dac);
}

static const struct of_device_id snd_rpi_boomberry_dac_of_match[] = {
	{ .compatible = "boomberry,boomberry-dac", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_boomberry_dac_of_match);

static struct platform_driver snd_rpi_boomberry_dac_driver = {
	.driver = {
		.name   = "snd-rpi-boomberry-dac",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_boomberry_dac_of_match,
	},
	.probe          = snd_rpi_boomberry_dac_probe,
	.remove         = snd_rpi_boomberry_dac_remove,
};

module_platform_driver(snd_rpi_boomberry_dac_driver);

MODULE_AUTHOR("Milan Neskovic <info@boomberry.co>");
MODULE_DESCRIPTION("ASoC Driver for BoomBerry PI DAC HAT Sound Card");
MODULE_LICENSE("GPL v2");
