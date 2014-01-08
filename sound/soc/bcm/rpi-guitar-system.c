/*
 * ASoC driver for the guitar system based on CS5343/CS5344 ADC
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

/*
 * The hardware of the guitar system allows to use only 48000
 * sampling rate (it is defined at powerup by pull-up
 * and pull-down resistors).
*/
static const unsigned int rpi_guitar_system_rates[] = {
	48000,
};

static struct snd_pcm_hw_constraint_list rpi_guitar_system_constraints = {
	.list = rpi_guitar_system_rates,
	.count = ARRAY_SIZE(rpi_guitar_system_rates),
};


static int snd_rpi_guitar_system_startup(struct snd_pcm_substream
					 *substream)
{
	/* Setup constraints, because the sampling rate is fixed */
	snd_pcm_hw_constraint_list(substream->runtime, 0,
				   SNDRV_PCM_HW_PARAM_RATE,
				   &rpi_guitar_system_constraints);
	return 0;
}

static int snd_rpi_guitar_system_init(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}

static int snd_rpi_guitar_system_hw_params(struct snd_pcm_substream
					   *substream,
					   struct snd_pcm_hw_params
					   *params)
{
	return 0;
}


/* machine stream operations */
static struct snd_soc_ops snd_rpi_guitar_system_ops = {
	.startup = snd_rpi_guitar_system_startup,
	.hw_params = snd_rpi_guitar_system_hw_params,
};

static struct snd_soc_dai_link snd_rpi_guitar_system_dai[] = {
{
	.name = "rpi-guitar-system-cs5343",
	.stream_name = "cs5343 HiFi",
	.cpu_dai_name = "bcm2708-i2s.0",
	.codec_dai_name = "cs534x-hifi",
	.platform_name = "bcm2708-i2s.0",
	.codec_name = "cs534x-codec",
	.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
	           SND_SOC_DAIFMT_CBM_CFM,
	.ops = &snd_rpi_guitar_system_ops,
	.init = snd_rpi_guitar_system_init,
},
{
	.name = "rpi-gutar-system-cs5344",
	.stream_name = "cs5344 HiFi",
	.cpu_dai_name = "bcm2708-i2s.0",
	.codec_dai_name = "cs534x-hifi",
	.platform_name = "bcm2708-i2s.0",
	.codec_name = "cs534x-codec",
	.dai_fmt = SND_SOC_DAIFMT_LEFT_J | SND_SOC_DAIFMT_NB_NF |
	           SND_SOC_DAIFMT_CBM_CFM,
	.ops = &snd_rpi_guitar_system_ops,
	.init = snd_rpi_guitar_system_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_guitar_system = {
	.name = "snd-rpi-guitar-system",
	.dai_link = snd_rpi_guitar_system_dai,
	.num_links = ARRAY_SIZE(snd_rpi_guitar_system_dai),
};

static int snd_rpi_guitar_system_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_guitar_system.dev = &pdev->dev;
	ret = snd_soc_register_card(&snd_rpi_guitar_system);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n",
			ret);
	}

	return ret;
}


static int snd_rpi_guitar_system_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_guitar_system);
}

static struct platform_driver snd_rpi_guitar_system_driver = {
	.driver = {
		   .name = "snd-rpi-guitar-system",
		   .owner = THIS_MODULE,
		   },
	.probe = snd_rpi_guitar_system_probe,
	.remove = snd_rpi_guitar_system_remove,
};

module_platform_driver(snd_rpi_guitar_system_driver);

MODULE_AUTHOR("Wojciech M. Zabolotny");
MODULE_DESCRIPTION
    ("ASoC Driver for guitar system with CS534x & Raspberry Pi");
MODULE_LICENSE("GPL");
