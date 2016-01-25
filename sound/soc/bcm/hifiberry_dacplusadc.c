/*
 * ASoC Driver for HiFiBerry DAC+ / DAC Pro with ADC
 *
 * Author:	Daniel Matuschek, Stuart MacLean <stuart@hifiberry.com>
 *		Copyright 2014-2015
 *		based on code by Florian Meier <florian.meier@koalo.de>
 *		ADC added by Joerg Schambacher <joscha@schambacher.com>
 *		Copyright 2018
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
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>

#include "../codecs/pcm512x.h"

#define HIFIBERRY_DACPRO_NOCLOCK 0
#define HIFIBERRY_DACPRO_CLK44EN 1
#define HIFIBERRY_DACPRO_CLK48EN 2

struct platform_device *dmic_codec_dev;

struct pcm512x_priv {
	struct regmap *regmap;
	struct clk *sclk;
};

/* Clock rate of CLK44EN attached to GPIO6 pin */
#define CLK_44EN_RATE 22579200UL
/* Clock rate of CLK48EN attached to GPIO3 pin */
#define CLK_48EN_RATE 24576000UL

static bool slave;
static bool snd_rpi_hifiberry_is_dacpro;
static bool digital_gain_0db_limit = true;
static bool leds_off;

static void snd_rpi_hifiberry_dacplusadc_select_clk(struct snd_soc_component *component,
	int clk_id)
{
	switch (clk_id) {
	case HIFIBERRY_DACPRO_NOCLOCK:
		snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1, 0x24, 0x00);
		break;
	case HIFIBERRY_DACPRO_CLK44EN:
		snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1, 0x24, 0x20);
		break;
	case HIFIBERRY_DACPRO_CLK48EN:
		snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1, 0x24, 0x04);
		break;
	}
}

static void snd_rpi_hifiberry_dacplusadc_clk_gpio(struct snd_soc_component *component)
{
	snd_soc_component_update_bits(component, PCM512x_GPIO_EN, 0x24, 0x24);
	snd_soc_component_update_bits(component, PCM512x_GPIO_OUTPUT_3, 0x0f, 0x02);
	snd_soc_component_update_bits(component, PCM512x_GPIO_OUTPUT_6, 0x0f, 0x02);
}

static bool snd_rpi_hifiberry_dacplusadc_is_sclk(struct snd_soc_component *component)
{
	unsigned int sck;

	sck = snd_soc_component_read(component, PCM512x_RATE_DET_4);
	return (!(sck & 0x40));
}

static bool snd_rpi_hifiberry_dacplusadc_is_sclk_sleep(
	struct snd_soc_component *component)
{
	msleep(2);
	return snd_rpi_hifiberry_dacplusadc_is_sclk(component);
}

static bool snd_rpi_hifiberry_dacplusadc_is_pro_card(struct snd_soc_component *component)
{
	bool isClk44EN, isClk48En, isNoClk;

	snd_rpi_hifiberry_dacplusadc_clk_gpio(component);

	snd_rpi_hifiberry_dacplusadc_select_clk(component, HIFIBERRY_DACPRO_CLK44EN);
	isClk44EN = snd_rpi_hifiberry_dacplusadc_is_sclk_sleep(component);

	snd_rpi_hifiberry_dacplusadc_select_clk(component, HIFIBERRY_DACPRO_NOCLOCK);
	isNoClk = snd_rpi_hifiberry_dacplusadc_is_sclk_sleep(component);

	snd_rpi_hifiberry_dacplusadc_select_clk(component, HIFIBERRY_DACPRO_CLK48EN);
	isClk48En = snd_rpi_hifiberry_dacplusadc_is_sclk_sleep(component);

	return (isClk44EN && isClk48En && !isNoClk);
}

static int snd_rpi_hifiberry_dacplusadc_clk_for_rate(int sample_rate)
{
	int type;

	switch (sample_rate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
	case 176400:
	case 352800:
		type = HIFIBERRY_DACPRO_CLK44EN;
		break;
	default:
		type = HIFIBERRY_DACPRO_CLK48EN;
		break;
	}
	return type;
}

