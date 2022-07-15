/*
 * ASoC Driver for Merus Audio Raspberry Pi HAT Sound Card
 *
 * Author: Ariel Muszkat <ariel.muszkat@infineon.com>
 *		Copyright 2018
 *    	based on code by Jorgen Kragh Jakobsen <jkj@merus-audio.com>
 *		based on code by Daniel Matuschek <info@crazy-audio.com>
 *		based on code by Florian Meier <florian.meier@koalo.de>
 *
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

#include "../codecs/ma120x0p.h"

static struct snd_soc_card snd_rpi_merus_amp;

/*

static struct gpio_desc *enable_gpio;
static struct gpio_desc *mute_gpio;
static struct gpio_desc *err_gpio;
static struct gpio_desc *bst_en_gpio;

static int snd_rpi_merus_amp_get_gpios(struct platform_device *pdev)
{
	int ret = 0;
	
	// Get mute line for amp 1 and make sure it is muted
	mute_gpio = devm_gpiod_get_optional(&pdev->dev, "mute1",
							    GPIOD_OUT_LOW);
			if (IS_ERR(mute_gpio)) {
				ret = PTR_ERR(mute_gpio);
				dev_err(&pdev->dev,
					"Failed to get mute 1 gpio: %d\n", ret);
				return ret;
				}
	// Get enable line for amp 1 and make sure it is disabled
	enable_gpio = devm_gpiod_get_optional(&pdev->dev, "enable1",
							    GPIOD_OUT_HIGH);
			if (IS_ERR(enable_gpio)) {
				ret = PTR_ERR(enable_gpio);
				dev_err(&pdev->dev,
					"Failed to get enable1 gpio: %d\n", ret);
				return ret;
				}

	// Get error line for amp 1
	err_gpio = devm_gpiod_get_optional(&pdev->dev, "err1",
								GPIOD_IN);
			if (IS_ERR(err_gpio)) {
				ret = PTR_ERR(err_gpio);
				dev_err(&pdev->dev,
					"Failed to get err1 gpio: %d\n", ret);
				return ret;
				}

	// Get bst_en line for mp3428a and make sure it is disabled
	bst_en_gpio = devm_gpiod_get_optional(&pdev->dev, "bst_en",
								GPIOD_OUT_LOW);
			if (IS_ERR(bst_en_gpio)) {
				ret = PTR_ERR(bst_en_gpio);
				dev_err(&pdev->dev,
					"Failed to get bst_en gpio: %d\n", ret);
				return ret;
				}
	printk(KERN_ERR "Merus_amp_get_gpios");
	
	return ret;
}

*/
static int snd_rpi_merus_amp_init(struct snd_soc_pcm_runtime *rtd)
{
  	printk(KERN_INFO "Start Merus amp init");

	return 0;
}

static int snd_rpi_merus_amp_hw_params( struct snd_pcm_substream *substream,
				                        struct snd_pcm_hw_params *params)
{

	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;

	printk(KERN_INFO "Hardcoding BCLK Ratio to x64");

	// Hardcoded BCLK ratio. BCLK = 64 * FS. 
	ret = snd_soc_dai_set_bclk_ratio(asoc_rtd_to_cpu(rtd, 0), 64);
	if (ret)
		return ret;
	ret = snd_soc_dai_set_bclk_ratio(asoc_rtd_to_codec(rtd, 0), 64);

	return ret; 
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_merus_amp_ops = {
	.hw_params = snd_rpi_merus_amp_hw_params,
};

SND_SOC_DAILINK_DEFS(snd_rpi_merus_amp,
	DAILINK_COMP_ARRAY(COMP_CPU("bcm2708-i2s.0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("ma120x0p.1-0020", "ma120x0p-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("bcm2708-i2s.0")));

static struct snd_soc_dai_link snd_rpi_merus_amp_dai[] = {
{
	.name		= "MerusAmp" ,
	.stream_name	= "Merus Audio Amp",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBS_CFS,
	.ops		= &snd_rpi_merus_amp_ops,
	.init		= snd_rpi_merus_amp_init,
	SND_SOC_DAILINK_REG(snd_rpi_merus_amp),
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_merus_amp = {
	.name         = "snd_rpi_merus_amp",
	.driver_name  = "MerusAudioAmp",
	.owner        = THIS_MODULE,
	.dai_link     = snd_rpi_merus_amp_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_merus_amp_dai),
};

static int snd_rpi_merus_amp_probe(struct platform_device *pdev)
{
	int ret = 0;
    printk(KERN_INFO "Amplifier Debug probe stage 1\n" );
	
    snd_rpi_merus_amp.dev = &pdev->dev;

		if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;

		dai = &snd_rpi_merus_amp_dai[0];
		i2s_node = of_parse_phandle(pdev->dev.of_node,
			"i2s-controller", 0);

		if (i2s_node) {
			dai->cpus->of_node = i2s_node;
			dai->platforms->of_node = i2s_node;
			dai->cpus->dai_name = NULL;
			dai->platforms->name = NULL;
		} else {
			return -EPROBE_DEFER;
			}

	}

	// ret = snd_rpi_merus_amp_get_gpios(pdev);
			
	printk(KERN_INFO "Registering Sound card");			

	ret = devm_snd_soc_register_card(&pdev->dev,
			&snd_rpi_merus_amp);
	if (ret && ret != -EPROBE_DEFER) {
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);
		return ret;
	}
	if (ret == -EPROBE_DEFER)
		return ret;

	return ret;	

	return ret;
};

static int snd_rpi_merus_amp_remove(struct platform_device *pdev)
{
  printk(KERN_INFO "snd_rpi_merus_amp exit\n");

	return snd_soc_unregister_card(&snd_rpi_merus_amp);
};


static const struct of_device_id snd_rpi_merus_amp_of_match[] = {
	{ .compatible = "merus,merus-amp", },
	{},
};

MODULE_DEVICE_TABLE(of, snd_rpi_merus_amp_of_match);


static struct platform_driver snd_rpi_merus_amp_driver = {
	.driver = {
		.name   = "snd-rpi-merus-amp",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_merus_amp_of_match,
	},
	.probe          = snd_rpi_merus_amp_probe,
	.remove         = snd_rpi_merus_amp_remove,
};

module_platform_driver(snd_rpi_merus_amp_driver);

MODULE_AUTHOR("Ariel Muszkat <ariel.muszkat@infineon.com>");
MODULE_DESCRIPTION("ASoC Driver for Merus Audio Amp HAT Sound Card");
MODULE_LICENSE("GPL v2");