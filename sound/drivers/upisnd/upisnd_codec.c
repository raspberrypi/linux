// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pisound Micro Linux kernel module.
 * Copyright (C) 2017-2025  Vilniaus Blokas UAB, https://blokas.io/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/list.h>
#include <linux/gcd.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "upisnd_codec.h"
#include "upisnd_debug.h"

// Based on ADAU1361/ADAU1461/ADAU1761/ADAU1961 driver by Lars-Peter Clausen.
// Customized specifically for ADAU1961 on Pisound Micro.

static int adau_calc_pll_cfg(unsigned int freq_in, unsigned int freq_out,
			     u8 regs[5])
{
	unsigned int r, n, m, i, j;
	unsigned int div;

	if (!freq_out) {
		r = 0;
		n = 0;
		m = 0;
		div = 0;
	} else {
		if (freq_out % freq_in != 0) {
			div = DIV_ROUND_UP(freq_in, 13500000);
			freq_in /= div;
			r = freq_out / freq_in;
			i = freq_out % freq_in;
			j = gcd(i, freq_in);
			n = i / j;
			m = freq_in / j;
			div--;
		} else {
			r = freq_out / freq_in;
			n = 0;
			m = 0;
			div = 0;
		}
		if (n > 0xffff || m > 0xffff || div > 3 || r > 8 || r < 2)
			return -EINVAL;
	}

	regs[0] = m >> 8;
	regs[1] = m & 0xff;
	regs[2] = n >> 8;
	regs[3] = n & 0xff;
	regs[4] = (r << 3) | (div << 1);
	if (m != 0)
		regs[4] |= 1; /* Fractional mode */

	return 0;
}

/**
 * enum adau1961_micbias_voltage - Microphone bias voltage
 * @ADAU1961_MICBIAS_0_90_AVDD: 0.9 * AVDD
 * @ADAU1961_MICBIAS_0_65_AVDD: 0.65 * AVDD
 */
enum adau1961_micbias_voltage {
	ADAU1961_MICBIAS_0_90_AVDD = 0,
	ADAU1961_MICBIAS_0_65_AVDD = 1,
};

/**
 * enum adau1961_output_mode - Output mode configuration
 * @ADAU1961_OUTPUT_MODE_HEADPHONE: Headphone output
 * @ADAU1961_OUTPUT_MODE_HEADPHONE_CAPLESS: Capless headphone output
 * @ADAU1961_OUTPUT_MODE_LINE: Line output
 */
enum adau1961_output_mode {
	ADAU1961_OUTPUT_MODE_HEADPHONE,
	ADAU1961_OUTPUT_MODE_HEADPHONE_CAPLESS,
	ADAU1961_OUTPUT_MODE_LINE,
};

/**
 * struct adau1961_platform_data - ADAU1961 Codec driver platform data
 * @input_differential: If true the input pins will be configured in
 *  differential mode.
 * @lineout_mode: Output mode for the LOUT/ROUT pins
 * @headphone_mode: Output mode for the LHP/RHP pins
 * @monoout_mode: Output mode for the MONOOUT pin. Ignored if headphone_mode
 *  is set to ADAU1961_OUTPUT_MODE_HEADPHONE_CAPLESS.
 */
struct adau1961_platform_data {
	bool input_differential;
	enum adau1961_output_mode lineout_mode;
	enum adau1961_output_mode headphone_mode;
	enum adau1961_output_mode monoout_mode;
};

struct snd_soc_component;
struct snd_pcm_substream;

struct i2c_client;

enum adau1961_pll {
	ADAU1961_PLL,
};

enum adau1961_pll_src {
	ADAU1961_PLL_SRC_MCLK,
};

enum adau1961_clk_src {
	/* Automatically configure PLL based on the sample rate */
	ADAU1961_CLK_SRC_PLL_AUTO,
	ADAU1961_CLK_SRC_MCLK,
	ADAU1961_CLK_SRC_PLL,
};

struct clk;

struct adau {
	unsigned int sysclk;
	unsigned int pll_freq;
	struct clk *mclk;

	enum adau1961_clk_src clk_src;
	void (*switch_mode)(struct device *dev);

	unsigned int dai_fmt;

	u8 pll_regs[6];

	bool master;
	bool vgnd_shorted;
	bool hpl_unmuted;
	bool hpr_unmuted;

	unsigned int tdm_slot[2];

	struct regmap *regmap;
};

#define ADAU1961_CLOCK_CONTROL                 0x4000
#define ADAU1961_PLL_CONTROL                   0x4002
#define ADAU1961_DIGMIC_JACKDETECT             0x4008
#define ADAU1961_REC_MIXER_LEFT0               0x400a
#define ADAU1961_REC_MIXER_LEFT1               0x400b
#define ADAU1961_REC_MIXER_RIGHT0              0x400c
#define ADAU1961_REC_MIXER_RIGHT1              0x400d
#define ADAU1961_LEFT_DIFF_INPUT_VOL           0x400e
#define ADAU1961_RIGHT_DIFF_INPUT_VOL          0x400f
#define ADAU1961_MICBIAS                       0x4010
#define ADAU1961_ALC_CTRL0                     0x4011
#define ADAU1961_ALC_CTRL1                     0x4012
#define ADAU1961_ALC_CTRL2                     0x4013
#define ADAU1961_ALC_CTRL3                     0x4014
#define ADAU1961_SERIAL_PORT0                  0x4015
#define ADAU1961_SERIAL_PORT1                  0x4016
#define ADAU1961_CONVERTER0                    0x4017
#define ADAU1961_CONVERTER1                    0x4018
#define ADAU1961_ADC_CONTROL                   0x4019
#define ADAU1961_LEFT_INPUT_DIGITAL_VOL        0x401a
#define ADAU1961_RIGHT_INPUT_DIGITAL_VOL       0x401b
#define ADAU1961_PLAY_MIXER_LEFT0              0x401c
#define ADAU1961_PLAY_MIXER_LEFT1              0x401d
#define ADAU1961_PLAY_MIXER_RIGHT0             0x401e
#define ADAU1961_PLAY_MIXER_RIGHT1             0x401f
#define ADAU1961_PLAY_LR_MIXER_LEFT            0x4020
#define ADAU1961_PLAY_LR_MIXER_RIGHT           0x4021
#define ADAU1961_PLAY_MIXER_MONO               0x4022
#define ADAU1961_PLAY_HP_LEFT_VOL              0x4023
#define ADAU1961_PLAY_HP_RIGHT_VOL             0x4024
#define ADAU1961_PLAY_LINE_LEFT_VOL            0x4025
#define ADAU1961_PLAY_LINE_RIGHT_VOL           0x4026
#define ADAU1961_PLAY_MONO_OUTPUT_VOL          0x4027
#define ADAU1961_POP_CLICK_SUPPRESS            0x4028
#define ADAU1961_PLAY_POWER_MGMT               0x4029
#define ADAU1961_DAC_CONTROL0                  0x402a
#define ADAU1961_DAC_CONTROL1                  0x402b
#define ADAU1961_DAC_CONTROL2                  0x402c
#define ADAU1961_SERIAL_PORT_PAD               0x402d
#define ADAU1961_CONTROL_PORT_PAD0             0x402f
#define ADAU1961_CONTROL_PORT_PAD1             0x4030
#define ADAU1961_JACK_DETECT_PIN               0x4031
#define ADAU1961_DEJITTER                      0x4036

#define ADAU1961_CLOCK_CONTROL_INFREQ_MASK     0x6
#define ADAU1961_CLOCK_CONTROL_CORECLK_SRC_PLL BIT(3)
#define ADAU1961_CLOCK_CONTROL_SYSCLK_EN       BIT(0)

#define ADAU1961_SERIAL_PORT0_BCLK_POL         BIT(4)
#define ADAU1961_SERIAL_PORT0_LRCLK_POL        BIT(3)
#define ADAU1961_SERIAL_PORT0_MASTER           BIT(0)
#define ADAU1961_SERIAL_PORT0_STEREO           (0x0 << 1)
#define ADAU1961_SERIAL_PORT0_TDM4             (0x1 << 1)
#define ADAU1961_SERIAL_PORT0_TDM_MASK         (0x3 << 1)
#define ADAU1961_SERIAL_PORT0_PULSE_MODE       BIT(5)

#define ADAU1961_SERIAL_PORT1_DELAY1           0x00
#define ADAU1961_SERIAL_PORT1_DELAY0           0x01
#define ADAU1961_SERIAL_PORT1_DELAY8           0x02
#define ADAU1961_SERIAL_PORT1_DELAY16          0x03
#define ADAU1961_SERIAL_PORT1_DELAY_MASK       0x03
#define ADAU1961_SERIAL_PORT1_BCLK64           (0x0 << 5)
#define ADAU1961_SERIAL_PORT1_BCLK32           (0x1 << 5)
#define ADAU1961_SERIAL_PORT1_BCLK48           (0x2 << 5)
#define ADAU1961_SERIAL_PORT1_BCLK128          (0x3 << 5)
#define ADAU1961_SERIAL_PORT1_BCLK256          (0x4 << 5)
#define ADAU1961_SERIAL_PORT1_BCLK_MASK        (0x7 << 5)

