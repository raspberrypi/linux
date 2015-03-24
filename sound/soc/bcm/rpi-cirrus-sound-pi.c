/*
 * ASoC machine driver for Cirrus Audio Card (with a WM5102 and WM8804 codecs )
 * connected to a Raspberry Pi
 *
 * Copyright 2015 Cirrus Logic Inc.
 *
 * Author:	Nikesh Oswal, <Nikesh.Oswal@wolfsonmicro.com>
 * Partly based on sound/soc/bcm/iqaudio-dac.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <sound/pcm_params.h>

#include "../codecs/wm5102.h"
#include "../codecs/wm8804.h"

#include <asm/system_info.h>

#define WM8804_CLKOUT_HZ 12000000

/*TODO: Shift this to platform data*/
#define GPIO_WM8804_RST 8
#define GPIO_WM8804_MODE 2
#define GPIO_WM8804_SW_MODE 23
#define GPIO_WM8804_I2C_ADDR_B 18
#define GPIO_WM8804_I2C_ADDR_B_PLUS 13
#define RPI_WLF_SR 44100
#define WM5102_MAX_SYSCLK_1 49152000 /*max sysclk for 4K family*/
#define WM5102_MAX_SYSCLK_2 45158400 /*max sysclk for 11.025K family*/

static struct snd_soc_card snd_rpi_wsp;

struct wm5102_machine_priv {
	int wm8804_sr;
	int wm5102_sr;
	int sync_path_enable;
};

enum {
	GPIO_FSEL_INPUT, GPIO_FSEL_OUTPUT,
	GPIO_FSEL_ALT5, GPIO_FSEL_ALT_4,
	GPIO_FSEL_ALT0, GPIO_FSEL_ALT1,
	GPIO_FSEL_ALT2, GPIO_FSEL_ALT3,
};

int spdif_rx_enable_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = &snd_rpi_wsp;
	struct wm5102_machine_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_codec *wm5102_codec = card->rtd[0].codec;
	int ret = 0;
	int clk_freq;
	int sr = priv->wm8804_sr;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* Enable sync path in case of SPDIF capture use case */
		clk_freq = (sr % 4000 == 0) ? WM5102_MAX_SYSCLK_1 : WM5102_MAX_SYSCLK_2;

		/*reset FLL1*/
		snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1_REFCLK,
					ARIZONA_FLL_SRC_NONE, 0, 0);
		snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1,
					ARIZONA_FLL_SRC_NONE, 0, 0);

		ret = snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1_REFCLK,
					    ARIZONA_CLK_SRC_MCLK1,
					    WM8804_CLKOUT_HZ,
					    clk_freq);
		if (ret != 0) {
			dev_err(wm5102_codec->dev, "Failed to enable FLL1 with Ref Clock Loop: %d\n", ret);
			return ret;
		}

		ret = snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1,
					    ARIZONA_CLK_SRC_AIF2BCLK,
					    sr * 64, clk_freq);
		if (ret != 0) {
			dev_err(wm5102_codec->dev, "Failed to enable FLL1  Sync Clock Loop: %d\n", ret);
			return ret;
		}
		priv->sync_path_enable = 1;
		break;
	case SND_SOC_DAPM_POST_PMD:
		priv->sync_path_enable = 0;
		break;
	}

	return ret;
}

