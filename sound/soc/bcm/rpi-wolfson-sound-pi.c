/*
 * ASoC machine driver for Wolfson Audio Card (with a WM5102 and WM8804 codecs )
 * connected to a Raspberry Pi
 *
 * Author:	Nikesh Oswal, <Nikesh.Oswal@wolfsonmicro.com>
 *		Copyright 2013/2014
 *
 * Author:	Florian Meier, <koalo@koalo.de>
 *		Copyright 2013
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/arizona/registers.h>
#include <sound/pcm_params.h>

#include "../codecs/wm5102.h"
#include "../codecs/wm8804.h"

#define WM8804_CLKOUT_HZ 12000000
#define RPI_WSP_DEFAULT_MCLK2 24000000

/*TODO: Shift this to platform data*/
#define GPIO_WM8804_RST 8
#define GPIO_WM8804_MODE 2
#define GPIO_WM8804_SW_MODE 23
#define GPIO_WM8804_I2C_ADDR 18
#define RPI_WLF_SR 44100
#define WM5102_MAX_SYSCLK_1 49152000 /*max sysclk for 4K family*/
#define WM5102_MAX_SYSCLK_2 45158400 /*max sysclk for 11.025K family*/

static struct snd_soc_card snd_rpi_wsp;

struct wm5102_machine_priv {
	void __iomem *gpio_base;
	void __iomem *gpctl_base;
	struct snd_soc_codec *codec;
	struct snd_soc_dai *aif[3];
	int aif1rate;
	int wm8804_sr;
};

/* Output clock from GPIO_GCLK(GPIO4) */
#define GPIOFSEL(x)  (0x00+(x)*4)
#define GP0CTL       (0x00)
#define GP0DIV       (0x04)

/* Clock register settings */
#define BCM2708_CLK_PASSWD	(0x5a000000)
#define BCM2708_CLK_MASH(v)	((v) << 9)
#define BCM2708_CLK_FLIP	(1 << 8)
#define BCM2708_CLK_BUSY	(1 << 7)
#define BCM2708_CLK_KILL	(1 << 5)
#define BCM2708_CLK_ENAB	(1 << 4)
#define BCM2708_CLK_SRC(v)	(v)

#define BCM2708_CLK_DIVI(v)	((v) << 12)
#define BCM2708_CLK_DIVF(v)	(v)

static inline void bcm2708_gpio_write_reg(struct wm5102_machine_priv *dev,
					   int reg, u32 val)
{
	__raw_writel(val, dev->gpio_base + reg);
}

static inline u32 bcm2708_gpio_read_reg(struct wm5102_machine_priv *dev, int reg)
{
	return __raw_readl(dev->gpio_base + reg);
}

static inline void bcm2708_gpctl_write_reg(struct wm5102_machine_priv *dev,
					   int reg, u32 val)
{
	__raw_writel(val, dev->gpctl_base + reg);
}

static inline u32 bcm2708_gpctl_read_reg(struct wm5102_machine_priv *dev, int reg)
{
	return __raw_readl(dev->gpctl_base + reg);
}

enum {
	GPIO_FSEL_INPUT, GPIO_FSEL_OUTPUT,
	GPIO_FSEL_ALT5, GPIO_FSEL_ALT_4,
	GPIO_FSEL_ALT0, GPIO_FSEL_ALT1,
	GPIO_FSEL_ALT2, GPIO_FSEL_ALT3,
};

struct GPCTL {
    char SRC         : 4;
    char ENAB        : 1;
    char KILL        : 1;
    char             : 1;
    char BUSY        : 1;
    char FLIP        : 1;
    char MASH        : 2;
    unsigned int     : 13;
    char PASSWD      : 8;
};

enum {
	BCM2708_CLK_MASH_0 = 0,
	BCM2708_CLK_MASH_1,
	BCM2708_CLK_MASH_2,
	BCM2708_CLK_MASH_3,
};

