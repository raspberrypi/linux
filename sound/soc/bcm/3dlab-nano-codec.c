/*
 * 3Dlab Nano codec ALSA SoC Audio driver.
 *
 * Copyright (C) 2018 3Dlab.
 *
 * Author: GT <dev@3d-lab-av.com>
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
#include <linux/i2c.h>
#include <sound/soc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/tlv.h>

#define NANO_ID		0x00
#define NANO_VER	0x01
#define NANO_CFG	0x02
#define NANO_STATUS	0x03
#define NANO_SPI_ADDR	0x04
#define NANO_SPI_DATA	0x05

#define NANO_ID_VAL	0x3D
#define NANO_CFG_OFF	0x00
#define NANO_CFG_MULT1	0
#define NANO_CFG_MULT2	1
#define NANO_CFG_MULT4	2
#define NANO_CFG_MULT8	3
#define NANO_CFG_MULT16	4
#define NANO_CFG_CLK22	0
#define NANO_CFG_CLK24	BIT(3)
#define NANO_CFG_DSD	BIT(4)
#define NANO_CFG_ENA	BIT(5)
#define NANO_CFG_BLINK	BIT(6)
#define NANO_STATUS_P1  BIT(0)
#define NANO_STATUS_P2  BIT(1)
#define NANO_STATUS_FLG BIT(2)
#define NANO_STATUS_CLK BIT(3)
#define NANO_SPI_READ	0
#define NANO_SPI_WRITE	BIT(5)

#define NANO_DAC_BASE	0x80
#define NANO_DAC_LATT	(NANO_DAC_BASE + 0x03)
#define NANO_DAC_RATT	(NANO_DAC_BASE + 0x04)

static const DECLARE_TLV_DB_SCALE(master_tlv, -12750, 50, 1);

static const struct snd_kcontrol_new nano_codec_snd_controls[] = {
	SOC_DOUBLE_R_TLV("Master Playback Volume", NANO_DAC_LATT, NANO_DAC_RATT,
			 0, 255, 0, master_tlv),
};

static const unsigned int nano_codec_rates[] = {
	44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000,
	705600, 768000 // only possible with fast clocks
};

static struct snd_pcm_hw_constraint_list nano_codec_constraint_rates = {
	.list	= nano_codec_rates,
	.count	= ARRAY_SIZE(nano_codec_rates),
};

static int nano_codec_startup(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	return snd_pcm_hw_constraint_list(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_RATE,
					  &nano_codec_constraint_rates);
}

static int nano_codec_set_fmt(struct snd_soc_dai *dai,
			      unsigned int fmt)
{
	/* only one format is supported by hardware */
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_CLOCK_MASK) != SND_SOC_DAIFMT_CONT)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBM_CFM)
		return -EINVAL;

	return 0;
}

static int nano_codec_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	unsigned int config;

	/* configure clocks */
	switch (params_rate(params)) {
	case 44100:
		config = NANO_CFG_MULT1 | NANO_CFG_CLK22;
		break;
	case 88200:
		config = NANO_CFG_MULT2 | NANO_CFG_CLK22;
		break;
	case 176400:
		config = NANO_CFG_MULT4 | NANO_CFG_CLK22;
		break;
	case 352800:
		config = NANO_CFG_MULT8 | NANO_CFG_CLK22;
		break;
	case 705600:
		config = NANO_CFG_MULT16 | NANO_CFG_CLK22;
		break;
	case 48000:
		config = NANO_CFG_MULT1 | NANO_CFG_CLK24;
		break;
	case 96000:
		config = NANO_CFG_MULT2 | NANO_CFG_CLK24;
		break;
	case 192000:
		config = NANO_CFG_MULT4 | NANO_CFG_CLK24;
		break;
	case 384000:
		config = NANO_CFG_MULT8 | NANO_CFG_CLK24;
		break;
	case 768000:
		config = NANO_CFG_MULT16 | NANO_CFG_CLK24;
		break;
	default:
		return -EINVAL;
	}

	/* enable DSD mode */
	if (snd_soc_codec_get_drvdata(dai->codec))
		config |= NANO_CFG_DSD;

	/* enable audio bus */
	config |= NANO_CFG_ENA;

	dev_dbg(dai->dev, "Send I2C CFG register 0x%02X\n", config);
	return snd_soc_write(dai->codec, NANO_CFG, config);
}

static const struct snd_soc_dai_ops nano_codec_ops = {
	.startup	= nano_codec_startup,
	.set_fmt	= nano_codec_set_fmt,
	.hw_params	= nano_codec_hw_params,
};

