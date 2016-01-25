/*
 * ASoC Driver for IQaudIO Raspberry Pi Codec board
 *
 * Author:	Gordon Garrity <gordon@iqaudio.com>
 *		(C) Copyright IQaudio Limited, 2017-2019
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
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>

#include <linux/acpi.h>
#include <linux/slab.h>
#include "../codecs/da7213.h"

static int pll_out = DA7213_PLL_FREQ_OUT_90316800;

static int snd_rpi_iqaudio_pll_control(struct snd_soc_dapm_widget *w,
				       struct snd_kcontrol *k, int  event)
{
	int ret = 0;
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct snd_soc_pcm_runtime *rtd =
		snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);

	if (SND_SOC_DAPM_EVENT_OFF(event)) {
		ret = snd_soc_dai_set_pll(codec_dai, 0, DA7213_SYSCLK_MCLK, 0,
					  0);
		if (ret)
			dev_err(card->dev, "Failed to bypass PLL: %d\n", ret);
		/* Allow PLL time to bypass */
		msleep(100);
	} else if (SND_SOC_DAPM_EVENT_ON(event)) {
		ret = snd_soc_dai_set_pll(codec_dai, 0, DA7213_SYSCLK_PLL, 0,
					  pll_out);
		if (ret)
			dev_err(card->dev, "Failed to enable PLL: %d\n", ret);
		/* Allow PLL time to lock */
		msleep(100);
	}

	return ret;
}

static int snd_rpi_iqaudio_post_dapm_event(struct snd_soc_dapm_widget *w,
                              struct snd_kcontrol *kcontrol,
                              int event)
{
     switch (event) {
     case SND_SOC_DAPM_POST_PMU:
           /* Delay for mic bias ramp */
           msleep(1000);
           break;
     default:
           break;
     }

     return 0;
}

static const struct snd_kcontrol_new dapm_controls[] = {
	SOC_DAPM_PIN_SWITCH("HP Jack"),
	SOC_DAPM_PIN_SWITCH("MIC Jack"),
	SOC_DAPM_PIN_SWITCH("Onboard MIC"),
	SOC_DAPM_PIN_SWITCH("AUX Jack"),
};

static const struct snd_soc_dapm_widget dapm_widgets[] = {
	SND_SOC_DAPM_HP("HP Jack", NULL),
	SND_SOC_DAPM_MIC("MIC Jack", NULL),
	SND_SOC_DAPM_MIC("Onboard MIC", NULL),
	SND_SOC_DAPM_LINE("AUX Jack", NULL),
	SND_SOC_DAPM_SUPPLY("PLL Control", SND_SOC_NOPM, 0, 0,
			    snd_rpi_iqaudio_pll_control,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_POST("Post Power Up Event", snd_rpi_iqaudio_post_dapm_event),
};

static const struct snd_soc_dapm_route audio_map[] = {
	{"HP Jack", NULL, "HPL"},
	{"HP Jack", NULL, "HPR"},
	{"HP Jack", NULL, "PLL Control"},

	{"AUXR", NULL, "AUX Jack"},
	{"AUXL", NULL, "AUX Jack"},
	{"AUX Jack", NULL, "PLL Control"},

	/* Assume Mic1 is linked to Headset and Mic2 to on-board mic */
	{"MIC1", NULL, "MIC Jack"},
	{"MIC Jack", NULL, "PLL Control"},
	{"MIC2", NULL, "Onboard MIC"},
	{"Onboard MIC", NULL, "PLL Control"},
};

/* machine stream operations */

static int snd_rpi_iqaudio_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	int ret;

	/*
	 * Disable AUX Jack Pin by default to prevent PLL being enabled at
	 * startup. This avoids holding the PLL to a fixed SR config for
	 * subsequent streams.
	 *
	 * This pin can still be enabled later, as required by user-space.
	 */
	snd_soc_dapm_disable_pin(&rtd->card->dapm, "AUX Jack");
	snd_soc_dapm_sync(&rtd->card->dapm);

	/* Set bclk ratio to align with codec's BCLK rate */
	ret = snd_soc_dai_set_bclk_ratio(cpu_dai, 64);
	if (ret) {
		dev_err(rtd->dev, "Failed to set CPU BLCK ratio\n");
		return ret;
	}

	/* Set MCLK frequency to codec, onboard 11.2896MHz clock */
	return snd_soc_dai_set_sysclk(codec_dai, DA7213_CLKSRC_MCLK, 11289600,
				      SND_SOC_CLOCK_OUT);
}

