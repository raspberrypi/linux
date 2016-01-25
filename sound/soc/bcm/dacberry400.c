/*
 * ASoC Driver for Dacberry400 soundcard
 * Author:
 *      Ashish Vara<ashishhvara@gmail.com>
 *      Copyright 2022
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
#include <linux/i2c.h>
#include <linux/acpi.h>
#include <linux/slab.h>
#include "../sound/soc/codecs/tlv320aic3x.h"

static const struct snd_kcontrol_new dacberry400_controls[] = {
	SOC_DAPM_PIN_SWITCH("MIC Jack"),
	SOC_DAPM_PIN_SWITCH("Line In"),
	SOC_DAPM_PIN_SWITCH("Line Out"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
};

static const struct snd_soc_dapm_widget dacberry400_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("MIC Jack", NULL),
	SND_SOC_DAPM_LINE("Line In", NULL),
	SND_SOC_DAPM_LINE("Line Out", NULL),
};

static const struct snd_soc_dapm_route dacberry400_audio_map[] = {
	{"Headphone Jack", NULL, "HPLOUT"},
	{"Headphone Jack", NULL, "HPROUT"},

	{"LINE1L", NULL, "Line In"},
	{"LINE1R", NULL, "Line In"},

	{"Line Out", NULL, "LLOUT"},
	{"Line Out", NULL, "RLOUT"},

	{"MIC3L", NULL, "MIC Jack"},
	{"MIC3R", NULL, "MIC Jack"},
};

static int snd_rpi_dacberry400_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	struct snd_soc_component *component = codec_dai->component;
	int ret;

	ret = snd_soc_dai_set_sysclk(codec_dai, 2, 12000000,
					SND_SOC_CLOCK_OUT);

	if (ret && ret != -ENOTSUPP)
		goto err;

	snd_soc_component_write(component, HPRCOM_CFG, 0x20);
	snd_soc_component_write(component, DACL1_2_HPLOUT_VOL, 0x80);
	snd_soc_component_write(component, DACR1_2_HPROUT_VOL, 0x80);
err:
	return ret;
}

static int snd_rpi_dacberry400_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *codec_dai;
	struct snd_soc_component *component;
	struct dacberry_priv *aic3x;
	u8 hpcom_reg = 0;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	codec_dai = asoc_rtd_to_codec(rtd, 0);
	component = codec_dai->component;
	aic3x = snd_soc_component_get_drvdata(component);
	if (dapm->dev != codec_dai->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		if (dapm->bias_level != SND_SOC_BIAS_STANDBY)
			break;
		/* UNMUTE ADC/DAC */
		hpcom_reg = snd_soc_component_read(component, HPLCOM_CFG);
		snd_soc_component_write(component, HPLCOM_CFG, hpcom_reg | 0x20);
		snd_soc_component_write(component, LINE1R_2_RADC_CTRL, 0x04);
		snd_soc_component_write(component, LINE1L_2_LADC_CTRL, 0x04);
		snd_soc_component_write(component, LADC_VOL, 0x00);
		snd_soc_component_write(component, RADC_VOL, 0x00);
		pr_info("%s: unmute ADC/DAC\n", __func__);
		break;

	case SND_SOC_BIAS_STANDBY:
		if (dapm->bias_level != SND_SOC_BIAS_PREPARE)
			break;
		/* MUTE ADC/DAC */
		snd_soc_component_write(component, LDAC_VOL, 0x80);
		snd_soc_component_write(component, RDAC_VOL, 0x80);
		snd_soc_component_write(component, LADC_VOL, 0x80);
		snd_soc_component_write(component, RADC_VOL, 0x80);
		snd_soc_component_write(component, LINE1R_2_RADC_CTRL, 0x00);
		snd_soc_component_write(component, LINE1L_2_LADC_CTRL, 0x00);
		snd_soc_component_write(component, HPLCOM_CFG, 0x00);
		pr_info("%s: mute ADC/DAC\n", __func__);
		break;
	default:
		break;
	}

	return 0;
}

