// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the TAS5754M Audio Amplifier in Master mode (only)
 * supports only standard audio frequencies 44.1 to 192 ksps
 *
 * Author: Joerg Schambacher <joerg@hifiberry.com>
 * with fragments from Andy Liu <andy-liu@ti.com>
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
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/delay.h>

#include <sound/tlv.h>
#include <sound/soc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>

#include "tas5754m.h"

#define TAS5754M_RATES		(SNDRV_PCM_RATE_48000  |	\
				 SNDRV_PCM_RATE_96000  |	\
				 SNDRV_PCM_RATE_192000 |	\
				 SNDRV_PCM_RATE_44100  |	\
				 SNDRV_PCM_RATE_88200  |	\
				 SNDRV_PCM_RATE_176400)
#define TAS5754M_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE  | \
				 SNDRV_PCM_FMTBIT_S20_LE  | \
				 SNDRV_PCM_FMTBIT_S24_LE  | \
				 SNDRV_PCM_FMTBIT_S32_LE)


static const struct reg_sequence tas5754m_init_sequence[] = {
	{ TAS5754M_RESET,		0x00 },
	{ TAS5754M_MUTE,		0x11 },
	{ TAS5754M_POWER,		0x00 },
	{ TAS5754M_PLL_EN,		0x00 },
	{ TAS5754M_RESET,		0x00 },
	{ TAS5754M_GPIO_OUTPUT_3,	0x02 },
	{ TAS5754M_GPIO_OUTPUT_4,	0x02 },
	{ TAS5754M_GPIO_OUTPUT_6,	0x02 },
	{ TAS5754M_GPIO_EN,		0x2c },
	{ TAS5754M_GPIO_CONTROL_1,	0x04 },
	{ TAS5754M_BCLK_LRCLK_CFG,	0x11 },
	{ TAS5754M_MASTER_MODE,		0x7c },
	{ TAS5754M_ERROR_DETECT,	0x77 },
	{ TAS5754M_PLL_EN,		0x01 },
	{ TAS5754M_PLL_REF,		0x00 },
	{ TAS5754M_PLL_COEFF_0,		0x03 },
	{ TAS5754M_PLL_COEFF_1,		0x0c },
	{ TAS5754M_PLL_COEFF_2,		0x00 },
	{ TAS5754M_PLL_COEFF_3,		0x00 },
	{ TAS5754M_PLL_COEFF_4,		0x00 },
	{ TAS5754M_DAC_REF,		0x30 },
	{ TAS5754M_DSP_CLKDIV,		0x01 },
	{ TAS5754M_DAC_CLKDIV,		0x0f },
	{ TAS5754M_NCP_CLKDIV,		0x03 },
	{ TAS5754M_OSR_CLKDIV,		0x00 },
	{ TAS5754M_FS_SPEED_MODE,	0x00 },
	{ TAS5754M_MASTER_CLKDIV_1,	0x0f },
	{ TAS5754M_MASTER_CLKDIV_2,	0x1f },
	{ TAS5754M_I2S_1,		0x00 },
	{ TAS5754M_I2S_2,		0x01 },
	{ TAS5754M_PLL_EN,		0x01 },
	{ TAS5754M_MASTER_MODE,		0x7f },
	{ TAS5754M_MUTE,		0x11 },
};

