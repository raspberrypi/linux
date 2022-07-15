// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ASoC Driver for Infineon Merus(TM) MA120x0p multi-level class-D amplifier
 *
 * Authors:	Ariel Muszkat <ariel.muszkat@gmail.com>
 *			Jorgen Kragh Jakobsen <jorgen.kraghjakobsen@infineon.com>
 *			Nicolai Dyre Bülow <nixen.dyre@gmail.com>			
 *
 * Copyright (C) 2022 Infineon Technologies AG
 *
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/i2c.h>
#include <linux/of_device.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <linux/interrupt.h>

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include "ma120x0p.h"

#define SOC_ENUM_ERR(xname, xenum)\
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.access = SNDRV_CTL_ELEM_ACCESS_READ,\
	.info = snd_soc_info_enum_double,\
	.get = snd_soc_get_enum_double, .put = snd_soc_put_enum_double,\
	.private_value = (unsigned long)&(xenum) }

static struct i2c_client *i2c;

struct ma120x0p_priv {
	struct regmap *regmap;
	int mclk_div;
	struct snd_soc_component *component;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *mute_gpio;
	struct gpio_desc *booster_gpio;
	struct gpio_desc *error_gpio;
};

static struct ma120x0p_priv *priv_data;

// Used to share the IRQ number within this file
static unsigned int irqNumber;

// Function prototype for the custom IRQ handler function
static irqreturn_t ma120x0p_irq_handler(int irq, void *data);

/*
 *    _   _    ___   _      ___         _           _
 *   /_\ | |  / __| /_\    / __|___ _ _| |_ _ _ ___| |___
 *  / _ \| |__\__ \/ _ \  | (__/ _ \ ' \  _| '_/ _ \ (_-<
 * /_/ \_\____|___/_/ \_\  \___\___/_||_\__|_| \___/_/__/
 *
 */

static const char * const limenable_text[] = {"Bypassed", "Enabled"};
static const char * const limatack_text[] = {"Slow", "Normal", "Fast"};
static const char * const limrelease_text[] = {"Slow", "Normal", "Fast"};
//static const char * const audioproc_mute_text[] = {"Play", "Mute"};

static const char * const err_flycap_text[] = {"Ok", "Error"};
static const char * const err_overcurr_text[] = {"Ok", "Error"};
static const char * const err_pllerr_text[] = {"Ok", "Error"};
static const char * const err_pvddunder_text[] = {"Ok", "Error"};
static const char * const err_overtempw_text[] = {"Ok", "Error"};
static const char * const err_overtempe_text[] = {"Ok", "Error"};
static const char * const err_pinlowimp_text[] = {"Ok", "Error"};
static const char * const err_dcprot_text[] = {"Ok", "Error"};

static const char * const pwr_mode_prof_text[] = {"PMF0", "PMF1", "PMF2",
"PMF3", "PMF4"};

static const struct soc_enum lim_enable_ctrl =
	SOC_ENUM_SINGLE(ma_audio_proc_limiterenable__a,
		ma_audio_proc_limiterenable__shift,
		ma_audio_proc_limiterenable__len + 1,
		limenable_text);
static const struct soc_enum limatack_ctrl =
	SOC_ENUM_SINGLE(ma_audio_proc_attack__a,
		ma_audio_proc_attack__shift,
		ma_audio_proc_attack__len + 1,
		limatack_text);
static const struct soc_enum limrelease_ctrl =
	SOC_ENUM_SINGLE(ma_audio_proc_release__a,
		ma_audio_proc_release__shift,
		ma_audio_proc_release__len + 1,
		limrelease_text);
static const struct soc_enum err_flycap_ctrl =
	SOC_ENUM_SINGLE(ma_error__a, 0, 3, err_flycap_text);
static const struct soc_enum err_overcurr_ctrl =
	SOC_ENUM_SINGLE(ma_error__a, 1, 3, err_overcurr_text);
