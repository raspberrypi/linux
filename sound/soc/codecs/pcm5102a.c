/*
 * Driver for the PCM5102A codec
 *
 * Author:	Florian Meier <florian.meier@koalo.de>
 *		Copyright 2013
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

static const u32 pcm5102a_rates[] = {
	8000, 16000, 32000, 44100, 48000, 88200, 96000, 176400, 192000,
	352800, 384000,
};

static const struct snd_pcm_hw_constraint_list pcm5102a_constraint_rates = {
	.count = ARRAY_SIZE(pcm5102a_rates),
	.list = pcm5102a_rates,
};

static int pcm5102a_dai_startup(struct snd_pcm_substream *substream,
	struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	int ret;

	dev_dbg(codec->dev, "%s: set rates (8k-384k) constraint\n", __func__);

	ret = snd_pcm_hw_constraint_list(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_RATE,
					 &pcm5102a_constraint_rates);
	if (ret != 0) {
		dev_err(codec->dev, "%s: Failed to set rates constraint: %d\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

static const struct snd_soc_dai_ops pcm5102a_dai_ops = {
	.startup = pcm5102a_dai_startup,
};

static struct snd_soc_dai_driver pcm5102a_dai = {
	.name = "pcm5102a-hifi",
	.playback = {
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_KNOT,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			   SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE
	},
	.ops = &pcm5102a_dai_ops,
};

static struct snd_soc_codec_driver soc_codec_dev_pcm5102a;

static int pcm5102a_probe(struct platform_device *pdev)
{
	return snd_soc_register_codec(&pdev->dev, &soc_codec_dev_pcm5102a,
			&pcm5102a_dai, 1);
}

static int pcm5102a_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);
	return 0;
}

static const struct of_device_id pcm5102a_of_match[] = {
	{ .compatible = "ti,pcm5102a", },
	{ }
};
MODULE_DEVICE_TABLE(of, pcm5102a_of_match);

static struct platform_driver pcm5102a_codec_driver = {
	.probe 		= pcm5102a_probe,
	.remove 	= pcm5102a_remove,
	.driver		= {
		.name	= "pcm5102a-codec",
		.owner	= THIS_MODULE,
		.of_match_table = pcm5102a_of_match,
	},
};

module_platform_driver(pcm5102a_codec_driver);

MODULE_DESCRIPTION("ASoC PCM5102A codec driver");
MODULE_AUTHOR("Florian Meier <florian.meier@koalo.de>");
MODULE_LICENSE("GPL v2");