static const struct reg_default tas5754m_reg_defaults[] = {
	{ TAS5754M_RESET,             0x00 },
	{ TAS5754M_POWER,             0x00 },
	{ TAS5754M_MUTE,              0x00 },
	{ TAS5754M_DSP,               0x00 },
	{ TAS5754M_PLL_REF,           0x00 },
	{ TAS5754M_DAC_REF,           0x00 },
	{ TAS5754M_DAC_ROUTING,       0x11 },
	{ TAS5754M_DSP_PROGRAM,       0x01 },
	{ TAS5754M_CLKDET,            0x00 },
	{ TAS5754M_AUTO_MUTE,         0x00 },
	{ TAS5754M_ERROR_DETECT,      0x00 },
	{ TAS5754M_DIGITAL_VOLUME_1,  0x00 },
	{ TAS5754M_DIGITAL_VOLUME_2,  0x30 },
	{ TAS5754M_DIGITAL_VOLUME_3,  0x30 },
	{ TAS5754M_DIGITAL_MUTE_1,    0x22 },
	{ TAS5754M_DIGITAL_MUTE_2,    0x00 },
	{ TAS5754M_DIGITAL_MUTE_3,    0x07 },
	{ TAS5754M_OUTPUT_AMPLITUDE,  0x00 },
	{ TAS5754M_ANALOG_GAIN_CTRL,  0x00 },
	{ TAS5754M_UNDERVOLTAGE_PROT, 0x00 },
	{ TAS5754M_ANALOG_MUTE_CTRL,  0x00 },
	{ TAS5754M_ANALOG_GAIN_BOOST, 0x00 },
	{ TAS5754M_VCOM_CTRL_1,       0x00 },
	{ TAS5754M_VCOM_CTRL_2,       0x01 },
	{ TAS5754M_BCLK_LRCLK_CFG,    0x00 },
	{ TAS5754M_MASTER_MODE,       0x7c },
	{ TAS5754M_GPIO_DACIN,        0x00 },
	{ TAS5754M_GPIO_PLLIN,        0x00 },
	{ TAS5754M_SYNCHRONIZE,       0x10 },
	{ TAS5754M_PLL_COEFF_0,       0x00 },
	{ TAS5754M_PLL_COEFF_1,       0x00 },
	{ TAS5754M_PLL_COEFF_2,       0x00 },
	{ TAS5754M_PLL_COEFF_3,       0x00 },
	{ TAS5754M_PLL_COEFF_4,       0x00 },
	{ TAS5754M_DSP_CLKDIV,        0x00 },
	{ TAS5754M_DAC_CLKDIV,        0x00 },
	{ TAS5754M_NCP_CLKDIV,        0x00 },
	{ TAS5754M_OSR_CLKDIV,        0x00 },
	{ TAS5754M_MASTER_CLKDIV_1,   0x00 },
	{ TAS5754M_MASTER_CLKDIV_2,   0x00 },
	{ TAS5754M_FS_SPEED_MODE,     0x00 },
	{ TAS5754M_IDAC_1,            0x01 },
	{ TAS5754M_IDAC_2,            0x00 },
};

static bool tas5754m_readable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TAS5754M_RESET:
	case TAS5754M_POWER:
	case TAS5754M_MUTE:
	case TAS5754M_PLL_EN:
	case TAS5754M_SPI_MISO_FUNCTION:
	case TAS5754M_DSP:
	case TAS5754M_GPIO_EN:
	case TAS5754M_BCLK_LRCLK_CFG:
	case TAS5754M_DSP_GPIO_INPUT:
	case TAS5754M_MASTER_MODE:
	case TAS5754M_PLL_REF:
	case TAS5754M_DAC_REF:
	case TAS5754M_GPIO_DACIN:
	case TAS5754M_GPIO_PLLIN:
	case TAS5754M_SYNCHRONIZE:
	case TAS5754M_PLL_COEFF_0:
	case TAS5754M_PLL_COEFF_1:
	case TAS5754M_PLL_COEFF_2:
	case TAS5754M_PLL_COEFF_3:
	case TAS5754M_PLL_COEFF_4:
	case TAS5754M_DSP_CLKDIV:
	case TAS5754M_DAC_CLKDIV:
	case TAS5754M_NCP_CLKDIV:
	case TAS5754M_OSR_CLKDIV:
	case TAS5754M_MASTER_CLKDIV_1:
	case TAS5754M_MASTER_CLKDIV_2:
	case TAS5754M_FS_SPEED_MODE:
	case TAS5754M_IDAC_1:
	case TAS5754M_IDAC_2:
	case TAS5754M_ERROR_DETECT:
	case TAS5754M_I2S_1:
	case TAS5754M_I2S_2:
	case TAS5754M_DAC_ROUTING:
	case TAS5754M_DSP_PROGRAM:
	case TAS5754M_CLKDET:
	case TAS5754M_AUTO_MUTE:
	case TAS5754M_DIGITAL_VOLUME_1:
	case TAS5754M_DIGITAL_VOLUME_2:
	case TAS5754M_DIGITAL_VOLUME_3:
	case TAS5754M_DIGITAL_MUTE_1:
	case TAS5754M_DIGITAL_MUTE_2:
	case TAS5754M_DIGITAL_MUTE_3:
	case TAS5754M_GPIO_OUTPUT_1:
	case TAS5754M_GPIO_OUTPUT_2:
	case TAS5754M_GPIO_OUTPUT_3:
	case TAS5754M_GPIO_OUTPUT_4:
	case TAS5754M_GPIO_OUTPUT_5:
	case TAS5754M_GPIO_OUTPUT_6:
	case TAS5754M_GPIO_CONTROL_1:
	case TAS5754M_GPIO_CONTROL_2:
	case TAS5754M_OVERFLOW:
	case TAS5754M_RATE_DET_1:
	case TAS5754M_RATE_DET_2:
	case TAS5754M_RATE_DET_3:
	case TAS5754M_RATE_DET_4:
	case TAS5754M_CLOCK_STATUS:
	case TAS5754M_ANALOG_MUTE_DET:
	case TAS5754M_GPIN:
	case TAS5754M_DIGITAL_MUTE_DET:
	case TAS5754M_OUTPUT_AMPLITUDE:
	case TAS5754M_ANALOG_GAIN_CTRL:
	case TAS5754M_UNDERVOLTAGE_PROT:
	case TAS5754M_ANALOG_MUTE_CTRL:
	case TAS5754M_ANALOG_GAIN_BOOST:
	case TAS5754M_VCOM_CTRL_1:
	case TAS5754M_VCOM_CTRL_2:
	case TAS5754M_CRAM_CTRL:
	case TAS5754M_FLEX_A:
	case TAS5754M_FLEX_B:
		return true;
	default:
		return reg < 0x7f;
	}
}