static const struct soc_enum err_pllerr_ctrl =
	SOC_ENUM_SINGLE(ma_error__a, 2, 3, err_pllerr_text);
static const struct soc_enum err_pvddunder_ctrl =
	SOC_ENUM_SINGLE(ma_error__a, 3, 3, err_pvddunder_text);
static const struct soc_enum err_overtempw_ctrl =
	SOC_ENUM_SINGLE(ma_error__a, 4, 3, err_overtempw_text);
static const struct soc_enum err_overtempe_ctrl =
	SOC_ENUM_SINGLE(ma_error__a, 5, 3, err_overtempe_text);
static const struct soc_enum err_pinlowimp_ctrl =
	SOC_ENUM_SINGLE(ma_error__a, 6, 3, err_pinlowimp_text);
static const struct soc_enum err_dcprot_ctrl =
	SOC_ENUM_SINGLE(ma_error__a, 7, 3, err_dcprot_text);
static const struct soc_enum pwr_mode_prof_ctrl =
	SOC_ENUM_SINGLE(ma_pmprofile__a, ma_pmprofile__shift, 5,
		pwr_mode_prof_text);

static const char * const pwr_mode_texts[] = {
		"Dynamic power mode",
		"Power mode 1",
		"Power mode 2",
		"Power mode 3",
	};

static const int pwr_mode_values[] = {
		0x10,
		0x50,
		0x60,
		0x70,
	};

static const SOC_VALUE_ENUM_SINGLE_DECL(pwr_mode_ctrl,
	ma_pm_man__a, 0, 0x70,
	pwr_mode_texts,
	pwr_mode_values);

static const DECLARE_TLV_DB_SCALE(ma120x0p_vol_tlv, -5000, 100,  0);
static const DECLARE_TLV_DB_SCALE(ma120x0p_lim_tlv, -5000, 100,  0);
static const DECLARE_TLV_DB_SCALE(ma120x0p_lr_tlv, -5000, 100,  0);

static const struct snd_kcontrol_new ma120x0p_snd_controls[] = {
	//Master Volume
	SOC_SINGLE_RANGE_TLV("A.Mstr Vol Volume",
		ma_vol_db_master__a, 0, 0x18, 0x4a, 1, ma120x0p_vol_tlv),
	//L-R Volume ch0
	SOC_SINGLE_RANGE_TLV("B.L Vol Volume",
		ma_vol_db_ch0__a, 0, 0x18, 0x4a, 1, ma120x0p_lr_tlv),
	SOC_SINGLE_RANGE_TLV("C.R Vol Volume",
		ma_vol_db_ch1__a, 0, 0x18, 0x4a, 1, ma120x0p_lr_tlv),

	//L-R Limiter Threshold ch0-ch1
	SOC_DOUBLE_R_RANGE_TLV("D.Lim thresh Volume",
		ma_thr_db_ch0__a, ma_thr_db_ch1__a, 0, 0x0e, 0x4a, 1, ma120x0p_lim_tlv),

	//Enum Switches/Selectors
	//SOC_ENUM("E.AudioProc Mute", audioproc_mute_ctrl),
	SOC_ENUM("F.Limiter Enable", lim_enable_ctrl),
	SOC_ENUM("G.Limiter Attck", limatack_ctrl),
	SOC_ENUM("H.Limiter Rls", limrelease_ctrl),

	//Enum Error Monitor (read-only)
	SOC_ENUM_ERR("I.Err flycap", err_flycap_ctrl),
	SOC_ENUM_ERR("J.Err overcurr", err_overcurr_ctrl),
	SOC_ENUM_ERR("K.Err pllerr", err_pllerr_ctrl),
	SOC_ENUM_ERR("L.Err pvddunder", err_pvddunder_ctrl),
	SOC_ENUM_ERR("M.Err overtempw", err_overtempw_ctrl),
	SOC_ENUM_ERR("N.Err overtempe", err_overtempe_ctrl),
	SOC_ENUM_ERR("O.Err pinlowimp", err_pinlowimp_ctrl),
	SOC_ENUM_ERR("P.Err dcprot", err_dcprot_ctrl),

	//Power modes profiles
	SOC_ENUM("Q.PM Prof", pwr_mode_prof_ctrl),

	// Power mode selection (Dynamic,1,2,3)
	SOC_ENUM("R.Power Mode", pwr_mode_ctrl),
};

