/*
 * ASoC Driver for HiFiBerry DAC+ / DAC Pro
 *
 * Author:	Daniel Matuschek, Stuart MacLean <stuart@hifiberry.com>
 *		Copyright 2014-2015
 *		based on code by Florian Meier <florian.meier@koalo.de>
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

struct pcm512x_priv {
	struct regmap *regmap;
	struct clk *sclk;
};

/* Clock rate of CLK44EN attached to GPIO6 pin */
#define CLK_44EN_RATE 22579200UL
/* Clock rate of CLK48EN attached to GPIO3 pin */
#define CLK_48EN_RATE 24576000UL

static bool snd_rpi_hifiberry_is_dacpro;

static void snd_rpi_hifiberry_dacplus_select_clk(struct snd_soc_codec *codec,
	int clk_id)
{
	switch (clk_id) {
	case HIFIBERRY_DACPRO_NOCLOCK:
		snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x24, 0x00);
		break;
	case HIFIBERRY_DACPRO_CLK44EN:
		snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x24, 0x20);
		break;
	case HIFIBERRY_DACPRO_CLK48EN:
		snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x24, 0x04);
		break;
	}
}

static void snd_rpi_hifiberry_dacplus_clk_gpio(struct snd_soc_codec *codec)
{
	snd_soc_update_bits(codec, PCM512x_GPIO_EN, 0x24, 0x24);
	snd_soc_update_bits(codec, PCM512x_GPIO_OUTPUT_3, 0x0f, 0x02);
	snd_soc_update_bits(codec, PCM512x_GPIO_OUTPUT_6, 0x0f, 0x02);
}

static bool snd_rpi_hifiberry_dacplus_is_sclk(struct snd_soc_codec *codec)
{
	int sck;

	sck = snd_soc_read(codec, PCM512x_RATE_DET_4);
	return (!(sck & 0x40));
}

static bool snd_rpi_hifiberry_dacplus_is_sclk_sleep(
	struct snd_soc_codec *codec)
{
	msleep(2);
	return snd_rpi_hifiberry_dacplus_is_sclk(codec);
}

static bool snd_rpi_hifiberry_dacplus_is_pro_card(struct snd_soc_codec *codec)
{
	bool isClk44EN, isClk48En, isNoClk;

	snd_rpi_hifiberry_dacplus_clk_gpio(codec);

	snd_rpi_hifiberry_dacplus_select_clk(codec, HIFIBERRY_DACPRO_CLK44EN);
	isClk44EN = snd_rpi_hifiberry_dacplus_is_sclk_sleep(codec);

	snd_rpi_hifiberry_dacplus_select_clk(codec, HIFIBERRY_DACPRO_NOCLOCK);
	isNoClk = snd_rpi_hifiberry_dacplus_is_sclk_sleep(codec);

	snd_rpi_hifiberry_dacplus_select_clk(codec, HIFIBERRY_DACPRO_CLK48EN);
	isClk48En = snd_rpi_hifiberry_dacplus_is_sclk_sleep(codec);

	return (isClk44EN && isClk48En && !isNoClk);
}

static int snd_rpi_hifiberry_dacplus_clk_for_rate(int sample_rate)
{
	int type;

	switch (sample_rate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
	case 176400:
		type = HIFIBERRY_DACPRO_CLK44EN;
		break;
	default:
		type = HIFIBERRY_DACPRO_CLK48EN;
		break;
	}
	return type;
}

static void snd_rpi_hifiberry_dacplus_set_sclk(struct snd_soc_codec *codec,
	int sample_rate)
{
	struct pcm512x_priv *pcm512x = snd_soc_codec_get_drvdata(codec);

	if (!IS_ERR(pcm512x->sclk)) {
		int ctype;

		ctype = snd_rpi_hifiberry_dacplus_clk_for_rate(sample_rate);
		clk_set_rate(pcm512x->sclk, (ctype == HIFIBERRY_DACPRO_CLK44EN)
			? CLK_44EN_RATE : CLK_48EN_RATE);
		snd_rpi_hifiberry_dacplus_select_clk(codec, ctype);
	}
}

