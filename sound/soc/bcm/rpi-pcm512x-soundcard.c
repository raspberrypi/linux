// SPDX-License-Identifier: GPL-2.0
/*
 * rpi-pcm512x.c -- ALSA SoC Raspberry Pi soundcard.
 *
 * Copyright (C) 2018 Raspberry Pi.
 *
 * Authors: Tim Gover <tim.gover@raspberrypi.org>
 *
 * Generic driver for Pi Hat PCM512x DAC sound cards
 *
 * Based upon code from:
 *
 * allo-piano-dac.c
 * by Baswaraj K <jaikumar@cem-solutions.net>
 * based on code by Florian Meier <florian.meier@koalo.de>
 *
 * dionaudio_loco-v2.c
 * by Miquel Blauw <info@dionaudio.nl>
 *
 * justboom-dac.c
 * by Milan Neskovic <info@justboom.co>
 *
 * iqaudio-dac.c
 * Florian Meier <florian.meier@koalo.de>
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

#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/module.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../codecs/pcm512x.h"

/* Parameters for generic functions */
struct snd_rpi_pcm512x_drvdata {
	/* Required - pointer to the DAI structure */
	struct snd_soc_dai_link *dai;
	/* Required - snd_soc_card name */
	const char *card_name;
	/* Optional DT node names if card info is defined in DT */
	const char *card_name_dt;
	const char *dai_name_dt;
	const char *dai_stream_name_dt;
	const char *digital_gain_0db_name_dt;
	/* Optional probe extension - called prior to register_card */
	int (*probe)(struct platform_device *pdev, struct snd_soc_card *card);
};

static bool digital_gain_0db_limit = true;

static int snd_rpi_pcm512x_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret;
	struct snd_soc_card *card = rtd->card;

	if (!digital_gain_0db_limit)
		return 0;

	ret = snd_soc_limit_volume(card, "Digital Playback Volume", 207);
	if (ret < 0)
		dev_warn(card->dev, "Failed to set volume limit: %d\n", ret);
	return 0;
}

static struct snd_soc_dai_link snd_allo_piano_dac_dai[] = {
{
	.name        = "Piano DAC",
	.stream_name = "Piano DAC HiFi",
},
};

static struct snd_rpi_pcm512x_drvdata drvdata_allo_piano_dac = {
	.card_name                = "PianoDAC",
	.dai                      = snd_allo_piano_dac_dai,
	.digital_gain_0db_name_dt = "allo,24db_digital_gain",
};

static struct snd_soc_dai_link snd_dion_audio_loco_v2_dai[] = {
{
	.name        = "DionAudio LOCO-V2",
	.stream_name = "DionAudio LOCO-V2 DAC-AMP",
	.codec_name  = "pcm512x.1-004d",
},
};

static struct snd_rpi_pcm512x_drvdata drvdata_dionaudio_loco_v2 = {
	.card_name                = "Dion Audio LOCO-V2",
	.dai                      = snd_dion_audio_loco_v2_dai,
	.digital_gain_0db_name_dt = "dionaudio,24db_digital_gain",
};

static struct gpio_desc *mute_gpio;
static void snd_rpi_iqaudio_gpio_mute(struct snd_soc_card *card)
{
	if (mute_gpio) {
		dev_info(card->dev, "%s: muting amp using GPIO22\n",
			 __func__);
		gpiod_set_value_cansleep(mute_gpio, 0);
	}
}

static void snd_rpi_iqaudio_gpio_unmute(struct snd_soc_card *card)
{
	if (mute_gpio) {
		dev_info(card->dev, "%s: un-muting amp using GPIO22\n",
			 __func__);
		gpiod_set_value_cansleep(mute_gpio, 1);
	}
}

static int snd_rpi_iqaudio_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *codec_dai;

	rtd = snd_soc_get_pcm_runtime(card, card->dai_link[0].name);
	codec_dai = rtd->codec_dai;

	if (dapm->dev != codec_dai->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		if (dapm->bias_level != SND_SOC_BIAS_STANDBY)
			break;

		/* UNMUTE AMP */
		snd_rpi_iqaudio_gpio_unmute(card);

		break;
	case SND_SOC_BIAS_STANDBY:
		if (dapm->bias_level != SND_SOC_BIAS_PREPARE)
			break;

		/* MUTE AMP */
		snd_rpi_iqaudio_gpio_mute(card);

		break;
	default:
		break;
	}

	return 0;
}

