// SPDX-License-Identifier: GPL-2.0
/*
 * hifiberry_studio.c -- driver for more complex
 * multichannel soundcards with own onboard firmware.
 *
 * Copyright (C) 2026 HiFiBerry
 *
 * Author: Joerg Schambacher <joerg@hifiberry.com>
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
#include <linux/regmap.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <linux/uuid.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/irq.h>

/* register definitions and firmware settings */
#define FIRMWARE_MAJOR			0x00
#define FIRMWARE_MINOR			0x01
#define FIRMWARE_SUBVERSION		0x02
#define HARDWARE_MAJOR			0x03
#define HARDWARE_MINOR			0x04
#define HARDWARE_SUBVERSION		0x05
#define UUID				0x10
#define SUPPORTED_RATES_0		0x20
#define SUPPORTED_RATES_1		0x21
#define SUPPORTED_RATES_2		0x22
#define SUPPORTED_RATES_3		0x23
#define SUPPORTED_FORMATS_0		0x24
#define SUPPORTED_FORMATS_1		0x25
#define SUPPORTED_FORMATS_2		0x26
#define SUPPORTED_FORMATS_3		0x27
#define NUM_OF_INPUT_CH			0x28
#define NUM_OF_OUTPUT_CH		0x29
#define CARD_RESET			0x2A
#define CURRENT_RATE			0x2B
#define CURRENT_FORMAT			0x2C
#define MAX_VOLUME			0x2D
#define MIN_VOLUME			0x2E
#define VOLUME_STP			0x2F
#define MAX_GAIN			0x30
#define MIN_GAIN			0x31
#define GAIN_STP			0x32
#define CARD_BUSY			0x33
#define CARD_NOERR			0x34
#define CARD_CLK_OPTIONS		0x35
#define CARD_CLOCK_MODE			0x36
#define CARD_CLK_ACT			0x37
#define CARD_CLK_OVRWR			0x38
#define CARD_STREAM_STATUS		0x39
#define CARD_DIR_FS			0x3A
#define DAC_STATE			0x40
#define DAC_CLOCK_SOURCE		0x41
#define DAC_SYS_CLK			0x42
#define DAC_SAMPLE_FORMAT		0x43
#define DAC_FILTER_SETTING_0		0x44
#define DAC_FILTER_SETTING_1		0x45
#define DAC_FILTER_SETTING_2		0x46
#define DAC_FILTER_SETTING_3		0x47
#define DAC_OUTPUT_MODE			0x48
#define MASTER_VOL			0x50
#define VOL_CH0				0x51
#define VOL_CH1				0x52
#define VOL_CH2				0x53
#define VOL_CH3				0x54
#define VOL_CH4				0x55
#define VOL_CH5				0x56
#define VOL_CH6				0x57
#define VOL_CH7				0x58
#define MUTE_OUTPUTS			0x59
#define ADC_STATE			0x70
#define ADC_CLOCK_SOURCE		0x71
#define ADC_SYS_CLK			0x72
#define ADC_SAMPLE_FORMAT		0x73
#define ADC_FILTER_SETTING_0		0x74
#define ADC_FILTER_SETTING_1		0x75
#define ADC_FILTER_SETTING_2		0x76
#define ADC_FILTER_SETTING_3		0x77
#define ADC_CLIPPING_ATT		0x78
#define GAIN_CH0			0x81
#define GAIN_CH1			0x82
#define GAIN_CH2			0x83
#define GAIN_CH3			0x84
#define GAIN_CH4			0x85
#define GAIN_CH5			0x86
#define GAIN_CH6			0x87
#define GAIN_CH7			0x88
#define MUTE_INPUTS			0x89
#define DIGI_CS_FORMAT			0x90	/* AES channel status format */
#define CLOCK_CONSUMER_MODE		0x00
#define CLOCK_PROVIDER_MODE		0x01

/* Mask encoding for provider frequency settings */
#define MASK_5512			0x00
#define MASK_8000			0x01
#define MASK_11025			0x02
#define MASK_16000			0x03
#define MASK_22050			0x04
#define MASK_32000			0x05
#define MASK_44100			0x06
#define MASK_48000			0x07
#define MASK_64000			0x08
#define MASK_88200			0x09
#define MASK_96000			0x0A
#define MASK_176400			0x0B
#define MASK_192000			0x0C
#define MASK_352800			0x0D
#define MASK_384000			0x0E

/* Mask encoding for sample formats */
#define MASK_16_BIT_SF			0x01
#define MASK_24_BIT_SF			0x02
#define MASK_32_BIT_SF			0x03

/* Card types */
#define DACADC				0x00
#define AES				0x01
#define AMP				0x02

/* Stream status */
#define CAPTURE				0x01
#define PLAY				0x10

