/*
 * Driver for the SABRE ESS CODEC
 *
 * Author: Jaikumar <jaikumar@cem-solutions.net>
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


#include <linux/init.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/gcd.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>

#include "sabre-ess.h"

struct sabre_ess_priv {
	struct regmap *regmap;
	int fmt;
};

static const struct reg_default sabre_ess_reg_defaults[] = {
	{ SABRE_ESS_RESET,			0x00 },
	{ SABRE_ESS_VOLUME_1,		0xF0 },
	{ SABRE_ESS_VOLUME_2,		0xF0 },
	{ SABRE_ESS_MUTE,			0x00 },
	{ SABRE_ESS_DSP_PROGRAM,	0x04 },
	{ SABRE_ESS_DEEMPHASIS,		0x00 },
	{ SABRE_ESS_DOP,			0x01 },
	{ SABRE_ESS_FORMAT,			0xb4 },
};

static const char * const sabre_ess_dsp_program_texts[] = {
	"Linear Phase Fast Roll-off Filter",
	"Linear Phase Slow Roll-off Filter",
	"Minimum Phase Fast Roll-off Filter",
	"Minimum Phase Slow Roll-off Filter",
	"Apodizing Fast Roll-off Filter",
	"Corrected Minimum Phase Fast Roll-off Filter",
	"Brick Wall Filter",
};

static const unsigned int sabre_ess_dsp_program_values[] = {
	0,
	1,
	2,
	3,
	4,
	6,
	7,
};

static SOC_VALUE_ENUM_SINGLE_DECL(sabre_ess_dsp_program,
				  SABRE_ESS_DSP_PROGRAM, 0, 0x07,
				  sabre_ess_dsp_program_texts,
				  sabre_ess_dsp_program_values);

static const char * const sabre_ess_deemphasis_texts[] = {
	"Bypass",
	"32kHz",
	"44.1kHz",
	"48kHz",
};

static const unsigned int sabre_ess_deemphasis_values[] = {
	0,
	1,
	2,
	3,
};

static SOC_VALUE_ENUM_SINGLE_DECL(sabre_ess_deemphasis,
				  SABRE_ESS_DEEMPHASIS, 0, 0x03,
				  sabre_ess_deemphasis_texts,
				  sabre_ess_deemphasis_values);

static const SNDRV_CTL_TLVD_DECLARE_DB_MINMAX(master_tlv, -12700, 0);

static const struct snd_kcontrol_new sabre_ess_controls[] = {
	SOC_DOUBLE_R_TLV("Master Playback Volume", SABRE_ESS_VOLUME_1,
			SABRE_ESS_VOLUME_2, 0, 255, 1, master_tlv),
	SOC_DOUBLE("Master Playback Switch", SABRE_ESS_MUTE, 0, 0, 1, 1),
	SOC_ENUM("DSP Program Route", sabre_ess_dsp_program),
	SOC_ENUM("Deemphasis Route", sabre_ess_deemphasis),
	SOC_SINGLE("DoP Playback Switch", SABRE_ESS_DOP, 0, 1, 1)
};

static bool sabre_ess_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case SABRE_ESS_CHIP_ID_REG:
		return true;
	default:
		return reg < 0xff;
	}
}

static int sabre_ess_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct sabre_ess_priv *sabre_ess = snd_soc_codec_get_drvdata(codec);
	int fmt = 0;
	int ret;

	dev_dbg(codec->dev, "hw_params %u Hz, %u channels\n",
			params_rate(params),
			params_channels(params));

	switch (sabre_ess->fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM: // master
		if (params_channels(params) == 2)
			fmt = SABRE_ESS_CHAN_STEREO;
		else
			fmt = SABRE_ESS_CHAN_MONO;

		switch (params_width(params)) {
		case 16:
			fmt |= SABRE_ESS_ALEN_16;
			break;
		case 24:
			fmt |= SABRE_ESS_ALEN_24;
			break;
		case 32:
			fmt |= SABRE_ESS_ALEN_32;
			break;
		default:
			dev_err(codec->dev, "Bad frame size: %d\n",
					params_width(params));
			return -EINVAL;
		}

		switch (params_rate(params)) {
		case 44100:
			fmt |= SABRE_ESS_RATE_44100;
			break;
		case 48000:
			fmt |= SABRE_ESS_RATE_48000;
			break;
		case 88200:
			fmt |= SABRE_ESS_RATE_88200;
			break;
		case 96000:
			fmt |= SABRE_ESS_RATE_96000;
			break;
		case 176400:
			fmt |= SABRE_ESS_RATE_176400;
			break;
		case 192000:
			fmt |= SABRE_ESS_RATE_192000;
			break;
		case 352800:
			fmt |= SABRE_ESS_RATE_352800;
			break;
		case 384000:
			fmt |= SABRE_ESS_RATE_384000;
			break;
		default:
			dev_err(codec->dev, "Bad sample rate: %d\n",
					params_rate(params));
			return -EINVAL;
		}

		ret = regmap_write(sabre_ess->regmap, SABRE_ESS_FORMAT, fmt);
		if (ret != 0) {
			dev_err(codec->dev, "Failed to set format: %d\n", ret);
			return ret;
		}
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int sabre_ess_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = dai->codec;
	struct sabre_ess_priv *sabre_ess = snd_soc_codec_get_drvdata(codec);

	sabre_ess->fmt = fmt;

	return 0;
}

static const struct snd_soc_dai_ops sabre_ess_dai_ops = {
	.hw_params = sabre_ess_hw_params,
	.set_fmt = sabre_ess_set_fmt,
};

static struct snd_soc_dai_driver sabre_ess_dai = {
	.name = "sabre-ess",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_CONTINUOUS,
		.rate_min = 44100,
		.rate_max = 384000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			SNDRV_PCM_FMTBIT_S32_LE
	},
	.ops = &sabre_ess_dai_ops,
};

static struct snd_soc_codec_driver sabre_ess_codec_driver = {
	.idle_bias_off = false,

	.component_driver = {
		.controls		= sabre_ess_controls,
		.num_controls	= ARRAY_SIZE(sabre_ess_controls),
	},
};

static const struct regmap_range_cfg sabre_ess_range = {
	.name = "Pages", .range_min = SABRE_ESS_VIRT_BASE,
	.range_max = SABRE_ESS_MAX_REGISTER,
	.selector_reg = SABRE_ESS_PAGE,
	.selector_mask = 0xff,
	.window_start = 0, .window_len = 0x100,
};

const struct regmap_config sabre_ess_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.ranges = &sabre_ess_range,
	.num_ranges = 1,

	.max_register = SABRE_ESS_MAX_REGISTER,
	.readable_reg = sabre_ess_readable_register,
	.reg_defaults = sabre_ess_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(sabre_ess_reg_defaults),
	.cache_type = REGCACHE_RBTREE,
};
EXPORT_SYMBOL_GPL(sabre_ess_regmap);

int sabre_ess_probe(struct device *dev, struct regmap *regmap)
{
	struct sabre_ess_priv *sabre_ess;
	unsigned int chip_id = 0;
	int ret;

	sabre_ess = devm_kzalloc(dev, sizeof(struct sabre_ess_priv),
								GFP_KERNEL);
	if (!sabre_ess)
		return -ENOMEM;

	dev_set_drvdata(dev, sabre_ess);
	sabre_ess->regmap = regmap;

	ret = regmap_read(regmap, SABRE_ESS_CHIP_ID_REG, &chip_id);
	if ((ret != 0) || (chip_id != SABRE_ESS_CHIP_ID)) {
		dev_err(dev, "Failed to read Chip or wrong Chip id: %d\n", ret);
		return ret;
	}
	regmap_update_bits(regmap, SABRE_ESS_RESET, 0x01, 0x01);
	msleep(10);

	ret = snd_soc_register_codec(dev, &sabre_ess_codec_driver,
				    &sabre_ess_dai, 1);
	if (ret != 0) {
		dev_err(dev, "failed to register codec: %d\n", ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(sabre_ess_probe);

void sabre_ess_remove(struct device *dev)
{
	snd_soc_unregister_codec(dev);
	pm_runtime_disable(dev);
}
EXPORT_SYMBOL_GPL(sabre_ess_remove);

MODULE_DESCRIPTION("ASoC SABRE ESS codec driver");
MODULE_AUTHOR("Jaikumar <jaikumar@cem-solutions.net>");
MODULE_LICENSE("GPL v2");