static int snd_rpi_hifiberry_dacplus_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct pcm512x_priv *priv;

	snd_rpi_hifiberry_is_dacpro
		= snd_rpi_hifiberry_dacplus_is_pro_card(codec);

	if (snd_rpi_hifiberry_is_dacpro) {
		struct snd_soc_dai_link *dai = rtd->dai_link;

		dai->name = "HiFiBerry DAC+ Pro";
		dai->stream_name = "HiFiBerry DAC+ Pro HiFi";
		dai->dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
			| SND_SOC_DAIFMT_CBM_CFM;

		snd_soc_update_bits(codec, PCM512x_BCLK_LRCLK_CFG, 0x31, 0x11);
		snd_soc_update_bits(codec, PCM512x_MASTER_MODE, 0x03, 0x03);
		snd_soc_update_bits(codec, PCM512x_MASTER_CLKDIV_2, 0x7f, 63);
	} else {
		priv = snd_soc_codec_get_drvdata(codec);
		priv->sclk = ERR_PTR(-ENOENT);
	}

	snd_soc_update_bits(codec, PCM512x_GPIO_EN, 0x08, 0x08);
	snd_soc_update_bits(codec, PCM512x_GPIO_OUTPUT_4, 0x0f, 0x02);
	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08, 0x08);

	return 0;
}

static int snd_rpi_hifiberry_dacplus_update_rate_den(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct pcm512x_priv *pcm512x = snd_soc_codec_get_drvdata(codec);
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

static int snd_rpi_hifiberry_dacplus_set_bclk_ratio_pro(
	struct snd_soc_dai *cpu_dai, struct snd_pcm_hw_params *params)
{
	int bratio = snd_pcm_format_physical_width(params_format(params))
		* params_channels(params);
	return snd_soc_dai_set_bclk_ratio(cpu_dai, bratio);
}

static int snd_rpi_hifiberry_dacplus_hw_params(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	int ret;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	if (snd_rpi_hifiberry_is_dacpro) {
		struct snd_soc_codec *codec = rtd->codec;

		snd_rpi_hifiberry_dacplus_set_sclk(codec,
			params_rate(params));

		ret = snd_rpi_hifiberry_dacplus_set_bclk_ratio_pro(cpu_dai,
			params);
		if (!ret)
			ret = snd_rpi_hifiberry_dacplus_update_rate_den(
				substream, params);
	} else {
		ret = snd_soc_dai_set_bclk_ratio(cpu_dai, 64);
	}
	return ret;
}

static int snd_rpi_hifiberry_dacplus_startup(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;

	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08, 0x08);
	return 0;
}

static void snd_rpi_hifiberry_dacplus_shutdown(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;

	snd_soc_update_bits(codec, PCM512x_GPIO_CONTROL_1, 0x08, 0x00);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_hifiberry_dacplus_ops = {
	.hw_params = snd_rpi_hifiberry_dacplus_hw_params,
	.startup = snd_rpi_hifiberry_dacplus_startup,
	.shutdown = snd_rpi_hifiberry_dacplus_shutdown,
};

static struct snd_soc_dai_link snd_rpi_hifiberry_dacplus_dai[] = {
{
	.name		= "HiFiBerry DAC+",
	.stream_name	= "HiFiBerry DAC+ HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "pcm512x-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "pcm512x.1-004d",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_hifiberry_dacplus_ops,
	.init		= snd_rpi_hifiberry_dacplus_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_hifiberry_dacplus = {
	.name         = "snd_rpi_hifiberry_dacplus",
	.dai_link     = snd_rpi_hifiberry_dacplus_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_hifiberry_dacplus_dai),
};

static int snd_rpi_hifiberry_dacplus_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_hifiberry_dacplus.dev = &pdev->dev;
	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;

		dai = &snd_rpi_hifiberry_dacplus_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
			"i2s-controller", 0);

		if (i2s_node) {
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = i2s_node;
			dai->platform_name = NULL;
			dai->platform_of_node = i2s_node;
		}
	}

	ret = snd_soc_register_card(&snd_rpi_hifiberry_dacplus);
	if (ret)
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static int snd_rpi_hifiberry_dacplus_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_hifiberry_dacplus);
}

static const struct of_device_id snd_rpi_hifiberry_dacplus_of_match[] = {
	{ .compatible = "hifiberry,hifiberry-dacplus", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_hifiberry_dacplus_of_match);

static struct platform_driver snd_rpi_hifiberry_dacplus_driver = {
	.driver = {
		.name   = "snd-rpi-hifiberry-dacplus",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_hifiberry_dacplus_of_match,
	},
	.probe          = snd_rpi_hifiberry_dacplus_probe,
	.remove         = snd_rpi_hifiberry_dacplus_remove,
};

module_platform_driver(snd_rpi_hifiberry_dacplus_driver);

MODULE_AUTHOR("Daniel Matuschek <daniel@hifiberry.com>");
MODULE_DESCRIPTION("ASoC Driver for HiFiBerry DAC+");
MODULE_LICENSE("GPL v2");
