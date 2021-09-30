// SPDX-License-Identifier: GPL-2.0
/*
 * ASoC Driver for HiFiBerry AMP3
 *
 * Author:	Joerg Schambacher <joerg@hifiberry.com>
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
#include <../drivers/gpio/gpiolib.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/i2c.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../codecs/tas5754m.h"

#define HIFIBERRY_DACPRO_NOCLOCK 0
#define HIFIBERRY_DACPRO_CLK44EN 1
#define HIFIBERRY_DACPRO_CLK48EN 2

struct tas5754m_priv {
	struct regmap *regmap;
	struct clk *sclk;
};

/* Clock rate of CLK44EN attached to GPIO6 pin */
#define CLK_44EN_RATE 22579200UL
/* Clock rate of CLK48EN attached to GPIO3 pin */
#define CLK_48EN_RATE 24576000UL

static bool digital_gain_0db_limit = true;
static bool leds_off;
static bool auto_mute;
static int mute_ext_ctl;
static int mute_ext;
static struct gpio_desc *snd_mute_gpio;
static struct snd_soc_card snd_rpi_hifiberry_amp3;

static int snd_rpi_hifiberry_amp3_mute_set(int mute)
{
	gpiod_set_value_cansleep(snd_mute_gpio, mute);
	return 1;
}

static int snd_rpi_hifiberry_amp3_mute_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = mute_ext;

	return 0;
}

static int snd_rpi_hifiberry_amp3_mute_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	if (mute_ext == ucontrol->value.integer.value[0])
		return 0;

	mute_ext = ucontrol->value.integer.value[0];

	return snd_rpi_hifiberry_amp3_mute_set(mute_ext);
}

static const char * const mute_text[] = {"Play", "Mute"};
static const struct soc_enum hb_amp3_opt_mute_enum =
	SOC_ENUM_SINGLE_EXT(2, mute_text);

static const struct snd_kcontrol_new hb_amp3_opt_mute_controls[] = {
	SOC_ENUM_EXT("Mute(ext)", hb_amp3_opt_mute_enum,
			      snd_rpi_hifiberry_amp3_mute_get,
			      snd_rpi_hifiberry_amp3_mute_put),
};

static void snd_rpi_hifiberry_amp3_select_clk(struct snd_soc_component *component,
	int clk_id)
{
	switch (clk_id) {
	case HIFIBERRY_DACPRO_NOCLOCK:
		snd_soc_component_update_bits(component,
				TAS5754M_GPIO_CONTROL_1, 0x24, 0x00);
		break;
	case HIFIBERRY_DACPRO_CLK44EN:
		snd_soc_component_update_bits(component,
				TAS5754M_GPIO_CONTROL_1, 0x24, 0x20);
		break;
	case HIFIBERRY_DACPRO_CLK48EN:
		snd_soc_component_update_bits(component,
				TAS5754M_GPIO_CONTROL_1, 0x24, 0x04);
		break;
	}
	usleep_range(2000, 2100);
}

static void snd_rpi_hifiberry_amp3_clk_gpio(struct snd_soc_component *component)
{
	snd_soc_component_update_bits(component, TAS5754M_GPIO_EN,
							0x24, 0x24);
	snd_soc_component_update_bits(component, TAS5754M_GPIO_OUTPUT_3,
							0x0f, 0x02);
	snd_soc_component_update_bits(component, TAS5754M_GPIO_OUTPUT_6,
							0x0f, 0x02);
}

static bool snd_rpi_hifiberry_amp3_is_sclk(struct snd_soc_component *component)
{
	unsigned int sck;

	sck = snd_soc_component_read(component, TAS5754M_RATE_DET_4);
	return (!(sck & 0x40));
}

static bool snd_rpi_hifiberry_amp3_test_clocks(
				struct snd_soc_component *component)
{
	bool isClk44EN, isClk48En, isNoClk;

	snd_rpi_hifiberry_amp3_clk_gpio(component);

	snd_rpi_hifiberry_amp3_select_clk(component, HIFIBERRY_DACPRO_CLK44EN);
	isClk44EN = snd_rpi_hifiberry_amp3_is_sclk(component);