/* struct definition for easier access to firmware registers */
struct hb_studio_regs_t {
	unsigned char firmware_major;
	unsigned char firmware_minor;
	unsigned char firmware_subversion;
	unsigned char hardware_major;
	unsigned char hardware_minor;
	unsigned char hardware_subversion;
	unsigned char res1[10];
	uuid_t uuid;
	unsigned int supported_rates;		// 0x20
	unsigned int supported_formats;		// 0x24
	unsigned char num_of_input_ch;		// 0x28
	unsigned char num_of_output_ch;		// 0x29
	unsigned char card_reset;		// 0x2a
	unsigned char current_rate;		// 0x2b
	unsigned char current_format;		// 0x2c
	unsigned char max_volume;		// 0x2d
	unsigned char min_volume;		// 0x2e
	unsigned char volume_stp;		// 0x2f
	unsigned char max_gain;			// 0x30
	unsigned char min_gain;			// 0x31
	unsigned char gain_stp;			// 0x32
	unsigned char card_busy;		// 0x33
	unsigned char card_noerr;		// 0x34
	unsigned char card_clk_options;		// 0x35
	unsigned char card_clk_mode;		// 0x36
	unsigned char card_clk_act;		// 0x37
	unsigned char card_clk_ovrwr;		// 0x38
	unsigned char card_stream_status;	// 0x39
	unsigned char res3[6];			// 0x3a
	unsigned char dac_state;		// 0x40
	unsigned char dac_clock_source;		// 0x41
	unsigned char dac_sys_clk;		// 0x42
	unsigned char dac_sample_format;
	unsigned char dac_filter_setting_0;
	unsigned char dac_filter_setting_1;
	unsigned char dac_filter_setting_2;
	unsigned char dac_filter_setting_3;
	unsigned char dac_output_mode;		// 0x48
	unsigned char res4[7];			// 0x49 - 0x4f
	unsigned char master_vol;		// 0x50
	unsigned char vol_ch0;			// 0x51
	unsigned char vol_ch1;			// 0x52
	unsigned char vol_ch2;			// 0x53
	unsigned char vol_ch3;			// 0x54
	unsigned char vol_ch4;			// 0x55
	unsigned char vol_ch5;			// 0x56
	unsigned char vol_ch6;			// 0x57
	unsigned char vol_ch7;			// 0x58
	unsigned char mute_outputs;		// 0x59
	unsigned char res5[22];			// 0x5a- 0x6f
	unsigned char adc_state;		// 0x70
	unsigned char adc_clock_source;		// 0x71
	unsigned char adc_sys_clk;		// 0x72
	unsigned char adc_sample_format;	// 0x73
	unsigned char adc_filter_setting_0;	// 0x74
	unsigned char adc_filter_setting_1;	// 0x75
	unsigned char adc_filter_setting_2;	// 0x76
	unsigned char adc_filter_setting_3;	// 0x77
	unsigned char adc_clipping_att;		// 0x78
	unsigned char res6[7];			// 0x79- 0x7f
	unsigned char res[1];			// 0x80
	unsigned char gain_ch0;			// 0x81
	unsigned char gain_ch1;			// 0x82
	unsigned char gain_ch2;			// 0x83
	unsigned char gain_ch3;			// 0x84
	unsigned char gain_ch4;			// 0x85
	unsigned char gain_ch5;			// 0x86
	unsigned char gain_ch6;			// 0x87
	unsigned char gain_ch7;			// 0x88
	unsigned char mute_inputs;		// 0x89
	};

static struct snd_soc_card snd_rpi_hifiberry_studio;
static struct i2c_client *hb_studio_i2c_client;
struct hb_studio_private {
	struct regmap *regmap;
	uuid_t uuid;
	unsigned int sample_bits;
	unsigned int current_rate;
	unsigned int allowed_rate;
	unsigned int clk_ovrwr;
	struct hb_studio_regs_t card_info;
	struct snd_pcm_substream *playback_substream;
	struct snd_pcm_substream *capture_substream;
	spinlock_t stream_lock;
	struct work_struct error_work;
	int card_type;
};

static struct hb_studio_private *priv;
static bool card_is_clk_provider;

static bool hb_studio_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CARD_BUSY:
	case CARD_RESET:
	case DAC_STATE:
	case DAC_CLOCK_SOURCE:
	case DAC_SYS_CLK:
	case CARD_CLK_ACT:
	case CARD_DIR_FS:
	case MASTER_VOL:
	case VOL_CH0:
	case VOL_CH1:
	case VOL_CH2:
	case VOL_CH3:
	case VOL_CH4:
	case VOL_CH5:
	case VOL_CH6:
	case VOL_CH7:
	case GAIN_CH0:
	case GAIN_CH1:
	case GAIN_CH2:
	case GAIN_CH3:
	case GAIN_CH4:
	case GAIN_CH5:
	case GAIN_CH6:
	case GAIN_CH7:
		return true;
	default:
		return false;
	}
}

static bool hb_studio_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case FIRMWARE_MAJOR:
	case FIRMWARE_MINOR:
	case FIRMWARE_SUBVERSION:
	case HARDWARE_MAJOR:
	case HARDWARE_MINOR:
	case HARDWARE_SUBVERSION:
	case NUM_OF_INPUT_CH:
	case NUM_OF_OUTPUT_CH:
	case SUPPORTED_RATES_0:
	case SUPPORTED_RATES_1:
	case SUPPORTED_RATES_2:
	case SUPPORTED_RATES_3:
	case SUPPORTED_FORMATS_0:
	case SUPPORTED_FORMATS_1:
	case SUPPORTED_FORMATS_2:
	case SUPPORTED_FORMATS_3:
	case UUID:
	case DAC_STATE:
	case CARD_BUSY:
	case DIGI_CS_FORMAT:
	case CARD_RESET:
	case CARD_CLOCK_MODE:
	case CARD_CLK_ACT:
	case CARD_DIR_FS:
	case DAC_CLOCK_SOURCE:
	case DAC_SYS_CLK:
	case DAC_SAMPLE_FORMAT:
	case DAC_FILTER_SETTING_0:
	case DAC_FILTER_SETTING_1:
	case DAC_FILTER_SETTING_2:
	case DAC_FILTER_SETTING_3:
	case DAC_OUTPUT_MODE:
	case GAIN_CH0:
	case GAIN_CH1:
	case GAIN_CH2:
	case GAIN_CH3:
	case GAIN_CH4:
	case GAIN_CH5:
	case GAIN_CH6:
	case GAIN_CH7:
	case ADC_CLIPPING_ATT:
		return true;
	default:
		return reg < 0xff;
	}
}

static const struct regmap_config hb_studio_regmap = {
	.reg_bits         = 8,
	.val_bits         = 8,
	.max_register     = 0xff,
	.cache_type       = REGCACHE_RBTREE,
	.volatile_reg     = hb_studio_volatile_reg,
	.readable_reg     = hb_studio_readable_reg,
};