static int snd_rpi_dacberry400_hw_params(struct snd_pcm_substream *substream,
					   struct snd_pcm_hw_params *params)
{
	int ret = 0;
	u8 data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	struct snd_soc_component *component = codec_dai->component;
	int fsref = (params_rate(params) % 11025 == 0) ? 44100 : 48000;
	int channels = params_channels(params);
	int width = 32;
	u8 clock = 0;

	data = (LDAC2LCH | RDAC2RCH);
	data |= (fsref == 44100) ? FSREF_44100 : FSREF_48000;
	if (params_rate(params) >= 64000)
		data |= DUAL_RATE_MODE;
	ret = snd_soc_component_write(component, 0x7, data);
	width = params_width(params);

	clock = snd_soc_component_read(component, 2);

	ret = snd_soc_dai_set_bclk_ratio(cpu_dai, channels*width);

	return ret;
}

static const struct snd_soc_ops snd_rpi_dacberry400_ops = {
	.hw_params = snd_rpi_dacberry400_hw_params,
};


SND_SOC_DAILINK_DEFS(rpi_dacberry400,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2835-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("tlv320aic3x.1-0018", "tlv320aic3x-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2835-i2s.0")));

static struct snd_soc_dai_link snd_rpi_dacberry400_dai[] = {
{
	.dai_fmt		= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBS_CFS,
	.init			= snd_rpi_dacberry400_init,
	.ops			= &snd_rpi_dacberry400_ops,
	.symmetric_rate		= 1,
	SND_SOC_DAILINK_REG(rpi_dacberry400),
},
};

static struct snd_soc_card snd_rpi_dacberry400 = {
	.owner			= THIS_MODULE,
	.dai_link		= snd_rpi_dacberry400_dai,
	.num_links		= ARRAY_SIZE(snd_rpi_dacberry400_dai),
	.controls		= dacberry400_controls,
	.num_controls		= ARRAY_SIZE(dacberry400_controls),
	.dapm_widgets		= dacberry400_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(dacberry400_widgets),
	.dapm_routes		= dacberry400_audio_map,
	.num_dapm_routes	= ARRAY_SIZE(dacberry400_audio_map),
	.set_bias_level		= snd_rpi_dacberry400_set_bias_level,
};

static int snd_rpi_dacberry400_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_dacberry400.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_card *card = &snd_rpi_dacberry400;
		struct snd_soc_dai_link *dai = &snd_rpi_dacberry400_dai[0];

		i2s_node = of_parse_phandle(pdev->dev.of_node,
					    "i2s-controller", 0);
		if (i2s_node) {
			dai->cpus->dai_name = NULL;
			dai->cpus->of_node = i2s_node;
			dai->platforms->name = NULL;
			dai->platforms->of_node = i2s_node;
			of_node_put(i2s_node);
		}

		if (of_property_read_string(pdev->dev.of_node, "card_name",
					    &card->name))
			card->name = "tlvaudioCODEC";

		if (of_property_read_string(pdev->dev.of_node, "dai_name",
					    &dai->name))
			dai->name = "tlvaudio CODEC";

	}

	ret = snd_soc_register_card(&snd_rpi_dacberry400);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"snd_soc_register_card() failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int snd_rpi_dacberry400_remove(struct platform_device *pdev)
{
	snd_soc_unregister_card(&snd_rpi_dacberry400);
	return 0;
}

static const struct of_device_id dacberry400_match_id[] = {
	{ .compatible = "osaelectronics,dacberry400",},
	{},
};
MODULE_DEVICE_TABLE(of, dacberry400_match_id);

static struct platform_driver snd_rpi_dacberry400_driver = {
	.driver = {
		.name = "snd-rpi-dacberry400",
		.owner = THIS_MODULE,
		.of_match_table = dacberry400_match_id,
	},
	.probe = snd_rpi_dacberry400_probe,
	.remove = snd_rpi_dacberry400_remove,
};

module_platform_driver(snd_rpi_dacberry400_driver);

MODULE_AUTHOR("Ashish Vara");
MODULE_DESCRIPTION("Dacberry400 sound card driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:dacberry400");
MODULE_SOFTDEP("pre: snd-soc-tlv320aic3x");
