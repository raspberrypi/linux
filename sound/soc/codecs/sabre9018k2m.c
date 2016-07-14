/*
 * Driver for the ESS SABRE9018K2M
 *
 * Author: Howard Qiao (howard.qiao@aoide.cc)
 *
 * Based on Sabre9018q2c Codec Driver
 * Satoru Kawase <satoru.kawase@gmail.com>, Takahito Nishiara 
 *      https://github.com/SatoruKawase/SabreBerry32
 *      Copyright 2016
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
#include <linux/regmap.h>
#include <linux/i2c.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>

#include "sabre9018k2m.h"


/* SABRE9018K2M Codec Private Data */
struct sabre9018k2m_priv {
	struct regmap *regmap;
	unsigned int fmt;
};


/* SABRE9018K2M Default Register Value */
static const struct reg_default sabre9018k2m_reg_defaults[] = {
	{ SABRE9018K2M_REG_0,  0x00 },
	{ SABRE9018K2M_REG_1,  0x8C },
	{ SABRE9018K2M_REG_4,  0x00 },
	{ SABRE9018K2M_REG_5,  0x68 },
	{ SABRE9018K2M_REG_6,  0x4A },
	{ SABRE9018K2M_REG_7,  0x80 },
	{ SABRE9018K2M_REG_8,  0x88 },
	{ SABRE9018K2M_REG_10, 0x02 },
	{ SABRE9018K2M_REG_11, 0x02 },
	{ SABRE9018K2M_REG_12, 0x5A },
	{ SABRE9018K2M_REG_13, 0x40 },
	{ SABRE9018K2M_REG_14, 0x8A },
	{ SABRE9018K2M_REG_15, 0x50 },
	{ SABRE9018K2M_REG_16, 0x50 },
	{ SABRE9018K2M_REG_17, 0xFF },
	{ SABRE9018K2M_REG_18, 0xFF },
	{ SABRE9018K2M_REG_19, 0xFF },
	{ SABRE9018K2M_REG_20, 0x7F },
	{ SABRE9018K2M_REG_21, 0x00 },
	{ SABRE9018K2M_REG_30, 0x00 },
	{ SABRE9018K2M_REG_39, 0x00 },
	{ SABRE9018K2M_REG_40, 0x00 },
	{ SABRE9018K2M_REG_41, 0x04 },
	{ SABRE9018K2M_REG_42, 0x20 },
};


static bool sabre9018k2m_writeable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case SABRE9018K2M_REG_0:
	case SABRE9018K2M_REG_1:
	case SABRE9018K2M_REG_4:
	case SABRE9018K2M_REG_5:
	case SABRE9018K2M_REG_6:
	case SABRE9018K2M_REG_7:
	case SABRE9018K2M_REG_8:
	case SABRE9018K2M_REG_10:
	case SABRE9018K2M_REG_11:
	case SABRE9018K2M_REG_12:
	case SABRE9018K2M_REG_13:
	case SABRE9018K2M_REG_14:
	case SABRE9018K2M_REG_15:
	case SABRE9018K2M_REG_16:
	case SABRE9018K2M_REG_17:
	case SABRE9018K2M_REG_18:
	case SABRE9018K2M_REG_19:
	case SABRE9018K2M_REG_20:
	case SABRE9018K2M_REG_21:
	case SABRE9018K2M_REG_22:
	case SABRE9018K2M_REG_23:
	case SABRE9018K2M_REG_24:
	case SABRE9018K2M_REG_25:
	case SABRE9018K2M_REG_26:
	case SABRE9018K2M_REG_27:
	case SABRE9018K2M_REG_28:
	case SABRE9018K2M_REG_29:
	case SABRE9018K2M_REG_30:
	case SABRE9018K2M_REG_39:
	case SABRE9018K2M_REG_40:
	case SABRE9018K2M_REG_41:
	case SABRE9018K2M_REG_42:
	case SABRE9018K2M_REG_43:
		return true;

	default:
		return false;
	}
}

static bool sabre9018k2m_readable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case SABRE9018K2M_REG_27:
	case SABRE9018K2M_REG_28:
	case SABRE9018K2M_REG_29:
	case SABRE9018K2M_REG_64:
	case SABRE9018K2M_REG_65:
	case SABRE9018K2M_REG_66:
	case SABRE9018K2M_REG_67:
	case SABRE9018K2M_REG_68:
	case SABRE9018K2M_REG_69:
	case SABRE9018K2M_REG_70:
	case SABRE9018K2M_REG_71:
	case SABRE9018K2M_REG_72:
	case SABRE9018K2M_REG_73:
	case SABRE9018K2M_REG_74:
		return true;

	default:
		return false;
	}
}

static bool sabre9018k2m_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case SABRE9018K2M_REG_65:
	case SABRE9018K2M_REG_66:
	case SABRE9018K2M_REG_67:
	case SABRE9018K2M_REG_68:
	case SABRE9018K2M_REG_69:
	case SABRE9018K2M_REG_70:
	case SABRE9018K2M_REG_71:
	case SABRE9018K2M_REG_72:
	case SABRE9018K2M_REG_73:
	case SABRE9018K2M_REG_74:
		return true;

	default:
		return false;
	}
}