static bool tas5754m_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TAS5754M_PLL_EN:
	case TAS5754M_OVERFLOW:
	case TAS5754M_RATE_DET_1:
	case TAS5754M_RATE_DET_2:
	case TAS5754M_RATE_DET_3:
	case TAS5754M_RATE_DET_4:
	case TAS5754M_CLOCK_STATUS:
	case TAS5754M_ANALOG_MUTE_DET:
	case TAS5754M_GPIN:
	case TAS5754M_DIGITAL_MUTE_DET:
	case TAS5754M_CRAM_CTRL:
		return true;
	default:
		return reg < 0x7f;
	}
}

struct tas5754m_priv {
	struct regmap *regmap;
	struct clk *sclk;
};

static const struct regmap_range_cfg tas5754m_range = {
	.name = "Pages",
	.range_min = TAS5754M_VIRT_BASE,
	.range_max = TAS5754M_MAX_REGISTER,
	.selector_reg = TAS5754M_PAGE,
	.selector_mask = 0x7f,
	.window_start = 0,
	.window_len = 128,
};

const struct regmap_config tas5754m_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.ranges = &tas5754m_range,
	.num_ranges = 1,
	.max_register = TAS5754M_MAX_REGISTER,

	.reg_defaults = tas5754m_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(tas5754m_reg_defaults),
	.readable_reg = tas5754m_readable,
	.volatile_reg = tas5754m_volatile,

	.cache_type = REGCACHE_RBTREE,
};
EXPORT_SYMBOL_GPL(tas5754m_regmap);

static const DECLARE_TLV_DB_SCALE(digital_tlv, -10350, 50, 1);
static const DECLARE_TLV_DB_SCALE(analog_tlv, -600, 600, 0);

static const struct snd_kcontrol_new tas5754m_controls[] = {
SOC_DOUBLE_R_TLV("Digital Playback Volume", TAS5754M_DIGITAL_VOLUME_2,
		 TAS5754M_DIGITAL_VOLUME_3, 0, 255, 1, digital_tlv),
SOC_DOUBLE_TLV("Analog Playback Volume", TAS5754M_ANALOG_GAIN_CTRL,
	     TAS5754M_LAGN_SHIFT, TAS5754M_RAGN_SHIFT, 1, 1, analog_tlv),
};