enum {
	BCM2708_CLK_SRC_GND = 0,
	BCM2708_CLK_SRC_OSC,
	BCM2708_CLK_SRC_DBG0,
	BCM2708_CLK_SRC_DBG1,
	BCM2708_CLK_SRC_PLLA,
	BCM2708_CLK_SRC_PLLC,
	BCM2708_CLK_SRC_PLLD,
	BCM2708_CLK_SRC_HDMI,
};

/* Most clocks are not useable (freq = 0) */
static const unsigned int bcm2708_clk_freq[BCM2708_CLK_SRC_HDMI+1] = {
	[BCM2708_CLK_SRC_GND]		= 0,
	[BCM2708_CLK_SRC_OSC]		= 19200000,
	[BCM2708_CLK_SRC_DBG0]		= 0,
	[BCM2708_CLK_SRC_DBG1]		= 0,
	[BCM2708_CLK_SRC_PLLA]		= 0,
	[BCM2708_CLK_SRC_PLLC]		= 0,
	[BCM2708_CLK_SRC_PLLD]		= 500000000,
	[BCM2708_CLK_SRC_HDMI]		= 0,
};

static void gpio_gclk_init(void)
{
	unsigned gpiodir;
	struct snd_soc_card *card = &snd_rpi_wsp;
	struct wm5102_machine_priv *wm5102 = snd_soc_card_get_drvdata(card);

	gpiodir = bcm2708_gpio_read_reg(wm5102, GPIOFSEL(0));
	gpiodir &= ~(7 << 12);
	gpiodir |= GPIO_FSEL_ALT0 << 12;
	bcm2708_gpio_write_reg(wm5102, GPIOFSEL(0), gpiodir);
}

static void set_gclk_clock_rate(int clock_rate)
{
	struct snd_soc_card *card = &snd_rpi_wsp;
	struct wm5102_machine_priv *wm5102 = snd_soc_card_get_drvdata(card);
	unsigned int mash = BCM2708_CLK_MASH_1;
	int clk_src = -1;
	uint64_t dividend;
	unsigned int divi, divf;

	clk_src = BCM2708_CLK_SRC_PLLD;

	dividend = bcm2708_clk_freq[clk_src];
	dividend *= 1024;
	do_div(dividend, clock_rate);
	divi = dividend / 1024;
	divf = dividend % 1024;
	dev_dbg(wm5102->codec->dev, "divi %d, divf %d\n", divi, divf);

	/* Set clock divider */
	bcm2708_gpctl_write_reg(wm5102, GP0DIV, BCM2708_CLK_PASSWD
		| BCM2708_CLK_DIVI(divi)
		| BCM2708_CLK_DIVF(divf));

	/* Setup clock, but don't start it yet */
	bcm2708_gpctl_write_reg(wm5102, GP0CTL, BCM2708_CLK_PASSWD
		| BCM2708_CLK_MASH(mash)
		| BCM2708_CLK_SRC(clk_src));
}

static void enable_gclk_clock(bool enable)
{
	unsigned int clkreg;
	struct snd_soc_card *card = &snd_rpi_wsp;
	struct wm5102_machine_priv *wm5102 = snd_soc_card_get_drvdata(card);
	if (enable) {
		/* start clock*/
		clkreg = bcm2708_gpctl_read_reg(wm5102,
				GP0CTL);
		bcm2708_gpctl_write_reg(wm5102, GP0CTL,
				BCM2708_CLK_PASSWD | clkreg | BCM2708_CLK_ENAB);
	} else {
		/* stop clock */
		clkreg = bcm2708_gpctl_read_reg(wm5102, GP0CTL);
		bcm2708_gpctl_write_reg(wm5102, GP0CTL, ~(BCM2708_CLK_ENAB)
		& (BCM2708_CLK_PASSWD | clkreg));
	}
}

