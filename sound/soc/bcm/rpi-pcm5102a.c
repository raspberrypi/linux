/*
 * ASoC driver for PCM5102A codec
 * connected to a Raspberry Pi
 *
 * Author:      		Francesco Valla, <valla.francesco@gmail.com>
 * Based on rpi-ess9018.c by: 	Florian Meier, <koalo@koalo.de>
 *              		Copyright 2013
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/jack.h>

static int snd_rpi_pcm5102a_init(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}

static int snd_rpi_pcm5102a_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	return 0;
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_pcm5102a_ops = {
	.hw_params = snd_rpi_pcm5102a_hw_params,
};

static struct snd_soc_dai_link snd_rpi_pcm5102a_dai[] = {
{
	.name		= "PCM5102A",
	.stream_name	= "PCM5102A HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "pcm5102a-hifi",
	.platform_name	= "bcm2708-pcm-audio.0",
	.codec_name	= "pcm5102a-codec",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_pcm5102a_ops,
	.init		= snd_rpi_pcm5102a_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_pcm5102a = {
	.name         = "snd_rpi_pcm5102a",
	.dai_link     = snd_rpi_pcm5102a_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_pcm5102a_dai),
};

static int snd_rpi_pcm5102a_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_pcm5102a.dev = &pdev->dev;
	ret = snd_soc_register_card(&snd_rpi_pcm5102a);
	if (ret)
        {
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n", ret);
        }

	return ret;
}


static int snd_rpi_pcm5102a_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_pcm5102a);
}

static struct platform_driver snd_rpi_pcm5102a_driver = {
        .driver = {
                .name   = "snd-rpi-pcm5102a",
                .owner  = THIS_MODULE,
        },
        .probe          = snd_rpi_pcm5102a_probe,
        .remove         = snd_rpi_pcm5102a_remove,
};

module_platform_driver(snd_rpi_pcm5102a_driver);

MODULE_AUTHOR("Francesco Valla");
MODULE_DESCRIPTION("ASoC Driver for Raspberry Pi connected to a PCM5102A");
MODULE_LICENSE("GPL");
