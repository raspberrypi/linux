/*
 * ASoC Driver for DurioSound-DAC.
 *
 * Author :	Pitichai Pitimaneeyakul <pitichai@2-cans.com>
 *		Copyright 2014
 *
 *	    based on code by Florian Meier <florian.meier@koalo.de>
 *
 * This Module is rewritten from rpi-dac.c and it is modified to 
 * use pcm5102a as codec to support DurioSound DAC.
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

static int snd_rpi_duriosound_dac_init(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}

static int snd_rpi_duriosound_dac_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
  
	unsigned int sample_bits =
		snd_pcm_format_physical_width(params_format(params));

	return snd_soc_dai_set_bclk_ratio(cpu_dai, sample_bits * 2);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_duriosound_dac_ops = {
	.hw_params = snd_rpi_duriosound_dac_hw_params,
};

static struct snd_soc_dai_link snd_rpi_duriosound_dac_dai[] = {
{
	.name		= "DurioSound-DAC",
	.stream_name	= "DurioSound-DAC HiFi",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "pcm5102a-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "pcm5102a-codec",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_duriosound_dac_ops,
	.init		= snd_rpi_duriosound_dac_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_duriosound_dac = {
	.name         = "snd_rpi_duriosound_dac",
	.dai_link     = snd_rpi_duriosound_dac_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_duriosound_dac_dai),
};

static int snd_rpi_duriosound_dac_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_duriosound_dac.dev = &pdev->dev;
	ret = snd_soc_register_card(&snd_rpi_duriosound_dac);
	if (ret)
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static int snd_rpi_duriosound_dac_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_duriosound_dac);
}

static struct platform_driver snd_rpi_duriosound_dac_driver = {
        .driver = {
                .name   = "snd-duriosound-dac",
                .owner  = THIS_MODULE,
        },
        .probe          = snd_rpi_duriosound_dac_probe,
        .remove         = snd_rpi_duriosound_dac_remove,
};

module_platform_driver(snd_rpi_duriosound_dac_driver);

MODULE_AUTHOR("Pitichai Pitimaneeyakul <pitichai@2-cans.com>");
MODULE_DESCRIPTION("ASoC Driver for DurioSound-DAC");
MODULE_LICENSE("GPL v2");