static const DECLARE_TLV_DB_MINMAX(adc_att_tlv, -600, -300);
static const DECLARE_TLV_DB_MINMAX(gain_tlv, -1200, 4000);
static const DECLARE_TLV_DB_MINMAX(volume_tlv, -10300, 2400);
static const DECLARE_TLV_DB_MINMAX(spkr_tlv, -10300, 0);
static const char * const pll_lock_texts[] = {"unlocked", "locked"};
static const char * const mute_texts[] = {"unmuted", "muted"};
static const char * const dac_filter_texts[] = {
	"FIR w/ De-Emph.",
	"Low Latency IIR w/ De-Emph.",
	"High Att. w/ De-Emph.",
	"Ringingless Low Latency FIR w/o Deemph.",
	};
static const char * const adc_att_texts[] = {
	"Clip att. off", "-3dB", "-4dB", "-5dB", "-6dB",
	};
static const char * const dix_clk_texts[] = {"TX", "RX"};
static const char * const cs_mode_texts[] = {"Consumer", "Professional",
					     "Professional CRC"};

struct hb_studio_vol_control_single {
	unsigned int reg;
	unsigned int shift;
	int min;
	int max;
	bool invert;
	const unsigned int *tlv;
};

static const char * const samplerate_texts[] = {
	"5512Hz", "8kHz", "11.025kHz", "16kHz",
	"22.050kHz", "32kHz", "44.1kHz", "48kHz", "64kHz",
	"88.2kHz", "96kHz", "176.4kHz", "192kHz", "352.8kHz", "384kHz",
	"na"
};

static int hb_studio_vol_info_single(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_info *uinfo)
{
	struct hb_studio_vol_control_single *ctl = (void *)kcontrol->private_value;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;  // mono
	uinfo->value.integer.min = ctl->min;
	uinfo->value.integer.max = ctl->max;
	uinfo->value.integer.step = 1;
	return 0;
}

static int hb_studio_vol_get_single(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct hb_studio_vol_control_single *ctl = (void *)kcontrol->private_value;
	unsigned int val;

	regmap_read(priv->regmap, ctl->reg, &val);
	val = (val >> ctl->shift) & 0xff;
	if (ctl->invert)
		val = ctl->max - val;
	ucontrol->value.integer.value[0] = val;
	return 0;
}

static int hb_studio_vol_put_single(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct hb_studio_vol_control_single *ctl = (void *)kcontrol->private_value;
	unsigned int val = ucontrol->value.integer.value[0];
	unsigned int new;

	if (ctl->invert)
		val = ctl->max - val;
	new = (val & 0xff) << ctl->shift;
	regmap_write(priv->regmap, ctl->reg, new);
	return 0;
}

struct hb_studio_enum_control {
	unsigned int reg;
	unsigned int shift;
	unsigned int mask;
	const char * const *texts;
	unsigned int items;
};

static int hb_studio_enum_info(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_info *uinfo)
{
	struct hb_studio_enum_control *ctl = (void *)kcontrol->private_value;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = ctl->items;

	if (uinfo->value.enumerated.item >= ctl->items)
		uinfo->value.enumerated.item = ctl->items - 1;

	strscpy(uinfo->value.enumerated.name,
		ctl->texts[uinfo->value.enumerated.item],
		sizeof(uinfo->value.enumerated.name) - 1);

	return 0;
}

static int hb_studio_enum_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct hb_studio_enum_control *ctl = (void *)kcontrol->private_value;
	unsigned int val;

	regmap_read(priv->regmap, ctl->reg, &val);

	val = (val >> ctl->shift) & ctl->mask;
	if (val >= ctl->items)
		val = 0;

	ucontrol->value.enumerated.item[0] = val;

	return 0;
}

static int hb_studio_enum_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct hb_studio_enum_control *ctl = (void *)kcontrol->private_value;
	unsigned int val = ucontrol->value.enumerated.item[0];

	if (val >= ctl->items)
		return -EINVAL;

	regmap_update_bits(priv->regmap, ctl->reg,
			   ctl->mask << ctl->shift,
			   (val & ctl->mask) << ctl->shift);

	return 0;
}

/*
 * Read the AES input rate.
 * Returns the rate in Hz, or 0 if no valid input is present.
 * Works in TX and RX clock modes.
 */
static int hb_studio_input_rate_hz(struct hb_studio_private *p)
{
	unsigned int raw = 0;
	int trials = 5;

	regmap_write(p->regmap, CARD_DIR_FS, 0x00);
	do {
		usleep_range(1000, 2000);
		regmap_read(p->regmap, CARD_DIR_FS, &raw);
	} while ((raw & 0x80) && --trials);

	switch (raw & 0x0f) {
	case 0x08: return 44100;
	case 0x09: return 48000;
	case 0x0b: return 88200;
	case 0x0c: return 96000;
	case 0x0e: return 176400;
	case 0x0f: return 192000;
	default:   return 0;
	}
}

static int hb_studio_samplerate_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct hb_studio_enum_control *ctl = (void *)kcontrol->private_value;
	unsigned int idx;

	switch (hb_studio_input_rate_hz(priv)) {
	case 44100:
		idx = 6;
		break;
	case 48000:
		idx = 7;
		break;
	case 88200:
		idx = 9;
		break;
	case 96000:
		idx = 10;
		break;
	case 176400:
		idx = 11;
		break;
	case 192000:
		idx = 12;
		break;
	default:
		idx = ctl->items - 1;	/* na */
		break;
	}

	ucontrol->value.enumerated.item[0] = idx;
	return 0;
}

#define VOL_CTL_SINGLE(kname, controls, ktlv) {\
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = kname, \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
		  SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.tlv.p = ktlv, \
	.info = hb_studio_vol_info_single, \
	.get = hb_studio_vol_get_single, \
	.put = hb_studio_vol_put_single, \
	.private_value = (unsigned long)&controls, }

#define ENUM_CTL_SINGLE(kname, controls) {\
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = kname, \
	.info = hb_studio_enum_info, \
	.get  = hb_studio_enum_get, \
	.put  = hb_studio_enum_put, \
	.private_value = (unsigned long)&controls, }