#define ADAU1961_CONVERTER0_DAC_PAIR(x)        (((x) - 1) << 5)
#define ADAU1961_CONVERTER0_DAC_PAIR_MASK      (0x3 << 5)
#define ADAU1961_CONVERTER0_CONVSR_MASK        0x7
#define ADAU1961_CONVERTER0_ADOSR              BIT(3)

#define ADAU1961_CONVERTER1_ADC_PAIR(x)        ((x) - 1)
#define ADAU1961_CONVERTER1_ADC_PAIR_MASK      0x3

#define ADAU1961_DIFF_INPUT_VOL_LDEN           BIT(0)

#define ADAU1961_PLAY_MIXER_MONO_EN            BIT(0)

#define ADAU1961_PLAY_MONO_OUTPUT_VOL_MODE_HP  BIT(0)
#define ADAU1961_PLAY_MONO_OUTPUT_VOL_UNMUTE   BIT(1)

#define ADAU1961_PLAY_HP_RIGHT_VOL_MODE_HP     BIT(0)

#define ADAU1961_PLAY_LINE_LEFT_VOL_MODE_HP    BIT(0)

#define ADAU1961_PLAY_LINE_RIGHT_VOL_MODE_HP   BIT(0)

static const DECLARE_TLV_DB_MINMAX(adau1961_digital_tlv, -9563, 0);

static int adau1961_pll_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct adau *adau = snd_soc_component_get_drvdata(component);

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		adau->pll_regs[5] = 1;
	} else {
		adau->pll_regs[5] = 0;
		// Bypass the PLL when disabled, otherwise registers will become
		// inaccessible.
		regmap_update_bits(adau->regmap, ADAU1961_CLOCK_CONTROL,
				   ADAU1961_CLOCK_CONTROL_CORECLK_SRC_PLL, 0);
	}

	// The PLL register is 6 bytes long and can only be written at once.
	regmap_raw_write(adau->regmap, ADAU1961_PLL_CONTROL,
			 adau->pll_regs, ARRAY_SIZE(adau->pll_regs));

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		mdelay(5);
		regmap_update_bits(adau->regmap, ADAU1961_CLOCK_CONTROL,
				   ADAU1961_CLOCK_CONTROL_CORECLK_SRC_PLL,
			ADAU1961_CLOCK_CONTROL_CORECLK_SRC_PLL);
	}

	return 0;
}

static int adau1961_adc_fixup(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct adau *adau = snd_soc_component_get_drvdata(component);

	/*
	 * If we are capturing, toggle the ADOSR bit in Converter Control 0 to
	 * avoid losing SNR (workaround from ADI). This must be done after
	 * the ADC(s) have been enabled. According to the data sheet, it is
	 * normally illegal to set this bit when the sampling rate is 96 kHz,
	 * but according to ADI it is acceptable for this workaround.
	 */
	regmap_update_bits(adau->regmap, ADAU1961_CONVERTER0,
			   ADAU1961_CONVERTER0_ADOSR, ADAU1961_CONVERTER0_ADOSR);
	regmap_update_bits(adau->regmap, ADAU1961_CONVERTER0,
			   ADAU1961_CONVERTER0_ADOSR, 0);

	return 0;
}

static const char * const adau1961_mono_stereo_text[] = {
	"Stereo",
	"Mono Left Channel (L+R)",
	"Mono Right Channel (L+R)",
	"Mono (L+R)",
};

static SOC_ENUM_SINGLE_DECL(adau1961_dac_mode_enum,
	ADAU1961_DAC_CONTROL0, 6, adau1961_mono_stereo_text);

static const struct snd_kcontrol_new adau1961_dac_mode_mux =
	SOC_DAPM_ENUM("DAC Mono-Stereo-Mode", adau1961_dac_mode_enum);

static const struct snd_soc_dapm_route adau1961_dapm_pll_route = {
	"SYSCLK", NULL, "PLL",
};

static int adau1961_set_dai_pll(struct snd_soc_dai *dai, int pll_id,
				int source, unsigned int freq_in, unsigned int freq_out)
{
	struct snd_soc_component *component = dai->component;
	struct adau *adau = snd_soc_component_get_drvdata(component);
	int ret;

	if (freq_in < 8000000 || freq_in > 27000000)
		return -EINVAL;

	ret = adau_calc_pll_cfg(freq_in, freq_out, adau->pll_regs);
	if (ret < 0)
		return ret;

	/* The PLL register is 6 bytes long and can only be written at once. */
	ret = regmap_raw_write(adau->regmap, ADAU1961_PLL_CONTROL,
			       adau->pll_regs, ARRAY_SIZE(adau->pll_regs));
	if (ret)
		return ret;

	adau->pll_freq = freq_out;

	return 0;
}

static int adau1961_set_dai_sysclk(struct snd_soc_dai *dai,
				   int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_dapm_context *dapm = snd_soc_component_get_dapm(dai->component);
	struct adau *adau = snd_soc_component_get_drvdata(dai->component);
	bool is_pll;
	bool was_pll;

	switch (clk_id) {
	case ADAU1961_CLK_SRC_MCLK:
		is_pll = false;
		break;
	case ADAU1961_CLK_SRC_PLL_AUTO:
		if (!adau->mclk)
			return -EINVAL;
		fallthrough;
	case ADAU1961_CLK_SRC_PLL:
		is_pll = true;
		break;
	default:
		return -EINVAL;
	}

	switch (adau->clk_src) {
	case ADAU1961_CLK_SRC_MCLK:
		was_pll = false;
		break;
	case ADAU1961_CLK_SRC_PLL:
	case ADAU1961_CLK_SRC_PLL_AUTO:
		was_pll = true;
		break;
	default:
		return -EINVAL;
	}

	adau->sysclk = freq;

	if (is_pll != was_pll) {
		if (is_pll) {
			snd_soc_dapm_add_routes(dapm,
						&adau1961_dapm_pll_route, 1);
		} else {
			snd_soc_dapm_del_routes(dapm,
						&adau1961_dapm_pll_route, 1);
		}
	}

	adau->clk_src = clk_id;

	return 0;
}

static int adau1961_auto_pll(struct snd_soc_dai *dai,
			     struct snd_pcm_hw_params *params)
{
	struct adau *adau = snd_soc_dai_get_drvdata(dai);
	unsigned int pll_rate;

	switch (params_rate(params)) {
	case 48000:
	case 8000:
	case 12000:
	case 16000:
	case 24000:
	case 32000:
	case 96000:
		pll_rate = 48000 * 1024;
		break;
	case 44100:
	case 7350:
	case 11025:
	case 14700:
	case 22050:
	case 29400:
	case 88200:
		pll_rate = 44100 * 1024;
		break;
	default:
		return -EINVAL;
	}

	return adau1961_set_dai_pll(dai, ADAU1961_PLL, ADAU1961_PLL_SRC_MCLK,
		clk_get_rate(adau->mclk), pll_rate);
}

static int adau1961_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct adau *adau = snd_soc_component_get_drvdata(component);
	unsigned int val, div, dsp_div;
	unsigned int freq;
	int ret;

	switch (adau->clk_src) {
	case ADAU1961_CLK_SRC_PLL_AUTO:
		ret = adau1961_auto_pll(dai, params);
		if (ret)
			return ret;
		fallthrough;
	case ADAU1961_CLK_SRC_PLL:
		freq = adau->pll_freq;
		break;
	default:
		freq = adau->sysclk;
		break;
	}

	if (freq % params_rate(params) != 0)
		return -EINVAL;

	switch (freq / params_rate(params)) {
	case 1024: /* fs */
		div = 0;
		dsp_div = 1;
		break;
	case 6144: /* fs / 6 */
		div = 1;
		dsp_div = 6;
		break;
	case 4096: /* fs / 4 */
		div = 2;
		dsp_div = 5;
		break;
	case 3072: /* fs / 3 */
		div = 3;
		dsp_div = 4;
		break;
	case 2048: /* fs / 2 */
		div = 4;
		dsp_div = 3;
		break;
	case 1536: /* fs / 1.5 */
		div = 5;
		dsp_div = 2;
		break;
	case 512: /* fs / 0.5 */
		div = 6;
		dsp_div = 0;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(adau->regmap, ADAU1961_CONVERTER0,
			   ADAU1961_CONVERTER0_CONVSR_MASK, div);

	if (adau->dai_fmt != SND_SOC_DAIFMT_RIGHT_J)
		return 0;

	switch (params_width(params)) {
	case 16:
		val = ADAU1961_SERIAL_PORT1_DELAY16;
		break;
	case 24:
		val = ADAU1961_SERIAL_PORT1_DELAY8;
		break;
	case 32:
		val = ADAU1961_SERIAL_PORT1_DELAY0;
		break;
	default:
		return -EINVAL;
	}

	return regmap_update_bits(adau->regmap, ADAU1961_SERIAL_PORT1,
			ADAU1961_SERIAL_PORT1_DELAY_MASK, val);
}