/*
 *  __  __         _    _            ___      _
 * |  \/  |__ _ __| |_ (_)_ _  ___  |   \ _ _(_)_ _____ _ _
 * | |\/| / _` / _| ' \| | ' \/ -_) | |) | '_| \ V / -_) '_|
 * |_|  |_\__,_\__|_||_|_|_||_\___| |___/|_| |_|\_/\___|_|
 *
 */

static int ma120x0p_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	u16 blen = 0x00;

	struct snd_soc_component *component = dai->component;

	priv_data->component = component;

	switch (params_format(params)) {
		case SNDRV_PCM_FORMAT_S16_LE:
			// Technically supported, but requires the BCLK to be locked at 64 x FS, effectively padding each word with an extra 16 zeros.
			blen = 0x16;
			// dev_info(dai->dev, "Configuring FMT to 16-bit \n");
			break;
		case SNDRV_PCM_FORMAT_S24_LE:
			// In the case of 24-bit, we just let the MA interpret it as though it's 32-bit.
			blen = 0x00;
			// dev_info(dai->dev, "Configuring FMT to 24-bit \n");
			break;
		case SNDRV_PCM_FORMAT_S24_3LE:
			// This format is not supported by the Raspberry PI! - But is supported by the amplifier. 
			blen = 0x08;
			// dev_info(dai->dev, "Configuring FMT to 24/3-bit \n");		
			break;		
		case SNDRV_PCM_FORMAT_S32_LE:
			// Preferred by the amplifier. In 32-bit format, no hardcoded BCLK ratio is technically, with this format, needed.
			blen = 0x00;
			// dev_info(dai->dev, "Configuring FMT to 32-bit \n");		
			break;
		default:
			dev_err(dai->dev, "Unsupported word length: %u\n", params_format(params));
			return -EINVAL;
	}

	/*
	Good read: 
	https://alsa-devel.alsa-project.narkive.com/aGiYbNu8/what-is-the-difference-
	between-sndrv-pcm-fmtbit-s24-le-and-sndrv-pcm-fmtbit-s24-3le
	*/

	// Set word length
	snd_soc_component_update_bits(component, ma_i2s_framesize__a,
		ma_i2s_framesize__mask, blen);

	return 0;
}

static int ma120x0p_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	int val = 0;

	struct ma120x0p_priv *ma120x0p;

	struct snd_soc_component *component = dai->component;

	ma120x0p = snd_soc_component_get_drvdata(component);

	if (mute)
		val = 0;
	else
		val = 1;

	gpiod_set_value_cansleep(priv_data->mute_gpio, val);

	return 0;
}

static const struct snd_soc_dai_ops ma120x0p_dai_ops = {
	.hw_params		=	ma120x0p_hw_params,
	.mute_stream	=	ma120x0p_mute_stream,
};