/* Volume Scale */
static const DECLARE_TLV_DB_SCALE(volume_tlv, -12750, 50, 0);


/* Filter Type */
static const char * const filter_type_texts[] = {
	"Fast Roll-Off",
	"Slow Roll-Off",
	"Minimum Phase",
};

static SOC_ENUM_SINGLE_DECL(sabre9018k2m_filter_type_enum,
				SABRE9018K2M_REG_7, 5, filter_type_texts);


/* Control */
static const struct snd_kcontrol_new sabre9018k2m_controls[] = {
SOC_DOUBLE_R_TLV("Digital Playback Volume",
				SABRE9018K2M_REG_15, SABRE9018K2M_REG_16,
				0, 0xFF, 1, volume_tlv),

SOC_ENUM("Filter Type", sabre9018k2m_filter_type_enum),

SOC_DOUBLE("Mute Switch", SABRE9018K2M_REG_7, 0, 1, 1, 0),
};


static const u32 sabre9018k2m_dai_rates_master[] = {
	44100, 48000, 88200, 96000, 176400, 192000
};

static const struct snd_pcm_hw_constraint_list constraints_master = {
	.list  = sabre9018k2m_dai_rates_master,
	.count = ARRAY_SIZE(sabre9018k2m_dai_rates_master),
};

static const u32 sabre9018k2m_dai_rates_slave[] = {
	8000, 11025, 16000, 22050, 32000,
	44100, 48000, 64000, 88200, 96000, 176400, 192000
};

static const struct snd_pcm_hw_constraint_list constraints_slave = {
	.list  = sabre9018k2m_dai_rates_slave,
	.count = ARRAY_SIZE(sabre9018k2m_dai_rates_slave),
};

static int sabre9018k2m_dai_startup_master(
		struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	int ret;

	ret = snd_pcm_hw_constraint_list(substream->runtime,
			0, SNDRV_PCM_HW_PARAM_RATE, &constraints_master);
	if (ret != 0) {
		dev_err(codec->dev, "Failed to setup rates constraints: %d\n", ret);
		return ret;
	}

	ret = snd_pcm_hw_constraint_mask64(substream->runtime,
			SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FMTBIT_S32_LE);
	if (ret != 0) {
		dev_err(codec->dev, "Failed to setup format constraints: %d\n", ret);
	}

	return ret;
}

static int sabre9018k2m_dai_startup_slave(
		struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	int ret;

	ret = snd_pcm_hw_constraint_list(substream->runtime,
			0, SNDRV_PCM_HW_PARAM_RATE, &constraints_slave);
	if (ret != 0) {
		dev_err(codec->dev, "Failed to setup rates constraints: %d\n", ret);
	}

	return ret;
}

static int sabre9018k2m_dai_startup(
		struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct snd_soc_codec     *codec = dai->codec;
	struct sabre9018k2m_priv *sabre9018k2m
					= snd_soc_codec_get_drvdata(codec);

	switch (sabre9018k2m->fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		return sabre9018k2m_dai_startup_master(substream, dai);

	case SND_SOC_DAIFMT_CBS_CFS:
		return sabre9018k2m_dai_startup_slave(substream, dai);

	default:
		return (-EINVAL);
	}
}

static int sabre9018k2m_hw_params(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params,
	struct snd_soc_dai *dai)
{
	struct snd_soc_codec     *codec = dai->codec;
	struct sabre9018k2m_priv *sabre9018k2m
					= snd_soc_codec_get_drvdata(codec);
	unsigned int daifmt;
	int format_width;

	dev_dbg(codec->dev, "hw_params %u Hz, %u channels\n",
			params_rate(params), params_channels(params));

	/* Check I2S Format (Bit Size) */
	format_width = snd_pcm_format_width(params_format(params));
	if (format_width == 32) {
		snd_soc_update_bits(codec, SABRE9018K2M_REG_1,  0xC0, 2 << 6);
	} else if (format_width == 16) {
		snd_soc_update_bits(codec, SABRE9018K2M_REG_1,  0xC0, 0 << 6);
	} else {
		dev_err(codec->dev, "Bad frame size: %d\n",
				snd_pcm_format_width(params_format(params)));
		return (-EINVAL);
	}

	/* Check Master/Slave Mode */
	daifmt = sabre9018k2m->fmt & SND_SOC_DAIFMT_MASTER_MASK;
	if ((daifmt != SND_SOC_DAIFMT_CBS_CFS)
			&& (daifmt != SND_SOC_DAIFMT_CBM_CFM)) {
		return (-EINVAL);
	}

	return 0;
}

static int sabre9018k2m_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec     *codec = dai->codec;
	struct sabre9018k2m_priv *sabre9018k2m
					= snd_soc_codec_get_drvdata(codec);

	/* interface format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		break;

	case SND_SOC_DAIFMT_RIGHT_J:
	case SND_SOC_DAIFMT_LEFT_J:
	default:
		return (-EINVAL);
	}

	/* clock inversion */
	if ((fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF) {
		return (-EINVAL);
	}

	/* Set Audio Data Format */
	sabre9018k2m->fmt = fmt;

	return 0;
}