static const struct snd_kcontrol_new rpi_wsp_controls[] = {
	SOC_DAPM_PIN_SWITCH("DMIC"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("SPDIF Out"),
	SOC_DAPM_PIN_SWITCH("SPDIF In"),
	SOC_DAPM_PIN_SWITCH("Line Input"),
};

const struct snd_soc_dapm_widget rpi_wsp_dapm_widgets[] = {
	SND_SOC_DAPM_MIC("DMIC", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Line Input", NULL),
	SND_SOC_DAPM_INPUT("dummy SPDIF in"),
	SND_SOC_DAPM_PGA_E("dummy SPDIFRX", SND_SOC_NOPM, 0, 0,NULL, 0,
			spdif_rx_enable_event,
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
};

const struct snd_soc_dapm_route rpi_wsp_dapm_routes[] = {
	{ "IN1L", NULL, "Headset Mic" },
	{ "IN1R", NULL, "Headset Mic" },
	{ "Headset Mic", NULL, "MICBIAS1" },

	{ "IN2L", NULL, "DMIC" },
	{ "IN2R", NULL, "DMIC" },
	{ "DMIC", NULL, "MICBIAS2" },

	{ "IN3L", NULL, "Line Input" },
	{ "IN3R", NULL, "Line Input" },
	{ "Line Input", NULL, "MICBIAS3" },

	/* Dummy routes to check whether SPDIF RX is enabled or not */
	{"dummy SPDIFRX", NULL, "dummy SPDIF in"},
	{"AIFTX", NULL, "dummy SPDIFRX"},
};

static int rpi_set_bias_level(struct snd_soc_card *card,
				struct snd_soc_dapm_context *dapm,
				enum snd_soc_bias_level level)
{
	struct snd_soc_codec *wm5102_codec = card->rtd[0].codec;
	struct wm5102_machine_priv *priv = snd_soc_card_get_drvdata(card);

	int ret;
	int sr = priv->wm5102_sr;
	int clk_freq = (sr % 4000 == 0) ? WM5102_MAX_SYSCLK_1 : WM5102_MAX_SYSCLK_2;

	switch (level) {
	case SND_SOC_BIAS_OFF:
		break;
	case SND_SOC_BIAS_ON:
		if (!priv->sync_path_enable) {
			ret = snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1,
						    ARIZONA_CLK_SRC_MCLK1,
						    WM8804_CLKOUT_HZ,
						    clk_freq);
			if (ret != 0) {
				dev_err(wm5102_codec->dev, "Failed to enable FLL1 with Ref Clock Loop: %d\n", ret);
				return ret;
			}
		}
		break;
	default:
		break;
	}

	dapm->bias_level = level;

	return 0;
}

static int rpi_set_bias_level_post(struct snd_soc_card *card,
		struct snd_soc_dapm_context *dapm,
		enum snd_soc_bias_level level)
{
	struct snd_soc_codec *wm5102_codec = card->rtd[0].codec;

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1,
			ARIZONA_FLL_SRC_NONE, 0, 0);
		snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1_REFCLK,
			ARIZONA_FLL_SRC_NONE, 0, 0);
		break;
	default:
		break;
	}

	dapm->bias_level = level;

	return 0;
}
static void bcm2708_set_gpio_alt(int pin, int alt)
{
	/*
	 * This is the common way to handle the GPIO pins for
	 * the Raspberry Pi.
	 * TODO This is a hack. Use pinmux / pinctrl.
	 */
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))
	unsigned int *gpio;
	gpio = ioremap(GPIO_BASE, SZ_16K);
	INP_GPIO(pin);
	SET_GPIO_ALT(pin, alt);
	iounmap(gpio);
#undef INP_GPIO
#undef SET_GPIO_ALT
}

