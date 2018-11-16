/*
 * 3Dlab Nano soundcard ALSA SoC Audio driver.
 *
 * Copyright (C) 2018 3Dlab.
 *
 * Author: GT <dev@3d-lab-av.com>
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
#include <sound/soc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

static int nano_soundcard_init(struct snd_soc_pcm_runtime *rtd)
{
	/* add DSD support to audio interfaces */
	rtd->cpu_dai->driver->playback.rate_max = 768000;
	rtd->cpu_dai->driver->playback.formats |= SNDRV_PCM_FMTBIT_DSD_U32_LE;
	rtd->codec_dai->driver->playback.formats |= SNDRV_PCM_FMTBIT_DSD_U32_LE;
	return 0;
}

static int nano_soundcard_hw_params(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params)
{
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int sample_bits = 32;

	if (snd_mask_test(fmt, SNDRV_PCM_FORMAT_DSD_U32_LE)) {
		/* embed DSD in PCM data */
		snd_mask_none(fmt);
		snd_mask_set(fmt, SNDRV_PCM_FORMAT_S32_LE);
		/* notify DSD by private data */
		snd_soc_codec_set_drvdata(rtd->codec, "DSD");
	} else {
		/* notify PCM by private data */
		snd_soc_codec_set_drvdata(rtd->codec, NULL);
	}

	/* frame length enforced by hardware */
	return snd_soc_dai_set_bclk_ratio(rtd->cpu_dai, sample_bits * 2);
}

static struct snd_soc_ops nano_soundcard_ops = {
	.hw_params = nano_soundcard_hw_params,
};

static struct snd_soc_dai_link nano_soundcard_dai = {
	.name		= "3Dlab Nano Soundcard",
	.stream_name	= "3Dlab Nano Soundcard HiFi",
	.platform_name	= "bcm2708-i2s.0",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_name	= "nano-codec.1-0041",
	.codec_dai_name	= "nano-hifi",
	.dai_fmt	= SND_SOC_DAIFMT_I2S |
			  SND_SOC_DAIFMT_CONT |
			  SND_SOC_DAIFMT_NB_NF |
			  SND_SOC_DAIFMT_CBM_CFM,
	.init		= nano_soundcard_init,
	.ops		= &nano_soundcard_ops,
};

static struct snd_soc_card nano_soundcard = {
	.name		= "3Dlab_Nano_Soundcard",
	.owner		= THIS_MODULE,
	.dai_link	= &nano_soundcard_dai,
	.num_links	= 1,
};

static int nano_soundcard_probe(struct platform_device *pdev)
{
	int ret;

	if (pdev->dev.of_node) {
		struct snd_soc_dai_link *dai = &nano_soundcard_dai;
		struct device_node *node;

		/* cpu handle configured by device tree */
		node = of_parse_phandle(pdev->dev.of_node, "i2s-controller", 0);
		if (node) {
			dai->platform_name = NULL;
			dai->platform_of_node = node;
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = node;
		}

		/* codec handle configured by device tree */
		node = of_parse_phandle(pdev->dev.of_node, "i2s-interface", 0);
		if (node) {
			dai->codec_name = NULL;
			dai->codec_of_node = node;
		}
	}

	nano_soundcard.dev = &pdev->dev;
	ret = snd_soc_register_card(&nano_soundcard);

	if (ret && ret != -EPROBE_DEFER)
		dev_err(&pdev->dev, "Failed to register card %d\n", ret);

	return ret;
}

static int nano_soundcard_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&nano_soundcard);
}

static const struct of_device_id nano_soundcard_of_match[] = {
	{ .compatible = "3dlab,nano-soundcard", },
	{ }
};
MODULE_DEVICE_TABLE(of, nano_soundcard_of_match);

static struct platform_driver nano_soundcard_driver = {
	.probe	= nano_soundcard_probe,
	.remove	= nano_soundcard_remove,
	.driver	= {
		.name		= "nano-soundcard",
		.owner		= THIS_MODULE,
		.of_match_table	= nano_soundcard_of_match,
	},
};

module_platform_driver(nano_soundcard_driver);

MODULE_DESCRIPTION("ASoC 3Dlab Nano soundcard driver");
MODULE_AUTHOR("GT <dev@3d-lab-av.com>");
MODULE_LICENSE("GPL v2");

/* EOF */
