/*
 * ALSA ASoC Machine Driver for Allo Piano DAC Plus Subwoofer
 *
 * Author:	Baswaraj K <jaikumar@cem-solutions.net>
 *		Copyright 2016
 *		based on code by Daniel Matuschek <info@crazy-audio.com>
 *		based on code by Florian Meier <florian.meier@koalo.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <sound/tlv.h>
#include "../codecs/pcm512x.h"

struct dsp_code {
	char i2c_addr;
	char offset;
	char val;
};

static struct snd_soc_pcm_runtime *rtd_glb;
static bool digital_gain_0db_limit = true;
unsigned int set_lowpass, set_mode, set_rate, dsp_page_number;

static const char * const allo_piano_mode_texts[] = {
	"2.0",
	"2.1",
	"2.2",
};

static const SOC_ENUM_SINGLE_DECL(allo_piano_mode_enum,
		0, 0, allo_piano_mode_texts);

static const char * const allo_piano_dsp_low_pass_texts[] = {
	"60",
	"70",
	"80",
	"90",
	"100",
	"110",
	"120",
	"130",
	"140",
	"150",
	"160",
	"170",
	"180",
	"190",
	"200",
};

static const SOC_ENUM_SINGLE_DECL(allo_piano_enum,
		0, 0, allo_piano_dsp_low_pass_texts);

static int snd_allo_piano_dsp_program(struct snd_soc_pcm_runtime *rtd,
		unsigned int mode, unsigned int rate, unsigned int lowpass)
{
	const struct firmware *fw;
	char firmware_name[40];
	int ret = 0, dac = 0;

	if (rate <= 46000)
		rate = 44100;
	else if (rate <= 68000)
		rate = 48000;
	else if (rate <= 92000)
		rate = 88200;
	else if (rate <= 136000)
		rate = 96000;
	else if (rate <= 184000)
		rate = 176400;
	else
		rate = 192000;

	/* same configuration loaded */
	if ((rate == set_rate) && (lowpass == set_lowpass)
			&& (mode == set_mode))
		return 1;

	set_rate = rate;
	set_mode = mode;

	if (mode == 0) { /* 2.0 */
		snd_soc_write(rtd->codec_dais[1]->codec,
			PCM512x_MUTE, 0x11);
		return 1;
	} else {
		snd_soc_write(rtd->codec_dais[1]->codec,
			PCM512x_MUTE, 0x00);
	}

	set_lowpass = lowpass;

	for (dac = 0; dac < rtd->num_codecs; dac++) {
		struct dsp_code *dsp_code_read;
		int i = 1;
		struct snd_soc_codec *codec = rtd->codec_dais[dac]->codec;

		if (dac == 0) { /* high */
			sprintf(firmware_name,
				"alloPiano/2.2/allo-piano-dsp-%d-%d-%d.bin",
				rate, ((set_lowpass * 10) + 60), dac);
		} else { /* low */
			sprintf(firmware_name,
				"alloPiano/2.%d/allo-piano-dsp-%d-%d-%d.bin",
				set_mode, rate, ((set_lowpass * 10) + 60), dac);
		}

		dev_info(codec->dev, "Dsp Firmware File Name: %s\n",
			firmware_name);

		ret = request_firmware(&fw, firmware_name, codec->dev);
		if (ret < 0) {
			dev_err(codec->dev, "Error: AlloPiano Firmware %s missing. %d\n",
					firmware_name, ret);
			goto err;
		}

		while (i < (fw->size - 1)) {
			dsp_code_read = (struct dsp_code *)&fw->data[i];

			if (dsp_code_read->offset == 0) {
				dsp_page_number = dsp_code_read->val;
				ret = snd_soc_write(rtd->codec_dais[dac]->codec,
					PCM512x_PAGE_BASE(0),
					dsp_code_read->val);

			} else if (dsp_code_read->offset != 0) {
				ret = snd_soc_write(rtd->codec_dais[dac]->codec,
					(PCM512x_PAGE_BASE(dsp_page_number) +
					dsp_code_read->offset),
					dsp_code_read->val);

			}
			if (ret < 0) {
				dev_err(codec->dev,
					"Failed to write Register: %d\n", ret);
				goto err;
			}
			i = i + 3;
		}
		release_firmware(fw);
	}
	return 1;

err:
	release_firmware(fw);
	return ret;
}

static int snd_allo_piano_mode_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = set_mode;
	return 0;
}