static int wm8804_reset(void)
 {
	int ret;
	unsigned int gpio_wm8804_i2c_addr;

	if ((system_rev & 0xffffff) >= 0x10) {
		/* Model B+ or later */
		gpio_wm8804_i2c_addr = GPIO_WM8804_I2C_ADDR_B_PLUS;
	} else {
		gpio_wm8804_i2c_addr = GPIO_WM8804_I2C_ADDR_B;
	}

	if (!gpio_is_valid(GPIO_WM8804_RST)) {
		pr_err("Skipping unavailable gpio %d (%s)\n", GPIO_WM8804_RST, "wm8804_rst");
		return -ENOMEM;
	}

	if (!gpio_is_valid(GPIO_WM8804_MODE)) {
		pr_err("Skipping unavailable gpio %d (%s)\n", GPIO_WM8804_MODE, "wm8804_mode");
		return -ENOMEM;
	}

	if (!gpio_is_valid(GPIO_WM8804_SW_MODE)) {
		pr_err("Skipping unavailable gpio %d (%s)\n", GPIO_WM8804_SW_MODE, "wm8804_sw_mode");
		return -ENOMEM;
	}

	if (!gpio_is_valid(gpio_wm8804_i2c_addr)) {
		pr_err("Skipping unavailable gpio %d (%s)\n", gpio_wm8804_i2c_addr, "wm8804_i2c_addr");
		return -ENOMEM;
	}

	ret = gpio_request(GPIO_WM8804_RST, "wm8804_rst");
	if (ret < 0) {
		pr_err("gpio_request wm8804_rst failed\n");
		return ret;
	}

	ret = gpio_request(GPIO_WM8804_MODE, "wm8804_mode");
	if (ret < 0) {
		pr_err("gpio_request wm8804_mode failed\n");
		return ret;
	}

	ret = gpio_request(GPIO_WM8804_SW_MODE, "wm8804_sw_mode");
	if (ret < 0) {
		pr_err("gpio_request wm8804_sw_mode failed\n");
		return ret;
	}

	ret = gpio_request(gpio_wm8804_i2c_addr, "wm8804_i2c_addr");
	if (ret < 0) {
		pr_err("gpio_request wm8804_i2c_addr failed\n");
		return ret;
	}

	/*GPIO2 is used for SW/HW Mode Select and after Reset the same pin is used as
	I2C data line, so initially it is configured as GPIO OUT from BCM perspective*/
	/*Set SW Mode*/
	ret = gpio_direction_output(GPIO_WM8804_MODE, 1);
	if (ret < 0) {
		pr_err("gpio_direction_output wm8804_mode failed\n");
	}

	/*Set 2 Wire (I2C) Mode*/
	ret = gpio_direction_output(GPIO_WM8804_SW_MODE, 0);
	if (ret < 0) {
		pr_err("gpio_direction_output wm8804_sw_mode failed\n");
	}

	/*Set 2 Wire (I2C) Addr to 0x3A, writing 1 will make the Addr as 0x3B*/
	ret = gpio_direction_output(gpio_wm8804_i2c_addr, 0);
	if (ret < 0) {
		pr_err("gpio_direction_output wm8804_i2c_addr failed\n");
	}

	/*Take WM8804 out of reset*/
	ret = gpio_direction_output(GPIO_WM8804_RST, 1);
	if (ret < 0) {
		pr_err("gpio_direction_output wm8804_rst failed\n");
	}

	/*Put WM8804 in reset*/
	gpio_set_value(GPIO_WM8804_RST, 0);
	mdelay(500);
	/*Take WM8804 out of reset*/
	gpio_set_value(GPIO_WM8804_RST, 1);
	mdelay(500);

	gpio_free(GPIO_WM8804_RST);
	gpio_free(GPIO_WM8804_MODE);
	gpio_free(GPIO_WM8804_SW_MODE);
	gpio_free(gpio_wm8804_i2c_addr);

	/*GPIO2 is used for SW/HW Mode Select and after Reset the same pin is used as
	I2C data line, so after reset  it is configured as I2C data line i.e ALT0 function*/
	bcm2708_set_gpio_alt(GPIO_WM8804_MODE, 0);

	return ret;
}

static int snd_rpi_wsp_config_5102_clks(struct snd_soc_codec *wm5102_codec, int sr)
{
	int ret;
	int clk_freq = (sr % 4000 == 0) ? WM5102_MAX_SYSCLK_1 : WM5102_MAX_SYSCLK_2;


	ret = snd_soc_codec_set_sysclk(wm5102_codec,
			ARIZONA_CLK_SYSCLK,
			ARIZONA_CLK_SRC_FLL1,
			clk_freq,
			SND_SOC_CLOCK_IN);
	if (ret != 0) {
		dev_err(wm5102_codec->dev, "Failed to set AYNCCLK: %d\n", ret);
		return ret;
	}

	return 0;
 }

