/*
 * ASoC Driver for Dion Audio LOCO-V2 DAC-AMP
 *
 * Author:      Miquel Blauw <info@dionaudio.nl>
 *              Copyright 2017
 *
 * Based on the software of the RPi-DAC writen by Florian Meier
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

static bool digital_gain_0db_limit = true;

static int snd_rpi_dionaudio_loco_v2_init(struct snd_soc_pcm_runtime *rtd)
{
	if (digital_gain_0db_limit) {
		int ret;
		struct snd_soc_card *card = rtd->card;

		ret = snd_soc_limit_volume(card, "Digital Playback Volume", 207);
		if (ret < 0)
			dev_warn(card->dev, "Failed to set volume limit: %d\n", ret);
	}

	return 0;
}

SND_SOC_DAILINK_DEFS(dionaudio_loco_v2,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2708-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("pcm512x.1-004d", "pcm512x-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2708-i2s.0")));

static struct snd_soc_dai_link snd_rpi_dionaudio_loco_v2_dai[] = {
{
	.name		= "DionAudio LOCO-V2",
	.stream_name	= "DionAudio LOCO-V2 DAC-AMP",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			  SND_SOC_DAIFMT_CBS_CFS,
	.init		= snd_rpi_dionaudio_loco_v2_init,
	SND_SOC_DAILINK_REG(dionaudio_loco_v2),
},};

/* audio machine driver */
static struct snd_soc_card snd_rpi_dionaudio_loco_v2 = {
	.name         = "Dion Audio LOCO-V2",
	.owner        = THIS_MODULE,
	.dai_link     = snd_rpi_dionaudio_loco_v2_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_dionaudio_loco_v2_dai),
};

static int snd_rpi_dionaudio_loco_v2_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_dionaudio_loco_v2.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai =
					&snd_rpi_dionaudio_loco_v2_dai[0];

		i2s_node = of_parse_phandle(pdev->dev.of_node,
					    "i2s-controller", 0);
		if (i2s_node) {
			dai->cpus->dai_name = NULL;
			dai->cpus->of_node = i2s_node;
			dai->platforms->name = NULL;
			dai->platforms->of_node = i2s_node;
		}

		digital_gain_0db_limit = !of_property_read_bool(
			pdev->dev.of_node, "dionaudio,24db_digital_gain");
	}

	ret = devm_snd_soc_register_card(&pdev->dev, &snd_rpi_dionaudio_loco_v2);
	if (ret)
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n",
			ret);

	return ret;
}

static const struct of_device_id dionaudio_of_match[] = {
	{ .compatible = "dionaudio,dionaudio-loco-v2", },
	{},
};
MODULE_DEVICE_TABLE(of, dionaudio_of_match);

static struct platform_driver snd_rpi_dionaudio_loco_v2_driver = {
	.driver = {
		.name   = "snd-rpi-dionaudio-loco-v2",
		.owner  = THIS_MODULE,
		.of_match_table = dionaudio_of_match,
	},
	.probe          = snd_rpi_dionaudio_loco_v2_probe,
};

module_platform_driver(snd_rpi_dionaudio_loco_v2_driver);

MODULE_AUTHOR("Miquel Blauw <info@dionaudio.nl>");
MODULE_DESCRIPTION("ASoC Driver for DionAudio LOCO-V2");
MODULE_LICENSE("GPL v2");