static struct snd_soc_dai_driver ma120x0p_dai = {
	.name		= "ma120x0p-amp",
	.playback	=	{
		.stream_name	= "Playback",
		.channels_min	= 2,
		.channels_max	= 2,
		.rates = SNDRV_PCM_RATE_CONTINUOUS,
		.rate_min = 44100,
		.rate_max = 48000,
		.formats = SNDRV_PCM_FMTBIT_S32_LE // | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_3LE

		/* Notes on format and sample rate:
		Currently only 32_LE works properly with the raspberry pi. This does, however, stll allow for 16 and 24 bit playback. 
		If the above is outcommented, the amplifier will be correctly configured for 16 and 24 bit accordingly. (But don't on a raspberry pi.) 
		
		Although upto 192KHz fs is supported by the amplifier - a master clock signal is required before enabling the amplifier, for it to work
		properly at higher (than 48KHz) sample rates. Unfortunately the raspberry pi I2S driver disables the I2S clocks whenever it's not playing audio, and 
		as a result, the amplifier will not play audio, as it looses it's master clock. So either we'd want to driver reboot and reconfigure (probe) the 
		amplifier whenever an audio stream is played, or we somehow have the I2S driver always output its BCLK (which doesn't seem doable). 
		*/
	},
	.ops        = &ma120x0p_dai_ops,
};

/*
 *   ___         _          ___      _
 *  / __|___  __| |___ __  |   \ _ _(_)_ _____ _ _
 * | (__/ _ \/ _` / -_) _| | |) | '_| \ V / -_) '_|
 *  \___\___/\__,_\___\__| |___/|_| |_|\_/\___|_|
 *
 */
static int ma120x0p_clear_err(struct snd_soc_component *component)
{
	int ret = 0;

	struct ma120x0p_priv *ma120x0p;

	ma120x0p = snd_soc_component_get_drvdata(component);

	ret = snd_soc_component_update_bits(component,
		ma_eh_clear__a, ma_eh_clear__mask, 0x00);
	if (ret < 0)
		return ret;

	ret = snd_soc_component_update_bits(component,
		ma_eh_clear__a, ma_eh_clear__mask, 0x04);
	if (ret < 0)
		return ret;

	ret = snd_soc_component_update_bits(component,
		ma_eh_clear__a, ma_eh_clear__mask, 0x00);
	if (ret < 0)
		return ret;

	return 0;
}

static void ma120x0p_remove(struct snd_soc_component *component)
{
	struct ma120x0p_priv *ma120x0p;

	ma120x0p = snd_soc_component_get_drvdata(component);
}

static int ma120x0p_probe(struct snd_soc_component *component)
{
	struct ma120x0p_priv *ma120x0p;

	int ret = 0;

	i2c = container_of(component->dev, struct i2c_client, dev);

	ma120x0p = snd_soc_component_get_drvdata(component);

	//Reset error
	ma120x0p_clear_err(component);
	if (ret < 0)
		return ret;

	// set serial audio format I2S and enable audio processor
	ret = snd_soc_component_write(component, ma_i2s_format__a, 0x08);
	if (ret < 0)
		return ret;

	// Enable audio limiter
	ret = snd_soc_component_update_bits(component,
		ma_audio_proc_limiterenable__a, ma_audio_proc_limiterenable__mask, 0x40);
	if (ret < 0)
		return ret;

	// Set lim attack to fast
	ret = snd_soc_component_update_bits(component,
		ma_audio_proc_attack__a,ma_audio_proc_attack__mask, 0x80);
	if (ret < 0)
		return ret;

	// Set lim attack to low
	ret = snd_soc_component_update_bits(component,
		ma_audio_proc_release__a, ma_audio_proc_release__mask, 0x00);
	if (ret < 0)
		return ret;

	// set volume to 0dB
	ret = snd_soc_component_write(component, ma_vol_db_master__a, 0x18);
	if (ret < 0)
		return ret;

	// set ch0 lim thresh to -15dB
	ret = snd_soc_component_write(component, ma_thr_db_ch0__a, 0x27);
	if (ret < 0)
		return ret;

	// set ch1 lim thresh to -15dB
	ret = snd_soc_component_write(component, ma_thr_db_ch1__a, 0x27);
	if (ret < 0)
		return ret;

	//Check for errors
	ret = snd_soc_component_test_bits(component, ma_error_acc__a, 0x00, 0);
	if (ret < 0)
		return ret;
	ret = snd_soc_component_test_bits(component, ma_error_acc__a, 0x01, 0);
	if (ret < 0)
		return ret;
	ret = snd_soc_component_test_bits(component, ma_error_acc__a, 0x02, 0);
	if (ret < 0)
		return ret;
	ret = snd_soc_component_test_bits(component, ma_error_acc__a, 0x08, 0);
	if (ret < 0)
		return ret;
	ret = snd_soc_component_test_bits(component, ma_error_acc__a, 0x10, 0);
	if (ret < 0)
		return ret;
	ret = snd_soc_component_test_bits(component, ma_error_acc__a, 0x20, 0);
	if (ret < 0)
		return ret;
	ret = snd_soc_component_test_bits(component, ma_error_acc__a, 0x40, 0);
	if (ret < 0)
		return ret;
	ret = snd_soc_component_test_bits(component, ma_error_acc__a, 0x80, 0);
	if (ret < 0)
		return ret;

	return 0;
}

