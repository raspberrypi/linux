/*
 * Driver for the PCM3060 codec configured in hardware mode
 *
 * Author:	Jon Ronen-Drori <jon_ronen@yahoo.com>
 *		Copyright 2014-2019
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


#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <sound/soc.h>

static struct snd_soc_dai_driver pcm3060_dai = {
	.name = "pcm3060-hifi",
	.playback = {
		.stream_name = "HiFi Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_96000,
		.formats = SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE
	},
	.capture = {
		.stream_name = "HiFi Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_96000,
		.formats = SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE
	},
	.symmetric_rates = 1
};

static struct snd_soc_component_driver soc_component_dev_pcm3060 = {
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
	.non_legacy_dai_naming	= 1,
};

static int pcm3060_probe(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "probe\n");
	return devm_snd_soc_register_component(&pdev->dev, &soc_component_dev_pcm3060,
			&pcm3060_dai, 1);
}

static const struct of_device_id pcm3060_of_match[] = {
	{ .compatible = "ti,pcm3060hw", },
	{ }
};
MODULE_DEVICE_TABLE(of, pcm3060_of_match);

static struct platform_driver pcm3060_codec_driver = {
	.probe		= pcm3060_probe,
	.driver		= {
		.name	= "pcm3060-codec",
		.of_match_table = pcm3060_of_match,
	},
};

module_platform_driver(pcm3060_codec_driver);

MODULE_DESCRIPTION("ASoC PCM3060 codec driver");
MODULE_AUTHOR("Jon Ronen-Drori <jon_ronen@yahoo.com>");
MODULE_LICENSE("GPL v2");
