/*
 * ASoC Driver for I-Sabre Q2M
 *
 * Author: Satoru Kawase
 * Modified by: Xiao Qingyong
 * Update kernel v4.18+ by : Audiophonics
 * 		Copyright 2018 Audiophonics
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "../codecs/i-sabre-codec.h"


static int snd_rpi_i_sabre_q2m_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component = asoc_rtd_to_codec(rtd, 0)->component;
	unsigned int value;

	/* Device ID */
	value = snd_soc_component_read(component, ISABRECODEC_REG_01);
	dev_info(component->card->dev, "Audiophonics Device ID : %02X\n", value);

	/* API revision */
	value = snd_soc_component_read(component, ISABRECODEC_REG_02);
	dev_info(component->card->dev, "Audiophonics API revision : %02X\n", value);

	return 0;
}

static int snd_rpi_i_sabre_q2m_hw_params(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd     = substream->private_data;
	struct snd_soc_dai         *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	int bclk_ratio;

	bclk_ratio = snd_pcm_format_physical_width(
			params_format(params)) * params_channels(params);
	return snd_soc_dai_set_bclk_ratio(cpu_dai, bclk_ratio);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_i_sabre_q2m_ops = {
	.hw_params = snd_rpi_i_sabre_q2m_hw_params,
};

SND_SOC_DAILINK_DEFS(rpi_i_sabre_q2m,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2708-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("i-sabre-codec-i2c.1-0048", "i-sabre-codec-dai")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2708-i2s.0")));

static struct snd_soc_dai_link snd_rpi_i_sabre_q2m_dai[] = {
	{
		.name           = "I-Sabre Q2M",
		.stream_name    = "I-Sabre Q2M DAC",
		.dai_fmt        = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
						| SND_SOC_DAIFMT_CBS_CFS,
		.init           = snd_rpi_i_sabre_q2m_init,
		.ops            = &snd_rpi_i_sabre_q2m_ops,
		SND_SOC_DAILINK_REG(rpi_i_sabre_q2m),
	}
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_i_sabre_q2m = {
	.name      = "I-Sabre Q2M DAC",
	.owner     = THIS_MODULE,
	.dai_link  = snd_rpi_i_sabre_q2m_dai,
	.num_links = ARRAY_SIZE(snd_rpi_i_sabre_q2m_dai)
};


static int snd_rpi_i_sabre_q2m_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_i_sabre_q2m.dev = &pdev->dev;
	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;

		dai = &snd_rpi_i_sabre_q2m_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
							"i2s-controller", 0);
		if (i2s_node) {
			dai->cpus->dai_name     = NULL;
			dai->cpus->of_node      = i2s_node;
			dai->platforms->name    = NULL;
			dai->platforms->of_node = i2s_node;
		} else {
			dev_err(&pdev->dev,
			    "Property 'i2s-controller' missing or invalid\n");
			return (-EINVAL);
		}

		dai->name        = "I-Sabre Q2M";
		dai->stream_name = "I-Sabre Q2M DAC";
		dai->dai_fmt     = SND_SOC_DAIFMT_I2S
					| SND_SOC_DAIFMT_NB_NF
					| SND_SOC_DAIFMT_CBS_CFS;
	}

	/* Wait for registering codec driver */
	mdelay(50);

	ret = snd_soc_register_card(&snd_rpi_i_sabre_q2m);
	if (ret) {
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);
	}

	return ret;
}

static int snd_rpi_i_sabre_q2m_remove(struct platform_device *pdev)
{
	snd_soc_unregister_card(&snd_rpi_i_sabre_q2m);
	return 0;
}

static const struct of_device_id snd_rpi_i_sabre_q2m_of_match[] = {
	{ .compatible = "audiophonics,i-sabre-q2m", },
	{}
};
MODULE_DEVICE_TABLE(of, snd_rpi_i_sabre_q2m_of_match);

static struct platform_driver snd_rpi_i_sabre_q2m_driver = {
	.driver = {
		.name           = "snd-rpi-i-sabre-q2m",
		.owner          = THIS_MODULE,
		.of_match_table = snd_rpi_i_sabre_q2m_of_match,
	},
	.probe  = snd_rpi_i_sabre_q2m_probe,
	.remove = snd_rpi_i_sabre_q2m_remove,
};
module_platform_driver(snd_rpi_i_sabre_q2m_driver);

MODULE_DESCRIPTION("ASoC Driver for I-Sabre Q2M");
MODULE_AUTHOR("Audiophonics <http://www.audiophonics.fr>");
MODULE_LICENSE("GPL");