static int snd_rpi_iqaudio_dac_probe(struct platform_device *pdev,
		struct snd_soc_card *card)
{
	bool gpio_unmute;
	bool auto_gpio_mute;

	/* gpio_unmute - one time unmute amp using GPIO */
	gpio_unmute = of_property_read_bool(pdev->dev.of_node,
			"iqaudio-dac,unmute-amp");

	/* auto_gpio_mute - mute/unmute amp using GPIO */
	auto_gpio_mute = of_property_read_bool(pdev->dev.of_node,
			"iqaudio-dac,auto-mute-amp");

	if (auto_gpio_mute || gpio_unmute) {
		mute_gpio = devm_gpiod_get_optional(&pdev->dev, "mute",
				GPIOD_OUT_LOW);
		if (IS_ERR(mute_gpio)) {
			int ret = PTR_ERR(mute_gpio);

			dev_err(&pdev->dev,
					"Failed to get mute gpio: %d\n", ret);
			return ret;
		}

		if (auto_gpio_mute && mute_gpio)
			card->set_bias_level = snd_rpi_iqaudio_set_bias_level;
	}
	return 0;
}

static struct snd_soc_dai_link snd_iqaudio_dac_dai[] = {
{
	.name        = "IQaudIO DAC",
	.stream_name = "IQaudIO DAC HiFi",
},
};

static struct snd_rpi_pcm512x_drvdata drvdata_iqaudio_dac = {
	.card_name                = "IQaudIO DAC",
	.dai                      = snd_iqaudio_dac_dai,
	.digital_gain_0db_name_dt = "iqaudio,24db_digital_gain",
	.card_name_dt             = "card_name",
	.dai_name_dt              = "dai_name",
	.dai_stream_name_dt       = "dai_stream_name",
	.probe                    = snd_rpi_iqaudio_dac_probe,
};

static int snd_rpi_justboom_dac_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *comp = rtd->codec_dai->component;

	snd_soc_component_update_bits(comp, PCM512x_GPIO_EN, 0x08, 0x08);
	snd_soc_component_update_bits(comp, PCM512x_GPIO_OUTPUT_4, 0xf, 0x02);
	snd_soc_component_update_bits(comp, PCM512x_GPIO_CONTROL_1, 0x08, 0x08);

	return snd_rpi_pcm512x_init(rtd);
}

static int snd_rpi_justboom_dac_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *comp = rtd->codec_dai->component;

	snd_soc_component_update_bits(comp, PCM512x_GPIO_CONTROL_1, 0x08, 0x08);
	return 0;
}

static void snd_rpi_justboom_dac_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *comp = rtd->codec_dai->component;

	snd_soc_component_update_bits(comp, PCM512x_GPIO_CONTROL_1, 0x08, 0x00);
};

static struct snd_soc_ops snd_rpi_justboom_dac_ops = {
	.startup = snd_rpi_justboom_dac_startup,
	.shutdown = snd_rpi_justboom_dac_shutdown,
};

static struct snd_soc_dai_link snd_justboom_dac_dai[] = {
{
	.name        = "JustBoom DAC",
	.stream_name = "JustBoom DAC HiFi",
	.codec_name  = "pcm512x.1-004d",
	.ops         = &snd_rpi_justboom_dac_ops,
	.init        = snd_rpi_justboom_dac_init,
},
};

static struct snd_rpi_pcm512x_drvdata drvdata_justboom_dac = {
	.card_name                = "snd_rpi_justboom_dac",
	.dai                      = snd_justboom_dac_dai,
	.digital_gain_0db_name_dt = "justboom,24db_digital_gain",
};

static const struct of_device_id snd_rpi_pcm512x_of_match[] = {
	{ .compatible = "allo,allo-piano-dac",
		.data = (void *) &drvdata_allo_piano_dac },
	{ .compatible = "dionaudio,dionaudio-loco-v2",
		.data = (void *) &drvdata_dionaudio_loco_v2 },
	{ .compatible = "iqaudio,iqaudio-dac",
		.data = (void *) &drvdata_iqaudio_dac },
	{ .compatible = "justboom,justboom-dac",
		.data = (void *) &drvdata_justboom_dac },
	{},
};