static int adau1961_set_dai_fmt(struct snd_soc_dai *dai,
				unsigned int fmt)
{
	struct adau *adau = snd_soc_component_get_drvdata(dai->component);
	unsigned int ctrl0, ctrl1;
	unsigned int ctrl0_mask;
	int lrclk_pol;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		ctrl0 = ADAU1961_SERIAL_PORT0_MASTER;
		adau->master = true;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		ctrl0 = 0;
		adau->master = false;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		lrclk_pol = 0;
		ctrl1 = ADAU1961_SERIAL_PORT1_DELAY1;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
	case SND_SOC_DAIFMT_RIGHT_J:
		lrclk_pol = 1;
		ctrl1 = ADAU1961_SERIAL_PORT1_DELAY0;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_NF:
		ctrl0 |= ADAU1961_SERIAL_PORT0_BCLK_POL;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		lrclk_pol = !lrclk_pol;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		ctrl0 |= ADAU1961_SERIAL_PORT0_BCLK_POL;
		lrclk_pol = !lrclk_pol;
		break;
	default:
		return -EINVAL;
	}

	if (lrclk_pol)
		ctrl0 |= ADAU1961_SERIAL_PORT0_LRCLK_POL;

	/* Set the mask to update all relevant bits in ADAU1961_SERIAL_PORT0 */
	ctrl0_mask = ADAU1961_SERIAL_PORT0_MASTER |
		     ADAU1961_SERIAL_PORT0_LRCLK_POL |
		     ADAU1961_SERIAL_PORT0_BCLK_POL |
		     ADAU1961_SERIAL_PORT0_PULSE_MODE;

	regmap_update_bits(adau->regmap, ADAU1961_SERIAL_PORT0, ctrl0_mask,
			   ctrl0);
	regmap_update_bits(adau->regmap, ADAU1961_SERIAL_PORT1,
			   ADAU1961_SERIAL_PORT1_DELAY_MASK, ctrl1);

	adau->dai_fmt = fmt & SND_SOC_DAIFMT_FORMAT_MASK;

	return 0;
}

static int adau1961_xlate_tdm_slot_mask(unsigned int slots,
					unsigned int *tx_mask,
					unsigned int *rx_mask)
{
	// Workaround for snd_soc_dai_set_tdm_slot issue when slots=0.
	return 0;
}

static int adau1961_set_dai_tdm_slot(struct snd_soc_dai *dai,
				     unsigned int tx_mask,
				     unsigned int rx_mask,
				     int slots,
				     int slot_width)
{
	struct adau *adau = snd_soc_component_get_drvdata(dai->component);
	unsigned int ser_ctrl0, ser_ctrl1;
	unsigned int conv_ctrl0, conv_ctrl1;

	/* I2S mode */
	if (slots == 0) {
		slots = 2;
		rx_mask = 3;
		tx_mask = 3;
		slot_width = 32;
	}

	switch (slots) {
	case 2:
		ser_ctrl0 = ADAU1961_SERIAL_PORT0_STEREO;
		break;
	case 4:
		ser_ctrl0 = ADAU1961_SERIAL_PORT0_TDM4;
		break;
	default:
		return -EINVAL;
	}

	switch (slot_width * slots) {
	case 32:
		ser_ctrl1 = ADAU1961_SERIAL_PORT1_BCLK32;
		break;
	case 64:
		ser_ctrl1 = ADAU1961_SERIAL_PORT1_BCLK64;
		break;
	case 48:
		ser_ctrl1 = ADAU1961_SERIAL_PORT1_BCLK48;
		break;
	case 128:
		ser_ctrl1 = ADAU1961_SERIAL_PORT1_BCLK128;
		break;
	default:
		return -EINVAL;
	}

	switch (rx_mask) {
	case 0x03:
		conv_ctrl1 = ADAU1961_CONVERTER1_ADC_PAIR(1);
		adau->tdm_slot[SNDRV_PCM_STREAM_CAPTURE] = 0;
		break;
	case 0x0c:
		conv_ctrl1 = ADAU1961_CONVERTER1_ADC_PAIR(2);
		adau->tdm_slot[SNDRV_PCM_STREAM_CAPTURE] = 1;
		break;
	case 0x30:
		conv_ctrl1 = ADAU1961_CONVERTER1_ADC_PAIR(3);
		adau->tdm_slot[SNDRV_PCM_STREAM_CAPTURE] = 2;
		break;
	case 0xc0:
		conv_ctrl1 = ADAU1961_CONVERTER1_ADC_PAIR(4);
		adau->tdm_slot[SNDRV_PCM_STREAM_CAPTURE] = 3;
		break;
	default:
		return -EINVAL;
	}

	switch (tx_mask) {
	case 0x03:
		conv_ctrl0 = ADAU1961_CONVERTER0_DAC_PAIR(1);
		adau->tdm_slot[SNDRV_PCM_STREAM_PLAYBACK] = 0;
		break;
	case 0x0c:
		conv_ctrl0 = ADAU1961_CONVERTER0_DAC_PAIR(2);
		adau->tdm_slot[SNDRV_PCM_STREAM_PLAYBACK] = 1;
		break;
	case 0x30:
		conv_ctrl0 = ADAU1961_CONVERTER0_DAC_PAIR(3);
		adau->tdm_slot[SNDRV_PCM_STREAM_PLAYBACK] = 2;
		break;
	case 0xc0:
		conv_ctrl0 = ADAU1961_CONVERTER0_DAC_PAIR(4);
		adau->tdm_slot[SNDRV_PCM_STREAM_PLAYBACK] = 3;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(adau->regmap, ADAU1961_CONVERTER0,
			   ADAU1961_CONVERTER0_DAC_PAIR_MASK, conv_ctrl0);
	regmap_update_bits(adau->regmap, ADAU1961_CONVERTER1,
			   ADAU1961_CONVERTER1_ADC_PAIR_MASK, conv_ctrl1);
	regmap_update_bits(adau->regmap, ADAU1961_SERIAL_PORT0,
			   ADAU1961_SERIAL_PORT0_TDM_MASK, ser_ctrl0);
	regmap_update_bits(adau->regmap, ADAU1961_SERIAL_PORT1,
			   ADAU1961_SERIAL_PORT1_BCLK_MASK, ser_ctrl1);

	return 0;
}

static int adau1961_startup(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *dai)
{
	struct adau *adau = snd_soc_component_get_drvdata(dai->component);
	(void)adau;
	return 0;
}

static const struct snd_soc_dai_ops adau1961_dai_ops = {
	.hw_params	= adau1961_hw_params,
	.set_sysclk	= adau1961_set_dai_sysclk,
	.set_fmt	= adau1961_set_dai_fmt,
	.set_pll	= adau1961_set_dai_pll,
	.set_tdm_slot	= adau1961_set_dai_tdm_slot,
	.startup	= adau1961_startup,
	.xlate_tdm_slot_mask = adau1961_xlate_tdm_slot_mask,
};

static bool adau1961_precious_register(struct device *dev, unsigned int reg)
{
	return false;
}

static bool adau1961_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	/* The PLL register is 6 bytes long */
	case ADAU1961_PLL_CONTROL:
	case ADAU1961_PLL_CONTROL + 1:
	case ADAU1961_PLL_CONTROL + 2:
	case ADAU1961_PLL_CONTROL + 3:
	case ADAU1961_PLL_CONTROL + 4:
	case ADAU1961_PLL_CONTROL + 5:
		return true;
	default:
		break;
	}

	return false;
}

static int adau1961_add_routes(struct snd_soc_component *component)
{
	struct snd_soc_dapm_context *dapm = snd_soc_component_get_dapm(component);
	struct adau *adau = snd_soc_component_get_drvdata(component);
	int ret;

	if (adau->clk_src != ADAU1961_CLK_SRC_MCLK)
		ret = snd_soc_dapm_add_routes(dapm, &adau1961_dapm_pll_route, 1);

	return ret;
}

static int adau1961_resume(struct snd_soc_component *component)
{
	struct adau *adau = snd_soc_component_get_drvdata(component);

	if (adau->switch_mode)
		adau->switch_mode(component->dev);

	regcache_sync(adau->regmap);

	return 0;
}

static void adau1961_remove(struct device *dev)
{
	struct adau *adau = dev_get_drvdata(dev);

	clk_disable_unprepare(adau->mclk);
}