static int snd_rpi_wsp_config_8804_clks(struct snd_soc_codec *wm8804_codec,
	struct snd_soc_dai *wm8804_dai, int sr)
 {
	int ret;

	/*Set OSC(12MHz) to CLK2 freq*/
	/*Based on MCLKDIV it will be 128fs (MCLKDIV=1) or 256fs mode (MCLKDIV=0)*/
	/*BCLK will be MCLK/2  (MCLKDIV=1) or MCLK/4  (MCLKDIV=0) so BCLK is 64fs always*/
	ret = snd_soc_dai_set_pll(wm8804_dai, 0, 0, WM8804_CLKOUT_HZ, sr * 256);
	if (ret != 0) {
		dev_err(wm8804_codec->dev, "Failed to set OSC to CLK2 frequency: %d\n", ret);
		return ret;
	}

	/*Set MCLK as PLL Output*/
	ret = snd_soc_dai_set_sysclk(wm8804_dai, WM8804_TX_CLKSRC_PLL, sr * 256, 0);
	if (ret != 0) {
		dev_err(wm8804_codec->dev, "Failed to set MCLK as PLL Output: %d\n", ret);
		return ret;
	}

	/*Fix MCLKDIV=0 for 256fs to avoid any issues switching between TX and RX. RX always expects 256fs*/
	ret = snd_soc_dai_set_clkdiv(wm8804_dai, WM8804_MCLK_DIV, 0 );
	if (ret != 0) {
		dev_err(wm8804_codec->dev, "Failed to set MCLK_DIV to 256fs: %d\n", ret);
		return ret;
	}

	/*Set CLKOUT as OSC Frequency*/
	ret = snd_soc_dai_set_sysclk(wm8804_dai, WM8804_CLKOUT_SRC_OSCCLK, WM8804_CLKOUT_HZ, 0);
	if (ret != 0) {
		dev_err(wm8804_codec->dev, "Failed to set CLKOUT as OSC Frequency: %d\n", ret);
		return ret;
	}

	return 0;
}

static int snd_rpi_wsp_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_codec *wm5102_codec = rtd->codec;
	struct snd_soc_dai *bcm_i2s_dai = rtd->cpu_dai;
	struct snd_soc_codec *wm8804_codec = card->rtd[1].codec;
	struct snd_soc_dai *wm8804_codec_dai = card->rtd[1].codec_dai;
	struct wm5102_machine_priv *priv = snd_soc_card_get_drvdata(card);
	int ret, capture_stream_opened,playback_stream_opened;
	unsigned int bclkratio, tx_mask, rx_mask;
	int width, num_slots=1;

	bclkratio = 2 * snd_pcm_format_physical_width(params_format(params));

	ret = snd_soc_dai_set_bclk_ratio(bcm_i2s_dai, bclkratio);
	if (ret < 0) {
		dev_err(wm5102_codec->dev, "set_bclk_ratio failed: %d\n", ret);
		return ret;
	}

	/*8804 supports sample rates from 32k only*/
	/*Setting <32k raises error from 8804 driver while setting the clock*/
	if(params_rate(params) >= 32000)
	{
		ret = snd_rpi_wsp_config_8804_clks(wm8804_codec, wm8804_codec_dai,
						params_rate(params));

		if (ret != 0) {
			dev_err(wm8804_codec->dev, "snd_rpi_wsp_config_8804_clks failed: %d\n",
				ret);
			return ret;
		}
	}

	capture_stream_opened =
		substream->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream_opened;
	playback_stream_opened =
		substream->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream_opened;

	priv->wm5102_sr =  params_rate(params);

	ret = snd_rpi_wsp_config_5102_clks(wm5102_codec,  params_rate(params));
	if (ret != 0) {
		dev_err(wm5102_codec->dev, "snd_rpi_wsp_config_5102_clks failed: %d\n", ret);
		return ret;
	}

	width = snd_pcm_format_physical_width(params_format(params));

	if (capture_stream_opened) {
		tx_mask = 0;
		rx_mask = 1;
	}
	if (playback_stream_opened) {
		tx_mask = 1;
		rx_mask = 0;
	}
	ret = snd_soc_dai_set_tdm_slot(rtd->codec_dai, tx_mask, rx_mask, num_slots, width);
	if (ret < 0)
		return ret;

	priv->wm8804_sr =  params_rate(params);

	return 0;
}