static int sabre9018k2m_dac_mute(struct snd_soc_dai *dai, int mute)
{
	struct snd_soc_codec *codec = dai->codec;

	if (mute) {
		snd_soc_update_bits(codec, SABRE9018K2M_REG_7, 0x03, 0x03);
	} else {
		snd_soc_update_bits(codec, SABRE9018K2M_REG_7, 0x03, 0x00);
	}

	return 0;
}


static const struct snd_soc_dai_ops sabre9018k2m_dai_ops = {
	.startup      = sabre9018k2m_dai_startup,
	.hw_params    = sabre9018k2m_hw_params,
	.set_fmt      = sabre9018k2m_set_fmt,
	.digital_mute = sabre9018k2m_dac_mute,
};

static struct snd_soc_dai_driver sabre9018k2m_dai = {
	.name = "sabre9018k2m-dai",
	.playback = {
		.stream_name  = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates        = SNDRV_PCM_RATE_8000_192000,
		.formats      = SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &sabre9018k2m_dai_ops,
};

static struct snd_soc_codec_driver sabre9018k2m_codec_driver = {
	.controls         = sabre9018k2m_controls,
	.num_controls     = ARRAY_SIZE(sabre9018k2m_controls),
};


static const struct regmap_config sabre9018k2m_regmap = {
	.reg_bits         = 8,
	.val_bits         = 8,
	.max_register     = SABRE9018K2M_MAX_REG,

	.reg_defaults     = sabre9018k2m_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(sabre9018k2m_reg_defaults),

	.writeable_reg    = sabre9018k2m_writeable,
	.readable_reg     = sabre9018k2m_readable,
	.volatile_reg     = sabre9018k2m_volatile,

	.cache_type       = REGCACHE_RBTREE,
};


bool sabre9018k2m_check_chip_id(struct snd_soc_codec *codec)
{
	unsigned int value;

	value = snd_soc_read(codec, SABRE9018K2M_REG_64);

	/* Check Chip ID */
	if (((value & 0x1C) >> 2) != 4) {
		return false;
	}
	
	value = snd_soc_read(codec, SABRE9018K2M_REG_65);

	/* Check Status */
	if (((value && 0x02) >> 1) != 0)
	{
		return false;
	}

	return true;
}
EXPORT_SYMBOL_GPL(sabre9018k2m_check_chip_id);


static int sabre9018k2m_probe(struct device *dev, struct regmap *regmap)
{
	struct sabre9018k2m_priv *sabre9018k2m;
	int ret;

	sabre9018k2m = devm_kzalloc(dev, sizeof(*sabre9018k2m), GFP_KERNEL);
	if (!sabre9018k2m) {
		dev_err(dev, "devm_kzalloc");
		return (-ENOMEM);
	}

	sabre9018k2m->regmap = regmap;

	dev_set_drvdata(dev, sabre9018k2m);

	ret = snd_soc_register_codec(dev,
			&sabre9018k2m_codec_driver, &sabre9018k2m_dai, 1);
	if (ret != 0) {
		dev_err(dev, "Failed to register CODEC: %d\n", ret);
		return ret;
	}

	return 0;
}

static void sabre9018k2m_remove(struct device *dev)
{
	snd_soc_unregister_codec(dev);
}


static int sabre9018k2m_i2c_probe(
		struct i2c_client *i2c, const struct i2c_device_id *id)
{
	struct regmap *regmap;

	regmap = devm_regmap_init_i2c(i2c, &sabre9018k2m_regmap);
	if (IS_ERR(regmap)) {
		return PTR_ERR(regmap);
	}

	return sabre9018k2m_probe(&i2c->dev, regmap);
}

static int sabre9018k2m_i2c_remove(struct i2c_client *i2c)
{
	sabre9018k2m_remove(&i2c->dev);

	return 0;
}


static const struct i2c_device_id sabre9018k2m_i2c_id[] = {
	{ "sabre9018k2m", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sabre9018k2m_i2c_id);

static const struct of_device_id sabre9018k2m_of_match[] = {
	{ .compatible = "ess,sabre9018k2m", },
	{ }
};
MODULE_DEVICE_TABLE(of, sabre9018k2m_of_match);

static struct i2c_driver sabre9018k2m_i2c_driver = {
	.driver = {
		.name           = "sabre9018k2m-i2c",
		.owner          = THIS_MODULE,
		.of_match_table = of_match_ptr(sabre9018k2m_of_match),
	},
	.probe    = sabre9018k2m_i2c_probe,
	.remove   = sabre9018k2m_i2c_remove,
	.id_table = sabre9018k2m_i2c_id,
};
module_i2c_driver(sabre9018k2m_i2c_driver);


MODULE_DESCRIPTION("ASoC SABRE9018K2M codec driver");
MODULE_AUTHOR("Howard Qiao <howard.qiao@aoide.cc>");
MODULE_LICENSE("GPL");