void adau1961_set_vgnd_shorted(struct snd_soc_component *component, bool shorted)
{
	struct adau *p = snd_soc_component_get_drvdata(component);

	struct snd_ctl_elem_value value;
	struct snd_kcontrol *k;

	long prev_left, prev_right;

	if (!p) {
		printe("Failed to get adau driver data!");
		return;
	}

	p->vgnd_shorted = shorted;

	k = snd_soc_card_get_kcontrol(component->card, "Headphone Playback Switch");
	if (!k) {
		printe("Failed to get Headphone Playback Switch control!");
		return;
	}

	snd_soc_get_volsw(k, &value);

	prev_left = value.value.integer.value[0];
	prev_right = value.value.integer.value[1];

	if (shorted) {
		p->hpl_unmuted = value.value.integer.value[0];
		p->hpr_unmuted = value.value.integer.value[1];

		value.value.integer.value[0] = 0;
		value.value.integer.value[1] = 0;
	} else {
		value.value.integer.value[0] = p->hpl_unmuted ? 1 : 0;
		value.value.integer.value[1] = p->hpr_unmuted ? 1 : 0;
	}

	if (value.value.integer.value[0] != prev_left ||
	    value.value.integer.value[1] != prev_right) {
		snd_soc_put_volsw(k, &value);
		snd_ctl_notify(component->card->snd_card, SNDRV_CTL_EVENT_MASK_VALUE, &k->id);
	}

	regmap_update_bits(p->regmap, ADAU1961_PLAY_MIXER_MONO,
			   ADAU1961_PLAY_MIXER_MONO_EN, shorted ? 0 : ADAU1961_PLAY_MIXER_MONO_EN);
}
EXPORT_SYMBOL(adau1961_set_vgnd_shorted);

bool adau1961_is_hp_capless(struct snd_soc_component *component)
{
	struct adau1961_platform_data *pdata = component->dev->platform_data;

	return pdata->headphone_mode == ADAU1961_OUTPUT_MODE_HEADPHONE_CAPLESS;
}
EXPORT_SYMBOL(adau1961_is_hp_capless);

static int upisnd_put_hp_mute_volsw(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct adau *adau = snd_soc_component_get_drvdata(component);

	if (adau->vgnd_shorted) {
		adau->hpl_unmuted = ucontrol->value.integer.value[0];
		adau->hpr_unmuted = ucontrol->value.integer.value[1];

		if (ucontrol->value.integer.value[0] != 0 ||
		    ucontrol->value.integer.value[1] != 0) {
			printe("Ignoring unmute request while VGND is shorted!");

			// Ensure alsamixer shows the correct state.
			snd_ctl_notify(component->card->snd_card,
				       SNDRV_CTL_EVENT_MASK_VALUE,
				       &kcontrol->id
			);
			return 0;
		}
	}

	return snd_soc_put_volsw(kcontrol, ucontrol);
}

static const struct reg_default adau1961_reg_defaults[] = {
	{ ADAU1961_DEJITTER,			0x03 },
	{ ADAU1961_REC_MIXER_LEFT0,		0x01 },
	{ ADAU1961_REC_MIXER_LEFT1,		0x00 },
	{ ADAU1961_REC_MIXER_RIGHT0,		0x01 },
	{ ADAU1961_REC_MIXER_RIGHT1,		0x00 },
	{ ADAU1961_LEFT_DIFF_INPUT_VOL,		0x00 },
	{ ADAU1961_ALC_CTRL0,			0x00 },
	{ ADAU1961_ALC_CTRL1,			0x00 },
	{ ADAU1961_ALC_CTRL2,			0x00 },
	{ ADAU1961_ALC_CTRL3,			0x00 },
	{ ADAU1961_RIGHT_DIFF_INPUT_VOL,	0x00 },
	{ ADAU1961_PLAY_LR_MIXER_LEFT,		0x01 },
	{ ADAU1961_PLAY_MIXER_LEFT0,		0x00 },
	{ ADAU1961_PLAY_MIXER_LEFT1,		0x00 },
	{ ADAU1961_PLAY_MIXER_RIGHT0,		0x00 },
	{ ADAU1961_PLAY_MIXER_RIGHT1,		0x00 },
	{ ADAU1961_PLAY_LR_MIXER_RIGHT,		0x01 },
	{ ADAU1961_PLAY_MIXER_MONO,		0x00 },
	{ ADAU1961_PLAY_HP_LEFT_VOL,		0x00 },
	{ ADAU1961_PLAY_HP_RIGHT_VOL,		0x00 },
	{ ADAU1961_PLAY_LINE_LEFT_VOL,		0x00 },
	{ ADAU1961_PLAY_LINE_RIGHT_VOL,		0x00 },
	{ ADAU1961_PLAY_MONO_OUTPUT_VOL,	0x02 },
	{ ADAU1961_POP_CLICK_SUPPRESS,		0x00 },
	{ ADAU1961_JACK_DETECT_PIN,		0x00 },
	{ ADAU1961_CLOCK_CONTROL,		0x00 },
	{ ADAU1961_PLL_CONTROL,			0x00 },
	{ ADAU1961_MICBIAS,			0x00 },
	{ ADAU1961_SERIAL_PORT0,		0x00 },
	{ ADAU1961_SERIAL_PORT1,		0x00 },
	{ ADAU1961_CONVERTER0,			0x00 },
	{ ADAU1961_CONVERTER1,			0x00 },
	{ ADAU1961_LEFT_INPUT_DIGITAL_VOL,	0x00 },
	{ ADAU1961_RIGHT_INPUT_DIGITAL_VOL,	0x00 },
	{ ADAU1961_ADC_CONTROL,			0x00 },
	{ ADAU1961_PLAY_POWER_MGMT,		0x03 },
	{ ADAU1961_DAC_CONTROL0,		0x00 },
	{ ADAU1961_DAC_CONTROL1,		0x00 },
	{ ADAU1961_DAC_CONTROL2,		0x00 },
	{ ADAU1961_SERIAL_PORT_PAD,		0xaa },
	{ ADAU1961_CONTROL_PORT_PAD0,		0xaa },
	{ ADAU1961_CONTROL_PORT_PAD1,		0x00 },
};

static const DECLARE_TLV_DB_RANGE(adau1961_mono_output_tlv,
	1, 1, TLV_DB_SCALE_ITEM(0, 0, 0),   //  0dB MX7[2:1]=01
	2, 2, TLV_DB_SCALE_ITEM(600, 0, 0)  // +6dB MX7[2:1]=10
);

static const DECLARE_TLV_DB_SCALE(adau1961_sing_in_tlv, -1500, 300, 1);
static const DECLARE_TLV_DB_SCALE(adau1961_diff_in_tlv, -1200, 75, 0);
static const DECLARE_TLV_DB_SCALE(adau1961_out_tlv, -5700, 100, 0);
static const DECLARE_TLV_DB_SCALE(adau1961_sidetone_tlv, -1800, 300, 1);
static const DECLARE_TLV_DB_SCALE(adau1961_boost_tlv, -600, 600, 1);
static const DECLARE_TLV_DB_SCALE(adau1961_pga_boost_tlv, -2000, 2000, 1);

static const DECLARE_TLV_DB_SCALE(adau1961_alc_max_gain_tlv, -1200, 600, 0);
static const DECLARE_TLV_DB_SCALE(adau1961_alc_target_tlv, -2850, 150, 0);
static const DECLARE_TLV_DB_SCALE(adau1961_alc_ng_threshold_tlv, -7650, 150, 0);

static const unsigned int adau1961_pga_slew_time_values[] = {
	3, 0, 1, 2,
};

static const char * const adau1961_pga_slew_time_text[] = {
	"Off",
	"24 ms",
	"48 ms",
	"96 ms",
};

static const char * const adau1961_alc_function_text[] = {
	"Off",
	"Right",
	"Left",
	"Stereo",
};

static const char * const adau1961_alc_hold_time_text[] = {
	"2.67 ms",
	"5.34 ms",
	"10.68 ms",
	"21.36 ms",
	"42.72 ms",
	"85.44 ms",
	"170.88 ms",
	"341.76 ms",
	"683.52 ms",
	"1367 ms",
	"2734.1 ms",
	"5468.2 ms",
	"10936 ms",
	"21873 ms",
	"43745 ms",
	"87491 ms",
};

static const char * const adau1961_alc_attack_time_text[] = {
	"6 ms",
	"12 ms",
	"24 ms",
	"48 ms",
	"96 ms",
	"192 ms",
	"384 ms",
	"768 ms",
	"1540 ms",
	"3070 ms",
	"6140 ms",
	"12290 ms",
	"24580 ms",
	"49150 ms",
	"98300 ms",
	"196610 ms",
};

static const char * const adau1961_alc_decay_time_text[] = {
	"24 ms",
	"48 ms",
	"96 ms",
	"192 ms",
	"384 ms",
	"768 ms",
	"15400 ms",
	"30700 ms",
	"61400 ms",
	"12290 ms",
	"24580 ms",
	"49150 ms",
	"98300 ms",
	"196610 ms",
	"393220 ms",
	"786430 ms",
};

static const char * const adau1961_alc_ng_type_text[] = {
	"Hold",
	"Mute",
	"Fade",
	"Fade + Mute",
};

