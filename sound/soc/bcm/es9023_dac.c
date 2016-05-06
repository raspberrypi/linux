/*
 * ASoC Machine Driver for ESS Sabre ES9023 DAC
 *
 * Author:     Clive Messer <clive.messer@digitaldreamtime.co.uk>
 *             Copyright 2014
 *
 * based on the HiFiBerry DAC driver
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

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>

struct es9023_dac_priv {
	bool bclk_ratio_int_div;
	struct snd_soc_dai_link dai;
	struct snd_soc_card card;
};

struct es9023_dac_variant {
	char *compatible;
	char *card_name;
	char *dai_name;
	char *dai_stream_name;
};

#define COMPAT_GENERIC	"es9023-dac"
#define COMPAT_ISABRE	"audiphonics,es9023-i-sabre-dac"
#define COMPAT_MAMBO	"collybia,es9023-mamboberry-dacplus"
#define COMPAT_AKK	"digitaldreamtime,es9023-akkordion-dac"
#define COMPAT_HBLIGHT	"hifiberry,es9023-dacpluslight"

static const struct es9023_dac_variant es9023_dac_variants[] = {
	{	/* generic */
		.compatible      = COMPAT_GENERIC,
		.card_name       = "ES9023",
		.dai_name        = "ES9023 DAC",
		.dai_stream_name = "ES9023 DAC HiFi",
	},
	{	/* AudioPhonics ISabre */
		.compatible	 = COMPAT_ISABRE,
		.card_name	 = "ISabre",
		.dai_name	 = "ISabre DAC",
		.dai_stream_name = "ISabre DAC HiFi",
	},
	{	/* MamboBerry DAC+ */
		.compatible	 = COMPAT_MAMBO,
		.card_name	 = "Mamboberry",
		.dai_name	 = "Mamboberry DAC",
		.dai_stream_name = "Mamboberry DAC HiFi",
	},
	{	/* Digital Dreamtime Akkordion */
		.compatible	 = COMPAT_AKK,
		.card_name	 = "Akkordion",
		.dai_name	 = "Akkordion DAC",
		.dai_stream_name = "Akkordion DAC HiFi",
	},
	{	/* HiFiBerry DAC+ Light */
		.compatible	 = COMPAT_HBLIGHT,
		.card_name	 = "snd_rpi_hifiberry_dac",
		.dai_name	 = "HifiBerry DAC",
		.dai_stream_name = "HifiBerry DAC HiFi",
	},
};

static const struct es9023_dac_variant *snd_rpi_es9023_dac_get_variant(
	struct device_node *np)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(es9023_dac_variants); i++) {
		if (of_device_is_compatible(np,
					    es9023_dac_variants[i].compatible))
			return &es9023_dac_variants[i];
	}

	return &es9023_dac_variants[0];
}

static int snd_rpi_es9023_dac_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct es9023_dac_priv *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	snd_pcm_format_t format = params_format(params);
	unsigned int sample_bits = snd_pcm_format_width(format);
	unsigned int sample_freq = params_rate(params);
	unsigned int channels = params_channels(params);
	unsigned int physical_bits =
		snd_pcm_format_physical_width(format);
	unsigned int bclk_ratio = sample_bits * channels;

	if (priv->bclk_ratio_int_div && channels == 2 &&
	    sample_freq < 192000 && sample_freq % 8000 == 0) {
		if (sample_bits == 16 || sample_bits == 24)
			bclk_ratio = 50;
		else if (sample_bits == 32)
			bclk_ratio = 100;
	}

	dev_dbg(rtd->dev, "%s: frequency=%u, format=%s, sample_bits=%u, "
		"physical_bits=%u, channels=%u. Setting bclk_ratio=%u.\n",
		__func__, sample_freq, snd_pcm_format_name(format),
		sample_bits, physical_bits, channels, bclk_ratio);

	return snd_soc_dai_set_bclk_ratio(cpu_dai, bclk_ratio);
}

/* machine stream operations */
static struct snd_soc_ops snd_rpi_es9023_dac_ops = {
	.hw_params = snd_rpi_es9023_dac_hw_params,
};

static int snd_rpi_es9023_dac_sub_parse_of(struct device_node *np,
	struct device_node **p_node, const char **name)
{
	struct of_phandle_args args;
	int ret;

	ret = of_parse_phandle_with_args(np, "sound-dai",
					 "#sound-dai-cells", 0, &args);
	if (ret)
		return ret;

	*p_node = args.np;

	ret = snd_soc_of_get_dai_name(np, name);
	if (ret < 0)
		return ret;

	return 0;
}

