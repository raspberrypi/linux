/*
 * ASoC Driver for TAS5713
 *
 * Author:	Sebastian Eickhoff <basti.eickhoff@googlemail.com>
 *		Copyright 2014
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
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/of_device.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/initval.h>
#include <sound/tlv.h>

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <asm/uaccess.h>

#include "tas5713.h"


static struct i2c_client *i2c;

struct tas5713_priv {
	struct regmap *regmap;
	int mclk_div;
	struct snd_soc_component *component;
};

static struct tas5713_priv *priv_data;




/*
 *    _   _    ___   _      ___         _           _
 *   /_\ | |  / __| /_\    / __|___ _ _| |_ _ _ ___| |___
 *  / _ \| |__\__ \/ _ \  | (__/ _ \ ' \  _| '_/ _ \ (_-<
 * /_/ \_\____|___/_/ \_\  \___\___/_||_\__|_| \___/_/__/
 *
 */

static const DECLARE_TLV_DB_SCALE(tas5713_vol_tlv, -10000, 50, 1);


static const struct snd_kcontrol_new tas5713_snd_controls[] = {
	SOC_SINGLE_TLV  ("Master"    , TAS5713_VOL_MASTER, 0, 248, 1, tas5713_vol_tlv),
	SOC_DOUBLE_R_TLV("Channels"  , TAS5713_VOL_CH1, TAS5713_VOL_CH2, 0, 248, 1, tas5713_vol_tlv)
};




/*
 *  __  __         _    _            ___      _
 * |  \/  |__ _ __| |_ (_)_ _  ___  |   \ _ _(_)_ _____ _ _
 * | |\/| / _` / _| ' \| | ' \/ -_) | |) | '_| \ V / -_) '_|
 * |_|  |_\__,_\__|_||_|_|_||_\___| |___/|_| |_|\_/\___|_|
 *
 */

static int tas5713_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	u16 blen = 0x00;

	struct snd_soc_component *component = dai->component;
	priv_data->component = component;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		blen = 0x03;
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		blen = 0x1;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		blen = 0x04;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		blen = 0x05;
		break;
	default:
		dev_err(dai->dev, "Unsupported word length: %u\n",
			params_format(params));
		return -EINVAL;
	}

	// set word length
	snd_soc_component_update_bits(component, TAS5713_SERIAL_DATA_INTERFACE, 0x7, blen);

	return 0;
}


static int tas5713_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	unsigned int val = 0;

	struct tas5713_priv *tas5713;
	struct snd_soc_component *component = dai->component;
	tas5713 = snd_soc_component_get_drvdata(component);

	if (mute) {
		val = TAS5713_SOFT_MUTE_ALL;
	}

	return regmap_write(tas5713->regmap, TAS5713_SOFT_MUTE, val);
}


static const struct snd_soc_dai_ops tas5713_dai_ops = {
	.hw_params 		= tas5713_hw_params,
	.mute_stream	= tas5713_mute_stream,
};


static struct snd_soc_dai_driver tas5713_dai = {
	.name		= "tas5713-hifi",
	.playback 	= {
		.stream_name	= "Playback",
		.channels_min	= 2,
		.channels_max	= 2,
		.rates		    = SNDRV_PCM_RATE_8000_48000,
		.formats	    = (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE ),
	},
	.ops        = &tas5713_dai_ops,
};




/*
 *   ___         _          ___      _
 *  / __|___  __| |___ __  |   \ _ _(_)_ _____ _ _
 * | (__/ _ \/ _` / -_) _| | |) | '_| \ V / -_) '_|
 *  \___\___/\__,_\___\__| |___/|_| |_|\_/\___|_|
 *
 */

static void tas5713_remove(struct snd_soc_component *component)
{
	struct tas5713_priv *tas5713;

	tas5713 = snd_soc_component_get_drvdata(component);
}