static int dai_link2_params_fixup(struct snd_soc_dapm_widget *w, int event)
{
	struct snd_soc_card *card = &snd_rpi_wsp;
	struct wm5102_machine_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_stream *config = w->params;

	if (event == SND_SOC_DAPM_PRE_PMU) {
		config->rate_min = priv->wm8804_sr;
		config->rate_max = priv->wm8804_sr;
	} else if (event == SND_SOC_DAPM_PRE_PMD) {
		config->rate_min = RPI_WLF_SR;
		config->rate_max = RPI_WLF_SR;
	}

	return 0;
}

static int snd_rpi_wsp_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *wm5102_codec = rtd->codec;
	int ret,playback_stream_opened,capture_stream_opened;

	playback_stream_opened = substream->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream_opened;

	capture_stream_opened = substream->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream_opened;

	if((playback_stream_opened + capture_stream_opened) == 1){

		ret = snd_soc_codec_set_sysclk(wm5102_codec,
			ARIZONA_CLK_SYSCLK,
			ARIZONA_CLK_SRC_FLL1,
			0,
			SND_SOC_CLOCK_IN);

		if (ret != 0) {
		dev_err(wm5102_codec->dev, "Failed to set SYSCLK to Zero: %d\n", ret);
		return ret;
		}
	}

	return 0;
}

static struct snd_soc_ops snd_rpi_wsp_ops = {
	.hw_params = snd_rpi_wsp_hw_params,
	.hw_free = snd_rpi_wsp_hw_free,
};

static struct snd_soc_pcm_stream dai_link2_params = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = RPI_WLF_SR,
	.rate_max = RPI_WLF_SR,
	.channels_min = 2,
	.channels_max = 2,
};

static struct snd_soc_dai_link snd_rpi_wsp_dai[] = {
	{
		.name		= "WM5102",
		.stream_name	= "WM5102 AiFi",
		.cpu_dai_name	= "bcm2708-i2s.0",
		.codec_dai_name	= "wm5102-aif1",
		.platform_name	= "bcm2708-i2s.0",
		.codec_name	= "wm5102-codec",
		.dai_fmt	= SND_SOC_DAIFMT_I2S
					| SND_SOC_DAIFMT_NB_NF
					| SND_SOC_DAIFMT_CBM_CFM,
		.ops		= &snd_rpi_wsp_ops,
	},
	{
		.name = "WM5102 SPDIF",
		.stream_name = "SPDIF Tx/Rx",
		.cpu_dai_name = "wm5102-aif2",
		.codec_dai_name = "wm8804-spdif",
		.codec_name = "wm8804.1-003a",
		.dai_fmt = SND_SOC_DAIFMT_I2S
			| SND_SOC_DAIFMT_NB_NF
			| SND_SOC_DAIFMT_CBM_CFM,
		.ignore_suspend = 1,
		.params = &dai_link2_params,
		.params_fixup = dai_link2_params_fixup,
	},
};