static SOC_VALUE_ENUM_SINGLE_DECL(adau1961_pga_slew_time_enum,
		ADAU1961_ALC_CTRL0, 6, 0x3, adau1961_pga_slew_time_text,
		adau1961_pga_slew_time_values);
static SOC_ENUM_SINGLE_DECL(adau1961_alc_function_enum,
		ADAU1961_ALC_CTRL0, 0, adau1961_alc_function_text);
static SOC_ENUM_SINGLE_DECL(adau1961_alc_hold_time_enum,
		ADAU1961_ALC_CTRL1, 4, adau1961_alc_hold_time_text);
static SOC_ENUM_SINGLE_DECL(adau1961_alc_attack_time_enum,
		ADAU1961_ALC_CTRL2, 4, adau1961_alc_attack_time_text);
static SOC_ENUM_SINGLE_DECL(adau1961_alc_decay_time_enum,
		ADAU1961_ALC_CTRL2, 0, adau1961_alc_decay_time_text);
static SOC_ENUM_SINGLE_DECL(adau1961_alc_ng_type_enum,
		ADAU1961_ALC_CTRL3, 6, adau1961_alc_ng_type_text);

static const struct snd_kcontrol_new adau1961_differential_mode_controls[] = {
	SOC_DOUBLE_R_TLV("Capture Volume", ADAU1961_LEFT_DIFF_INPUT_VOL,
			 ADAU1961_RIGHT_DIFF_INPUT_VOL, 2, 0x3f, 0,
		adau1961_diff_in_tlv),
	SOC_DOUBLE_R("Capture Switch", ADAU1961_LEFT_DIFF_INPUT_VOL,
		     ADAU1961_RIGHT_DIFF_INPUT_VOL, 1, 1, 0),

	SOC_DOUBLE_R_TLV("PGA Boost Capture Volume", ADAU1961_REC_MIXER_LEFT1,
			 ADAU1961_REC_MIXER_RIGHT1, 3, 2, 0, adau1961_pga_boost_tlv),

	SOC_ENUM("PGA Capture Slew Time", adau1961_pga_slew_time_enum),

	SOC_SINGLE_TLV("ALC Capture Max Gain Volume", ADAU1961_ALC_CTRL0,
		       3, 7, 0, adau1961_alc_max_gain_tlv),
	SOC_ENUM("ALC Capture Function", adau1961_alc_function_enum),
	SOC_ENUM("ALC Capture Hold Time", adau1961_alc_hold_time_enum),
	SOC_SINGLE_TLV("ALC Capture Target Volume", ADAU1961_ALC_CTRL1,
		       0, 15, 0, adau1961_alc_target_tlv),
	SOC_ENUM("ALC Capture Attack Time", adau1961_alc_decay_time_enum),
	SOC_ENUM("ALC Capture Decay Time", adau1961_alc_attack_time_enum),
	SOC_ENUM("ALC Capture Noise Gate Type", adau1961_alc_ng_type_enum),
	SOC_SINGLE("ALC Capture Noise Gate Switch",
		   ADAU1961_ALC_CTRL3, 5, 1, 0),
	SOC_SINGLE_TLV("ALC Capture Noise Gate Threshold Volume",
		       ADAU1961_ALC_CTRL3, 0, 31, 0, adau1961_alc_ng_threshold_tlv),
};

static const struct snd_kcontrol_new adau1961_single_mode_controls[] = {
	SOC_SINGLE_TLV("Input 1 Capture Volume", ADAU1961_REC_MIXER_LEFT0,
		       4, 7, 0, adau1961_sing_in_tlv),
	SOC_SINGLE_TLV("Input 2 Capture Volume", ADAU1961_REC_MIXER_LEFT0,
		       1, 7, 0, adau1961_sing_in_tlv),
	SOC_SINGLE_TLV("Input 3 Capture Volume", ADAU1961_REC_MIXER_RIGHT0,
		       4, 7, 0, adau1961_sing_in_tlv),
	SOC_SINGLE_TLV("Input 4 Capture Volume", ADAU1961_REC_MIXER_RIGHT0,
		       1, 7, 0, adau1961_sing_in_tlv),
};

static const struct snd_kcontrol_new adau1961_mono_controls[] = {
	SOC_SINGLE_TLV("Mono Playback Volume", ADAU1961_PLAY_MONO_OUTPUT_VOL,
		       2, 0x3f, 0, adau1961_out_tlv),
	SOC_SINGLE("Mono Playback Switch", ADAU1961_PLAY_MONO_OUTPUT_VOL,
		   1, 1, 0),
	SOC_SINGLE_TLV("Mono Playback Boost Volume", ADAU1961_PLAY_MIXER_MONO,
		       1, 2, 0, adau1961_mono_output_tlv),
};

static const struct snd_kcontrol_new adau1961_left_mixer_controls[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Left DAC Switch",
				    ADAU1961_PLAY_MIXER_LEFT0, 5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("Right DAC Switch",
				    ADAU1961_PLAY_MIXER_LEFT0, 6, 1, 0),
	SOC_DAPM_SINGLE_TLV("Aux Bypass Volume",
			    ADAU1961_PLAY_MIXER_LEFT0, 1, 8, 0, adau1961_sidetone_tlv),
	SOC_DAPM_SINGLE_TLV("Right Bypass Volume",
			    ADAU1961_PLAY_MIXER_LEFT1, 4, 8, 0, adau1961_sidetone_tlv),
	SOC_DAPM_SINGLE_TLV("Left Bypass Volume",
			    ADAU1961_PLAY_MIXER_LEFT1, 0, 8, 0, adau1961_sidetone_tlv),
};

static const struct snd_kcontrol_new adau1961_right_mixer_controls[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("Left DAC Switch",
				    ADAU1961_PLAY_MIXER_RIGHT0, 5, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("Right DAC Switch",
				    ADAU1961_PLAY_MIXER_RIGHT0, 6, 1, 0),
	SOC_DAPM_SINGLE_TLV("Aux Bypass Volume",
			    ADAU1961_PLAY_MIXER_RIGHT0, 1, 8, 0, adau1961_sidetone_tlv),
	SOC_DAPM_SINGLE_TLV("Right Bypass Volume",
			    ADAU1961_PLAY_MIXER_RIGHT1, 4, 8, 0, adau1961_sidetone_tlv),
	SOC_DAPM_SINGLE_TLV("Left Bypass Volume",
			    ADAU1961_PLAY_MIXER_RIGHT1, 0, 8, 0, adau1961_sidetone_tlv),
};

static const struct snd_kcontrol_new adau1961_left_lr_mixer_controls[] = {
	SOC_DAPM_SINGLE_TLV("Left Volume",
			    ADAU1961_PLAY_LR_MIXER_LEFT, 1, 2, 0, adau1961_boost_tlv),
	SOC_DAPM_SINGLE_TLV("Right Volume",
			    ADAU1961_PLAY_LR_MIXER_LEFT, 3, 2, 0, adau1961_boost_tlv),
};

static const struct snd_kcontrol_new adau1961_right_lr_mixer_controls[] = {
	SOC_DAPM_SINGLE_TLV("Left Volume",
			    ADAU1961_PLAY_LR_MIXER_RIGHT, 1, 2, 0, adau1961_boost_tlv),
	SOC_DAPM_SINGLE_TLV("Right Volume",
			    ADAU1961_PLAY_LR_MIXER_RIGHT, 3, 2, 0, adau1961_boost_tlv),
};

static const struct snd_kcontrol_new adau1961_controls[] = {
	SOC_DOUBLE_R_TLV("Digital Capture Volume",
			 ADAU1961_LEFT_INPUT_DIGITAL_VOL,
		ADAU1961_RIGHT_INPUT_DIGITAL_VOL,
		0, 0xff, 1, adau1961_digital_tlv),
	SOC_DOUBLE_R_TLV("Digital Playback Volume", ADAU1961_DAC_CONTROL1,
			 ADAU1961_DAC_CONTROL2, 0, 0xff, 1, adau1961_digital_tlv),

	SOC_SINGLE("ADC High Pass Filter Switch", ADAU1961_ADC_CONTROL,
		   5, 1, 0),
	SOC_SINGLE("Playback De-emphasis Switch", ADAU1961_DAC_CONTROL0,
		   2, 1, 0),

	SOC_DOUBLE_R_TLV("Aux Capture Volume", ADAU1961_REC_MIXER_LEFT1,
			 ADAU1961_REC_MIXER_RIGHT1, 0, 7, 0, adau1961_sing_in_tlv),

	SOC_DOUBLE_R_TLV("Headphone Playback Volume", ADAU1961_PLAY_HP_LEFT_VOL,
			 ADAU1961_PLAY_HP_RIGHT_VOL, 2, 0x3f, 0, adau1961_out_tlv),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Headphone Playback Switch",
		.info = snd_soc_info_volsw,
		.get = snd_soc_get_volsw,
		.put = upisnd_put_hp_mute_volsw,
		.private_value = SOC_DOUBLE_R_VALUE(ADAU1961_PLAY_HP_LEFT_VOL,
			ADAU1961_PLAY_HP_RIGHT_VOL, 1, 1, 0)
	},
	SOC_DOUBLE_R_TLV("Lineout Playback Volume", ADAU1961_PLAY_LINE_LEFT_VOL,
			 ADAU1961_PLAY_LINE_RIGHT_VOL, 2, 0x3f, 0, adau1961_out_tlv),
	SOC_DOUBLE_R("Lineout Playback Switch", ADAU1961_PLAY_LINE_LEFT_VOL,
		     ADAU1961_PLAY_LINE_RIGHT_VOL, 1, 1, 0),
	SOC_SINGLE("MicBias Switch", ADAU1961_MICBIAS, 0, 1, 0),
};