#define ENUM_CTL_SINGLE_RO(kname, controls) {\
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = kname, \
	.info = hb_studio_enum_info, \
	.get  = hb_studio_enum_get, \
	.put  = NULL, \
	.private_value = (unsigned long)&controls, }

#define ENUM_CTL_SINGLE_RO_GET(kname, controls, getfn) {\
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = kname, \
	.info = hb_studio_enum_info, \
	.get  = getfn, \
	.put  = NULL, \
	.private_value = (unsigned long)&controls, }

static const struct hb_studio_vol_control_single hb_studio_vol_ctls_single[] = {
	{ MASTER_VOL, 0, 0, 254, true, volume_tlv },
	{ VOL_CH0, 0, 0, 206, true, spkr_tlv },
	{ VOL_CH1, 0, 0, 206, true, spkr_tlv },
	{ VOL_CH2, 0, 0, 206, true, spkr_tlv },
	{ VOL_CH3, 0, 0, 206, true, spkr_tlv },
	{ VOL_CH4, 0, 0, 206, true, spkr_tlv },
	{ VOL_CH5, 0, 0, 206, true, spkr_tlv },
	{ VOL_CH6, 0, 0, 206, true, spkr_tlv },
	{ VOL_CH7, 0, 0, 206, true, spkr_tlv },
};

static const struct hb_studio_vol_control_single hb_studio_gain_ctls_single[] = {
	{ GAIN_CH0, 0, 0, 104, false, gain_tlv },
	{ GAIN_CH1, 0, 0, 104, false, gain_tlv },
	{ GAIN_CH2, 0, 0, 104, false, gain_tlv },
	{ GAIN_CH3, 0, 0, 104, false, gain_tlv },
	{ GAIN_CH4, 0, 0, 104, false, gain_tlv },
	{ GAIN_CH5, 0, 0, 104, false, gain_tlv },
	{ GAIN_CH6, 0, 0, 104, false, gain_tlv },
	{ GAIN_CH7, 0, 0, 104, false, gain_tlv },
};

static const struct snd_kcontrol_new hb_studio_play_controls_single[] = {
	VOL_CTL_SINGLE("Master Playback Volume",    hb_studio_vol_ctls_single[0], volume_tlv),
	VOL_CTL_SINGLE("Output Ch0 Playback Volume", hb_studio_vol_ctls_single[1], spkr_tlv),
	VOL_CTL_SINGLE("Output Ch1 Playback Volume", hb_studio_vol_ctls_single[2], spkr_tlv),
	VOL_CTL_SINGLE("Output Ch2 Playback Volume", hb_studio_vol_ctls_single[3], spkr_tlv),
	VOL_CTL_SINGLE("Output Ch3 Playback Volume", hb_studio_vol_ctls_single[4], spkr_tlv),
	VOL_CTL_SINGLE("Output Ch4 Playback Volume", hb_studio_vol_ctls_single[5], spkr_tlv),
	VOL_CTL_SINGLE("Output Ch5 Playback Volume", hb_studio_vol_ctls_single[6], spkr_tlv),
	VOL_CTL_SINGLE("Output Ch6 Playback Volume", hb_studio_vol_ctls_single[7], spkr_tlv),
	VOL_CTL_SINGLE("Output Ch7 Playback Volume", hb_studio_vol_ctls_single[8], spkr_tlv),
};

static const struct snd_kcontrol_new hb_studio_rec_controls_single[] = {
	VOL_CTL_SINGLE("Input Ch0 Capture Volume", hb_studio_gain_ctls_single[0], gain_tlv),
	VOL_CTL_SINGLE("Input Ch1 Capture Volume", hb_studio_gain_ctls_single[1], gain_tlv),
	VOL_CTL_SINGLE("Input Ch2 Capture Volume", hb_studio_gain_ctls_single[2], gain_tlv),
	VOL_CTL_SINGLE("Input Ch3 Capture Volume", hb_studio_gain_ctls_single[3], gain_tlv),
	VOL_CTL_SINGLE("Input Ch4 Capture Volume", hb_studio_gain_ctls_single[4], gain_tlv),
	VOL_CTL_SINGLE("Input Ch5 Capture Volume", hb_studio_gain_ctls_single[5], gain_tlv),
	VOL_CTL_SINGLE("Input Ch6 Capture Volume", hb_studio_gain_ctls_single[6], gain_tlv),
	VOL_CTL_SINGLE("Input Ch7 Capture Volume", hb_studio_gain_ctls_single[7], gain_tlv),
};

static const struct hb_studio_enum_control hb_studio_play_enum_ctls[] = {
	{ DAC_STATE, 0, 0x1, pll_lock_texts, ARRAY_SIZE(pll_lock_texts) },
	{ DAC_FILTER_SETTING_0, 0, 0x03, dac_filter_texts, ARRAY_SIZE(dac_filter_texts) },
	{ MUTE_OUTPUTS, 0, 0x01, mute_texts, ARRAY_SIZE(mute_texts) },
};

static const struct hb_studio_enum_control hb_studio_rec_enum_ctls[] = {
	{ ADC_CLIPPING_ATT, 0, 0x7, adc_att_texts, ARRAY_SIZE(adc_att_texts) },
	{ MUTE_INPUTS, 0, 0x01, mute_texts, ARRAY_SIZE(mute_texts) },
};

/* ---- Studio Digi / AES (card_type AES) controls ---- */
static const struct hb_studio_enum_control hb_studio_dix_clk_enum_ctls[] = {
	{ CARD_CLK_OVRWR, 0, 0x01, dix_clk_texts, ARRAY_SIZE(dix_clk_texts) },
};

static const struct hb_studio_enum_control hb_studio_cs_mode_enum_ctls[] = {
	{ DIGI_CS_FORMAT, 0, 0x03, cs_mode_texts, ARRAY_SIZE(cs_mode_texts) },
};

