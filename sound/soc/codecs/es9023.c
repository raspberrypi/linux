/*
 * Driver for the ESS ES9023 codec
 *
 * Author:     Clive Messer <clive.messer@digitaldreamtime.co.uk>
 *             Copyright 2014
 *
 * based on the PCM1794A codec driver
 *     by Florian Meier <florian.meier@koalo.de> Copyright 2013
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


#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <sound/soc.h>

static struct snd_soc_dai_driver es9023_dai = {
	.name     = "es9023-hifi",
	.playback = {
		.stream_name  = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates        = SNDRV_PCM_RATE_8000_192000,
		.formats      = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_3LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S32_LE
	},
};

static struct snd_soc_codec_driver soc_codec_dev_es9023;

static int es9023_probe(struct platform_device *pdev)
{
	return snd_soc_register_codec(&pdev->dev, &soc_codec_dev_es9023,
				      &es9023_dai, 1);
}

static int es9023_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);
	return 0;
}

static const struct of_device_id es9023_of_match[] = {
	{ .compatible = "ess,es9023", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, es9023_of_match);

static struct platform_driver es9023_codec_driver = {
	.probe  = es9023_probe,
	.remove = es9023_remove,
	.driver = {
		.name           = "es9023-codec",
		.owner          = THIS_MODULE,
		.of_match_table = es9023_of_match,
	},
};

module_platform_driver(es9023_codec_driver);

MODULE_AUTHOR("Clive Messer <clive.messer@digitaldreamtime.co.uk>");
MODULE_DESCRIPTION("ASoC ESS Sabre ES9023 codec driver");
MODULE_LICENSE("GPL v2");