static int tas5754m_set_bias_level(struct snd_soc_component *component,
					enum snd_soc_bias_level level)
{
	struct tas5754m_priv *tas5754m =
				snd_soc_component_get_drvdata(component);
	int ret;

	switch (level) {
	case SND_SOC_BIAS_ON:
	case SND_SOC_BIAS_PREPARE:
		break;

	case SND_SOC_BIAS_STANDBY:
		ret = regmap_update_bits(tas5754m->regmap,
				TAS5754M_POWER, TAS5754M_RQST, 0);
		if (ret != 0) {
			dev_err(component->dev,
				"Failed to remove standby: %d\n", ret);
			return ret;
		}
		break;

	case SND_SOC_BIAS_OFF:
		ret = regmap_update_bits(tas5754m->regmap,
				TAS5754M_POWER, TAS5754M_RQST, TAS5754M_RQST);
		if (ret != 0) {
			dev_err(component->dev,
				"Failed to request standby: %d\n", ret);
			return ret;
		}
		break;
	}

	return 0;
}

static int tas5754m_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct tas5754m_priv *tas5754m =
			snd_soc_component_get_drvdata(component);
	unsigned long bclk;
	unsigned long mclk;
	int sample_len;
	int bclk_div;
	int lrclk_div;
	int alen;
	int ret;

	switch (params_width(params)) {
	case 16:
		sample_len = 16;
		alen = TAS5754M_ALEN_16;
		break;
	case 20:
		sample_len = 32;
		alen = TAS5754M_ALEN_20;
		break;
	case 24:
		sample_len = 32;
		alen = TAS5754M_ALEN_24;
		break;
	case 32:
		sample_len = 32;
		alen = TAS5754M_ALEN_32;
		break;
	default:
		dev_err(component->dev, "Unsupported sample size: %d\n",
			params_width(params));
		return -EINVAL;
	}
	ret = regmap_update_bits(tas5754m->regmap, TAS5754M_I2S_1, alen, alen);
	if (ret != 0) {
		dev_err(component->dev,
			"Cannot set sample size: %d\n", ret);
		return ret;
	}

	switch (params_rate(params)) {
	case 44100:
	case 48000:
		ret = regmap_write(tas5754m->regmap,
			TAS5754M_FS_SPEED_MODE, TAS5754M_FSSP_48KHZ);
		break;
	case 88200:
	case 96000:
		ret = regmap_write(tas5754m->regmap,
			TAS5754M_FS_SPEED_MODE, TAS5754M_FSSP_96KHZ);
		break;
	case 176400:
	case 192000:
		ret = regmap_write(tas5754m->regmap,
			TAS5754M_FS_SPEED_MODE, TAS5754M_FSSP_192KHZ);
		break;
	default:
		dev_err(component->dev, "Sample rate not supported: %d\n",
			params_rate(params));
		return -EINVAL;
	}
	if (ret != 0) {
		dev_err(component->dev, "Failed to config PLL\n");
		return ret;
	}


	mclk = clk_get_rate(tas5754m->sclk);
	bclk = sample_len * 2 * params_rate(params);
	bclk_div = mclk / bclk;
	lrclk_div = sample_len * 2;

	// stop LR / SCLK clocks
	ret = regmap_write(tas5754m->regmap, TAS5754M_MASTER_MODE, 0x7c);

	// set SCLK divider
	ret |= regmap_write(tas5754m->regmap, TAS5754M_MASTER_CLKDIV_1,
								bclk_div - 1);

	// set LRCLK divider
	ret |= regmap_write(tas5754m->regmap, TAS5754M_MASTER_CLKDIV_2,
								lrclk_div - 1);

	// restart LR / SCLK clocks
	ret |= regmap_write(tas5754m->regmap, TAS5754M_MASTER_MODE, 0x7f);
	if (ret != 0) {
		dev_err(component->dev, "Failed to config PLL\n");
		return ret;
	}

	return 0;
}

const static struct snd_soc_component_driver tas5754m_soc_component = {
	.set_bias_level = tas5754m_set_bias_level,
	.idle_bias_on = true,
	.controls = tas5754m_controls,
	.num_controls = ARRAY_SIZE(tas5754m_controls),
};

