/*
 * ASoC Driver for G2LABS-DAC-1543.
 *
 * Author:	Georgios F <georgios@gdis.se>
 *				Copyright 2017
 *				based on code by Florian Meier <florian.meier@koalo.de>
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

static int snd_g2labs_dac_init(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}

static int snd_g2labs_dac_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	return snd_soc_dai_set_bclk_ratio(cpu_dai, 128);
}

/* machine stream operations */
static struct snd_soc_ops snd_g2labs_dac_ops = {
	.hw_params = snd_g2labs_dac_hw_params,
};

static struct snd_soc_dai_link snd_g2labs_dac_dai[] = {
{
	.name	    	= "G2LABS-1543",
	.stream_name	= "G2LABS-DAC",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_dai_name	= "tda1543-hifi",
	.platform_name	= "bcm2708-i2s.0",
	.codec_name	= "tda1543-codec",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBM_CFS,
	.ops		= &snd_g2labs_dac_ops,
	.init		= snd_g2labs_dac_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_g2labs_dac = {
	.name         = "snd_g2labs_dac",
	.owner        = THIS_MODULE,
	.dai_link     = snd_g2labs_dac_dai,
	.num_links    = ARRAY_SIZE(snd_g2labs_dac_dai),
};

static int snd_g2labs_dac_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_g2labs_dac.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai = &snd_g2labs_dac_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node, "i2s-controller", 0);

		if (i2s_node) {
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = i2s_node;
			dai->platform_name = NULL;
			dai->platform_of_node = i2s_node;
		}
	}

	ret = snd_soc_register_card(&snd_g2labs_dac);
	if (ret)
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static int snd_g2labs_dac_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_g2labs_dac);
}

static const struct of_device_id snd_g2labs_dac_of_match[] = {
	{ .compatible = "rpi,g2labs-dac", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_g2labs_dac_of_match);

static struct platform_driver snd_g2labs_dac_driver = {
        .driver = {
                .name   = "snd-g2labs-dac",
                .owner  = THIS_MODULE,
                .of_match_table = snd_g2labs_dac_of_match,
        },
        .probe          = snd_g2labs_dac_probe,
        .remove         = snd_g2labs_dac_remove,
};

module_platform_driver(snd_g2labs_dac_driver);

MODULE_AUTHOR("Georgios F <georgios@gdis.se>");
MODULE_DESCRIPTION("ASoC Driver for G2LABS-DAC-1543");
MODULE_LICENSE("GPL v2");