static int tas5713_probe(struct snd_soc_component *component)
{
	struct tas5713_priv *tas5713;
	int i, ret;

	i2c = container_of(component->dev, struct i2c_client, dev);

	tas5713 = snd_soc_component_get_drvdata(component);

	// Reset error
	ret = snd_soc_component_write(component, TAS5713_ERROR_STATUS, 0x00);
	if (ret < 0) return ret;

	// Trim oscillator
	ret = snd_soc_component_write(component, TAS5713_OSC_TRIM, 0x00);
	if (ret < 0) return ret;
	msleep(1000);

	// Reset error
	ret = snd_soc_component_write(component, TAS5713_ERROR_STATUS, 0x00);
	if (ret < 0) return ret;

	// Clock mode: 44/48kHz, MCLK=64xfs
	ret = snd_soc_component_write(component, TAS5713_CLOCK_CTRL, 0x60);
	if (ret < 0) return ret;

	// I2S 24bit
	ret = snd_soc_component_write(component, TAS5713_SERIAL_DATA_INTERFACE, 0x05);
	if (ret < 0) return ret;

	// Unmute
	ret = snd_soc_component_write(component, TAS5713_SYSTEM_CTRL2, 0x00);
	if (ret < 0) return ret;
	ret = snd_soc_component_write(component, TAS5713_SOFT_MUTE, 0x00);
	if (ret < 0) return ret;

	// Set volume to 0db
	ret = snd_soc_component_write(component, TAS5713_VOL_MASTER, 0x00);
	if (ret < 0) return ret;

	// Now start programming the default initialization sequence
	for (i = 0; i < ARRAY_SIZE(tas5713_init_sequence); ++i) {
		ret = i2c_master_send(i2c,
				     tas5713_init_sequence[i].data,
				     tas5713_init_sequence[i].size);
		if (ret < 0) {
			printk(KERN_INFO "TAS5713 CODEC PROBE: InitSeq returns: %d\n", ret);
		}
	}

	// Unmute
	ret = snd_soc_component_write(component, TAS5713_SYSTEM_CTRL2, 0x00);
	if (ret < 0) return ret;

	return 0;
}


static struct snd_soc_component_driver soc_codec_dev_tas5713 = {
	.probe = tas5713_probe,
	.remove = tas5713_remove,
	.controls = tas5713_snd_controls,
	.num_controls = ARRAY_SIZE(tas5713_snd_controls),
};




/*
 *   ___ ___ ___   ___      _
 *  |_ _|_  ) __| |   \ _ _(_)_ _____ _ _
 *   | | / / (__  | |) | '_| \ V / -_) '_|
 *  |___/___\___| |___/|_| |_|\_/\___|_|
 *
 */

static const struct reg_default tas5713_reg_defaults[] = {
	{ 0x07 ,0x80 },     // R7  - VOL_MASTER    - -40dB
	{ 0x08 ,  30 },     // R8  - VOL_CH1	   -   0dB
	{ 0x09 ,  30 },     // R9  - VOL_CH2       -   0dB
	{ 0x0A ,0x80 },     // R10 - VOL_HEADPHONE - -40dB
};


static bool tas5713_reg_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
		case TAS5713_DEVICE_ID:
		case TAS5713_ERROR_STATUS:
			return true;
	default:
			return false;
	}
}


static const struct of_device_id tas5713_of_match[] = {
	{ .compatible = "ti,tas5713", },
	{ }
};
MODULE_DEVICE_TABLE(of, tas5713_of_match);


static struct regmap_config tas5713_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = TAS5713_MAX_REGISTER,
	.volatile_reg = tas5713_reg_volatile,

	.cache_type = REGCACHE_RBTREE,
	.reg_defaults = tas5713_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(tas5713_reg_defaults),
};


static int tas5713_i2c_probe(struct i2c_client *i2c,
			    const struct i2c_device_id *id)
{
	int ret;

	priv_data = devm_kzalloc(&i2c->dev, sizeof *priv_data, GFP_KERNEL);
	if (!priv_data)
		return -ENOMEM;

	priv_data->regmap = devm_regmap_init_i2c(i2c, &tas5713_regmap_config);
	if (IS_ERR(priv_data->regmap)) {
		ret = PTR_ERR(priv_data->regmap);
		return ret;
	}

	i2c_set_clientdata(i2c, priv_data);

	ret = snd_soc_register_component(&i2c->dev,
				     &soc_codec_dev_tas5713, &tas5713_dai, 1);

	return ret;
}


static int tas5713_i2c_remove(struct i2c_client *i2c)
{
	snd_soc_unregister_component(&i2c->dev);
	i2c_set_clientdata(i2c, NULL);

	kfree(priv_data);

	return 0;
}


static const struct i2c_device_id tas5713_i2c_id[] = {
	{ "tas5713", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, tas5713_i2c_id);


static struct i2c_driver tas5713_i2c_driver = {
	.driver = {
		.name = "tas5713",
		.owner = THIS_MODULE,
		.of_match_table = tas5713_of_match,
	},
	.probe = tas5713_i2c_probe,
	.remove = tas5713_i2c_remove,
	.id_table = tas5713_i2c_id
};


static int __init tas5713_modinit(void)
{
	int ret = 0;

	ret = i2c_add_driver(&tas5713_i2c_driver);
	if (ret) {
		printk(KERN_ERR "Failed to register tas5713 I2C driver: %d\n",
		       ret);
	}

	return ret;
}
module_init(tas5713_modinit);


static void __exit tas5713_exit(void)
{
	i2c_del_driver(&tas5713_i2c_driver);
}
module_exit(tas5713_exit);


MODULE_AUTHOR("Sebastian Eickhoff <basti.eickhoff@googlemail.com>");
MODULE_DESCRIPTION("ASoC driver for TAS5713");
MODULE_LICENSE("GPL v2");