static const struct snd_kcontrol_new hb_studio_gen_controls_single[] = {
	ENUM_CTL_SINGLE("DAC Filter", hb_studio_play_enum_ctls[1]),
	ENUM_CTL_SINGLE("Output Mute", hb_studio_play_enum_ctls[2]),
};

static const struct snd_kcontrol_new adc_controls_single[] = {
	ENUM_CTL_SINGLE("Clipping Attenuation Capture Volume", hb_studio_rec_enum_ctls[0]),
};

static const struct hb_studio_enum_control hb_studio_samplerate_ctl = {
	/* DIR FS calculator (0x3A): real input rate, any clock mode */
	.reg = CARD_DIR_FS,
	.shift = 0,
	.mask = 0x0F,		/* rate is stored in lower 4 bits */
	.texts = samplerate_texts,
	.items = ARRAY_SIZE(samplerate_texts),
};

static const struct snd_kcontrol_new dix_controls_single[] = {
	ENUM_CTL_SINGLE("Clock mode", hb_studio_dix_clk_enum_ctls[0]),
	ENUM_CTL_SINGLE("Output Mute", hb_studio_play_enum_ctls[2]),
	ENUM_CTL_SINGLE("Input Mute", hb_studio_rec_enum_ctls[1]),
	ENUM_CTL_SINGLE_RO_GET("Current Sample Rate", hb_studio_samplerate_ctl,
				hb_studio_samplerate_get),
	ENUM_CTL_SINGLE("AES CS Mode", hb_studio_cs_mode_enum_ctls[0]),
};

static int snd_rpi_hifiberry_studio_hw_params(
		struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct device *dev = rtd->dev;
	unsigned char tmp;
	int trials;
	int busy;
	int err;

	priv->sample_bits = snd_pcm_format_width(params_format(params));
	priv->sample_bits = priv->sample_bits <= 16 ? 16 : 32;
	priv->current_rate = params_rate(params);

	dev_info(dev, "requesting %ibits @ %isps\n",
		 priv->sample_bits, priv->current_rate);

	/* write requested samplerate and word length back to card */
	switch (priv->current_rate) {
	case 5512:
		tmp = MASK_5512;
		break;
	case 8000:
		tmp = MASK_8000;
		break;
	case 11025:
		tmp = MASK_11025;
		break;
	case 16000:
		tmp = MASK_16000;
		break;
	case 22050:
		tmp = MASK_22050;
		break;
	case 32000:
		tmp = MASK_32000;
		break;
	case 44100:
		tmp = MASK_44100;
		break;
	case 88200:
		tmp = MASK_88200;
		break;
	case 176400:
		tmp = MASK_176400;
		break;
	case 352800:
		tmp = MASK_352800;
		break;
	case 48000:
		tmp = MASK_48000;
		break;
	case 96000:
		tmp = MASK_96000;
		break;
	case 192000:
		tmp = MASK_192000;
		break;
	case 384000:
		tmp = MASK_384000;
		break;
	default:
		dev_info(dev, "rate not supported (%u)\n", priv->current_rate);
		return -EINVAL;
	}

	/*
	 * AES clock selection & rate locking:
	 *  - in RX-clock mode every stream (capture or playback) must run at
	 *    the detected (AES) input rate;
	 *  - playback in TX-clock mode is unconstrained (the card is
	 *    the clock master).
	 */
	if (priv->card_type == AES) {
		bool capture = (substream->stream == SNDRV_PCM_STREAM_CAPTURE);
		unsigned int mode = 0;
		int in_rate;

		if (capture) {
			in_rate = hb_studio_input_rate_hz(priv);
			if (!in_rate) {
				dev_err(dev, "no AES input detected, cannot capture\n");
				return -EINVAL;
			}
			if (in_rate != (int)priv->current_rate) {
				dev_err(dev, "capture rate %u does not match AES input %d Hz\n",
					priv->current_rate, in_rate);
				return -EINVAL;
			}
			/* valid input and matching rate: lock the card to the input */
			regmap_write(priv->regmap, CARD_CLK_OVRWR, 0x01);
		} else {
			regmap_read(priv->regmap, CARD_CLK_OVRWR, &mode);
			if (mode == 0x01) {
				in_rate = hb_studio_input_rate_hz(priv);
				if (!in_rate || in_rate != (int)priv->current_rate) {
					dev_err(dev, "playback rate %u does not match AES input %d Hz\n",
						priv->current_rate, in_rate);
					return -EINVAL;
				}
			}
		}
	}

	err = regmap_write(priv->regmap, CURRENT_RATE, tmp);
	if (err < 0)
		return err;

	switch (priv->sample_bits) {
	case 16:
		tmp = MASK_16_BIT_SF;
		break;
	case 24:
		tmp = MASK_24_BIT_SF;
		break;
	case 32:
		tmp = MASK_32_BIT_SF;
		break;
	default:
		dev_info(dev, "word length not supported (%u)\n",
			 priv->sample_bits);
		return -EINVAL;
	}
	err = regmap_write(priv->regmap, CURRENT_FORMAT, tmp);
	if (err < 0)
		return err;

	/* If card provides clocks wait max. ~40ms for PLL */
	if (card_is_clk_provider) {
		/* trigger card to set new rate and format */
		err = regmap_write(priv->regmap, CARD_CLOCK_MODE, 0x02);
		if (err < 0)
			return err;
		trials = 10;
		do {
			usleep_range(3000, 4000);
			regmap_read(priv->regmap, CARD_BUSY, &busy);
		} while (busy && --trials);
		if (!trials) {
			dev_err(dev, "Card is unable to set clocks\n");
			return -EINVAL;
		}
	}
	/* always run with 64bit frames */
	return snd_soc_dai_set_bclk_ratio(cpu_dai, 64);
}