static int adau1961_dejitter_fixup(struct snd_soc_dapm_widget *w,
				   struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct adau *adau = snd_soc_component_get_drvdata(component);

	// After any power changes have been made the dejitter circuit
	// has to be reinitialized.
	regmap_write(adau->regmap, ADAU1961_DEJITTER, 0);
	if (!adau->master)
		regmap_write(adau->regmap, ADAU1961_DEJITTER, 3);

	return 0;
}

static const struct snd_soc_dapm_widget adau1961_dapm_widgets[] = {
	SND_SOC_DAPM_MIXER("Left Input Mixer", SND_SOC_NOPM, 0, 0,
			   NULL, 0),
	SND_SOC_DAPM_MIXER("Right Input Mixer", SND_SOC_NOPM, 0, 0,
			   NULL, 0),

	SND_SOC_DAPM_SUPPLY("MICBIAS", ADAU1961_MICBIAS, 0, 0, NULL, 0),

	SOC_MIXER_ARRAY("Left Playback Mixer", ADAU1961_PLAY_MIXER_LEFT0,
			0, 0, adau1961_left_mixer_controls),
	SOC_MIXER_ARRAY("Right Playback Mixer", ADAU1961_PLAY_MIXER_RIGHT0,
			0, 0, adau1961_right_mixer_controls),

	SOC_MIXER_ARRAY("Left LR Playback Mixer", SND_SOC_NOPM,
			0, 0, adau1961_left_lr_mixer_controls),
	SOC_MIXER_ARRAY("Right LR Playback Mixer", SND_SOC_NOPM,
			0, 0, adau1961_right_lr_mixer_controls),

	SND_SOC_DAPM_SUPPLY("Headphone", ADAU1961_PLAY_HP_LEFT_VOL,
			    0, 0, NULL, 0),

	SND_SOC_DAPM_SUPPLY_S("SYSCLK", 2, SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_POST("Dejitter fixup", adau1961_dejitter_fixup),

	SND_SOC_DAPM_INPUT("LAUX"),
	SND_SOC_DAPM_INPUT("RAUX"),
	SND_SOC_DAPM_INPUT("LINP"),
	SND_SOC_DAPM_INPUT("LINN"),
	SND_SOC_DAPM_INPUT("RINP"),
	SND_SOC_DAPM_INPUT("RINN"),

	SND_SOC_DAPM_OUTPUT("LOUT"),
	SND_SOC_DAPM_OUTPUT("ROUT"),
	SND_SOC_DAPM_OUTPUT("LHP"),
	SND_SOC_DAPM_OUTPUT("RHP"),

	SND_SOC_DAPM_SUPPLY_S("PLL", 3, SND_SOC_NOPM, 0, 0, adau1961_pll_event,
			      SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY("AIFCLK", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_SUPPLY("Left Playback Enable", SND_SOC_NOPM,
			    0, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("Right Playback Enable", SND_SOC_NOPM,
			    1, 0, NULL, 0),

	SND_SOC_DAPM_MUX("Left DAC Mode Mux", SND_SOC_NOPM, 0, 0,
			 &adau1961_dac_mode_mux),
	SND_SOC_DAPM_MUX("Right DAC Mode Mux", SND_SOC_NOPM, 0, 0,
			 &adau1961_dac_mode_mux),

	SND_SOC_DAPM_ADC_E("Left Decimator", NULL, ADAU1961_ADC_CONTROL, 0, 0,
			   adau1961_adc_fixup, SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_ADC("Right Decimator", NULL, ADAU1961_ADC_CONTROL, 1, 0),
	SND_SOC_DAPM_DAC("Left DAC", NULL, ADAU1961_DAC_CONTROL0, 0, 0),
	SND_SOC_DAPM_DAC("Right DAC", NULL, ADAU1961_DAC_CONTROL0, 1, 0),
};

static const struct snd_kcontrol_new adau1961_mono_mixer_controls[] = {
	SOC_DAPM_SINGLE_TLV("Mono Mixer Gain", ADAU1961_PLAY_MIXER_MONO,
			    1, 2, 0, adau1961_mono_output_tlv),
};

static const struct snd_soc_dapm_widget adau1961_mono_dapm_widgets[] = {
	SND_SOC_DAPM_MIXER("Mono Playback Mixer", ADAU1961_PLAY_MIXER_MONO,
			   0, 0, adau1961_mono_mixer_controls,
			   ARRAY_SIZE(adau1961_mono_mixer_controls)),

	SND_SOC_DAPM_OUTPUT("MONOOUT"),
};

static const struct snd_soc_dapm_widget adau1961_capless_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY_S("Headphone VGND", 1, ADAU1961_PLAY_MIXER_MONO,
			      0, 0, NULL, 0),
};

static const struct snd_soc_dapm_route adau1961_dapm_routes[] = {
	{ "Left Input Mixer", NULL, "LINP" },
	{ "Left Input Mixer", NULL, "LINN" },
	{ "Left Input Mixer", NULL, "LAUX" },

	{ "Right Input Mixer", NULL, "RINP" },
	{ "Right Input Mixer", NULL, "RINN" },
	{ "Right Input Mixer", NULL, "RAUX" },

	{ "Left Input Mixer", NULL, "MICBIAS" },
	{ "Right Input Mixer", NULL, "MICBIAS" },

	{ "Left Playback Mixer", NULL, "Left Playback Enable"},
	{ "Right Playback Mixer", NULL, "Right Playback Enable"},
	{ "Left LR Playback Mixer", NULL, "Left Playback Enable"},
	{ "Right LR Playback Mixer", NULL, "Right Playback Enable"},

	{ "Left Playback Mixer", "Left DAC Switch", "Left DAC" },
	{ "Left Playback Mixer", "Right DAC Switch", "Right DAC" },

	{ "Right Playback Mixer", "Left DAC Switch", "Left DAC" },
	{ "Right Playback Mixer", "Right DAC Switch", "Right DAC" },

	{ "Left LR Playback Mixer", "Left Volume", "Left Playback Mixer" },
	{ "Left LR Playback Mixer", "Right Volume", "Right Playback Mixer" },

	{ "Right LR Playback Mixer", "Left Volume", "Left Playback Mixer" },
	{ "Right LR Playback Mixer", "Right Volume", "Right Playback Mixer" },

	{ "LHP", NULL, "Left Playback Mixer" },
	{ "RHP", NULL, "Right Playback Mixer" },

	{ "LHP", NULL, "Headphone" },
	{ "RHP", NULL, "Headphone" },

	{ "LOUT", NULL, "Left LR Playback Mixer" },
	{ "ROUT", NULL, "Right LR Playback Mixer" },

	{ "Left Playback Mixer", "Aux Bypass Volume", "LAUX" },
	{ "Left Playback Mixer", "Left Bypass Volume", "Left Input Mixer" },
	{ "Left Playback Mixer", "Right Bypass Volume", "Right Input Mixer" },
	{ "Right Playback Mixer", "Aux Bypass Volume", "RAUX" },
	{ "Right Playback Mixer", "Left Bypass Volume", "Left Input Mixer" },
	{ "Right Playback Mixer", "Right Bypass Volume", "Right Input Mixer" },

	{ "Left Decimator", NULL, "SYSCLK" },
	{ "Right Decimator", NULL, "SYSCLK" },
	{ "Left DAC", NULL, "SYSCLK" },
	{ "Right DAC", NULL, "SYSCLK" },
	{ "Capture", NULL, "SYSCLK" },
	{ "Playback", NULL, "SYSCLK" },

	{ "Left DAC", NULL, "Left DAC Mode Mux" },
	{ "Right DAC", NULL, "Right DAC Mode Mux" },

	{ "Capture", NULL, "AIFCLK" },
	{ "Playback", NULL, "AIFCLK" },

	{ "Left DAC Mode Mux", "Stereo", "Playback" },
	{ "Left DAC Mode Mux", "Mono (L+R)", "Playback" },
	{ "Left DAC Mode Mux", "Mono Left Channel (L+R)", "Playback" },
	{ "Right DAC Mode Mux", "Stereo", "Playback" },
	{ "Right DAC Mode Mux", "Mono (L+R)", "Playback" },
	{ "Right DAC Mode Mux", "Mono Right Channel (L+R)", "Playback" },
	{ "Capture", NULL, "Left Decimator" },
	{ "Capture", NULL, "Right Decimator" },

	{ "Left Decimator", NULL, "Left Input Mixer" },
	{ "Right Decimator", NULL, "Right Input Mixer" },
};

static const struct snd_soc_dapm_route adau1961_mono_dapm_routes[] = {
	{ "Mono Playback Mixer", NULL, "Left Playback Mixer" },
	{ "Mono Playback Mixer", NULL, "Right Playback Mixer" },

	{ "MONOOUT", NULL, "Mono Playback Mixer" },
};

static const struct snd_soc_dapm_route adau1961_capless_dapm_routes[] = {
	{ "Headphone", NULL, "Headphone VGND" },
};

static int adau1961_set_bias_level(struct snd_soc_component *component,
				   enum snd_soc_bias_level level)
{
	struct adau *adau = snd_soc_component_get_drvdata(component);

	switch (level) {
	case SND_SOC_BIAS_ON:
		break;
	case SND_SOC_BIAS_PREPARE:
		break;
	case SND_SOC_BIAS_STANDBY:
		regcache_cache_only(adau->regmap, false);
		regmap_update_bits(adau->regmap, ADAU1961_CLOCK_CONTROL,
				   ADAU1961_CLOCK_CONTROL_SYSCLK_EN,
			ADAU1961_CLOCK_CONTROL_SYSCLK_EN);
		if (snd_soc_component_get_bias_level(component) == SND_SOC_BIAS_OFF)
			regcache_sync(adau->regmap);
		break;
	case SND_SOC_BIAS_OFF:
		regmap_update_bits(adau->regmap, ADAU1961_CLOCK_CONTROL,
				   ADAU1961_CLOCK_CONTROL_SYSCLK_EN, 0);
		regcache_cache_only(adau->regmap, true);
		break;
	}
	return 0;
}

static enum adau1961_output_mode adau1961_get_lineout_mode(struct snd_soc_component *component)
{
	struct adau1961_platform_data *pdata = component->dev->platform_data;

	if (pdata)
		return pdata->lineout_mode;

	return ADAU1961_OUTPUT_MODE_LINE;
}

static enum adau1961_output_mode adau1961_get_monoout_mode(struct snd_soc_component *component)
{
	struct adau1961_platform_data *pdata = component->dev->platform_data;

	if (pdata)
		return pdata->monoout_mode;

	return ADAU1961_OUTPUT_MODE_LINE;
}

static int adau1961_setup_headphone_mode(struct snd_soc_component *component)
{
	struct snd_soc_dapm_context *dapm = snd_soc_component_get_dapm(component);
	struct adau *adau = snd_soc_component_get_drvdata(component);
	struct adau1961_platform_data *pdata = component->dev->platform_data;
	enum adau1961_output_mode mode;
	int ret;

	if (pdata)
		mode = pdata->headphone_mode;
	else
		mode = ADAU1961_OUTPUT_MODE_HEADPHONE;

	switch (mode) {
	case ADAU1961_OUTPUT_MODE_LINE:
		break;
	case ADAU1961_OUTPUT_MODE_HEADPHONE_CAPLESS:
		regmap_update_bits(adau->regmap, ADAU1961_PLAY_MONO_OUTPUT_VOL,
				   ADAU1961_PLAY_MONO_OUTPUT_VOL_MODE_HP |
			ADAU1961_PLAY_MONO_OUTPUT_VOL_UNMUTE,
			ADAU1961_PLAY_MONO_OUTPUT_VOL_MODE_HP |
			ADAU1961_PLAY_MONO_OUTPUT_VOL_UNMUTE);
		fallthrough;
	case ADAU1961_OUTPUT_MODE_HEADPHONE:
		regmap_update_bits(adau->regmap, ADAU1961_PLAY_HP_RIGHT_VOL,
				   ADAU1961_PLAY_HP_RIGHT_VOL_MODE_HP,
			ADAU1961_PLAY_HP_RIGHT_VOL_MODE_HP);
		break;
	default:
		return -EINVAL;
	}

	if (mode == ADAU1961_OUTPUT_MODE_HEADPHONE_CAPLESS) {
		ret = snd_soc_dapm_new_controls(dapm,
						adau1961_capless_dapm_widgets,
			ARRAY_SIZE(adau1961_capless_dapm_widgets));
		if (ret)
			return ret;
		ret = snd_soc_dapm_add_routes(dapm,
					      adau1961_capless_dapm_routes,
			ARRAY_SIZE(adau1961_capless_dapm_routes));
	} else {
		ret = snd_soc_add_component_controls(component, adau1961_mono_controls,
						     ARRAY_SIZE(adau1961_mono_controls));
		if (ret)
			return ret;
		ret = snd_soc_dapm_new_controls(dapm,
						adau1961_mono_dapm_widgets,
			ARRAY_SIZE(adau1961_mono_dapm_widgets));
		if (ret)
			return ret;
		ret = snd_soc_dapm_add_routes(dapm,
					      adau1961_mono_dapm_routes,
			ARRAY_SIZE(adau1961_mono_dapm_routes));
	}

	return ret;
}

static bool adau1961_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ADAU1961_REC_MIXER_LEFT0:
	case ADAU1961_REC_MIXER_LEFT1:
	case ADAU1961_REC_MIXER_RIGHT0:
	case ADAU1961_REC_MIXER_RIGHT1:
	case ADAU1961_LEFT_DIFF_INPUT_VOL:
	case ADAU1961_RIGHT_DIFF_INPUT_VOL:
	case ADAU1961_PLAY_LR_MIXER_LEFT:
	case ADAU1961_PLAY_MIXER_LEFT0:
	case ADAU1961_PLAY_MIXER_LEFT1:
	case ADAU1961_PLAY_MIXER_RIGHT0:
	case ADAU1961_PLAY_MIXER_RIGHT1:
	case ADAU1961_PLAY_LR_MIXER_RIGHT:
	case ADAU1961_PLAY_MIXER_MONO:
	case ADAU1961_PLAY_HP_LEFT_VOL:
	case ADAU1961_PLAY_HP_RIGHT_VOL:
	case ADAU1961_PLAY_LINE_LEFT_VOL:
	case ADAU1961_PLAY_LINE_RIGHT_VOL:
	case ADAU1961_PLAY_MONO_OUTPUT_VOL:
	case ADAU1961_POP_CLICK_SUPPRESS:
	case ADAU1961_JACK_DETECT_PIN:
	case ADAU1961_DEJITTER:
	case ADAU1961_ALC_CTRL0:
	case ADAU1961_ALC_CTRL1:
	case ADAU1961_ALC_CTRL2:
	case ADAU1961_ALC_CTRL3:
	case ADAU1961_CLOCK_CONTROL:
	case ADAU1961_PLL_CONTROL:
	case ADAU1961_MICBIAS:
	case ADAU1961_SERIAL_PORT0:
	case ADAU1961_SERIAL_PORT1:
	case ADAU1961_CONVERTER0:
	case ADAU1961_CONVERTER1:
	case ADAU1961_LEFT_INPUT_DIGITAL_VOL:
	case ADAU1961_RIGHT_INPUT_DIGITAL_VOL:
	case ADAU1961_ADC_CONTROL:
	case ADAU1961_PLAY_POWER_MGMT:
	case ADAU1961_DAC_CONTROL0:
	case ADAU1961_DAC_CONTROL1:
	case ADAU1961_DAC_CONTROL2:
	case ADAU1961_SERIAL_PORT_PAD:
	case ADAU1961_CONTROL_PORT_PAD0:
	case ADAU1961_CONTROL_PORT_PAD1:
		return true;
	default:
		break;
	}

	return false;
}

static int adau1961_component_probe(struct snd_soc_component *component)
{
	struct adau1961_platform_data *pdata = component->dev->platform_data;
	struct adau *adau = snd_soc_component_get_drvdata(component);
	int ret;

	if (pdata && pdata->input_differential) {
		regmap_update_bits(adau->regmap, ADAU1961_LEFT_DIFF_INPUT_VOL,
				   ADAU1961_DIFF_INPUT_VOL_LDEN,
			ADAU1961_DIFF_INPUT_VOL_LDEN);
		regmap_update_bits(adau->regmap, ADAU1961_RIGHT_DIFF_INPUT_VOL,
				   ADAU1961_DIFF_INPUT_VOL_LDEN,
			ADAU1961_DIFF_INPUT_VOL_LDEN);
		ret = snd_soc_add_component_controls(component,
						     adau1961_differential_mode_controls,
			ARRAY_SIZE(adau1961_differential_mode_controls));
		if (ret)
			return ret;
	} else {
		ret = snd_soc_add_component_controls(component,
						     adau1961_single_mode_controls,
			ARRAY_SIZE(adau1961_single_mode_controls));
		if (ret)
			return ret;
	}

	switch (adau1961_get_lineout_mode(component)) {
	case ADAU1961_OUTPUT_MODE_LINE:
		break;
	case ADAU1961_OUTPUT_MODE_HEADPHONE:
		regmap_update_bits(adau->regmap, ADAU1961_PLAY_LINE_LEFT_VOL,
				   ADAU1961_PLAY_LINE_LEFT_VOL_MODE_HP,
			ADAU1961_PLAY_LINE_LEFT_VOL_MODE_HP);
		regmap_update_bits(adau->regmap, ADAU1961_PLAY_LINE_RIGHT_VOL,
				   ADAU1961_PLAY_LINE_RIGHT_VOL_MODE_HP,
			ADAU1961_PLAY_LINE_RIGHT_VOL_MODE_HP);
		break;
	default:
		return -EINVAL;
	}

	switch (adau1961_get_monoout_mode(component)) {
	case ADAU1961_OUTPUT_MODE_LINE:
		break;
	case ADAU1961_OUTPUT_MODE_HEADPHONE:
		regmap_update_bits(adau->regmap, ADAU1961_PLAY_MONO_OUTPUT_VOL,
				   ADAU1961_PLAY_MONO_OUTPUT_VOL_MODE_HP,
			ADAU1961_PLAY_MONO_OUTPUT_VOL_MODE_HP);
		break;
	default:
		return -EINVAL;
	}

	ret = adau1961_setup_headphone_mode(component);
	if (ret)
		return ret;

	ret = adau1961_add_routes(component);
	if (ret < 0)
		return ret;

	return 0;
}

static const struct snd_soc_component_driver adau1961_component_driver = {
	.probe                 = adau1961_component_probe,
	.resume                = adau1961_resume,
	.set_bias_level        = adau1961_set_bias_level,
	.controls              = adau1961_controls,
	.num_controls          = ARRAY_SIZE(adau1961_controls),
	.dapm_widgets          = adau1961_dapm_widgets,
	.num_dapm_widgets      = ARRAY_SIZE(adau1961_dapm_widgets),
	.dapm_routes           = adau1961_dapm_routes,
	.num_dapm_routes       = ARRAY_SIZE(adau1961_dapm_routes),
	.suspend_bias_off      = 1,
	.idle_bias_on          = 1,
	.use_pmdown_time       = 1,
	.endianness            = 1,
	.legacy_dai_naming     = 0,
};

#define ADAU1961_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \
	SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver adau1961_dai_driver = {
	.name = "adau-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_96000,
		.formats = ADAU1961_FORMATS,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_96000,
		.formats = ADAU1961_FORMATS,
	},
	.ops = &adau1961_dai_ops,
};

static int adau1961_probe(struct device *dev, struct regmap *regmap,
			  void (*switch_mode)(struct device *dev))
{
	struct snd_soc_dai_driver *dai_drv;
	int ret;

	dai_drv = &adau1961_dai_driver;

	struct adau *adau;

	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	adau = devm_kzalloc(dev, sizeof(*adau), GFP_KERNEL);
	if (!adau)
		return -ENOMEM;

	adau->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(adau->mclk)) {
		if (PTR_ERR(adau->mclk) != -ENOENT)
			return PTR_ERR(adau->mclk);
		/* Clock is optional (for the driver) */
		adau->mclk = NULL;
	} else if (adau->mclk) {
		adau->clk_src = ADAU1961_CLK_SRC_PLL_AUTO;

		/*
		 * Any valid PLL output rate will work at this point, use one
		 * that is likely to be chosen later as well. The register will
		 * be written when the PLL is powered up for the first time.
		 */
		ret = adau_calc_pll_cfg(clk_get_rate(adau->mclk), 48000 * 1024,
					adau->pll_regs);
		if (ret < 0)
			return ret;

		ret = clk_prepare_enable(adau->mclk);
		if (ret)
			return ret;
	}

	adau->regmap = regmap;
	adau->switch_mode = switch_mode;

	dev_set_drvdata(dev, adau);

	if (switch_mode)
		switch_mode(dev);

	// Enable cache only mode as we could miss writes before bias level
	// reaches standby and the core clock is enabled
	regcache_cache_only(regmap, true);

	return devm_snd_soc_register_component(dev, &adau1961_component_driver, dai_drv, 1);
}