static int snd_rpi_iqaudio_codec_hw_params(struct snd_pcm_substream *substream,
					   struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	unsigned int samplerate = params_rate(params);

	switch (samplerate) {
	case  8000:
	case 16000:
	case 32000:
	case 48000:
	case 96000:
		pll_out = DA7213_PLL_FREQ_OUT_98304000;
		break;
	case 44100:
	case 88200:
		pll_out = DA7213_PLL_FREQ_OUT_90316800;
		break;
	default:
		dev_err(rtd->dev,"Unsupported samplerate %d\n", samplerate);
		return -EINVAL;
	}

	return snd_soc_dai_set_pll(codec_dai, 0, DA7213_SYSCLK_PLL, 0, pll_out);
}

static const struct snd_soc_ops snd_rpi_iqaudio_codec_ops = {
	.hw_params = snd_rpi_iqaudio_codec_hw_params,
};

SND_SOC_DAILINK_DEFS(rpi_iqaudio,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2708-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("da7213.1-001a", "da7213-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2835-i2s.0")));

static struct snd_soc_dai_link snd_rpi_iqaudio_codec_dai[] = {
{
	.dai_fmt 		= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_CBM_CFM,
	.init			= snd_rpi_iqaudio_codec_init,
	.ops			= &snd_rpi_iqaudio_codec_ops,
	.symmetric_rate	= 1,
	.symmetric_channels	= 1,
	.symmetric_sample_bits	= 1,
	SND_SOC_DAILINK_REG(rpi_iqaudio),
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_iqaudio_codec = {
	.owner			= THIS_MODULE,
	.dai_link		= snd_rpi_iqaudio_codec_dai,
	.num_links		= ARRAY_SIZE(snd_rpi_iqaudio_codec_dai),
	.controls		= dapm_controls,
	.num_controls		= ARRAY_SIZE(dapm_controls),
	.dapm_widgets		= dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(dapm_widgets),
	.dapm_routes		= audio_map,
	.num_dapm_routes	= ARRAY_SIZE(audio_map),
};

static int snd_rpi_iqaudio_codec_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_iqaudio_codec.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_card *card = &snd_rpi_iqaudio_codec;
		struct snd_soc_dai_link *dai = &snd_rpi_iqaudio_codec_dai[0];

		i2s_node = of_parse_phandle(pdev->dev.of_node,
					    "i2s-controller", 0);
		if (i2s_node) {
			dai->cpus->dai_name = NULL;
			dai->cpus->of_node = i2s_node;
			dai->platforms->name = NULL;
			dai->platforms->of_node = i2s_node;
		}

		if (of_property_read_string(pdev->dev.of_node, "card_name",
					    &card->name))
			card->name = "IQaudIOCODEC";

		if (of_property_read_string(pdev->dev.of_node, "dai_name",
					    &dai->name))
			dai->name = "IQaudIO CODEC";

		if (of_property_read_string(pdev->dev.of_node,
					"dai_stream_name", &dai->stream_name))
			dai->stream_name = "IQaudIO CODEC HiFi v1.2";

	}

	ret = snd_soc_register_card(&snd_rpi_iqaudio_codec);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"snd_soc_register_card() failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int snd_rpi_iqaudio_codec_remove(struct platform_device *pdev)
{
	snd_soc_unregister_card(&snd_rpi_iqaudio_codec);
	return 0;
}

static const struct of_device_id iqaudio_of_match[] = {
	{ .compatible = "iqaudio,iqaudio-codec", },
	{},
};

MODULE_DEVICE_TABLE(of, iqaudio_of_match);

static struct platform_driver snd_rpi_iqaudio_codec_driver = {
	.driver = {
		.name   = "snd-rpi-iqaudio-codec",
		.owner  = THIS_MODULE,
		.of_match_table = iqaudio_of_match,
	},
	.probe          = snd_rpi_iqaudio_codec_probe,
	.remove         = snd_rpi_iqaudio_codec_remove,
};



module_platform_driver(snd_rpi_iqaudio_codec_driver);

MODULE_AUTHOR("Gordon Garrity <gordon@iqaudio.com>");
MODULE_DESCRIPTION("ASoC Driver for IQaudIO CODEC");
MODULE_LICENSE("GPL v2");