static int snd_rpi_hifiberry_studio_startup(
	struct snd_pcm_substream *substream)
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		priv->playback_substream = substream;
		regmap_update_bits(priv->regmap, CARD_STREAM_STATUS, 0x10,
				   0x10);
	} else {
		priv->capture_substream = substream;
		regmap_update_bits(priv->regmap, CARD_STREAM_STATUS, 0x01,
				   0x01);
	}

	return 0;
}

static void snd_rpi_hifiberry_studio_shutdown(
	struct snd_pcm_substream *substream)
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		regmap_update_bits(priv->regmap, CARD_STREAM_STATUS, 0x10,
				   0x00);
		priv->playback_substream = NULL;
	} else {
		regmap_update_bits(priv->regmap, CARD_STREAM_STATUS, 0x01,
				   0x00);
		priv->capture_substream = NULL;
	}
}

static const struct snd_soc_ops snd_rpi_hifiberry_studio_ops = {
	.startup   = snd_rpi_hifiberry_studio_startup,
	.hw_params = snd_rpi_hifiberry_studio_hw_params,
	.shutdown  = snd_rpi_hifiberry_studio_shutdown,
};

SND_SOC_DAILINK_DEFS(hifiberry_studio,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC("snd-soc-dummy", "snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static void hb_studio_error_work(struct work_struct *work);

static int hifiberry_studio_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_card *card = rtd->card;

	/* Configure playback */
	codec_dai->driver->playback.channels_max =
		priv->card_info.num_of_output_ch;
	codec_dai->driver->playback.rates = priv->card_info.supported_rates;
	codec_dai->driver->playback.formats = priv->card_info.supported_formats;

	if (priv->card_info.num_of_input_ch) {
		struct snd_soc_dai_link *dai = rtd->dai_link;

		dev_info(card->dev, "inputs detected: capture enabled\n");
		codec_dai->driver->symmetric_rate = 1;
		codec_dai->driver->symmetric_sample_bits = 1;
		codec_dai->driver->capture.formats =
			priv->card_info.supported_formats;
		codec_dai->driver->capture.rates =
			priv->card_info.supported_rates;
		codec_dai->driver->capture.channels_max =
			priv->card_info.num_of_input_ch;
		dai->stream_name = "HiFiBerry Studio HiFi";
	} else {
		rtd->dai_link->playback_only = 1;  // Disable capture
	}

	if ((priv->card_info.card_clk_options & 0x02) && card_is_clk_provider) {
		struct snd_soc_dai_link *dai = rtd->dai_link;

		dai->stream_name = "HiFiBerry Studio Pro HiFi";
		dai->dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
			| SND_SOC_DAIFMT_CBP_CFP;
	}
	dev_info(card->dev,
		 "HiFiBerry Studio Soundcard successfully initialized\n");

	spin_lock_init(&priv->stream_lock);
	INIT_WORK(&priv->error_work, hb_studio_error_work);
	return 0;
}

static struct snd_soc_dai_link snd_rpi_hifiberry_studio_dai[] = {
	{
		.name           = "HiFiBerry Studio Soundcard",
		.stream_name    = "HifiBerry Studio HiFi",
		.dai_fmt        = SND_SOC_DAIFMT_I2S |
					SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBC_CFC,
		.init           = hifiberry_studio_init,
		.ops            = &snd_rpi_hifiberry_studio_ops,
		SND_SOC_DAILINK_REG(hifiberry_studio),
	},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_hifiberry_studio = {
	.name         = "Hifiberry Studio Soundcard",
	.driver_name  = "HifiberryStudio",
	.owner        = THIS_MODULE,
	.dai_link     = snd_rpi_hifiberry_studio_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_hifiberry_studio_dai),
};

/*
 * Read the fixed hardware/firmware identity out of the controller: versions,
 * UUID (-> card_type), channel counts, supported rates/formats and the
 * CARD_BUSY.. capability block.
 */
static int hb_studio_ctrl_read_info(struct i2c_client *client,
				    struct hb_studio_private *p)
{
	u32 uuid_end;
	int ret;

	/* read basic card info */
	ret = regmap_bulk_read(p->regmap, 0x00, &p->card_info, 0x06);
	if (ret) {
		dev_err(&client->dev, "Failed to read card info: %d\n", ret);
		return ret;
	}

	dev_info(&client->dev, "hardware V%d.%d.%d\n",
		 p->card_info.hardware_major,
		 p->card_info.hardware_minor,
		 p->card_info.hardware_subversion);

	dev_info(&client->dev, "firmware V%d.%d.%d\n",
		 p->card_info.firmware_major,
		 p->card_info.firmware_minor,
		 p->card_info.firmware_subversion);

	/* read card capabilities */
	ret = regmap_bulk_read(p->regmap, UUID, &p->card_info.uuid, 0x20);
	if (ret) {
		dev_err(&client->dev, "Failed to read card info: %d\n", ret);
		return ret;
	}

	dev_info(&client->dev, "UUID: %*phN\n",
		 (int)sizeof(p->card_info.uuid.b), p->card_info.uuid.b);
	dev_info(&client->dev, "%i output channels reported\n",
		 p->card_info.num_of_output_ch);
	dev_dbg(&client->dev, "supported rates %08x\n",
		p->card_info.supported_rates);
	dev_dbg(&client->dev, "supported formats %08x\n",
		p->card_info.supported_formats);

	if (p->card_info.num_of_output_ch > 8 ||
	    p->card_info.num_of_input_ch > 8) {
		dev_err(&client->dev, "Maximum of 8 channels exceeded!\n");
		return -EINVAL;
	}

	if (p->card_info.num_of_input_ch > 0) {
		dev_info(&client->dev,
			 "Inputs detected: %u channels\n",
			 p->card_info.num_of_input_ch);
	} else {
		dev_info(&client->dev, "No inputs present, playback only\n");
	}

	ret = regmap_bulk_read(p->regmap, CARD_BUSY,
			       &p->card_info.card_busy, 20);
	if (ret) {
		dev_err(&client->dev, "Failed to read card info: %d\n", ret);
		return ret;
	}

	uuid_end = cpu_to_be32(*(u32 *)((u8 *)&p->card_info.uuid + 12));
	dev_info(&client->dev, "Card UUID end %08x\n", uuid_end);

	switch (uuid_end) {
	case 0x7c641980:
		dev_info(&client->dev, "Card type Analog\n");
		p->card_type = DACADC;
		break;
	case 0x0eb0104d:
		dev_info(&client->dev, "Card type Digital/AES\n");
		p->card_type = AES;
		break;
	default:
		dev_info(&client->dev, "No card type detected, assuming Analog\n");
		p->card_type = DACADC;
		break;
	}

	return 0;
}

/*
 * Check the clock capability (CARD_CLK_OPTIONS, already read
 * by hb_studio_ctrl_read_info()) and verfiy vs. the "clk-provider"
 * DT property
 */
static int hb_studio_validate_clk_config(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;

	if (np && of_property_read_bool(np, "clk-provider"))
		card_is_clk_provider = true;

	if (card_is_clk_provider) {
		if (priv->card_info.card_clk_options & 0x02) {
			dev_info(&pdev->dev, "Card provides i2s clocks\n");
		} else {
			dev_err(&pdev->dev,
				"Card cannot provide i2s clocks\n");
			return -EINVAL;
		}
	} else {
		if (priv->card_info.card_clk_options == 0x02) {
			dev_err(&pdev->dev,
				"Card cannot run as i2s clock consumer\n");
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * Register the ALSA controls for an analog Studio (DAC/ADC) cards (card_type
 * DACADC): DAC filter + output mute, the output volumes (sized to the actual
 * output-channel count), and if inputs are present the ADC gains and the
 * clipping-attenuation control.
 */
static int hb_studio_add_dacadc_controls(struct platform_device *pdev)
{
	int ret;

	ret = snd_soc_add_card_controls(&snd_rpi_hifiberry_studio,
					hb_studio_gen_controls_single,
					ARRAY_SIZE(hb_studio_gen_controls_single));
	if (ret < 0) {
		dev_err(&pdev->dev,
			"snd_soc_add_card_controls() failed: %d\n", ret);
		return ret;
	}
	ret = snd_soc_add_card_controls(&snd_rpi_hifiberry_studio,
					hb_studio_play_controls_single,
					ARRAY_SIZE(hb_studio_play_controls_single) / 9 *
					(priv->card_info.num_of_output_ch + 1));
	if (ret < 0) {
		dev_err(&pdev->dev,
			"snd_soc_add_card_controls() failed: %d\n", ret);
		return ret;
	}

	/* add optional ADC controls if inputs detected */
	if (priv->card_info.num_of_input_ch > 0) {
		ret = snd_soc_add_card_controls(&snd_rpi_hifiberry_studio,
						hb_studio_rec_controls_single,
						ARRAY_SIZE(hb_studio_rec_controls_single) / 8 *
						priv->card_info.num_of_input_ch);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"snd_soc_add_card_controls() failed: %d\n", ret);
		}
		ret = snd_soc_add_card_controls(&snd_rpi_hifiberry_studio,
						adc_controls_single,
						ARRAY_SIZE(adc_controls_single));
		if (ret < 0) {
			dev_err(&pdev->dev,
				"snd_soc_add_card_controls() failed: %d\n", ret);
		}
	}
	return ret;
}

/*
 * Register the ALSA controls for a Studio Digi/AES card (card_type AES):
 * the controls for Clock mode, Current Sample Rate, Input/Output Mute.
 */
static int hb_studio_add_dix_controls(struct platform_device *pdev)
{
	int ret = snd_soc_add_card_controls(&snd_rpi_hifiberry_studio,
		dix_controls_single, ARRAY_SIZE(dix_controls_single));
	if (ret < 0)
		dev_err(&pdev->dev,
			"snd_soc_add_card_controls() failed: %d\n", ret);
	return ret;
}

/*
 * The DAC8x and Digi cards share one controller and this driver; the card
 * type is auto-detected from the controller UUID (hb_studio_ctrl_read_info):
 *   DACADC -> DAC8x DAC/ADC controls (hb_studio_add_dacadc_controls)
 *   AES    -> Digi DIX controls      (hb_studio_add_dix_controls)
 * Any other/unknown type registers no extra card controls.
 */
static int hb_studio_add_card_controls(struct platform_device *pdev)
{
	switch (priv->card_type) {
	case DACADC:
		return hb_studio_add_dacadc_controls(pdev);
	case AES:
		return hb_studio_add_dix_controls(pdev);
	default:
		return 0;
	}
}

/*
 * I2C client driver for the onboard controller (hb-studio-ctrl @ 0x10).  It is
 * instantiated from the "hifiberry,hb-studio-ctrl" child node under &i2c1 in
 * the DT overlay
 */
static int hb_studio_ctrl_probe(struct i2c_client *client)
{
	struct hb_studio_private *p;
	int ret;

	p = devm_kzalloc(&client->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->regmap = devm_regmap_init_i2c(client, &hb_studio_regmap);
	if (IS_ERR(p->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(p->regmap),
				      "Failed to init regmap\n");

	i2c_set_clientdata(client, p);

	/*
	 * Read into the not-yet-published 'p' first. The machine driver polls
	 * the shared 'priv' pointer and treats non-NULL as "fully ready", so
	 * 'priv'/'hb_studio_i2c_client' must only be assigned once the read
	 * below has actually succeeded - probing the two drivers can and does
	 * happen concurrently, and publishing early let the machine driver
	 * see a card_info that was still all zeroes.
	 */
	ret = hb_studio_ctrl_read_info(client, p);
	if (ret) {
		dev_err(&client->dev,
			"Failed to read card info or wrong configuration!\n");
		return ret;
	}

	/* fully populated: now safe for the machine driver to consume */
	priv = p;
	hb_studio_i2c_client = client;

	return 0;
}

static void hb_studio_ctrl_remove(struct i2c_client *client)
{
	if (hb_studio_i2c_client == client) {
		priv = NULL;
		hb_studio_i2c_client = NULL;
	}
}

static const struct i2c_device_id hb_studio_ctrl_id[] = {
	{ "hb-studio-ctrl", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, hb_studio_ctrl_id);

static const struct of_device_id hb_studio_ctrl_of_match[] = {
	{ .compatible = "hifiberry,hb-studio-ctrl" },
	{ }
};
MODULE_DEVICE_TABLE(of, hb_studio_ctrl_of_match);

static struct i2c_driver hb_studio_ctrl_driver = {
	.driver = {
		.name                = "hb-studio-ctrl",
		.of_match_table      = hb_studio_ctrl_of_match,
		.suppress_bind_attrs = true,
	},
	.probe    = hb_studio_ctrl_probe,
	.remove   = hb_studio_ctrl_remove,
	.id_table = hb_studio_ctrl_id,
};

static void hb_studio_error_work(struct work_struct *work)
{
	struct hb_studio_private *p =
	    container_of(work, struct hb_studio_private, error_work);
	unsigned long flags;
	struct snd_pcm_substream *play, *capt;

	dev_err(&hb_studio_i2c_client->dev, "PLL lock lost, stopping streams\n");

	spin_lock_irqsave(&p->stream_lock, flags);
	play = p->playback_substream;
	capt = p->capture_substream;

	if (play)
		snd_pcm_stop(play, SNDRV_PCM_STATE_SUSPENDED);
	if (capt)
		snd_pcm_stop(capt, SNDRV_PCM_STATE_SUSPENDED);
	spin_unlock_irqrestore(&p->stream_lock, flags);
}

/* FS-change / PLL-lost interrupt: hand off to the error work queue. */
static irqreturn_t hb_studio_irq_handler(int irq, void *dev_id)
{
	struct hb_studio_private *p = dev_id;

	schedule_work(&p->error_work);
	return IRQ_HANDLED;
}

static int snd_rpi_hifiberry_studio_probe(struct platform_device *pdev)
{
	bool no_controls;
	int gpio, irq;
	int ret = 0;

	/* wait for the I2C controller driver to have probed and populated priv */
	if (!priv)
		return -EPROBE_DEFER;

	ret = hb_studio_validate_clk_config(pdev);
	if (ret < 0)
		return ret;

	no_controls = of_property_read_bool(pdev->dev.of_node, "no-controls");

	/*
	 * The FS-change / PLL-lost interrupt only exists on the Digi/AES
	 * board's overlay (gpios/interrupts wired to GPIO7); the DAC8x
	 * overlays don't route it at all, and there's nothing for a purely
	 * analog card to report here, so only look for it for AES cards.
	 */
	if (priv->card_type == AES) {
		gpio = of_get_named_gpio(pdev->dev.of_node, "gpios", 0);
		if (!gpio_is_valid(gpio))
			return dev_err_probe(&pdev->dev, gpio, "Invalid GPIO\n");

		ret = devm_gpio_request_one(&pdev->dev, gpio, GPIOF_IN,
					    "hifiberry-studio-fs-change");
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "Failed to request GPIO\n");

		irq = gpio_to_irq(gpio);
		if (irq < 0)
			return irq;

		ret = devm_request_threaded_irq(&pdev->dev, irq,
						hb_studio_irq_handler, NULL,
						IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						"hifiberry-studio-fs-change", priv);
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "Failed to request IRQ\n");

		dev_info(&pdev->dev, "GPIO interrupt registered on GPIO %d (IRQ %d)\n",
			 gpio, irq);
	}

	snd_rpi_hifiberry_studio.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;

		dai = &snd_rpi_hifiberry_studio_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
					    "i2s-controller", 0);

		if (i2s_node) {
			dai->cpus->dai_name = NULL;
			dai->cpus->of_node = i2s_node;
			dai->platforms->name = NULL;
			dai->platforms->of_node = i2s_node;
		}
	}

	ret = devm_snd_soc_register_card(&pdev->dev,
					 &snd_rpi_hifiberry_studio);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"devm_snd_soc_register_card() failed: %d\n", ret);
		return ret;
	}

	if (no_controls) {
		dev_info(&pdev->dev, "ALSA controls disabled (no-controls)\n");
		return 0;
	}

	/* as we do not have components use card-controls */
	ret = hb_studio_add_card_controls(pdev);

	return ret;
}