static int snd_allo_piano_mode_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	return(snd_allo_piano_dsp_program(rtd_glb,
			ucontrol->value.integer.value[0],
			set_rate, set_lowpass));
}

static int snd_allo_piano_lowpass_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = set_lowpass;
	return 0;
}

static int snd_allo_piano_lowpass_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	return(snd_allo_piano_dsp_program(rtd_glb,
			set_mode, set_rate,
			ucontrol->value.integer.value[0]));
}

static int pcm512x_get_reg_sub(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	unsigned int left_val = 0;
	unsigned int right_val = 0;

	left_val = snd_soc_read(rtd_glb->codec_dais[1]->codec,
			PCM512x_DIGITAL_VOLUME_2);
	if (left_val < 0)
		return left_val;

	right_val = snd_soc_read(rtd_glb->codec_dais[1]->codec,
			PCM512x_DIGITAL_VOLUME_3);
	if (right_val < 0)
		return right_val;

	ucontrol->value.integer.value[0] =
				(~(left_val  >> mc->shift)) & mc->max;
	ucontrol->value.integer.value[1] =
				(~(right_val >> mc->shift)) & mc->max;

	return 0;
}

static int pcm512x_set_reg_sub(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	unsigned int left_val = (ucontrol->value.integer.value[0] & mc->max);
	unsigned int right_val = (ucontrol->value.integer.value[1] & mc->max);
	int ret = 0;

	ret = snd_soc_write(rtd_glb->codec_dais[1]->codec,
			PCM512x_DIGITAL_VOLUME_2, (~left_val));
	if (ret < 0)
		return ret;

	ret = snd_soc_write(rtd_glb->codec_dais[1]->codec,
			PCM512x_DIGITAL_VOLUME_3, (~right_val));
	if (ret < 0)
		return ret;

	return 1;
}

static int pcm512x_get_reg_sub_switch(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	int val = 0;

	val = snd_soc_read(rtd_glb->codec_dais[1]->codec, PCM512x_MUTE);
	if (val < 0)
		return val;

	ucontrol->value.integer.value[0] = (val & 0x10) ? 0 : 1;
	ucontrol->value.integer.value[1] = (val & 0x01) ? 0 : 1;

	return val;
}

static int pcm512x_set_reg_sub_switch(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	unsigned int left_val = (ucontrol->value.integer.value[0]);
	unsigned int right_val = (ucontrol->value.integer.value[1]);
	int ret = 0;

	ret = snd_soc_write(rtd_glb->codec_dais[1]->codec, PCM512x_MUTE,
			~((left_val & 0x01)<<4 | (right_val & 0x01)));
	if (ret < 0)
		return ret;

	return 1;

}

static const DECLARE_TLV_DB_SCALE(digital_tlv_sub, -10350, 50, 1);

static const struct snd_kcontrol_new allo_piano_controls[] = {
	SOC_ENUM_EXT("Subwoofer mode",
			allo_piano_mode_enum,
			snd_allo_piano_mode_get,
			snd_allo_piano_mode_put),

	SOC_ENUM_EXT("Lowpass", allo_piano_enum,
			snd_allo_piano_lowpass_get,
			snd_allo_piano_lowpass_put),

	SOC_DOUBLE_R_EXT_TLV("Subwoofer Digital Playback Volume",
			PCM512x_DIGITAL_VOLUME_2,
			PCM512x_DIGITAL_VOLUME_3, 0, 255, 1,
			pcm512x_get_reg_sub,
			pcm512x_set_reg_sub,
			digital_tlv_sub),

	SOC_DOUBLE_EXT("Subwoofer Digital Playback Switch",
			PCM512x_MUTE,
			PCM512x_RQML_SHIFT,
			PCM512x_RQMR_SHIFT, 1, 1,
			pcm512x_get_reg_sub_switch,
			pcm512x_set_reg_sub_switch),
};

static int snd_allo_piano_dac_init(struct snd_soc_pcm_runtime *rtd)
{
	rtd_glb = rtd;

	if (digital_gain_0db_limit) {
		int ret;
		struct snd_soc_card *card = rtd->card;

		ret = snd_soc_limit_volume(card, "Digital Playback Volume",
					207);
		if (ret < 0)
			dev_warn(card->dev, "Failed to set volume limit: %d\n",
				ret);
	}

	return 0;
}