static int ma120x0p_set_bias_level(struct snd_soc_component *component,	enum snd_soc_bias_level level)
{
	int ret = 0;

	struct ma120x0p_priv *ma120x0p;

	ma120x0p = snd_soc_component_get_drvdata(component);

	switch (level) {
	case SND_SOC_BIAS_ON:
		break;

	case SND_SOC_BIAS_PREPARE:
		break;

	case SND_SOC_BIAS_STANDBY:
		ret = gpiod_get_value_cansleep(priv_data->enable_gpio);
		if (ret != 0) {
			dev_err(component->dev, "Device ma120x0p disabled in STANDBY BIAS: %d\n",
			ret);
			return ret;
		}
		break;

	case SND_SOC_BIAS_OFF:
		break;
	}

	return 0;
}

static const struct snd_soc_dapm_widget ma120x0p_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("OUT_A"),
	SND_SOC_DAPM_OUTPUT("OUT_B"),
};

static const struct snd_soc_dapm_route ma120x0p_dapm_routes[] = {
	{ "OUT_B",  NULL, "Playback" },
	{ "OUT_A",  NULL, "Playback" },
};

static const struct snd_soc_component_driver ma120x0p_component_driver = {
	.probe = ma120x0p_probe,
	.remove = ma120x0p_remove,
	.set_bias_level = ma120x0p_set_bias_level,
	.dapm_widgets		= ma120x0p_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(ma120x0p_dapm_widgets),
	.dapm_routes		= ma120x0p_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(ma120x0p_dapm_routes),
	.controls = ma120x0p_snd_controls,
	.num_controls = ARRAY_SIZE(ma120x0p_snd_controls),
	.use_pmdown_time	= 1,
	.endianness		= 1,
	.non_legacy_dai_naming	= 1,
};

/*
 *   ___ ___ ___   ___      _
 *  |_ _|_  ) __| |   \ _ _(_)_ _____ _ _
 *   | | / / (__  | |) | '_| \ V / -_) '_|
 *  |___/___\___| |___/|_| |_|\_/\___|_|
 *
 */

static const struct reg_default ma120x0p_reg_defaults[] = {
	{	0x01,	0x3c	},
};

static bool ma120x0p_reg_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ma_error__a:
	case ma_error_acc__a:
			return true;
	default:
			return false;
	}
}

static const struct of_device_id ma120x0p_of_match[] = {
	{ .compatible = "ma,ma120x0p", },
	{ }
};

MODULE_DEVICE_TABLE(of, ma120x0p_of_match);

static struct regmap_config ma120x0p_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = 255,
	.volatile_reg = ma120x0p_reg_volatile,

	.cache_type = REGCACHE_RBTREE,
	.reg_defaults = ma120x0p_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(ma120x0p_reg_defaults),
};