static int snd_rpi_es9023_dac_probe(struct platform_device *pdev)
{
	char prop[128];
	char *prefix = "es9023-dac,";
	const struct es9023_dac_variant *variant;
	struct device_node *codec_np, *cpu_np, *np = pdev->dev.of_node;
	struct es9023_dac_priv *priv;
	int ret = 0;

	snprintf(prop, sizeof(prop), "%scpu", prefix);
	cpu_np = of_get_child_by_name(np, prop);
	if (!cpu_np) {
		dev_err(&pdev->dev, "%s: failed to find %s DT node\n",
			__func__, prop);
		ret = -EINVAL;
		goto cpu_end;
	}

	snprintf(prop, sizeof(prop), "%scodec", prefix);
	codec_np = of_get_child_by_name(np, prop);
	if (!codec_np) {
		dev_err(&pdev->dev, "%s: failed to find %s DT node\n",
			__func__, prop);
		ret = -EINVAL;
		goto end;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto end;
	}

	/* priv->dai.cpu_dai_name = "bcm2708-i2s.0"; */
	ret = snd_rpi_es9023_dac_sub_parse_of(cpu_np,
					      &priv->dai.cpu_of_node,
					      &priv->dai.cpu_dai_name);
	if (ret != 0) {
		dev_err(&pdev->dev, "%s: failed to get cpu dai name: %d\n",
			__func__, ret);
		goto end;
	}
	priv->dai.cpu_dai_name = NULL;

	/* priv->dai.platform_name = "bcm2708-i2s.0"; */
	priv->dai.platform_of_node = priv->dai.cpu_of_node;

	/* priv->dai.codec_name = "es9023-codec"; */
	/* priv->dai.codec_dai_name = "es9023-hifi"; */
	ret = snd_rpi_es9023_dac_sub_parse_of(codec_np,
					      &priv->dai.codec_of_node,
					      &priv->dai.codec_dai_name);
	if (ret != 0) {
		dev_err(&pdev->dev, "%s: failed to get codec dai name: %d\n",
			__func__, ret);
		goto end;
	}

	priv->dai.dai_fmt = SND_SOC_DAIFMT_I2S |
			    SND_SOC_DAIFMT_NB_NF |
			    SND_SOC_DAIFMT_CBS_CFS;

	priv->dai.ops = &snd_rpi_es9023_dac_ops;

	priv->card.dai_link = &priv->dai;
	priv->card.dev = &pdev->dev;
	priv->card.num_links = 1;
	priv->card.owner = THIS_MODULE;

	snprintf(prop, sizeof(prop), "%sbclk-ratio-int-div", prefix);
	priv->bclk_ratio_int_div = of_property_read_bool(np, prop);

	variant = snd_rpi_es9023_dac_get_variant(np);

	snprintf(prop, sizeof(prop), "%scard-name", prefix);
	if (of_property_read_string(np, prop, &priv->card.name))
		priv->card.name = variant->card_name;

	snprintf(prop, sizeof(prop), "%sdai-name", prefix);
	if (of_property_read_string(np, prop, &priv->dai.name))
		priv->dai.name = variant->dai_name;

	snprintf(prop, sizeof(prop), "%sdai-stream-name", prefix);
	if (of_property_read_string(np, prop, &priv->dai.stream_name))
		priv->dai.stream_name = variant->dai_stream_name;

	platform_set_drvdata(pdev, &priv->card);
	snd_soc_card_set_drvdata(&priv->card, priv);

	ret = devm_snd_soc_register_card(&pdev->dev, &priv->card);
	if (ret)
		dev_err(&pdev->dev, "%s: snd_soc_register_card failed: %d\n",
			__func__, ret);

end:
	of_node_put(codec_np);
cpu_end:
	of_node_put(cpu_np);

	return ret;
}

static const struct of_device_id snd_rpi_es9023_dac_of_match[] = {
	/* generic */
	{ .compatible = COMPAT_GENERIC, },
	/* Manufacturer compatible definitions BELOW! */
	{ .compatible = COMPAT_ISABRE, },
	{ .compatible = COMPAT_MAMBO, },
	{ .compatible = COMPAT_AKK, },
	{ .compatible = COMPAT_HBLIGHT, },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, snd_rpi_es9023_dac_of_match);

static struct platform_driver snd_rpi_es9023_dac_driver = {
	.driver = {
		.name           = "snd-es9023-dac",
		.owner          = THIS_MODULE,
		.of_match_table = snd_rpi_es9023_dac_of_match,
	},
	.probe  = snd_rpi_es9023_dac_probe,
};

module_platform_driver(snd_rpi_es9023_dac_driver);

MODULE_AUTHOR("Clive Messer <clive.messer@digitaldreamtime.co.uk>");
MODULE_DESCRIPTION("ASoC ESS Sabre ES9023 card driver");
MODULE_LICENSE("GPL v2");
