/*
 * ASoC driver for TDA1541A codec
 * connected to a Raspberry Pi
 *
 * Author:      Florian Meier, <koalo@koalo.de>
 *              Copyright 2013
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

static int snd_rpi_tda1541a_init(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}

static int snd_rpi_tda1541a_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	return 0;
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_tda1541a_ops = {
	.hw_params = snd_rpi_tda1541a_hw_params,
};

static struct snd_soc_dai_link snd_rpi_tda1541a_dai[] = {
{
	.name		= "TDA1541A",
	.stream_name	= "TDA1541A HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "tda1541a-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "tda1541a-codec",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_tda1541a_ops,
	.init		= snd_rpi_tda1541a_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_tda1541a = {
	.name         = "snd_rpi_tda1541a",
	.dai_link     = snd_rpi_tda1541a_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_tda1541a_dai),
};

static int snd_rpi_tda1541a_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_tda1541a.dev = &pdev->dev;
	ret = snd_soc_register_card(&snd_rpi_tda1541a);
	if (ret)
        {
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n", ret);
        }

	return ret;
}


static int snd_rpi_tda1541a_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_tda1541a);
}

static struct platform_driver snd_rpi_tda1541a_driver = {
        .driver = {
                .name   = "snd-rpi-tda1541a",
                .owner  = THIS_MODULE,
        },
        .probe          = snd_rpi_tda1541a_probe,
        .remove         = snd_rpi_tda1541a_remove,
};

module_platform_driver(snd_rpi_tda1541a_driver);

MODULE_AUTHOR("Florian Meier");
MODULE_DESCRIPTION("ASoC Driver for Raspberry Pi connected to a TDA1541A");
MODULE_LICENSE("GPL");
