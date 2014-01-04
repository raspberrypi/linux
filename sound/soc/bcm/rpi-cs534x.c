/*
 * ASoC driver for CS5343/CS5344 ADC
 * connected to a Raspberry Pi
 *
 * Author:      Wojciech M. Zabolotny <wzab01@gmail.com>
 * Based on rpi-ess9018.c by:   Florian Meier, <koalo@koalo.de>
 *                              Copyright 2013
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

static int snd_rpi_cs534x_init(struct snd_soc_pcm_runtime *rtd)
{
        return 0;
}

static int snd_rpi_cs534x_hw_params(struct snd_pcm_substream *substream,
                                       struct snd_pcm_hw_params *params)
{
        return 0;
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_cs534x_ops = {
        .hw_params = snd_rpi_cs534x_hw_params,
};

static struct snd_soc_dai_link snd_rpi_cs534x_dai[] = {
{
        .name           = "cs5343",
        .stream_name    = "cs5343 HiFi",
        .cpu_dai_name   = "bcm2708-i2s.0",
        .codec_dai_name = "cs534x-hifi",
        .platform_name  = "bcm2708-pcm-audio.0",
        .codec_name     = "cs534x-codec",
        .dai_fmt        = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
                                SND_SOC_DAIFMT_CBM_CFM,
        .ops            = &snd_rpi_cs534x_ops,
        .init           = snd_rpi_cs534x_init,
},
{
        .name           = "cs5344",
        .stream_name    = "cs5344 HiFi",
        .cpu_dai_name   = "bcm2708-i2s.0",
        .codec_dai_name = "cs534x-hifi",
        .platform_name  = "bcm2708-pcm-audio.0",
        .codec_name     = "cs534x-codec",
        .dai_fmt        = SND_SOC_DAIFMT_LEFT_J | SND_SOC_DAIFMT_NB_NF |
                                SND_SOC_DAIFMT_CBM_CFM,
        .ops            = &snd_rpi_cs534x_ops,
        .init           = snd_rpi_cs534x_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_cs534x = {
        .name         = "snd_rpi_cs534x",
        .dai_link     = snd_rpi_cs534x_dai,
        .num_links    = ARRAY_SIZE(snd_rpi_cs534x_dai),
};

static int snd_rpi_cs534x_probe(struct platform_device *pdev)
{
        int ret = 0;

        snd_rpi_cs534x.dev = &pdev->dev;
        ret = snd_soc_register_card(&snd_rpi_cs534x);
        if (ret)
        {
                dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n", ret);
        }

        return ret;
}


static int snd_rpi_cs534x_remove(struct platform_device *pdev)
{
        return snd_soc_unregister_card(&snd_rpi_cs534x);
}

static struct platform_driver snd_rpi_cs534x_driver = {
        .driver = {
                .name   = "snd-rpi-cs534x",
                .owner  = THIS_MODULE,
        },
        .probe          = snd_rpi_cs534x_probe,
        .remove         = snd_rpi_cs534x_remove,
};

module_platform_driver(snd_rpi_cs534x_driver);

MODULE_AUTHOR("Wojciech M. Zabolotny");
MODULE_DESCRIPTION("ASoC Driver for Raspberry Pi connected to a cs534x");
MODULE_LICENSE("GPL");