static int snd_allo_piano_dac_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	unsigned int sample_bits =
		snd_pcm_format_physical_width(params_format(params));
	unsigned int rate = params_rate(params);
	struct snd_soc_card *card = rtd->card;
	int ret = 0;

	rtd_glb = rtd;  /* TODO */
	if (digital_gain_0db_limit) {
		ret = snd_soc_limit_volume(card,
				"Subwoofer Digital Playback Volume", 207);
		if (ret < 0)
			dev_warn(card->dev, "Failed to set volume limit: %d\n",
				ret);
	}
	ret = snd_allo_piano_dsp_program(rtd, set_mode, rate, set_lowpass);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_bclk_ratio(cpu_dai, sample_bits * 2);

	return ret;
}

/* machine stream operations */
static struct snd_soc_ops snd_allo_piano_dac_ops = {
	.hw_params = snd_allo_piano_dac_hw_params,
};

static struct snd_soc_dai_link_component allo_piano_2_1_codecs[] = {
	{
		.dai_name = "pcm512x-hifi",
	},
	{
		.dai_name = "pcm512x-hifi",
	},
};

static struct snd_soc_dai_link snd_allo_piano_dac_dai[] = {
	{
		.name		= "PianoDACPlus",
		.stream_name	= "PianoDACPlus",
		.cpu_dai_name	= "bcm2708-i2s.0",
		.platform_name	= "bcm2708-i2s.0",
		.codecs		= allo_piano_2_1_codecs,
		.num_codecs	= 2,
		.dai_fmt	= SND_SOC_DAIFMT_I2S |
				SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
		.ops		= &snd_allo_piano_dac_ops,
		.init		= snd_allo_piano_dac_init,
	},
};

/* audio machine driver */
static struct snd_soc_card snd_allo_piano_dac = {
	.name = "PianoDACPlus",
	.owner = THIS_MODULE,
	.dai_link = snd_allo_piano_dac_dai,
	.num_links = ARRAY_SIZE(snd_allo_piano_dac_dai),
	.controls = allo_piano_controls,
	.num_controls = ARRAY_SIZE(allo_piano_controls),
};

static int snd_allo_piano_dac_probe(struct platform_device *pdev)
{
	int ret = 0, i = 0;
	struct snd_soc_card *card = &snd_allo_piano_dac;

	card->dev = &pdev->dev;
	snd_allo_piano_dac.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;

		dai = &snd_allo_piano_dac_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
					"i2s-controller", 0);
		if (i2s_node) {
			for (i = 0; i < card->num_links; i++) {
				dai->cpu_dai_name = NULL;
				dai->cpu_of_node = i2s_node;
				dai->platform_name = NULL;
				dai->platform_of_node = i2s_node;
			}
		}
		digital_gain_0db_limit =
			!of_property_read_bool(pdev->dev.of_node,
						"allo,24db_digital_gain");

		allo_piano_2_1_codecs[0].of_node =
			of_parse_phandle(pdev->dev.of_node, "audio-codec", 0);
		if (!allo_piano_2_1_codecs[0].of_node) {
			dev_err(&pdev->dev,
				"Property 'audio-codec' missing or invalid\n");
			return -EINVAL;
		}

		allo_piano_2_1_codecs[1].of_node =
			of_parse_phandle(pdev->dev.of_node, "audio-codec", 1);
		if (!allo_piano_2_1_codecs[1].of_node) {
			dev_err(&pdev->dev,
				"Property 'audio-codec' missing or invalid\n");
			return -EINVAL;
		}
	}

	ret = snd_soc_register_card(&snd_allo_piano_dac);
	if (ret < 0)
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n",
			ret);

	return ret;
}

static int snd_allo_piano_dac_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_allo_piano_dac);
}

static const struct of_device_id snd_allo_piano_dac_of_match[] = {
	{ .compatible = "allo,piano-dac-plus", },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, snd_allo_piano_dac_of_match);

static struct platform_driver snd_allo_piano_dac_driver = {
	.driver = {
		.name = "snd-allo-piano-dac-plus",
		.owner = THIS_MODULE,
		.of_match_table = snd_allo_piano_dac_of_match,
	},
	.probe = snd_allo_piano_dac_probe,
	.remove = snd_allo_piano_dac_remove,
};

module_platform_driver(snd_allo_piano_dac_driver);

MODULE_AUTHOR("Baswaraj K <jaikumar@cem-solutions.net>");
MODULE_DESCRIPTION("ALSA ASoC Machine Driver for Allo Piano DAC Plus");
MODULE_LICENSE("GPL v2");
