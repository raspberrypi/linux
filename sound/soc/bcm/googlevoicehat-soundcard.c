/*
 * ASoC Driver for Google voiceHAT SoundCard
 *
 * Author: Peter Malkin <petermalkin@google.com>
 *         Copyright 2016
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
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>

static int snd_rpi_googlevoicehat_soundcard_init(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}

static int snd_rpi_googlevoicehat_soundcard_hw_params(
	struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	unsigned int sample_bits =
		snd_pcm_format_physical_width(params_format(params));

	return snd_soc_dai_set_bclk_ratio(cpu_dai, sample_bits * 2);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_googlevoicehat_soundcard_ops = {
	.hw_params = snd_rpi_googlevoicehat_soundcard_hw_params,
};

static struct snd_soc_dai_link snd_rpi_googlevoicehat_soundcard_dai[] = {
{
	.name		= "Google voiceHAT SoundCard",
	.stream_name	= "Google voiceHAT SoundCard HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "voicehat-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "voicehat-codec",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_googlevoicehat_soundcard_ops,
	.init		= snd_rpi_googlevoicehat_soundcard_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_googlevoicehat_soundcard = {
	.name         = "snd_rpi_googlevoicehat_soundcard",
	.owner        = THIS_MODULE,
	.dai_link     = snd_rpi_googlevoicehat_soundcard_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_googlevoicehat_soundcard_dai),
};

static int snd_rpi_googlevoicehat_soundcard_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_googlevoicehat_soundcard.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai = &snd_rpi_googlevoicehat_soundcard_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
					"i2s-controller", 0);

		if (i2s_node) {
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = i2s_node;
			dai->platform_name = NULL;
			dai->platform_of_node = i2s_node;
		}
	}

	ret = devm_snd_soc_register_card(&pdev->dev,
			&snd_rpi_googlevoicehat_soundcard);
	if (ret)
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static const struct of_device_id snd_rpi_googlevoicehat_soundcard_of_match[] = {
	{ .compatible = "googlevoicehat,googlevoicehat-soundcard", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_googlevoicehat_soundcard_of_match);

static struct platform_driver snd_rpi_googlevoicehat_soundcard_driver = {
	.driver = {
		.name   = "snd-googlevoicehat-soundcard",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_googlevoicehat_soundcard_of_match,
	},
	.probe          = snd_rpi_googlevoicehat_soundcard_probe,
};

module_platform_driver(snd_rpi_googlevoicehat_soundcard_driver);

MODULE_AUTHOR("Peter Malkin <petermalkin@google.com>");
MODULE_DESCRIPTION("ASoC Driver for Google voiceHAT SoundCard");
MODULE_LICENSE("GPL v2");