static void snd_rpi_hifiberry_dacplusadc_set_sclk(struct snd_soc_component *component,
	int sample_rate)
{
	struct pcm512x_priv *pcm512x = snd_soc_component_get_drvdata(component);

	if (!IS_ERR(pcm512x->sclk)) {
		int ctype;

		ctype = snd_rpi_hifiberry_dacplusadc_clk_for_rate(sample_rate);
		clk_set_rate(pcm512x->sclk, (ctype == HIFIBERRY_DACPRO_CLK44EN)
			? CLK_44EN_RATE : CLK_48EN_RATE);
		snd_rpi_hifiberry_dacplusadc_select_clk(component, ctype);
	}
}

static int snd_rpi_hifiberry_dacplusadc_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct pcm512x_priv *priv;

	if (slave)
		snd_rpi_hifiberry_is_dacpro = false;
	else
		snd_rpi_hifiberry_is_dacpro =
				snd_rpi_hifiberry_dacplusadc_is_pro_card(component);

	if (snd_rpi_hifiberry_is_dacpro) {
		struct snd_soc_dai_link *dai = rtd->dai_link;

		dai->name = "HiFiBerry ADCDAC+ Pro";
		dai->stream_name = "HiFiBerry ADCDAC+ Pro HiFi";
		dai->dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
			| SND_SOC_DAIFMT_CBM_CFM;

		snd_soc_component_update_bits(component, PCM512x_BCLK_LRCLK_CFG, 0x31, 0x11);
		snd_soc_component_update_bits(component, PCM512x_MASTER_MODE, 0x03, 0x03);
		snd_soc_component_update_bits(component, PCM512x_MASTER_CLKDIV_2, 0x7f, 63);
	} else {
		priv = snd_soc_component_get_drvdata(component);
		priv->sclk = ERR_PTR(-ENOENT);
	}

	snd_soc_component_update_bits(component, PCM512x_GPIO_EN, 0x08, 0x08);
	snd_soc_component_update_bits(component, PCM512x_GPIO_OUTPUT_4, 0x0f, 0x02);
	if (leds_off)
		snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1, 0x08, 0x00);
	else
		snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1, 0x08, 0x08);

	if (digital_gain_0db_limit) {
		int ret;
		struct snd_soc_card *card = rtd->card;

		ret = snd_soc_limit_volume(card, "Digital Playback Volume", 207);
		if (ret < 0)
			dev_warn(card->dev, "Failed to set volume limit: %d\n", ret);
	}

	return 0;
}

static int snd_rpi_hifiberry_dacplusadc_update_rate_den(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct pcm512x_priv *pcm512x = snd_soc_component_get_drvdata(component);
	struct snd_ratnum *rats_no_pll;
	unsigned int num = 0, den = 0;
	int err;

	rats_no_pll = devm_kzalloc(rtd->dev, sizeof(*rats_no_pll), GFP_KERNEL);
	if (!rats_no_pll)
		return -ENOMEM;

	rats_no_pll->num = clk_get_rate(pcm512x->sclk) / 64;
	rats_no_pll->den_min = 1;
	rats_no_pll->den_max = 128;
	rats_no_pll->den_step = 1;

	err = snd_interval_ratnum(hw_param_interval(params,
		SNDRV_PCM_HW_PARAM_RATE), 1, rats_no_pll, &num, &den);
	if (err >= 0 && den) {
		params->rate_num = num;
		params->rate_den = den;
	}

	devm_kfree(rtd->dev, rats_no_pll);
	return 0;
}

static int snd_rpi_hifiberry_dacplusadc_hw_params(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int channels = params_channels(params);
	int width = snd_pcm_format_width(params_format(params));

	if (snd_rpi_hifiberry_is_dacpro) {
		struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;

		snd_rpi_hifiberry_dacplusadc_set_sclk(component,
			params_rate(params));

		ret = snd_rpi_hifiberry_dacplusadc_update_rate_den(
			substream, params);
	}

	ret = snd_soc_dai_set_bclk_ratio(snd_soc_rtd_to_cpu(rtd, 0), channels * width);
	if (ret)
		return ret;
	ret = snd_soc_dai_set_bclk_ratio(snd_soc_rtd_to_codec(rtd, 0), channels * width);
	return ret;
}

static int hifiberry_dacplusadc_LED_cnt;

static int snd_rpi_hifiberry_dacplusadc_startup(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;

	if (leds_off)
		return 0;
	snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1,
					 0x08, 0x08);
	hifiberry_dacplusadc_LED_cnt++;
	return 0;
}

