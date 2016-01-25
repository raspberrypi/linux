/*
 * ASoC Driver for HiFiBerry DAC+ / DAC Pro / AMP100
 *
 * Author:	Daniel Matuschek, Stuart MacLean <stuart@hifiberry.com>
 *		Copyright 2014-2015
 *		based on code by Florian Meier <florian.meier@koalo.de>
 *		Headphone/AMP100 Joerg Schambacher <joerg@hifiberry.com>
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

static bool slave;
static bool snd_rpi_hifiberry_is_dacpro;
static bool digital_gain_0db_limit = true;
static bool leds_off;
static bool auto_mute;
static int mute_ext_ctl;
static int mute_ext;
static bool tas_device;
static struct gpio_desc *snd_mute_gpio;
static struct gpio_desc *snd_reset_gpio;
static struct snd_soc_card snd_rpi_hifiberry_dacplus;

static const u32 master_dai_rates[] = {
	44100, 48000, 88200, 96000,
	176400, 192000, 352800, 384000,
};

static const struct snd_pcm_hw_constraint_list constraints_master = {
	.count = ARRAY_SIZE(master_dai_rates),
	.list  = master_dai_rates,
};

static int snd_rpi_hifiberry_dacplus_mute_set(int mute)
{
	gpiod_set_value_cansleep(snd_mute_gpio, mute);
	return 1;
}

static int snd_rpi_hifiberry_dacplus_mute_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = mute_ext;

	return 0;
}

static int snd_rpi_hifiberry_dacplus_mute_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	if (mute_ext == ucontrol->value.integer.value[0])
		return 0;

	mute_ext = ucontrol->value.integer.value[0];

	return snd_rpi_hifiberry_dacplus_mute_set(mute_ext);
}

static const char * const mute_text[] = {"Play", "Mute"};
static const struct soc_enum hb_dacplus_opt_mute_enum =
	SOC_ENUM_SINGLE_EXT(2, mute_text);

static const struct snd_kcontrol_new hb_dacplus_opt_mute_controls[] = {
	SOC_ENUM_EXT("Mute(ext)", hb_dacplus_opt_mute_enum,
			      snd_rpi_hifiberry_dacplus_mute_get,
			      snd_rpi_hifiberry_dacplus_mute_put),
};

static void snd_rpi_hifiberry_dacplus_select_clk(struct snd_soc_component *component,
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
	usleep_range(3000, 4000);
}

static void snd_rpi_hifiberry_dacplus_clk_gpio(struct snd_soc_component *component)
{
	snd_soc_component_update_bits(component, PCM512x_GPIO_EN, 0x24, 0x24);
	snd_soc_component_update_bits(component, PCM512x_GPIO_OUTPUT_3, 0x0f, 0x02);
	snd_soc_component_update_bits(component, PCM512x_GPIO_OUTPUT_6, 0x0f, 0x02);
}

static bool snd_rpi_hifiberry_dacplus_is_sclk(struct snd_soc_component *component)
{
	unsigned int sck;

	sck = snd_soc_component_read(component, PCM512x_RATE_DET_4);
	return (!(sck & 0x40));
}

static bool snd_rpi_hifiberry_dacplus_is_pro_card(struct snd_soc_component *component)
{
	bool isClk44EN, isClk48En, isNoClk;

	snd_rpi_hifiberry_dacplus_clk_gpio(component);

	snd_rpi_hifiberry_dacplus_select_clk(component, HIFIBERRY_DACPRO_CLK44EN);
	isClk44EN = snd_rpi_hifiberry_dacplus_is_sclk(component);

	snd_rpi_hifiberry_dacplus_select_clk(component, HIFIBERRY_DACPRO_NOCLOCK);
	isNoClk = snd_rpi_hifiberry_dacplus_is_sclk(component);

	snd_rpi_hifiberry_dacplus_select_clk(component, HIFIBERRY_DACPRO_CLK48EN);
	isClk48En = snd_rpi_hifiberry_dacplus_is_sclk(component);

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
	case 352800:
		type = HIFIBERRY_DACPRO_CLK44EN;
		break;
	default:
		type = HIFIBERRY_DACPRO_CLK48EN;
		break;
	}
	return type;
}

static void snd_rpi_hifiberry_dacplus_set_sclk(struct snd_soc_component *component,
	int sample_rate)
{
	struct pcm512x_priv *pcm512x = snd_soc_component_get_drvdata(component);

	if (!IS_ERR(pcm512x->sclk)) {
		int ctype;

		ctype = snd_rpi_hifiberry_dacplus_clk_for_rate(sample_rate);
		clk_set_rate(pcm512x->sclk, (ctype == HIFIBERRY_DACPRO_CLK44EN)
			? CLK_44EN_RATE : CLK_48EN_RATE);
		snd_rpi_hifiberry_dacplus_select_clk(component, ctype);
	}
}

static int snd_rpi_hifiberry_dacplus_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct pcm512x_priv *priv;
	struct snd_soc_card *card = &snd_rpi_hifiberry_dacplus;

	if (slave)
		snd_rpi_hifiberry_is_dacpro = false;
	else
		snd_rpi_hifiberry_is_dacpro =
				snd_rpi_hifiberry_dacplus_is_pro_card(component);

	if (snd_rpi_hifiberry_is_dacpro) {
		struct snd_soc_dai_link *dai = rtd->dai_link;

		if (tas_device) {
			dai->name = "HiFiBerry AMP4 Pro";
			dai->stream_name = "HiFiBerry AMP4 Pro HiFi";
		} else {
			dai->name = "HiFiBerry DAC+ Pro";
			dai->stream_name = "HiFiBerry DAC+ Pro HiFi";
		}
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
	if (snd_reset_gpio) {
		gpiod_set_value_cansleep(snd_reset_gpio, 0);
		msleep(1);
		gpiod_set_value_cansleep(snd_reset_gpio, 1);
		msleep(1);
		gpiod_set_value_cansleep(snd_reset_gpio, 0);
	}

	if (mute_ext_ctl)
		snd_soc_add_card_controls(card,	hb_dacplus_opt_mute_controls,
				ARRAY_SIZE(hb_dacplus_opt_mute_controls));

	if (snd_mute_gpio)
		gpiod_set_value_cansleep(snd_mute_gpio,	mute_ext);

	return 0;
}

static int snd_rpi_hifiberry_dacplus_update_rate_den(
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

static int snd_rpi_hifiberry_dacplus_hw_params(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int channels = params_channels(params);
	int width = snd_pcm_format_width(params_format(params));

	/* Using powers of 2 allows for an integer clock divisor */
	width = width <= 16 ? 16 : 32;

	if (snd_rpi_hifiberry_is_dacpro) {
		struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;

		snd_rpi_hifiberry_dacplus_set_sclk(component,
			params_rate(params));

		ret = snd_rpi_hifiberry_dacplus_update_rate_den(
			substream, params);
	}

	ret = snd_soc_dai_set_bclk_ratio(snd_soc_rtd_to_cpu(rtd, 0), channels * width);
	if (ret)
		return ret;
	ret = snd_soc_dai_set_bclk_ratio(snd_soc_rtd_to_codec(rtd, 0), channels * width);
	return ret;
}