static const struct of_device_id snd_rpi_hifiberry_studio_of_match[] = {
	{ .compatible = "hifiberry,hifiberry-studio-dac8x", },
	{ .compatible = "hifiberry,hifiberry-studio", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_hifiberry_studio_of_match);

static struct platform_driver snd_rpi_hifiberry_studio_driver = {
	.driver = {
		.name   = "snd-rpi-hifiberry-studio",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_hifiberry_studio_of_match,
	},
	.probe  = snd_rpi_hifiberry_studio_probe,
};

static int __init hb_studio_driver_init(void)
{
	int ret;

	ret = i2c_add_driver(&hb_studio_ctrl_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&snd_rpi_hifiberry_studio_driver);
	if (ret)
		i2c_del_driver(&hb_studio_ctrl_driver);

	return ret;
}
module_init(hb_studio_driver_init);

static void __exit hb_studio_driver_exit(void)
{
	platform_driver_unregister(&snd_rpi_hifiberry_studio_driver);
	i2c_del_driver(&hb_studio_ctrl_driver);
}
module_exit(hb_studio_driver_exit);

MODULE_AUTHOR("Joerg Schambacher <joerg@hifiberry.com>");
MODULE_DESCRIPTION("HiFiBerry Studio soundcard driver (DAC8x, Digi)");
MODULE_LICENSE("GPL");