static irqreturn_t ma120x0p_exception_handler(int irq, void *data)
{
        struct snd_soc_component *component;

        int ret;

        component = priv_data->component;
        ret = snd_soc_component_read(component, ma_error_acc__a);

	if (ret != 0){
		if (ret & 0x01){dev_info(component->dev, "Flying Capacitor Overvoltage Error\n");}
		if (ret & 0x02){dev_info(component->dev, "Over Current Protection Error\n");}
		if (ret & 0x04){dev_info(component->dev, "Amplifier PLL Error\n");}
		if (ret & 0x08){dev_info(component->dev, "Over temperature Warning\n");}
		if (ret & 0x10){dev_info(component->dev, "Over Temperature Error\n");}
		if (ret & 0x20){dev_info(component->dev, "Pin to Pin low impedance");}
		if (ret & 0x40){dev_info(component->dev, "DC Protection \n");}

		ma120x0p_clear_err(component);
		dev_info(component->dev, "Error Register Attemped Cleared \n");

		/* 
		If an error occurs, the error will be stated in dmesg, after which the error register
		in the amplifier will be cleared, and the amplifier will continue. 
		One could here implement some more protection/mittigation, however, the amplifier is already 
		protected from the most catastrophic failures in hardware.
		*/
	}
	
    return IRQ_HANDLED;
}

static int ma120x0p_i2c_probe(struct i2c_client *i2c,
	const struct i2c_device_id *id)
{
	printk(KERN_INFO "Start Merus I2C Probe init");

	int ret;

	priv_data = devm_kzalloc(&i2c->dev, sizeof(*priv_data), GFP_KERNEL);
	if (!priv_data)
		return -ENOMEM;
	i2c_set_clientdata(i2c, priv_data);

	priv_data->regmap = devm_regmap_init_i2c(i2c, &ma120x0p_regmap_config);
	if (IS_ERR(priv_data->regmap)) {
		ret = PTR_ERR(priv_data->regmap);
		return ret;
	}

	// Startup sequence:
	// Make sure the device is muted
	priv_data->mute_gpio = devm_gpiod_get(&i2c->dev, 
		"mute_gp",GPIOD_OUT_LOW);
	if (IS_ERR(priv_data->mute_gpio)) {
		ret = PTR_ERR(priv_data->mute_gpio);
		dev_err(&i2c->dev, "Failed to get mute gpio line: %d\n", ret);
		return ret;
	}
	msleep(50);

	// MA120xx0P devices can be powered by an integrated boost converter.
	// An option GPIO control line is provided to enable a booster properly and
	// in sync with the enable and mute GPIO lines.
	priv_data->booster_gpio = devm_gpiod_get_optional(&i2c->dev,
		"booster_gp", GPIOD_OUT_LOW);
	if (IS_ERR(priv_data->booster_gpio)) {
		ret = PTR_ERR(priv_data->booster_gpio);
		dev_err(&i2c->dev, "Failed to get booster enable gpio line: %d\n", ret);
		return ret;
	}
	msleep(50);

	// Enable booster and wait 200ms until PVDD stablises. 
	gpiod_set_value_cansleep(priv_data->booster_gpio, 1);
	msleep(200);

	printk(KERN_INFO "Boost Converter enabled");


	// Enable MA120x0p
	priv_data->enable_gpio = devm_gpiod_get(&i2c->dev, "enable_gp",GPIOD_OUT_LOW);
	if (IS_ERR(priv_data->enable_gpio)) {
		ret = PTR_ERR(priv_data->enable_gpio);
		dev_err(&i2c->dev, "Failed to get ma120x0p enable gpio line: %d\n", ret);
		return ret;
	}
	msleep(50);

	// Optional use of MA120x0p error line as an interrupt trigger to
	// platform GPIO.
	// Get error input gpio MA120x0p and register it as interrupt, with appropriate callback.
	priv_data->error_gpio = devm_gpiod_get_optional(&i2c->dev,
		 "error_gp", GPIOD_IN);
	if (IS_ERR(priv_data->error_gpio)) {
		ret = PTR_ERR(priv_data->error_gpio);
		dev_err(&i2c->dev, "Failed to get ma120x0p error gpio line: %d\n", ret);
		return ret;
	}

	printk(KERN_INFO "Registering Error interrupt");

	if (priv_data->error_gpio != NULL) {
		irqNumber = gpiod_to_irq(priv_data->error_gpio);
	   	printk(KERN_INFO "GPIO: The button is mapped to IRQ: %d\n",
		 				irqNumber);

		 ret = devm_request_threaded_irq(&i2c->dev,
			irqNumber, ma120x0p_irq_handler,
			ma120x0p_exception_handler, IRQF_TRIGGER_FALLING,
			"ma120x0p", priv_data);

			if (ret != 0) {
				dev_warn(&i2c->dev, "Failed to request IRQ: %d\n", ret);
			} else {
				printk(KERN_INFO "GPIO_TEST: The interrupt request result is: %d\n",
				ret);
			}
	}
	ret = devm_snd_soc_register_component(&i2c->dev,
		&ma120x0p_component_driver, &ma120x0p_dai, 1);

	return ret;
}