static int snd_rpi_wsp_late_probe(struct snd_soc_card *card)
{
	struct wm5102_machine_priv *priv = snd_soc_card_get_drvdata(card);
	int ret;

	priv->wm8804_sr = RPI_WLF_SR;
	priv->wm5102_sr = RPI_WLF_SR;
	priv->sync_path_enable = 0;

	ret = snd_soc_codec_set_sysclk(card->rtd[0].codec, ARIZONA_CLK_SYSCLK, ARIZONA_CLK_SRC_FLL1,
					0, SND_SOC_CLOCK_IN);
	if (ret != 0) {
		dev_err(card->rtd[0].codec->dev, "Failed to set SYSCLK to Zero: %d\n", ret);
		return ret;
	}

	ret = snd_rpi_wsp_config_8804_clks(card->rtd[1].codec, card->rtd[1].codec_dai, RPI_WLF_SR);

	if (ret != 0) {
		dev_err(card->rtd[1].codec->dev, "snd_rpi_wsp_config_8804_clks failed: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(card->rtd[0].codec_dai,  ARIZONA_CLK_SYSCLK, 0, 0);
	if (ret != 0) {
		dev_err(card->rtd[0].codec->dev, "Failed to set codec dai clk domain: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(card->rtd[1].cpu_dai, ARIZONA_CLK_SYSCLK, 0, 0);
	if (ret != 0) {
		dev_err(card->rtd[0].codec->dev, "Failed to set codec dai clk domain: %d\n", ret);
		return ret;
	}

	return 0;
}

/* audio machine driver */
static struct snd_soc_card snd_rpi_wsp = {
	.name		= "snd_rpi_wsp",
	.dai_link	= snd_rpi_wsp_dai,
	.num_links	= ARRAY_SIZE(snd_rpi_wsp_dai),
	.late_probe = snd_rpi_wsp_late_probe,
	.controls = rpi_wsp_controls,
	.num_controls = ARRAY_SIZE(rpi_wsp_controls),
	.dapm_widgets = rpi_wsp_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(rpi_wsp_dapm_widgets),
	.dapm_routes = rpi_wsp_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(rpi_wsp_dapm_routes),
	.set_bias_level = rpi_set_bias_level,
	.set_bias_level_post = rpi_set_bias_level_post,
};

static int snd_rpi_wsp_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct wm5102_machine_priv *wm5102;

	wm8804_reset();

	wm5102 = kzalloc(sizeof *wm5102, GFP_KERNEL);
	if (!wm5102)
		return -ENOMEM;

	snd_soc_card_set_drvdata(&snd_rpi_wsp, wm5102);

	if (pdev->dev.of_node) {
	    struct device_node *i2s_node;
	    struct snd_soc_dai_link *dai = &snd_rpi_wsp_dai[0];
	    i2s_node = of_parse_phandle(pdev->dev.of_node,
					"i2s-controller", 0);

	    if (i2s_node) {
		dai->cpu_dai_name = NULL;
		dai->cpu_of_node = i2s_node;
		dai->platform_name = NULL;
		dai->platform_of_node = i2s_node;
	    }
	}

	snd_rpi_wsp.dev = &pdev->dev;
	ret = snd_soc_register_card(&snd_rpi_wsp);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register card: %d\n", ret);
		kfree(wm5102);
	}

	return ret;
}

static int snd_rpi_wsp_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_rpi_wsp;
	struct wm5102_machine_priv *wm5102 = snd_soc_card_get_drvdata(card);

	snd_soc_unregister_card(&snd_rpi_wsp);
	kfree(wm5102);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id snd_rpi_wsp_of_match[] = {
		{ .compatible = "wlf,rpi-wm5102", },
		{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_wsp_of_match);
#endif /* CONFIG_OF */

static struct platform_driver snd_rpi_wsp_driver = {
	.driver = {
		.name   = "snd-rpi-wsp",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(snd_rpi_wsp_of_match),
	},
	.probe	  = snd_rpi_wsp_probe,
	.remove	 = snd_rpi_wsp_remove,
};

module_platform_driver(snd_rpi_wsp_driver);

MODULE_AUTHOR("Nikesh Oswal");
MODULE_AUTHOR("Liu Xin");
MODULE_DESCRIPTION("ASoC Driver for Raspberry Pi connected to Cirrus sound pi");
MODULE_LICENSE("GPL");