static struct snd_soc_card snd_rpi_pcm512x = {
	.driver_name  = "RPi-PCM512x",
	.owner        = THIS_MODULE,
	.dai_link     = NULL,
	.num_links    = 1,
};

static int snd_rpi_pcm512x_probe(struct platform_device *pdev)
{
	int ret = 0;
	const struct of_device_id *of_id;

	snd_rpi_pcm512x.dev = &pdev->dev;
	of_id = of_match_node(snd_rpi_pcm512x_of_match, pdev->dev.of_node);

	if (pdev->dev.of_node && of_id->data) {
		struct device_node *i2s_node;
		struct snd_rpi_pcm512x_drvdata *drvdata =
			(struct snd_rpi_pcm512x_drvdata *) of_id->data;
		struct snd_soc_dai_link *dai = drvdata->dai;

		snd_soc_card_set_drvdata(&snd_rpi_pcm512x, drvdata);

		if (!dai->init)
			dai->init = snd_rpi_pcm512x_init;
		if (!dai->codec_dai_name)
			dai->codec_dai_name = "pcm512x-hifi";
		if (!dai->codec_name)
			dai->codec_name = "pcm512x.1-004c";
		if (!dai->dai_fmt)
			dai->dai_fmt = SND_SOC_DAIFMT_I2S |
				SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS;

		snd_rpi_pcm512x.dai_link = dai;
		i2s_node = of_parse_phandle(pdev->dev.of_node,
				"i2s-controller", 0);
		if (!i2s_node) {
			pr_err("Failed to find i2s-controller DT node\n");
			return -ENODEV;
		}

		if (drvdata->digital_gain_0db_name_dt)
			digital_gain_0db_limit = !of_property_read_bool(
					pdev->dev.of_node,
					drvdata->digital_gain_0db_name_dt);

		snd_rpi_pcm512x.name = drvdata->card_name;

		/* If requested by in drvdata get card & DAI names from DT */
		if (drvdata->card_name_dt)
			of_property_read_string(i2s_node,
					drvdata->card_name_dt,
					&snd_rpi_pcm512x.name);

		if (drvdata->dai_name_dt)
			of_property_read_string(i2s_node,
					drvdata->dai_name_dt,
					&dai->name);

		if (drvdata->dai_stream_name_dt)
			of_property_read_string(i2s_node,
					drvdata->dai_stream_name_dt,
					&dai->stream_name);

		dai->cpu_of_node = i2s_node;
		dai->platform_of_node = i2s_node;

		if (drvdata->probe) {
			ret = drvdata->probe(pdev, &snd_rpi_pcm512x);
			if (ret < 0) {
				dev_err(&pdev->dev, "Custom probe failed %d\n",
						ret);
				return ret;
			}
		}

		pr_debug("%s card: %s dai: %s stream: %s\n", __func__,
				snd_rpi_pcm512x.name,
				dai->name, dai->stream_name);
	}

	ret = devm_snd_soc_register_card(&pdev->dev, &snd_rpi_pcm512x);
	if (ret && ret != -EPROBE_DEFER)
		dev_err(&pdev->dev, "Failed to register card %d\n", ret);

	return ret;
}

static struct platform_driver snd_rpi_pcm512x_driver = {
	.driver = {
		.name           = "snd-rpi-pcm512x",
		.owner          = THIS_MODULE,
		.of_match_table = snd_rpi_pcm512x_of_match,
	},
	.probe  = snd_rpi_pcm512x_probe,
};
MODULE_DEVICE_TABLE(of, snd_rpi_pcm512x_of_match);

module_platform_driver(snd_rpi_pcm512x_driver);

MODULE_AUTHOR("Tim Gover <tim.gover@raspberrypi.org>");
MODULE_DESCRIPTION("ASoC Raspberry Pi Hat generic DAC driver for PCM512x based cards");
MODULE_LICENSE("GPL v2");