static const struct regmap_config adau1961_regmap_config = {
	.val_bits = 8,
	.reg_bits = 16,
	.max_register = 0x4036,
	.reg_defaults = adau1961_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(adau1961_reg_defaults),
	.readable_reg = adau1961_readable_register,
	.volatile_reg = adau1961_volatile_register,
	.precious_reg = adau1961_precious_register,
	.cache_type = REGCACHE_RBTREE,
};

static int upisnd_codec_fill_pdata(struct device_node *of_node,
				   struct adau1961_platform_data *pdata)
{
	const char *input_mode = "differential";
	const char *hp_out_mode = "capless-headphone";
	const char *line_out_mode = "line-out";
	const char *mono_out_mode = "line-out";

	of_property_read_string(of_node, "input-mode", &input_mode);
	of_property_read_string(of_node, "hp-out-mode", &hp_out_mode);
	of_property_read_string(of_node, "line-out-mode", &line_out_mode);
	of_property_read_string(of_node, "mono-out-mode", &mono_out_mode);

	if (strncmp(input_mode, "differential", 13) == 0) {
		pdata->input_differential = true;
	} else if (strncmp(input_mode, "single-ended", 13) == 0) {
		pdata->input_differential = false;
	} else {
		printe("Invalid input mode: %s", input_mode);
		return -EINVAL;
	}