static void snd_rpi_hifiberry_dacplusadc_shutdown(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;

	hifiberry_dacplusadc_LED_cnt--;
	if (!hifiberry_dacplusadc_LED_cnt)
		snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1,
						 0x08, 0x00);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_hifiberry_dacplusadc_ops = {
	.hw_params = snd_rpi_hifiberry_dacplusadc_hw_params,
	.startup = snd_rpi_hifiberry_dacplusadc_startup,
	.shutdown = snd_rpi_hifiberry_dacplusadc_shutdown,
};

SND_SOC_DAILINK_DEFS(hifi,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2708-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("pcm512x.1-004d", "pcm512x-hifi"),
			   COMP_CODEC("dmic-codec", "dmic-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2708-i2s.0")));

static struct snd_soc_dai_link snd_rpi_hifiberry_dacplusadc_dai[] = {
{
	.name		= "HiFiBerry DAC+ADC",
	.stream_name	= "HiFiBerry DAC+ADC HiFi",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_hifiberry_dacplusadc_ops,
	.init		= snd_rpi_hifiberry_dacplusadc_init,
	SND_SOC_DAILINK_REG(hifi),
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_hifiberry_dacplusadc = {
	.name         = "snd_rpi_hifiberry_dacplusadc",
	.driver_name  = "HifiberryDacpAdc",
	.owner        = THIS_MODULE,
	.dai_link     = snd_rpi_hifiberry_dacplusadc_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_hifiberry_dacplusadc_dai),
};


static int snd_rpi_hifiberry_dacplusadc_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_hifiberry_dacplusadc.dev = &pdev->dev;
	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;

		dai = &snd_rpi_hifiberry_dacplusadc_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
			"i2s-controller", 0);
		if (i2s_node) {
			dai->cpus->of_node = i2s_node;
			dai->platforms->of_node = i2s_node;
			dai->cpus->dai_name = NULL;
			dai->platforms->name = NULL;
		}
	}
	digital_gain_0db_limit = !of_property_read_bool(
		pdev->dev.of_node, "hifiberry,24db_digital_gain");
	slave = of_property_read_bool(pdev->dev.of_node,
					"hifiberry-dacplusadc,slave");
	leds_off = of_property_read_bool(pdev->dev.of_node,
					"hifiberry-dacplusadc,leds_off");

	ret = devm_snd_soc_register_card(&pdev->dev,
						 &snd_rpi_hifiberry_dacplusadc);
	if (ret && ret != -EPROBE_DEFER)
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static const struct of_device_id snd_rpi_hifiberry_dacplusadc_of_match[] = {
	{ .compatible = "hifiberry,hifiberry-dacplusadc", },
	{},
};

MODULE_DEVICE_TABLE(of, snd_rpi_hifiberry_dacplusadc_of_match);

static struct platform_driver snd_rpi_hifiberry_dacplusadc_driver = {
	.driver = {
		.name   = "snd-rpi-hifiberry-dacplusadc",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_hifiberry_dacplusadc_of_match,
	},
	.probe          = snd_rpi_hifiberry_dacplusadc_probe,
};

static int __init hifiberry_dacplusadc_init(void)
{
	int ret;

	dmic_codec_dev = platform_device_register_simple("dmic-codec", -1, NULL,
							 0);
	if (IS_ERR(dmic_codec_dev)) {
		pr_err("%s: dmic-codec device registration failed\n", __func__);
		return PTR_ERR(dmic_codec_dev);
	}

	ret = platform_driver_register(&snd_rpi_hifiberry_dacplusadc_driver);
	if (ret) {
		pr_err("%s: platform driver registration failed\n", __func__);
		platform_device_unregister(dmic_codec_dev);
	}

	return ret;
}
module_init(hifiberry_dacplusadc_init);

static void __exit hifiberry_dacplusadc_exit(void)
{
	platform_driver_unregister(&snd_rpi_hifiberry_dacplusadc_driver);
	platform_device_unregister(dmic_codec_dev);
}
module_exit(hifiberry_dacplusadc_exit);

MODULE_AUTHOR("Joerg Schambacher <joscha@schambacher.com>");
MODULE_AUTHOR("Daniel Matuschek <daniel@hifiberry.com>");
MODULE_DESCRIPTION("ASoC Driver for HiFiBerry DAC+ADC");
MODULE_LICENSE("GPL v2");
