// SPDX-License-Identifier: GPL-2.0
/*
 * rpi--wm8804.c -- ALSA SoC Raspberry Pi soundcard.
 *
 * Authors: Johannes Krude <johannes@krude.de
 *
 * Driver for when connecting simultaneously justboom-digi and justboom-dac
 *
 * Based upon code from:
 * justboom-digi.c
 * by Milan Neskovic <info@justboom.co>
 * justboom-dac.c
 * by Milan Neskovic <info@justboom.co>
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

#include "../codecs/wm8804.h"
#include "../codecs/pcm512x.h"


static bool digital_gain_0db_limit = true;

static int snd_rpi_justboom_both_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *digi = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_component *dac = snd_soc_rtd_to_codec(rtd, 1)->component;

	/* enable  TX output */
	snd_soc_component_update_bits(digi, WM8804_PWRDN, 0x4, 0x0);

	snd_soc_component_update_bits(dac, PCM512x_GPIO_EN, 0x08, 0x08);
	snd_soc_component_update_bits(dac, PCM512x_GPIO_OUTPUT_4, 0xf, 0x02);
	snd_soc_component_update_bits(dac, PCM512x_GPIO_CONTROL_1, 0x08, 0x08);

	if (digital_gain_0db_limit) {
		int ret;
		struct snd_soc_card *card = rtd->card;

		ret = snd_soc_limit_volume(card, "Digital Playback Volume",
									207);
		if (ret < 0)
			dev_warn(card->dev, "Failed to set volume limit: %d\n",
									ret);
	}

	return 0;
}

static int snd_rpi_justboom_both_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_component *digi = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	int sysclk = 27000000; /* This is fixed on this board */

	long mclk_freq    = 0;
	int mclk_div      = 1;
	int sampling_freq = 1;

	int ret;

	int samplerate = params_rate(params);

	if (samplerate <= 96000) {
		mclk_freq = samplerate*256;
		mclk_div  = WM8804_MCLKDIV_256FS;
	} else {
		mclk_freq = samplerate*128;
		mclk_div  = WM8804_MCLKDIV_128FS;
	}

	switch (samplerate) {
	case 32000:
		sampling_freq = 0x03;
		break;
	case 44100:
		sampling_freq = 0x00;
		break;
	case 48000:
		sampling_freq = 0x02;
		break;
	case 88200:
		sampling_freq = 0x08;
		break;
	case 96000:
		sampling_freq = 0x0a;
		break;
	case 176400:
		sampling_freq = 0x0c;
		break;
	case 192000:
		sampling_freq = 0x0e;
		break;
	default:
		dev_err(rtd->card->dev,
		"Failed to set WM8804 SYSCLK, unsupported samplerate %d\n",
		samplerate);
	}

	snd_soc_dai_set_clkdiv(codec_dai, WM8804_MCLK_DIV, mclk_div);
	snd_soc_dai_set_pll(codec_dai, 0, 0, sysclk, mclk_freq);

	ret = snd_soc_dai_set_sysclk(codec_dai, WM8804_TX_CLKSRC_PLL,
					sysclk, SND_SOC_CLOCK_OUT);
	if (ret < 0) {
		dev_err(rtd->card->dev,
		"Failed to set WM8804 SYSCLK: %d\n", ret);
		return ret;
	}

	/* Enable TX output */
	snd_soc_component_update_bits(digi, WM8804_PWRDN, 0x4, 0x0);

	/* Power on */
	snd_soc_component_update_bits(digi, WM8804_PWRDN, 0x9, 0);

	/* set sampling frequency status bits */
	snd_soc_component_update_bits(digi, WM8804_SPDTX4, 0x0f, sampling_freq);

	return snd_soc_dai_set_bclk_ratio(cpu_dai, 64);
}

static int snd_rpi_justboom_both_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *digi = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_component *dac = snd_soc_rtd_to_codec(rtd, 1)->component;

	/* turn on digital output */
	snd_soc_component_update_bits(digi, WM8804_PWRDN, 0x3c, 0x00);

	snd_soc_component_update_bits(dac, PCM512x_GPIO_CONTROL_1, 0x08, 0x08);

	return 0;
}

static void snd_rpi_justboom_both_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *digi = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_component *dac = snd_soc_rtd_to_codec(rtd, 1)->component;

	snd_soc_component_update_bits(dac, PCM512x_GPIO_CONTROL_1, 0x08, 0x00);

	/* turn off output */
	snd_soc_component_update_bits(digi, WM8804_PWRDN, 0x3c, 0x3c);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_justboom_both_ops = {
	.hw_params = snd_rpi_justboom_both_hw_params,
	.startup   = snd_rpi_justboom_both_startup,
	.shutdown  = snd_rpi_justboom_both_shutdown,
};

SND_SOC_DAILINK_DEFS(rpi_justboom_both,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2708-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("pcm512x.1-004d", "pcm512x-hifi"),
			   COMP_CODEC("wm8804.1-003b", "wm8804-spdif")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2708-i2s.0")));

static struct snd_soc_dai_link snd_rpi_justboom_both_dai[] = {
{
	.name           = "JustBoom Digi",
	.stream_name    = "JustBoom Digi HiFi",
	.dai_fmt        = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBM_CFM,
	.ops            = &snd_rpi_justboom_both_ops,
	.init           = snd_rpi_justboom_both_init,
	SND_SOC_DAILINK_REG(rpi_justboom_both),
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_justboom_both = {
	.name             = "snd_rpi_justboom_both",
	.driver_name      = "JustBoomBoth",
	.owner            = THIS_MODULE,
	.dai_link         = snd_rpi_justboom_both_dai,
	.num_links        = ARRAY_SIZE(snd_rpi_justboom_both_dai),
};

static int snd_rpi_justboom_both_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct snd_soc_card *card = &snd_rpi_justboom_both;

	snd_rpi_justboom_both.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai = &snd_rpi_justboom_both_dai[0];

		i2s_node = of_parse_phandle(pdev->dev.of_node,
					    "i2s-controller", 0);

		if (i2s_node) {
			int i;

			for (i = 0; i < card->num_links; i++) {
				dai->cpus->dai_name = NULL;
				dai->cpus->of_node = i2s_node;
				dai->platforms->name = NULL;
				dai->platforms->of_node = i2s_node;
			}
		}

		digital_gain_0db_limit = !of_property_read_bool(
			pdev->dev.of_node, "justboom,24db_digital_gain");
	}

	ret = snd_soc_register_card(card);
	if (ret && ret != -EPROBE_DEFER) {
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);
	}

	return ret;
}

static int snd_rpi_justboom_both_remove(struct platform_device *pdev)
{
	snd_soc_unregister_card(&snd_rpi_justboom_both);
	return 0;
}

static const struct of_device_id snd_rpi_justboom_both_of_match[] = {
	{ .compatible = "justboom,justboom-both", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_justboom_both_of_match);

static struct platform_driver snd_rpi_justboom_both_driver = {
	.driver = {
		.name   = "snd-rpi-justboom-both",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_justboom_both_of_match,
	},
	.probe          = snd_rpi_justboom_both_probe,
	.remove         = snd_rpi_justboom_both_remove,
};

module_platform_driver(snd_rpi_justboom_both_driver);

MODULE_AUTHOR("Johannes Krude <johannes@krude.de>");
MODULE_DESCRIPTION("ASoC Driver for simultaneous use of JustBoom PI Digi & DAC HAT Sound Cards");
MODULE_LICENSE("GPL v2");
