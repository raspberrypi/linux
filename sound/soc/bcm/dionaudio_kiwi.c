/*
 * ASoC Driver for Dion Audio Kiwi Streamer.
 *
 * Author:      Miquel Blauw <info@dionaudio.nl>
 *              Copyright 2022
 *
 * Based on the software of the RPi-DAC writen by Florian Meier
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

static int snd_rpi_dionaudio_kiwi_init(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}

static int snd_rpi_dionaudio_kiwi_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	return snd_soc_dai_set_bclk_ratio(cpu_dai, 32*2);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_dionaudio_kiwi_ops = {
	.hw_params = snd_rpi_dionaudio_kiwi_hw_params,
};

static struct snd_soc_dai_link snd_rpi_dionaudio_kiwi_dai[] = {
{
	.name		= "DIONAUDIO-KIWI",
	.stream_name	= "DIONAUDIO-KIWI HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "pcm1794a-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "pcm1794a-codec",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_dionaudio_kiwi_ops,
	.init		= snd_rpi_dionaudio_kiwi_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_dionaudio_kiwi = {
	.name         = "snd_rpi_dionaudio_kiwi",
	.dai_link     = snd_rpi_dionaudio_kiwi_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_dionaudio_kiwi_dai),
};

static int snd_rpi_dionaudio_kiwi_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_dionaudio_kiwi.dev = &pdev->dev;
	
	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai = &snd_rpi_dionaudio_kiwi_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node, "i2s-controller", 0);

		if (i2s_node) {
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = i2s_node;
			dai->platform_name = NULL;
			dai->platform_of_node = i2s_node;
		}
	}
	
	ret = snd_soc_register_card(&snd_rpi_dionaudio_kiwi);
	if (ret)
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static int snd_rpi_dionaudio_kiwi_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_dionaudio_kiwi);
}

static const struct of_device_id snd_rpi_dionaudio_kiwi_of_match[] = {
	{ .compatible = "dionaudio,dionaudio-kiwi", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_dionaudio_kiwi_of_match);

static struct platform_driver snd_rpi_dionaudio_kiwi_driver = {
        .driver = {
                .name   = "snd-dionaudio-kiwi",
                .owner  = THIS_MODULE,
                .of_match_table = snd_rpi_dionaudio_kiwi_of_match,
        },
        .probe          = snd_rpi_dionaudio_kiwi_probe,
        .remove         = snd_rpi_dionaudio_kiwi_remove,
};

module_platform_driver(snd_rpi_dionaudio_kiwi_driver);

MODULE_AUTHOR("Florian Meier <florian.meier@koalo.de>");
MODULE_DESCRIPTION("ASoC Driver for DionAudio-KIWI");
MODULE_LICENSE("GPL v2");