	if (strncmp(hp_out_mode, "headphone", 10) == 0) {
		pdata->headphone_mode = ADAU1961_OUTPUT_MODE_HEADPHONE;
	} else if (strncmp(hp_out_mode, "capless-headphone", 18) == 0) {
		pdata->headphone_mode = ADAU1961_OUTPUT_MODE_HEADPHONE_CAPLESS;
	} else if (strncmp(hp_out_mode, "line-out", 9) == 0) {
		pdata->headphone_mode = ADAU1961_OUTPUT_MODE_LINE;
	} else {
		printe("Invalid headphone output mode: %s", hp_out_mode);
		return -EINVAL;
	}

	if (strncmp(line_out_mode, "line-out", 9) == 0) {
		pdata->lineout_mode = ADAU1961_OUTPUT_MODE_LINE;
	} else if (strncmp(line_out_mode, "headphone", 10) == 0) {
		pdata->lineout_mode = ADAU1961_OUTPUT_MODE_HEADPHONE;
	} else {
		printe("Invalid line-out mode: %s", line_out_mode);
		return -EINVAL;
	}

	if (strncmp(mono_out_mode, "line-out", 9) == 0) {
		pdata->monoout_mode = ADAU1961_OUTPUT_MODE_LINE;
	} else if (strncmp(line_out_mode, "headphone", 10) == 0) {
		pdata->monoout_mode = ADAU1961_OUTPUT_MODE_HEADPHONE;
	} else {
		printe("Invalid mono-out mode: %s", mono_out_mode);
		return -EINVAL;
	}

	return 0;
}

static int upisnd_codec_i2c_probe(struct i2c_client *client)
{
	struct adau1961_platform_data *pdata = NULL;

	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
				     sizeof(struct adau1961_platform_data), GFP_KERNEL);

		if (!pdata)
			return -ENOMEM;

		int err = upisnd_codec_fill_pdata(client->dev.of_node, pdata);

		if (err < 0) {
			printe("Failed to fill platform data from device tree! (%d)", err);
			return err;
		}
	}

	client->dev.platform_data = pdata;

	struct regmap_config config;

	config = adau1961_regmap_config;
	config.val_bits = 8;
	config.reg_bits = 16;

	return adau1961_probe(&client->dev, devm_regmap_init_i2c(client, &config), NULL);
}

static void upisnd_codec_i2c_remove(struct i2c_client *client)
{
	adau1961_remove(&client->dev);
}

static const struct i2c_device_id upisnd_codec_i2c_ids[] = {
	{ "upisnd-codec", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, upisnd_codec_i2c_ids);

static const struct of_device_id upisnd_codec_i2c_dt_ids[] = {
	{ .compatible = "blokas,upisnd-codec", },
	{ },
};
MODULE_DEVICE_TABLE(of, upisnd_codec_i2c_dt_ids);

struct i2c_driver upisnd_codec_i2c_driver = {
	.driver = {
		.name = "upisnd_codec",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(upisnd_codec_i2c_dt_ids),
	},
	.probe = upisnd_codec_i2c_probe,
	.remove = upisnd_codec_i2c_remove,
	.id_table = upisnd_codec_i2c_ids,
};
module_i2c_driver(upisnd_codec_i2c_driver);

MODULE_DESCRIPTION("Codec Driver for Pisound Micro");
MODULE_AUTHOR("Giedrius Trainavi\xc4\x8dius <giedrius@blokas.io>");
MODULE_LICENSE("GPL v2");

/* vim: set ts=8 sw=8 noexpandtab: */