static int tas5754m_mute(struct snd_soc_dai *dai, int mute, int stream)
{
	struct snd_soc_component *component = dai->component;

	if (mute) {
		snd_soc_component_write(component, TAS5754M_MUTE, 0x11);
	} else {
		usleep_range(1000, 2000);
		snd_soc_component_write(component, TAS5754M_MUTE, 0x00);
	}
	return 0;
}

static const struct snd_soc_dai_ops tas5754m_dai_ops = {
	.mute_stream = tas5754m_mute,
	.hw_params = tas5754m_hw_params,
};

static struct snd_soc_dai_driver tas5754m_dai = {
	.name		= "tas5754m-amplifier",
	.playback	= {
		.stream_name	= "Playback",
		.channels_min	= 2,
		.channels_max	= 2,
		.rates		= TAS5754M_RATES,
		.formats	= TAS5754M_FORMATS,
	},
	.ops = &tas5754m_dai_ops,
};

static int tas5754m_probe(struct device *dev, struct regmap *regmap)
{
	struct tas5754m_priv *tas5754m;
	int ret;

	tas5754m = devm_kzalloc(dev, sizeof(struct tas5754m_priv), GFP_KERNEL);
	if (!tas5754m)
		return -ENOMEM;

	dev_set_drvdata(dev, tas5754m);
	tas5754m->regmap = regmap;

	ret = regmap_multi_reg_write(regmap, tas5754m_init_sequence,
					ARRAY_SIZE(tas5754m_init_sequence));

	if (ret != 0) {
		dev_err(dev, "Failed to initialize TAS5754M: %d\n", ret);
		goto err;
	}

	tas5754m->sclk = devm_clk_get(dev, NULL);
	if (PTR_ERR(tas5754m->sclk) == -EPROBE_DEFER) {
		ret = -EPROBE_DEFER;
		goto err;
	}
	if (!IS_ERR(tas5754m->sclk)) {
		ret = clk_prepare_enable(tas5754m->sclk);
		if (ret != 0) {
			dev_err(dev, "Failed to enable SCLK: %d\n", ret);
			goto err;
		}
	}

	ret = snd_soc_register_component(dev,
			&tas5754m_soc_component, &tas5754m_dai, 1);
	if (ret != 0) {
		dev_err(dev, "Failed to register CODEC: %d\n", ret);
		goto err;
	}

	return 0;

err:
	return ret;

}

static int tas5754m_i2c_probe(struct i2c_client *i2c, const struct i2c_device_id *id)
{
	struct regmap *regmap;
	struct regmap_config config = tas5754m_regmap;

	/* enable auto-increment mode */
	config.read_flag_mask = 0x80;
	config.write_flag_mask = 0x80;

	regmap = devm_regmap_init_i2c(i2c, &config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return tas5754m_probe(&i2c->dev, regmap);
}

static int tas5754m_remove(struct device *dev)
{
	snd_soc_unregister_component(dev);

	return 0;
}

static int tas5754m_i2c_remove(struct i2c_client *i2c)
{
	tas5754m_remove(&i2c->dev);

	return 0;
}

static const struct i2c_device_id tas5754m_i2c_id[] = {
	{ "tas5754m", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tas5754m_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id tas5754m_of_match[] = {
	{ .compatible = "ti,tas5754m", },
	{ .compatible = "ti,tas5756m", },
	{ }
};
MODULE_DEVICE_TABLE(of, tas5754m_of_match);
#endif

static struct i2c_driver tas5754m_i2c_driver = {
	.probe		= tas5754m_i2c_probe,
	.remove		= tas5754m_i2c_remove,
	.id_table	= tas5754m_i2c_id,
	.driver		= {
		.name	= "tas5754m",
		.of_match_table = tas5754m_of_match,
	},
};

module_i2c_driver(tas5754m_i2c_driver);

MODULE_AUTHOR("Joerg Schambacher <joerg@hifiberry.com>");
MODULE_DESCRIPTION("TAS5754M Audio Amplifier Driver - Master mode only");
MODULE_LICENSE("GPL");