static int snd_rpi_hifiberry_dacplus_startup(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;
	int ret;

	if (tas_device && !slave) {
		ret = snd_pcm_hw_constraint_list(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_RATE,
					  &constraints_master);
		if (ret < 0) {
			dev_err(rtd->card->dev,
				"Cannot apply constraints for sample rates\n");
			return ret;
		}
	}

	if (auto_mute)
		gpiod_set_value_cansleep(snd_mute_gpio, 0);
	if (leds_off)
		return 0;
	snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1, 0x08, 0x08);
	return 0;
}

static void snd_rpi_hifiberry_dacplus_shutdown(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;

	snd_soc_component_update_bits(component, PCM512x_GPIO_CONTROL_1, 0x08, 0x00);
	if (auto_mute)
		gpiod_set_value_cansleep(snd_mute_gpio, 1);
}

/* machine stream operations */
static const struct snd_soc_ops snd_rpi_hifiberry_dacplus_ops = {
	.hw_params = snd_rpi_hifiberry_dacplus_hw_params,
	.startup = snd_rpi_hifiberry_dacplus_startup,
	.shutdown = snd_rpi_hifiberry_dacplus_shutdown,
};

SND_SOC_DAILINK_DEFS(rpi_hifiberry_dacplus,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2708-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("pcm512x.1-004d", "pcm512x-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2708-i2s.0")));

static struct snd_soc_dai_link snd_rpi_hifiberry_dacplus_dai[] = {
{
	.name		= "HiFiBerry DAC+",
	.stream_name	= "HiFiBerry DAC+ HiFi",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_hifiberry_dacplus_ops,
	.init		= snd_rpi_hifiberry_dacplus_init,
	SND_SOC_DAILINK_REG(rpi_hifiberry_dacplus),
},
};

/* aux device for optional headphone amp */
static struct snd_soc_aux_dev hifiberry_dacplus_aux_devs[] = {
	{
		.dlc = {
			.name = "tpa6130a2.1-0060",
		},
	},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_hifiberry_dacplus = {
	.name         = "snd_rpi_hifiberry_dacplus",
	.driver_name  = "HifiberryDacp",
	.owner        = THIS_MODULE,
	.dai_link     = snd_rpi_hifiberry_dacplus_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_hifiberry_dacplus_dai),
};

static int hb_hp_detect(void)
{
	struct i2c_adapter *adap = i2c_get_adapter(1);
	int ret;
	struct i2c_client tpa_i2c_client = {
		.addr = 0x60,
		.adapter = adap,
	};

	if (!adap)
		return -EPROBE_DEFER;	/* I2C module not yet available */

	ret = i2c_smbus_read_byte(&tpa_i2c_client) >= 0;
	i2c_put_adapter(adap);
	return ret;
};

