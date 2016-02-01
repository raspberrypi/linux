/*
 * ASoC Driver for a PCM3060 board with static chip configuration
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

#include <linux/module.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>

static int snd_rpi_pcm3060_board_init(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}

static int snd_rpi_pcm3060_board_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	return snd_soc_dai_set_bclk_ratio(cpu_dai, 32 * 2);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_pcm3060_board_ops = {
	.hw_params = snd_rpi_pcm3060_board_hw_params,
};

static struct snd_soc_dai_link snd_rpi_pcm3060_board_dai[] = {
{
	.name		= "PCM3060 Board",
	.stream_name	= "PCM3060 Board HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "pcm3060-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "pcm3060-codec",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBM_CFM,
	.ops		= &snd_rpi_pcm3060_board_ops,
	.init		= snd_rpi_pcm3060_board_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_pcm3060_board = {
	.name         = "snd_rpi_pcm3060_board",
	.dai_link     = snd_rpi_pcm3060_board_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_pcm3060_board_dai),
};

static int snd_rpi_pcm3060_board_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_pcm3060_board.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai = &snd_rpi_pcm3060_board_dai[0];

		i2s_node = of_parse_phandle(pdev->dev.of_node,
					"i2s-controller", 0);

		if (i2s_node) {
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = i2s_node;
			dai->platform_name = NULL;
			dai->platform_of_node = i2s_node;
		}
	}

	ret = snd_soc_register_card(&snd_rpi_pcm3060_board);
	if (ret)
		dev_err(&pdev->dev, "snd_soc_register_card failed: %d\n", ret);

	return ret;
}

static int snd_rpi_pcm3060_board_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_pcm3060_board);
}

static const struct of_device_id snd_rpi_pcm3060_board_of_match[] = {
	{ .compatible = "pcm3060,pcm3060-board", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_pcm3060_board_of_match);

static struct platform_driver snd_rpi_pcm3060_board_driver = {
	.driver = {
		.name   = "snd-pcm3060-board",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_pcm3060_board_of_match,
	},
	.probe          = snd_rpi_pcm3060_board_probe,
	.remove         = snd_rpi_pcm3060_board_remove,
};


static struct platform_device *pcm3060_board_dev;

int __init pcm3060_board_dev_init(void)
{
	pcm3060_board_dev = platform_device_register_simple(
		"snd-pcm3060-board", -1, NULL, 0);

	if (IS_ERR(pcm3060_board_dev)) {
		pr_err("error registering PCM3060 board\n");
		return PTR_ERR(pcm3060_board_dev);
	}

	return platform_driver_register(&snd_rpi_pcm3060_board_driver);
}

void __exit pcm3060_board_dev_exit(void)
{
	platform_device_unregister(pcm3060_board_dev);
	platform_driver_unregister(&snd_rpi_pcm3060_board_driver);
}

module_init(pcm3060_board_dev_init);
module_exit(pcm3060_board_dev_exit);

MODULE_AUTHOR("Jon Ronen-Drori <jon_ronen@yahoo.com>");
MODULE_DESCRIPTION("ASoC Driver for a PCM3060 Board with Static Chip Config");
MODULE_LICENSE("GPL v2");
