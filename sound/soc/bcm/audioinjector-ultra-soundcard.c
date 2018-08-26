/*
 * ASoC Driver for AudioInjector Pi ultra soundcard (hat)
 *
 *  Created on: 11-September-2017
 *      Author: flatmax@flatmax.org
 *              based on audioinjector-octo-soundcard.c
 *
 * Copyright (C) 2017 Flatmax Pty. Ltd.
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
#include <linux/types.h>
#include <linux/gpio/consumer.h>

#include <sound/core.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/control.h>

static int audioinjector_ultra_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret=snd_soc_dai_set_bclk_ratio(rtd->cpu_dai, 64);
	if (ret==0)
		ret=snd_soc_dai_set_sysclk(rtd->codec_dai, 0, 12288000, 0);
	return ret;
}

static struct snd_soc_dai_link audioinjector_ultra_dai[] = {
	{
		.name = "AudioInjector Ultra",
		.stream_name = "AudioInject-HIFI",
		.codec_dai_name = "cs4265-dai1",
		.init = audioinjector_ultra_dai_init,
		.dai_fmt = SND_SOC_DAIFMT_CBM_CFM|SND_SOC_DAIFMT_I2S|SND_SOC_DAIFMT_NB_NF,
	},
};

static const struct snd_soc_dapm_widget audioinjector_ultra_widgets[] = {
	SND_SOC_DAPM_OUTPUT("OUTPUTS"),
	SND_SOC_DAPM_INPUT("INPUTS"),
};

static const struct snd_soc_dapm_route audioinjector_ultra_route[] = {
	/* outputs */
	{"OUTPUTS", NULL, "LINEOUTL"},
	{"OUTPUTS", NULL, "LINEOUTR"},
	{"OUTPUTS", NULL, "SPDIFOUT"},

	/* inputs */
	{"LINEINL", NULL, "INPUTS"},
	{"LINEINR", NULL, "INPUTS"},

	/* mic inputs */
	{"MICL", NULL, "INPUTS"},
	{"MICR", NULL, "INPUTS"},
};

static struct snd_soc_card snd_soc_audioinjector_ultra = {
	.name = "audioinjector-ultra-soundcard",
	.dai_link = audioinjector_ultra_dai,
	.num_links = ARRAY_SIZE(audioinjector_ultra_dai),

	.dapm_widgets = audioinjector_ultra_widgets,
	.num_dapm_widgets = ARRAY_SIZE(audioinjector_ultra_widgets),
	.dapm_routes = audioinjector_ultra_route,
	.num_dapm_routes = ARRAY_SIZE(audioinjector_ultra_route),
};

static int audioinjector_ultra_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_audioinjector_ultra;
	int ret;

	card->dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct snd_soc_dai_link *dai = &audioinjector_ultra_dai[0];
		struct device_node *i2s_node =
					of_parse_phandle(pdev->dev.of_node,
							"i2s-controller", 0);
		struct device_node *codec_node =
					of_parse_phandle(pdev->dev.of_node,
								"codec", 0);

		if (i2s_node && codec_node) {
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = i2s_node;
			dai->platform_name = NULL;
			dai->platform_of_node = i2s_node;
			dai->codec_name = NULL;
			dai->codec_of_node = codec_node;
		} else
			if (!dai->cpu_of_node) {
				dev_err(&pdev->dev,
				"i2s-controller missing or invalid in DT\n");
				return -EINVAL;
			} else {
				dev_err(&pdev->dev,
				"Property 'codec' missing or invalid\n");
				return -EINVAL;
			}
	}

	ret = snd_soc_register_card(card);
	if (ret != 0)
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
	return ret;
}

static int audioinjector_ultra_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	return snd_soc_unregister_card(card);
}

static const struct of_device_id audioinjector_ultra_of_match[] = {
	{ .compatible = "ai,audioinjector-ultra-soundcard", },
	{},
};
MODULE_DEVICE_TABLE(of, audioinjector_ultra_of_match);

static struct platform_driver audioinjector_ultra_driver = {
	.driver	= {
		.name			= "audioinjector-ultra",
		.owner			= THIS_MODULE,
		.of_match_table = audioinjector_ultra_of_match,
	},
	.probe	= audioinjector_ultra_probe,
	.remove	= audioinjector_ultra_remove,
};

module_platform_driver(audioinjector_ultra_driver);
MODULE_AUTHOR("Matt Flax <flatmax@flatmax.org>");
MODULE_DESCRIPTION("AudioInjector.net ultra Soundcard");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:audioinjector-ultra-soundcard");
