// SPDX-License-Identifier: GPL-2.0
/*
 * ASoC Driver for HiFiBerry DAC + DSP
 *
 * Author:	Joerg Schambacher <joscha@schambacher.com>
 *		Copyright 2018
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
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

static struct snd_soc_component_driver dacplusdsp_component_driver;

static struct snd_soc_dai_driver dacplusdsp_dai = {
	.name = "dacplusdsp-hifi",
	.capture = {
		.stream_name = "DAC+DSP Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_CONTINUOUS,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			   SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.playback = {
		.stream_name = "DACP+DSP Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_CONTINUOUS,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			   SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.symmetric_rate = 1};

#ifdef CONFIG_OF
static const struct of_device_id dacplusdsp_ids[] = {
	{
		.compatible = "hifiberry,dacplusdsp",
	},
	{} };
MODULE_DEVICE_TABLE(of, dacplusdsp_ids);
#endif

static int dacplusdsp_platform_probe(struct platform_device *pdev)
{
	int ret;

	ret = snd_soc_register_component(&pdev->dev,
			&dacplusdsp_component_driver, &dacplusdsp_dai, 1);
	if (ret) {
		pr_alert("snd_soc_register_component failed\n");
		return ret;
	}

	return 0;
}

static void dacplusdsp_platform_remove(struct platform_device *pdev)
{
	snd_soc_unregister_component(&pdev->dev);
}

static struct platform_driver dacplusdsp_driver = {
	.driver = {
		.name = "hifiberry-dacplusdsp-codec",
		.of_match_table = of_match_ptr(dacplusdsp_ids),
		},
		.probe = dacplusdsp_platform_probe,
		.remove = dacplusdsp_platform_remove,
};

module_platform_driver(dacplusdsp_driver);

MODULE_AUTHOR("Joerg Schambacher <joerg@i2audio.com>");
MODULE_DESCRIPTION("ASoC Driver for HiFiBerry DAC+DSP");
MODULE_LICENSE("GPL v2");