static irqreturn_t ma120x0p_irq_handler(int irq, void *data)
{
	// gpiod_set_value_cansleep(priv_data->mute_gpio, 0);
	// gpiod_set_value_cansleep(priv_data->enable_gpio, 1);

	// ^ If commented in, the amplifier with mute and disable in case of any error!
	return IRQ_WAKE_THREAD;
}

static int ma120x0p_i2c_remove(struct i2c_client *i2c)
{
	snd_soc_unregister_component(&i2c->dev);
	i2c_set_clientdata(i2c, NULL);

	gpiod_set_value_cansleep(priv_data->mute_gpio, 0);
	msleep(30);
	gpiod_set_value_cansleep(priv_data->enable_gpio, 1);
	msleep(200);
	gpiod_set_value_cansleep(priv_data->booster_gpio, 0);
	msleep(200);

	kfree(priv_data);

	return 0;
}

static void ma120x0p_i2c_shutdown(struct i2c_client *i2c)
{
	snd_soc_unregister_component(&i2c->dev);
	i2c_set_clientdata(i2c, NULL);

	gpiod_set_value_cansleep(priv_data->mute_gpio, 0);
	msleep(30);
	gpiod_set_value_cansleep(priv_data->enable_gpio, 1);
	msleep(200);
	gpiod_set_value_cansleep(priv_data->booster_gpio, 0);
	msleep(200);

	kfree(priv_data);
}

static const struct i2c_device_id ma120x0p_i2c_id[] = {
	{ "ma120x0p", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, ma120x0p_i2c_id);

static struct i2c_driver ma120x0p_i2c_driver = {
	.driver = {
		.name = "ma120x0p",
		.owner = THIS_MODULE,
		.of_match_table = ma120x0p_of_match,
	},
	.probe = ma120x0p_i2c_probe,
	.remove = ma120x0p_i2c_remove,
	.shutdown = ma120x0p_i2c_shutdown,
	.id_table = ma120x0p_i2c_id
};

static int __init ma120x0p_modinit(void)
{
	int ret = 0;

	ret = i2c_add_driver(&ma120x0p_i2c_driver);
	if (ret) {
		printk(KERN_ERR "Failed to register ma120x0p I2C driver: %d\n", ret);
	}

	return ret;
}

module_init(ma120x0p_modinit);

static void __exit ma120x0p_exit(void)
{
	i2c_del_driver(&ma120x0p_i2c_driver);
}
module_exit(ma120x0p_exit);

MODULE_AUTHOR("Ariel Muszkat ariel.muszkat@gmail.com>");
MODULE_DESCRIPTION("ASoC driver for ma120x0p");
MODULE_LICENSE("GPL v2");