	snd_rpi_hifiberry_amp3_select_clk(component, HIFIBERRY_DACPRO_NOCLOCK);
	isNoClk = snd_rpi_hifiberry_amp3_is_sclk(component);

	snd_rpi_hifiberry_amp3_select_clk(component, HIFIBERRY_DACPRO_CLK48EN);
	isClk48En = snd_rpi_hifiberry_amp3_is_sclk(component);

	return (isClk44EN && isClk48En && !isNoClk);
}

static int snd_rpi_hifiberry_amp3_clk_for_rate(int sample_rate)
{
	int type;

	switch (sample_rate) {
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

static void snd_rpi_hifiberry_amp3_set_sclk(struct snd_soc_component *component,
	int sample_rate)
{
	struct tas5754m_priv *tas5754m =
			snd_soc_component_get_drvdata(component);

	if (!IS_ERR(tas5754m->sclk)) {
		int ctype;

		ctype = snd_rpi_hifiberry_amp3_clk_for_rate(sample_rate);
		clk_set_rate(tas5754m->sclk, (ctype == HIFIBERRY_DACPRO_CLK44EN)
			? CLK_44EN_RATE : CLK_48EN_RATE);
		snd_rpi_hifiberry_amp3_select_clk(component, ctype);
	}
}

static int snd_rpi_hifiberry_amp3_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component =
				asoc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_card *card = &snd_rpi_hifiberry_amp3;

	if (!snd_rpi_hifiberry_amp3_test_clocks(component)) {
		dev_err(rtd->dev, "Clocks not available\n");
		return -ENODEV;
	};

	snd_rpi_hifiberry_amp3_select_clk(component, HIFIBERRY_DACPRO_CLK48EN);

	snd_soc_component_update_bits(component, TAS5754M_GPIO_EN,
							0x08, 0x08);
	snd_soc_component_update_bits(component, TAS5754M_GPIO_OUTPUT_4,
							0x0f, 0x02);
	if (leds_off)
		snd_soc_component_update_bits(component,
				TAS5754M_GPIO_CONTROL_1, 0x08, 0x00);
	else
		snd_soc_component_update_bits(component,
				TAS5754M_GPIO_CONTROL_1, 0x08, 0x08);

	if (digital_gain_0db_limit) {
		int ret;
		struct snd_soc_card *card = rtd->card;

		ret = snd_soc_limit_volume(card,
					"Digital Playback Volume", 207);
		if (ret < 0)
			dev_warn(card->dev,
				"Failed to set volume limit: %d\n", ret);
	}

	if (mute_ext_ctl) {
		snd_soc_add_card_controls(card,	hb_amp3_opt_mute_controls,
				ARRAY_SIZE(hb_amp3_opt_mute_controls));
	}

	if (snd_mute_gpio)
		gpiod_set_value_cansleep(snd_mute_gpio,	mute_ext);

	return 0;
}

static int snd_rpi_hifiberry_amp3_hw_params(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int channels = params_channels(params);
	int width = 32;
	struct snd_soc_component *component =
			asoc_rtd_to_codec(rtd, 0)->component;

	width = snd_pcm_format_physical_width(params_format(params));

	snd_rpi_hifiberry_amp3_set_sclk(component, params_rate(params));
	ret = snd_soc_dai_set_bclk_ratio(asoc_rtd_to_cpu(rtd, 0),
		channels * width);
	if (ret)
		return ret;
	ret = snd_soc_dai_set_bclk_ratio(asoc_rtd_to_cpu(rtd, 0),
		channels * width);
	return ret;
}

static int snd_rpi_hifiberry_amp3_startup(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *component =
			asoc_rtd_to_codec(rtd, 0)->component;

	if (auto_mute)
		gpiod_set_value_cansleep(snd_mute_gpio, 0);
	if (leds_off)
		return 0;
	snd_soc_component_update_bits(component, TAS5754M_GPIO_CONTROL_1,
								0x08, 0x08);

	return 0;
}

static void snd_rpi_hifiberry_amp3_shutdown(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *component =
				asoc_rtd_to_codec(rtd, 0)->component;

	snd_soc_component_update_bits(component, TAS5754M_GPIO_CONTROL_1,
								0x08, 0x00);
	if (auto_mute)
		gpiod_set_value_cansleep(snd_mute_gpio, 1);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_hifiberry_amp3_ops = {
	.hw_params = snd_rpi_hifiberry_amp3_hw_params,
	.startup = snd_rpi_hifiberry_amp3_startup,
	.shutdown = snd_rpi_hifiberry_amp3_shutdown,
};

SND_SOC_DAILINK_DEFS(rpi_hifiberry_amp3,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2708-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("tas5754m.1-004d", "tas5754m-amplifier")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2708-i2s.0")));

static struct snd_soc_dai_link snd_rpi_hifiberry_amp3_dai[] = {
{
	.name		= "HiFiBerry AMP3 Pro",
	.stream_name	= "HiFiBerry AMP3 Pro HiFi",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBM_CFM,
	.ops		= &snd_rpi_hifiberry_amp3_ops,
	.init		= snd_rpi_hifiberry_amp3_init,
	SND_SOC_DAILINK_REG(rpi_hifiberry_amp3),
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_hifiberry_amp3 = {
	.name         = "snd_rpi_hifiberry_amp3",
	.driver_name  = "HifiberryAmp3",
	.owner        = THIS_MODULE,
	.dai_link     = snd_rpi_hifiberry_amp3_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_hifiberry_amp3_dai),
};

static int snd_rpi_hifiberry_amp3_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct property *pp;
	int tmp;

	snd_rpi_hifiberry_amp3.dev = &pdev->dev;
	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;

		dai = &snd_rpi_hifiberry_amp3_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
			"i2s-controller", 0);

		if (i2s_node) {
			dai->cpus->dai_name = NULL;
			dai->cpus->of_node = i2s_node;
			dai->platforms->name = NULL;
			dai->platforms->of_node = i2s_node;
		}

		digital_gain_0db_limit = !of_property_read_bool(
			pdev->dev.of_node, "hifiberry-amp3,24db_digital_gain");
		leds_off = of_property_read_bool(pdev->dev.of_node,
						"hifiberry-amp3,leds_off");
		auto_mute = of_property_read_bool(pdev->dev.of_node,
						"hifiberry-amp3,auto_mute");

		/*
		 * check for HW MUTE as defined in DT-overlay
		 * active low, therefore default to LOW to MUTE
		 */
		snd_mute_gpio =	devm_gpiod_get_optional(&pdev->dev,
						 "mute", GPIOD_OUT_LOW);
		if (IS_ERR(snd_mute_gpio)) {
			dev_err(&pdev->dev, "Can't allocate GPIO (HW-MUTE)\n");
			return PTR_ERR(snd_mute_gpio);
		}

		/* add ALSA control if requested in DT-overlay */
		pp = of_find_property(pdev->dev.of_node,
				"hifiberry-amp3,mute_ext_ctl", &tmp);
		if (pp) {
			if (!of_property_read_u32(pdev->dev.of_node,
				"hifiberry-amp3,mute_ext_ctl", &mute_ext)) {
				/* ALSA control will be used */
				mute_ext_ctl = 1;
			}
		}
	}
	ret = devm_snd_soc_register_card(&pdev->dev,
			&snd_rpi_hifiberry_amp3);

	if (ret && ret != -EPROBE_DEFER)
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);
	if (!ret) {
		if (snd_mute_gpio)
			dev_info(&pdev->dev, "GPIO%i for HW-MUTE selected\n",
					gpio_chip_hwgpio(snd_mute_gpio));
	}
	return ret;
}

static const struct of_device_id snd_rpi_hifiberry_amp3_of_match[] = {
	{ .compatible = "hifiberry,hifiberry-amp3", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_hifiberry_amp3_of_match);

static struct platform_driver snd_rpi_hifiberry_amp3_driver = {
	.driver = {
		.name   = "snd-rpi-hifiberry-amp3",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_hifiberry_amp3_of_match,
	},
	.probe          = snd_rpi_hifiberry_amp3_probe,
};

module_platform_driver(snd_rpi_hifiberry_amp3_driver);

MODULE_AUTHOR("Joerg Schambacher <joerg@hifiberry.com>");
MODULE_DESCRIPTION("ASoC Driver for HiFiBerry AMP3");
MODULE_LICENSE("GPL v2");
