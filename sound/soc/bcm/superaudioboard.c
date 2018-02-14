/*
 * ASoC Driver for SuperAudioBoard
 *
 * Author:	R F William Hollender <whollender@gmail.com>
 *		Copyright 2015
 *		based on code by Daniel Matuschek <daniel@hifiberry.com>
 *		and Florian Meier <florian.meier@koalo.de>
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

//#include "../codecs/pcm512x.h"

static int snd_rpi_superaudioboard_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	// Don't think any init code to put here
	return 0;
}

static int snd_rpi_superaudioboard_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_codec *codec = rtd->codec;

	// Need to tell the codec what it's system clock is (24.576MHz crystal)
	int sysclk = 24576000;

	int ret = snd_soc_dai_set_sysclk(codec_dai,0,sysclk,0); // Don't worry about clock id and direction (it's ignored in cs4271 driver)

	if (ret < 0)
	{
		dev_err(codec->dev, "Unable to set CS4271 system clock.");
		return ret;
	}

	// Note, the bclk ratio is always 64 in master mode
	return snd_soc_dai_set_bclk_ratio(cpu_dai, 64);
}

static int snd_rpi_superaudioboard_startup(struct snd_pcm_substream *substream) {
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	return 0;
}

static void snd_rpi_superaudioboard_shutdown(struct snd_pcm_substream *substream) {
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_superaudioboard_ops = {
	.hw_params = snd_rpi_superaudioboard_hw_params,
	.startup = snd_rpi_superaudioboard_startup,
	.shutdown = snd_rpi_superaudioboard_shutdown,
};

static struct snd_soc_dai_link snd_rpi_superaudioboard_dai[] = {
{
	.name		= "SuperAudioBoard",
	.stream_name	= "SuperAudioBoard HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "cs4271-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "cs4271.1-0010", // TODO: This seems to be codec name . i2c # (1) - i2c address, but not sure
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_IF | // Using inverted frame clock and normal bit clock, I2S mode
				SND_SOC_DAIFMT_CBM_CFM, // Codec bit clock and frame clock master
	.ops		= &snd_rpi_superaudioboard_ops,
	.init		= snd_rpi_superaudioboard_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_superaudioboard = {
	.name         = "snd_rpi_superaudioboard",
	.dai_link     = snd_rpi_superaudioboard_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_superaudioboard_dai),
};

static int snd_rpi_superaudioboard_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_superaudioboard.dev = &pdev->dev;

	if (pdev->dev.of_node) {
	    struct device_node *i2s_node;
	    struct snd_soc_dai_link *dai = &snd_rpi_superaudioboard_dai[0];
	    i2s_node = of_parse_phandle(pdev->dev.of_node,
					"i2s-controller", 0);

	    if (i2s_node) {
		dai->cpu_dai_name = NULL;
		dai->cpu_of_node = i2s_node;
		dai->platform_name = NULL;
		dai->platform_of_node = i2s_node;
	    }
	}

	ret = snd_soc_register_card(&snd_rpi_superaudioboard);
	if (ret)
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static int snd_rpi_superaudioboard_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_superaudioboard);
}

static const struct of_device_id snd_rpi_superaudioboard_of_match[] = {
	{ .compatible = "superaudio,superaudioboard", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_superaudioboard_of_match);

static struct platform_driver snd_rpi_superaudioboard_driver = {
	.driver = {
		.name   = "snd-rpi-superaudioboard",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_superaudioboard_of_match,
	},
	.probe          = snd_rpi_superaudioboard_probe,
	.remove         = snd_rpi_superaudioboard_remove,
};

module_platform_driver(snd_rpi_superaudioboard_driver);

MODULE_AUTHOR("R F William Hollender <whollender@gmail.com>");
MODULE_DESCRIPTION("ASoC Driver for SuperAudioBoard");
MODULE_LICENSE("GPL v2");
