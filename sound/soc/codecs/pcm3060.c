/*
 * Driver for the PCM3060 codec
 *
 * Author:	Jon Ronen-Drori <jon_ronen@yahoo.com>
 *		Copyright 2014-2015
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

static struct snd_soc_dai_driver pcm3060_dai = {
	.name = "pcm3060-hifi",
	.playback = {
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE
	},
	.capture = {
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE
	},
};

static struct snd_soc_codec_driver soc_codec_dev_pcm3060;

static int pcm3060_probe(struct platform_device *pdev)
{
	return snd_soc_register_codec(&pdev->dev, &soc_codec_dev_pcm3060,
			&pcm3060_dai, 1);
}

static int pcm3060_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);
	return 0;
}

static const struct of_device_id pcm3060_of_match[] = {
	{ .compatible = "ti,pcm3060", },
	{ }
};
MODULE_DEVICE_TABLE(of, pcm3060_of_match);

static struct platform_driver pcm3060_codec_driver = {
	.probe		= pcm3060_probe,
	.remove		= pcm3060_remove,
	.driver		= {
		.name	= "pcm3060-codec",
		.owner	= THIS_MODULE,
		.of_match_table = pcm3060_of_match,
	},
};


static struct platform_device *pcm3060_codec_dev;

int __init pcm3060_codec_dev_init(void)
{
	pcm3060_codec_dev = platform_device_register_simple(
		"pcm3060-codec", -1, NULL, 0);

	if (IS_ERR(pcm3060_codec_dev)) {
		pr_err("error registering PCM3060 codec\n");
		return PTR_ERR(pcm3060_codec_dev);
	}

	return platform_driver_register(&pcm3060_codec_driver);
}

void __exit pcm3060_codec_dev_exit(void)
{
	platform_driver_unregister(&pcm3060_codec_driver);
	platform_device_unregister(pcm3060_codec_dev);
}

module_init(pcm3060_codec_dev_init);
module_exit(pcm3060_codec_dev_exit);


MODULE_DESCRIPTION("ASoC PCM3060 codec driver");
MODULE_AUTHOR("Jon Ronen-Drori <jon_ronen@yahoo.com>");
MODULE_LICENSE("GPL v2");