static struct snd_soc_dai_driver nano_codec_dai = {
	.name = "nano-hifi",
	.playback = {
		.stream_name	= "Playback",
		.channels_min	= 2,
		.channels_max	= 2,
		.rate_min	= 44100,
		.rate_max	= 768000,
		.rates		= SNDRV_PCM_RATE_KNOT,
		.formats	= SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &nano_codec_ops,
};

static int nano_codec_spi_addr(unsigned int reg, unsigned int mode)
{
	/* compute SPI address from regbank address */
	int chip = reg & 0x60;
	int addr = reg & 0x1F;

	return (chip << 1) | addr | mode;
}

static int nano_codec_reg_read_i2c(void *context, unsigned int reg,
				   unsigned int *val)
{
	/* direct I2C read access */
	struct i2c_client *i2c = context;
	int ret;

	ret = i2c_smbus_read_byte_data(i2c, reg);
	if (ret < 0)
		return ret;

	*val = ret;
	return 0;
}

static int nano_codec_reg_read_spi(void *context, unsigned int reg,
				   unsigned int *val)
{
	/* indirect SPI read access */
	struct i2c_client *i2c = context;
	int ret, addr;

	addr = nano_codec_spi_addr(reg, NANO_SPI_READ);
	ret = i2c_smbus_write_byte_data(i2c, NANO_SPI_ADDR, addr);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_read_byte_data(i2c, NANO_SPI_DATA);
	if (ret < 0)
		return ret;

	*val = ret;
	return 0;
}

static int nano_codec_reg_read(void *context, unsigned int reg,
			       unsigned int *val)
{
	/* dispatch read access */
	if (reg < 128)
		return nano_codec_reg_read_i2c(context, reg, val);
	else
		return nano_codec_reg_read_spi(context, reg, val);
}

static int nano_codec_reg_write_i2c(void *context, unsigned int reg,
				    unsigned int val)
{
	/* direct I2C write access */
	struct i2c_client *i2c = context;

	return i2c_smbus_write_byte_data(i2c, reg, val);
}

static int nano_codec_reg_write_spi(void *context, unsigned int reg,
				    unsigned int val)
{
	/* indirect SPI write access */
	struct i2c_client *i2c = context;
	int ret, addr;

	ret = i2c_smbus_write_byte_data(i2c, NANO_SPI_DATA, val);
	if (ret < 0)
		return ret;

	addr = nano_codec_spi_addr(reg, NANO_SPI_WRITE);
	return i2c_smbus_write_byte_data(i2c, NANO_SPI_ADDR, addr);
}

static int nano_codec_reg_write(void *context, unsigned int reg,
				unsigned int val)
{
	/* dispatch write access */
	if (reg < 128)
		return nano_codec_reg_write_i2c(context, reg, val);
	else
		return nano_codec_reg_write_spi(context, reg, val);
}

static bool nano_codec_volatile_reg(struct device *dev, unsigned int reg)
{
	return false;
}

static const struct regmap_config nano_codec_regmap = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= 255,
	.cache_type	= REGCACHE_RBTREE,
	.reg_read	= nano_codec_reg_read,
	.reg_write	= nano_codec_reg_write,
	.volatile_reg	= nano_codec_volatile_reg,
};

static int nano_codec_driver_probe(struct snd_soc_codec *codec)
{
	unsigned int val;

	/* check if hardware looks legitimate */
	val = snd_soc_read(codec, NANO_ID);
	if (val != NANO_ID_VAL) {
		dev_err(codec->dev, "Invalid I2C ID register 0x%02X\n", val);
		return -ENODEV;
	}

	/* report status to the user */
	val = snd_soc_read(codec, NANO_VER);
	dev_notice(codec->dev, "Started 3Dlab codec driver (ver. %d)\n", val);

	/* configure supported stream rates */
	val = snd_soc_read(codec, NANO_STATUS);
	if (val & NANO_STATUS_CLK) {
		dev_notice(codec->dev, "Board with fast clocks installed\n");
		nano_codec_dai.playback.rate_max = 768000;
	} else {
		dev_notice(codec->dev, "Board with normal clocks installed\n");
		nano_codec_dai.playback.rate_max = 384000;
	}

	/* enable internal audio bus and blink status LED */
	return snd_soc_write(codec, NANO_CFG, NANO_CFG_ENA | NANO_CFG_BLINK);
}

static int nano_codec_driver_remove(struct snd_soc_codec *codec)
{
	/* disable internal audio bus */
	return snd_soc_write(codec, NANO_CFG, NANO_CFG_OFF);
}

static struct snd_soc_codec_driver nano_codec_driver = {
	.component_driver = {
		.controls	= nano_codec_snd_controls,
		.num_controls	= ARRAY_SIZE(nano_codec_snd_controls),
	},
	.probe	= nano_codec_driver_probe,
	.remove	= nano_codec_driver_remove,
};

static int nano_codec_i2c_probe(struct i2c_client *i2c,
				const struct i2c_device_id *id)
{
	struct regmap *regmap;
	int ret;

	regmap = devm_regmap_init(&i2c->dev, NULL, i2c, &nano_codec_regmap);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&i2c->dev, "Failed to init regmap %d\n", ret);
		return ret;
	}

	ret = snd_soc_register_codec(&i2c->dev, &nano_codec_driver,
				     &nano_codec_dai, 1);

	if (ret != 0)
		dev_err(&i2c->dev, "Failed to register codec %d\n", ret);

	return ret;
}

static int nano_codec_i2c_remove(struct i2c_client *client)
{
	snd_soc_unregister_codec(&client->dev);
	return 0;
}

static const struct of_device_id nano_codec_of_match[] = {
	{ .compatible = "3dlab,nano-codec", },
	{ }
};
MODULE_DEVICE_TABLE(of, nano_codec_of_match);

static const struct i2c_device_id nano_codec_i2c_id[] = {
	{ "nano-codec", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nano_codec_i2c_id);

static struct i2c_driver nano_codec_i2c_driver = {
	.probe	= nano_codec_i2c_probe,
	.remove	= nano_codec_i2c_remove,
	.id_table = nano_codec_i2c_id,
	.driver	= {
		.name		= "nano-codec",
		.owner		= THIS_MODULE,
		.of_match_table	= nano_codec_of_match,
	},
};

module_i2c_driver(nano_codec_i2c_driver);

MODULE_DESCRIPTION("ASoC 3Dlab Nano codec driver");
MODULE_AUTHOR("GT <dev@3d-lab-av.com>");
MODULE_LICENSE("GPL v2");

/* EOF */