static const struct snd_kcontrol_new rpi_wsp_controls[] = {
	SOC_DAPM_PIN_SWITCH("DMIC"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("SPDIF out"),
	SOC_DAPM_PIN_SWITCH("SPDIF in"),
};

const struct snd_soc_dapm_widget rpi_wsp_dapm_widgets[] = {
	SND_SOC_DAPM_MIC("DMIC", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Line Input", NULL),
	/* Create widgets for SPDIF output and input */
	SND_SOC_DAPM_OUTPUT("SPDIF out"),
	SND_SOC_DAPM_INPUT("SPDIF in"),
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
	{ "Line Input", NULL, "MICVDD" },

	{ "SPDIF out", NULL, "Playback" },
	{ "Capture", NULL, "SPDIF in" },
	{ "SYSCLK", NULL, "OPCLK" },
	{ "ASYNCCLK", NULL, "ASYNCOPCLK" },
};
static int rpi_set_bias_level(struct snd_soc_card *card,
				struct snd_soc_dapm_context *dapm,
				enum snd_soc_bias_level level)
{
	struct snd_soc_codec *wm8804_codec = card->rtd[1].codec;

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		if (dapm->bias_level != SND_SOC_BIAS_OFF)
			break;

		snd_soc_update_bits(wm8804_codec, WM8804_PWRDN, 0x8, 0x0);
		break;
	case SND_SOC_BIAS_PREPARE:
		if (dapm->bias_level != SND_SOC_BIAS_STANDBY)
			break;

		snd_soc_update_bits(wm8804_codec, WM8804_PWRDN, 0x1, 0x0);
		break;
	default:
		break;
	}

	return 0;
}
static int rpi_set_bias_level_post(struct snd_soc_card *card,
		struct snd_soc_dapm_context *dapm,
		enum snd_soc_bias_level level)
{
	struct snd_soc_codec *wm8804_codec = card->rtd[1].codec;

	switch (level) {
	case SND_SOC_BIAS_OFF:
		snd_soc_update_bits(wm8804_codec, WM8804_PWRDN, 0x8, 0x8);
		break;
	case SND_SOC_BIAS_STANDBY:
		snd_soc_update_bits(wm8804_codec, WM8804_PWRDN, 0x1, 0x1);
		break;
	default:
		break;
	}

	dapm->bias_level = level;

	return 0;
}
static void bcm2708_set_gpio_out(int pin)
{
	/*
	 * This is the common way to handle the GPIO pins for
	 * the Raspberry Pi.
	 * TODO This is a hack. Use pinmux / pinctrl.
	 */
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define SET_GPIO_OUT(g) *(gpio+(((g)/10))) |= (1<<(((g)%10)*3))
	unsigned int *gpio;
	gpio = ioremap(GPIO_BASE, SZ_16K);
	INP_GPIO(pin);
	SET_GPIO_OUT(pin);
	iounmap(gpio);
#undef INP_GPIO
#undef SET_GPIO_OUT

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

	if (!gpio_is_valid(GPIO_WM8804_I2C_ADDR)) {
		pr_err("Skipping unavailable gpio %d (%s)\n", GPIO_WM8804_I2C_ADDR, "wm8804_i2c_addr");
		return -ENOMEM;
	}

	ret = gpio_request(GPIO_WM8804_RST, "wm8804_rst");
	if (ret < 0) {
		pr_err("gpio_request wm8804_rst failed\n");
		return ret;
	}

	/*GPIO2 is used for SW/HW Mode Select and after Reset the same pin is used as
	I2C data line, so initially it is configured as GPIO OUT from BCM perspective*/
	bcm2708_set_gpio_out(GPIO_WM8804_MODE);

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

	ret = gpio_request(GPIO_WM8804_I2C_ADDR, "wm8804_i2c_addr");
	if (ret < 0) {
		pr_err("gpio_request wm8804_i2c_addr failed\n");
		return ret;
	}

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
	ret = gpio_direction_output(GPIO_WM8804_I2C_ADDR, 0);
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
	gpio_free(GPIO_WM8804_I2C_ADDR);

	/*GPIO2 is used for SW/HW Mode Select and after Reset the same pin is used as
	I2C data line, so after reset  it is configured as I2C data line i.e ALT0 function*/
	bcm2708_set_gpio_alt(GPIO_WM8804_MODE, 0);

	return ret;
}

static int snd_rpi_wsp_config_5102_clks(struct snd_soc_codec *wm5102_codec, int sr, bool enable_fllsync)
{
	int ret;
	int sr_mult = (sr % 4000 == 0) ? (WM5102_MAX_SYSCLK_1/sr) : (WM5102_MAX_SYSCLK_2/sr);

	/*reset FLL1*/
	snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1_REFCLK,
				ARIZONA_FLL_SRC_NONE, 0, 0);
	snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1,
				ARIZONA_FLL_SRC_NONE, 0, 0);

	if (enable_fllsync) {
		ret = snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1_REFCLK,
					    ARIZONA_CLK_SRC_MCLK1,
					    WM8804_CLKOUT_HZ,
					    sr * sr_mult);
		if (ret != 0) {
			dev_err(wm5102_codec->dev, "Failed to enable FLL1 with Ref Clock Loop: %d\n", ret);
			return ret;
		}

		ret = snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1,
					    ARIZONA_CLK_SRC_AIF2BCLK,
					    sr * 64, sr * sr_mult);
		if (ret != 0) {
			dev_err(wm5102_codec->dev, "Failed to enable FLL1  Sync Clock Loop: %d\n", ret);
			return ret;
		}
	} else {
		ret = snd_soc_codec_set_pll(wm5102_codec, WM5102_FLL1,
						ARIZONA_CLK_SRC_MCLK1,
						WM8804_CLKOUT_HZ,
						sr * sr_mult);
		if (ret != 0) {
			dev_err(wm5102_codec->dev, "Failed to enable FLL1 with Ref Clock Loop: %d\n", ret);
			return ret;
		}
	}

	ret = snd_soc_codec_set_sysclk(wm5102_codec,
			ARIZONA_CLK_SYSCLK,
			ARIZONA_CLK_SRC_FLL1,
			sr * sr_mult,
			SND_SOC_CLOCK_IN);
	if (ret != 0) {
		dev_err(wm5102_codec->dev, "Failed to set AYNCCLK: %d\n", ret);
		return ret;
	}

	ret = snd_soc_codec_set_sysclk(wm5102_codec,
					ARIZONA_CLK_OPCLK, 0,
					sr  * sr_mult,
					SND_SOC_CLOCK_OUT);
	if (ret != 0) {
		dev_err(wm5102_codec->dev, "Failed to set OPCLK: %d\n", ret);
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

static int snd_rpi_wsp_config_clks(struct snd_soc_codec *wm8804_codec,
		struct snd_soc_codec *wm5102_codec, struct snd_soc_dai *wm8804_dai,
		int sr, bool enable_fllsync,int wm8804_rxtx_status)
{
	int ret=0,rx_disabled,tx_disabled;

	rx_disabled = wm8804_rxtx_status & 0x2;
	tx_disabled = wm8804_rxtx_status & 0x4;

	if(!rx_disabled || !tx_disabled){
		ret = snd_rpi_wsp_config_8804_clks(wm8804_codec, wm8804_dai,sr);

		if (ret != 0) {
			dev_err(wm8804_codec->dev, "snd_rpi_wsp_config_8804_clks failed: %d\n", ret);
		return ret;
		}

	}

	ret = snd_rpi_wsp_config_5102_clks(wm5102_codec,  sr, enable_fllsync);
	if (ret != 0) {
		dev_err(wm5102_codec->dev, "snd_rpi_wsp_config_5102_clks failed: %d\n", ret);
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
	int ret, rxtx_status,rx_disabled,capture_stream_opened;
	bool enable_fllsync;
	unsigned int bclkratio;

	bclkratio = 2 * snd_pcm_format_physical_width(params_format(params));
	if  (bcm_i2s_dai->driver->ops->set_bclk_ratio) {
		ret = bcm_i2s_dai->driver->ops->set_bclk_ratio(bcm_i2s_dai, bclkratio);
		if (ret < 0) {
			dev_err(wm5102_codec->dev, "set_bclk_ratio failed: %d\n", ret);
			return ret;
		}
	}

	rxtx_status = snd_soc_read(wm8804_codec, WM8804_PWRDN);
	rx_disabled = rxtx_status & 0x2;

	capture_stream_opened =
		substream->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream_opened;

	if (capture_stream_opened &&  !rx_disabled)
		enable_fllsync = true;
	else
		enable_fllsync = false;


	ret = snd_rpi_wsp_config_clks(wm8804_codec, wm5102_codec,
					  wm8804_codec_dai,
					  params_rate(params),
					  enable_fllsync,
					  rxtx_status);

	priv->wm8804_sr =  params_rate(params);

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
	},
};

static int snd_rpi_wsp_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_codec *codec = card->rtd[0].codec;
	struct wm5102_machine_priv *priv = snd_soc_card_get_drvdata(card);
	int i, ret;

	priv->codec = codec;
	priv->wm8804_sr = RPI_WLF_SR;

	for (i = 0; i < ARRAY_SIZE(snd_rpi_wsp_dai); i++)
		priv->aif[i] = card->rtd[i].codec_dai;

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

	/*Configure SAMPLE_RATE_1 and ASYNC_SAMPLE_RATE_1 by default to
	44.1KHz these values can be changed in runtime by corresponding
	DAI hw_params callback */
	snd_soc_update_bits(card->rtd[0].codec, ARIZONA_SAMPLE_RATE_1,
		ARIZONA_SAMPLE_RATE_1_MASK, 0x0B);
	snd_soc_update_bits(card->rtd[0].codec, ARIZONA_ASYNC_SAMPLE_RATE_1,
		ARIZONA_ASYNC_SAMPLE_RATE_MASK, 0x0B);

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
	int i;
	struct wm5102_machine_priv *wm5102;

	void __iomem *base[2];

	/* request both ioareas */
	for (i = 0; i < ARRAY_SIZE(base); i++) {
		struct resource *mem, *ioarea;
		mem = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!mem) {
			dev_err(&pdev->dev, "%s: Memory resource could not be found\n", __func__);
			return -ENODEV;
		}

		ioarea = devm_request_mem_region(&pdev->dev, mem->start,
					   resource_size(mem),
					   pdev->name);
		if (!ioarea) {
			dev_err(&pdev->dev, "%s: Memory region already claimed\n", __func__);
			return -EBUSY;
		}

		base[i] = devm_ioremap(&pdev->dev, mem->start,
				resource_size(mem));
		if (!base[i]) {
			dev_err(&pdev->dev, "%s: ioremap failed\n", __func__);
			return -ENOMEM;
		}
	}

	wm8804_reset();

	wm5102 = kzalloc(sizeof *wm5102, GFP_KERNEL);
	if (!wm5102)
		return -ENOMEM;

	wm5102->gpio_base = base[0];
	wm5102->gpctl_base = base[1];

	snd_soc_card_set_drvdata(&snd_rpi_wsp, wm5102);

	gpio_gclk_init();
	set_gclk_clock_rate(RPI_WSP_DEFAULT_MCLK2);
	enable_gclk_clock(true);

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
	enable_gclk_clock(false);
	kfree(wm5102);

	return 0;
}

static struct platform_driver snd_rpi_wsp_driver = {
	.driver = {
		.name   = "snd-rpi-wsp",
		.owner  = THIS_MODULE,
	},
	.probe	  = snd_rpi_wsp_probe,
	.remove	 = snd_rpi_wsp_remove,
};

module_platform_driver(snd_rpi_wsp_driver);

MODULE_AUTHOR("Nikesh Oswal");
MODULE_AUTHOR("Liu Xin");
MODULE_DESCRIPTION("ASoC Driver for Raspberry Pi connected to Wolfson sound pi");
MODULE_LICENSE("GPL");
