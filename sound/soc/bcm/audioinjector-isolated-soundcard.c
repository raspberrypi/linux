/*
 * ASoC Driver for AudioInjector.net isolated soundcard
 *
 *  Created on: 20-February-2020
 *      Author: flatmax@flatmax.org
 *              based on audioinjector-octo-soundcard.c
 *
 * Copyright (C) 2020 Flatmax Pty. Ltd.
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

static struct gpio_desc *mute_gpio;

static const unsigned int audioinjector_isolated_rates[] = {
	192000, 96000, 48000, 32000, 24000, 16000, 8000
};

static struct snd_pcm_hw_constraint_list audioinjector_isolated_constraints = {
	.list = audioinjector_isolated_rates,
	.count = ARRAY_SIZE(audioinjector_isolated_rates),
};

static int audioinjector_isolated_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret=snd_soc_dai_set_sysclk(rtd->codec_dai, 0, 24576000, 0);
	if (ret)
		return ret;
	return snd_soc_dai_set_bclk_ratio(rtd->cpu_dai, 64);
}

static int audioinjector_isolated_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	snd_pcm_hw_constraint_list(substream->runtime, 0,
			SNDRV_PCM_HW_PARAM_RATE, &audioinjector_isolated_constraints);

	gpiod_set_value(mute_gpio, 1);
	return 0;
}

static struct snd_soc_ops audioinjector_isolated_ops = {
	.startup	= audioinjector_isolated_startup,
};

static struct snd_soc_dai_link audioinjector_isolated_dai[] = {
	{
		.name = "AudioInjector ISO",
		.stream_name = "AI-HIFI",
		.codec_dai_name = "cs4271-hifi",
		.ops = &audioinjector_isolated_ops,
		.init = audioinjector_isolated_dai_init,
		.symmetric_rates = 1,
		.symmetric_channels = 1,
		.dai_fmt = SND_SOC_DAIFMT_CBM_CFM|SND_SOC_DAIFMT_I2S|SND_SOC_DAIFMT_NB_NF,
	}
};

static const struct snd_soc_dapm_widget audioinjector_isolated_widgets[] = {
	SND_SOC_DAPM_OUTPUT("OUTPUTS"),
	SND_SOC_DAPM_INPUT("INPUTS"),
};

static const struct snd_soc_dapm_route audioinjector_isolated_route[] = {
	/* Balanced outputs */
	{"OUTPUTS", NULL, "AOUTA+"},
	{"OUTPUTS", NULL, "AOUTA-"},
	{"OUTPUTS", NULL, "AOUTB+"},
	{"OUTPUTS", NULL, "AOUTB-"},

	/* Balanced inputs */
	{"AINA", NULL, "INPUTS"},
	{"AINB", NULL, "INPUTS"},
};

static struct snd_soc_card snd_soc_audioinjector_isolated = {
	.name = "audioinjector-isolated-soundcard",
	.dai_link = audioinjector_isolated_dai,
	.num_links = ARRAY_SIZE(audioinjector_isolated_dai),

	.dapm_widgets = audioinjector_isolated_widgets,
	.num_dapm_widgets = ARRAY_SIZE(audioinjector_isolated_widgets),
	.dapm_routes = audioinjector_isolated_route,
	.num_dapm_routes = ARRAY_SIZE(audioinjector_isolated_route),
};

static int audioinjector_isolated_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_audioinjector_isolated;
	int ret;

	card->dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct snd_soc_dai_link *dai = &audioinjector_isolated_dai[0];
		struct device_node *i2s_node =
					of_parse_phandle(pdev->dev.of_node, "i2s-controller", 0);
		struct device_node *codec_node =
					of_parse_phandle(pdev->dev.of_node, "codec", 0);

		mute_gpio = devm_gpiod_get_optional(&pdev->dev, "mute", GPIOD_OUT_LOW);
		if (IS_ERR(mute_gpio)){
			dev_err(&pdev->dev, "mute gpio not found in dt overlay\n");
			return PTR_ERR(mute_gpio);
		}
		gpiod_set_value(mute_gpio, 0);

		if (i2s_node && codec_node) {
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = i2s_node;
			dai->platform_name = NULL;
			dai->platform_of_node = i2s_node;
			dai->codec_name = NULL;
			dai->codec_of_node = codec_node;
		} else
			if (!i2s_node) {
				dev_err(&pdev->dev,
				"i2s-controller missing or invalid in DT\n");
				return -EINVAL;
			} else {
				dev_err(&pdev->dev,
				"Property 'codec' missing or invalid\n");
				return -EINVAL;
			}
	}

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret != 0)
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
	return ret;
}

static const struct of_device_id audioinjector_isolated_of_match[] = {
	{ .compatible = "ai,audioinjector-isolated-soundcard", },
	{},
};
MODULE_DEVICE_TABLE(of, audioinjector_isolated_of_match);

static struct platform_driver audioinjector_isolated_driver = {
	.driver	= {
		.name			= "audioinjector-isolated",
		.owner			= THIS_MODULE,
		.of_match_table = audioinjector_isolated_of_match,
	},
	.probe	= audioinjector_isolated_probe,
};

module_platform_driver(audioinjector_isolated_driver);
MODULE_AUTHOR("Matt Flax <flatmax@flatmax.org>");
MODULE_DESCRIPTION("AudioInjector.net isolated Soundcard");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:audioinjector-isolated-soundcard");