static struct property tpa_enable_prop = {
	       .name = "status",
	       .length = 4 + 1, /* length 'okay' + 1 */
	       .value = "okay",
	};

static int snd_rpi_hifiberry_dacplus_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct snd_soc_card *card = &snd_rpi_hifiberry_dacplus;
	int len;
	struct device_node *tpa_node;
	struct device_node *tas_node;
	struct property *tpa_prop;
	struct of_changeset ocs;
	struct property *pp;
	int tmp;

	/* probe for head phone amp */
	ret = hb_hp_detect();
	if (ret < 0)
		return ret;
	if (ret) {
		card->aux_dev = hifiberry_dacplus_aux_devs;
		card->num_aux_devs =
				ARRAY_SIZE(hifiberry_dacplus_aux_devs);
		tpa_node = of_find_compatible_node(NULL, NULL, "ti,tpa6130a2");
		tpa_prop = of_find_property(tpa_node, "status", &len);

		if (strcmp((char *)tpa_prop->value, "okay")) {
			/* and activate headphone using change_sets */
			dev_info(&pdev->dev, "activating headphone amplifier");
			of_changeset_init(&ocs);
			ret = of_changeset_update_property(&ocs, tpa_node,
							&tpa_enable_prop);
			if (ret) {
				dev_err(&pdev->dev,
				"cannot activate headphone amplifier\n");
				return -ENODEV;
			}
			ret = of_changeset_apply(&ocs);
			if (ret) {
				dev_err(&pdev->dev,
				"cannot activate headphone amplifier\n");
				return -ENODEV;
			}
		}
	}

	tas_node = of_find_compatible_node(NULL, NULL, "ti,tas5756");
	if (tas_node) {
		tas_device = true;
		dev_info(&pdev->dev, "TAS5756 device found!\n");
	};

	snd_rpi_hifiberry_dacplus.dev = &pdev->dev;
	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;

		dai = &snd_rpi_hifiberry_dacplus_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
			"i2s-controller", 0);

		if (i2s_node) {
			dai->cpus->dai_name = NULL;
			dai->cpus->of_node = i2s_node;
			dai->platforms->name = NULL;
			dai->platforms->of_node = i2s_node;
		}

		digital_gain_0db_limit = !of_property_read_bool(
			pdev->dev.of_node, "hifiberry,24db_digital_gain");
		slave = of_property_read_bool(pdev->dev.of_node,
						"hifiberry-dacplus,slave");
		leds_off = of_property_read_bool(pdev->dev.of_node,
						"hifiberry-dacplus,leds_off");
		auto_mute = of_property_read_bool(pdev->dev.of_node,
						"hifiberry-dacplus,auto_mute");

		/*
		 * check for HW MUTE as defined in DT-overlay
		 * active high, therefore default to HIGH to MUTE
		 */
		snd_mute_gpio =	devm_gpiod_get_optional(&pdev->dev,
						 "mute", GPIOD_OUT_HIGH);
		if (IS_ERR(snd_mute_gpio)) {
			dev_err(&pdev->dev, "Can't allocate GPIO (HW-MUTE)");
			return PTR_ERR(snd_mute_gpio);
		}

		/* add ALSA control if requested in DT-overlay (AMP100) */
		pp = of_find_property(pdev->dev.of_node,
				"hifiberry-dacplus,mute_ext_ctl", &tmp);
		if (pp) {
			if (!of_property_read_u32(pdev->dev.of_node,
				"hifiberry-dacplus,mute_ext_ctl", &mute_ext)) {
				/* ALSA control will be used */
				mute_ext_ctl = 1;
			}
		}

		/* check for HW RESET (AMP100) */
		snd_reset_gpio = devm_gpiod_get_optional(&pdev->dev,
						"reset", GPIOD_OUT_HIGH);
		if (IS_ERR(snd_reset_gpio)) {
			dev_err(&pdev->dev, "Can't allocate GPIO (HW-RESET)");
			return PTR_ERR(snd_reset_gpio);
		}

	}

	ret = devm_snd_soc_register_card(&pdev->dev,
			&snd_rpi_hifiberry_dacplus);
	if (ret && ret != -EPROBE_DEFER)
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);
	if (!ret) {
		if (snd_mute_gpio)
			dev_info(&pdev->dev, "GPIO%i for HW-MUTE selected",
					gpio_chip_hwgpio(snd_mute_gpio));
		if (snd_reset_gpio)
			dev_info(&pdev->dev, "GPIO%i for HW-RESET selected",
					gpio_chip_hwgpio(snd_reset_gpio));
	}
	return ret;
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
};

module_platform_driver(snd_rpi_hifiberry_dacplus_driver);

MODULE_AUTHOR("Daniel Matuschek <daniel@hifiberry.com>");
MODULE_DESCRIPTION("ASoC Driver for HiFiBerry DAC+");
MODULE_LICENSE("GPL v2");
