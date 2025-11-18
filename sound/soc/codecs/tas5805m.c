// SPDX-License-Identifier: GPL-2.0
//
// Driver for the TAS5805M Audio Amplifier
//
// Author: Andy Liu <andy-liu@ti.com>
// Author: Daniel Beer <daniel.beer@igorinstitute.com>
// Author: Andriy Malyshenko <andriy@sonocotta.com>
//
// This is based on a driver originally written by Andy Liu at TI and
// posted here:
//
//    https://e2e.ti.com/support/audio-group/audio/f/audio-forum/722027/linux-tas5825m-linux-drivers
//
// It has been simplified a little and reworked for the 5.x ALSA SoC API.

#define DEBUG 1

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>

#include <sound/soc.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include "tas5805m.h"

/* This sequence of register writes must always be sent, prior to the
 * 5ms delay while we wait for the DSP to boot.
 */
static const uint8_t dsp_cfg_preboot[] = {
	REG_PAGE, TAS5805M_REG_PAGE_0, 
	REG_BOOK, TAS5805M_BOOK_CONTROL_PORT, 
	TAS5805M_REG_DEVICE_CTRL_2, TAS5805M_DCTRL2_MODE_HIZ, 
	TAS5805M_REG_RESET_CTRL, TAS5805M_RESET_CONTROL_PORT | TAS5805M_RESET_DSP,
	REG_PAGE, TAS5805M_REG_PAGE_0, 
	REG_PAGE, TAS5805M_REG_PAGE_0, 
	REG_PAGE, TAS5805M_REG_PAGE_0, 
	REG_PAGE, TAS5805M_REG_PAGE_0,
	REG_PAGE, TAS5805M_REG_PAGE_0, 
	REG_BOOK, TAS5805M_BOOK_CONTROL_PORT, 
	TAS5805M_REG_DEVICE_CTRL_2, TAS5805M_DCTRL2_MODE_HIZ,
};

struct tas5805m_priv {
	struct i2c_client		*i2c;
	struct regulator		*pvdd;
	struct gpio_desc		*gpio_pdn_n;

	uint8_t					*dsp_cfg_data;
	int						dsp_cfg_len;

	struct regmap			*regmap;

	int						vol;
	int						gain;
	bool					is_powered;
	bool					is_muted;
	bool					dsp_initialized;

	struct work_struct		work;
	struct mutex			lock;
};

static void tas5805m_decode_faults(struct device *dev, unsigned int chan,
				   unsigned int global1, unsigned int global2,
				   unsigned int ot_warning)
{
	if (chan) {
		if (chan & BIT(0))
			dev_warn(dev, "%s: Right channel over current fault\n", __func__);

		if (chan & BIT(1))
			dev_warn(dev, "%s: Left channel over current fault\n", __func__);

		if (chan & BIT(2))
			dev_warn(dev, "%s: Right channel DC fault\n", __func__);

		if (chan & BIT(3))
			dev_warn(dev, "%s: Left channel DC fault\n", __func__);
	}

	if (global1) {
		if (global1 & BIT(0))
			dev_warn(dev, "%s: PVDD UV fault\n", __func__);

		if (global1 & BIT(1))
			dev_warn(dev, "%s: PVDD OV fault\n", __func__);

		// This fault is often triggered by lack of I2S clock, which is expected
		// during longer pauses (when mute state is triggeered).
		if (global1 & BIT(2))
			dev_dbg(dev, "%s: Clock fault\n", __func__);

		if (global1 & BIT(6))
			dev_warn(dev, "%s: The recent BQ write failed\n", __func__);

		if (global1 & BIT(7))
			dev_warn(dev, "%s: OTP CRC check error\n", __func__);
	}

	if (global2) {
		if (global2 & BIT(0))
			dev_warn(dev, "%s: Over temperature shut down fault\n", __func__);
	}

	if (ot_warning) {
		if (ot_warning & BIT(2))
			dev_warn(dev, "%s: Over temperature warning\n", __func__);
	}
}

static void tas5805m_refresh(struct tas5805m_priv *tas5805m)
{
	unsigned int chan, global1, global2, ot_warning;
	struct regmap *rm = tas5805m->regmap;
	int db_value = 24 - (tas5805m->vol / 2);  /* 0x00=+24dB, each step is 0.5dB */
	int db_gain = -(tas5805m->gain / 2);      /* TAS5805M_AGAIN_MAX=0dB, TAS5805M_AGAIN_MIN=-15.5dB, each step is -0.5dB */

	dev_dbg(&tas5805m->i2c->dev, "%s: is_muted=%d, vol=0x%02x (%ddB), gain=0x%02x (%ddB)\n", 
		__func__, tas5805m->is_muted, tas5805m->vol, db_value, tas5805m->gain, db_gain);

	regmap_write(rm, REG_PAGE, TAS5805M_REG_PAGE_0);
	regmap_write(rm, REG_BOOK, TAS5805M_BOOK_CONTROL_PORT);

	/* Validate fault states */
	regmap_read(rm, TAS5805M_REG_CHAN_FAULT, &chan);
	regmap_read(rm, TAS5805M_REG_GLOBAL_FAULT1, &global1);
	regmap_read(rm, TAS5805M_REG_GLOBAL_FAULT2, &global2);
	regmap_read(rm, TAS5805M_REG_OT_WARNING, &ot_warning);

	tas5805m_decode_faults(&tas5805m->i2c->dev, chan, global1, global2, ot_warning);

	if (chan != 0 || global1 != 0 || global2 != 0 || ot_warning != 0) {
		dev_warn(&tas5805m->i2c->dev, "%s: fault detected: CHAN=0x%02x, GLOBAL1=0x%02x, GLOBAL2=0x%02x, OT_WARNING=0x%02x\n",
			__func__, chan, global1, global2, ot_warning);

		/* Optionally, we could take further action here, such as muting the device */
		dev_dbg(&tas5805m->i2c->dev, "%s: clearing faults\n",
			__func__);
		regmap_write(rm, TAS5805M_REG_FAULT, TAS5805M_ANALOG_FAULT_CLEAR);
	}

	/* Write hardware volume register. Applies to both channels.
	 * Register value 0x00=+24dB, 0x30=0dB, 0xFE=-103dB, 0xFF=Mute
	 */
	dev_dbg(&tas5805m->i2c->dev, "%s: writing volume reg 0x%02x\n",
				__func__, tas5805m->vol);
	regmap_write(rm, TAS5805M_REG_VOL_CTRL, tas5805m->vol);

	/* Write analog gain register
	 * Register value 0=0dB, 31=-15.5dB, 0.5dB steps
	 */
	dev_dbg(&tas5805m->i2c->dev, "%s: writing analog gain reg 0x%02x\n",
				__func__, tas5805m->gain);
	regmap_write(rm, TAS5805M_REG_ANALOG_GAIN, tas5805m->gain);

	/* Set/clear digital soft-mute */
	uint8_t device_state = (tas5805m->is_muted ? TAS5805M_DCTRL2_MUTE : 0) |
			TAS5805M_DCTRL2_MODE_PLAY;
	dev_dbg(&tas5805m->i2c->dev, "%s: writing device state 0x%02x\n",
				__func__, device_state);
	regmap_write(rm, TAS5805M_REG_DEVICE_CTRL_2, device_state);
}

static int tas5805m_vol_info(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;

	/* ALSA range: 0 (min) to 127 (max), 1dB steps */
	uinfo->value.integer.min = TAS5805M_VOLUME_MAX;
	uinfo->value.integer.max = TAS5805M_VOLUME_MIN / 2;
	return 0;
}

static int tas5805m_vol_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct tas5805m_priv *tas5805m =
		snd_soc_component_get_drvdata(component);

	mutex_lock(&tas5805m->lock);
	/* Invert and convert: hardware has 0.5dB steps, ALSA gets 1dB steps */
	ucontrol->value.integer.value[0] = (TAS5805M_VOLUME_MIN - tas5805m->vol) / 2;
	mutex_unlock(&tas5805m->lock);

	return 0;
}

static inline int volume_is_valid(int v)
{
	/* ALSA range: 0 to 127 (1dB steps, hardware 0xFE-0x00 is 254 steps of 0.5dB) */
	return (v >= 0) && (v <= (TAS5805M_VOLUME_MIN / 2));
}

static int tas5805m_vol_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct tas5805m_priv *tas5805m =
		snd_soc_component_get_drvdata(component);
	int alsa_vol = ucontrol->value.integer.value[0];
	int hw_vol;
	int ret = 0;

	dev_dbg(component->dev, "%s: alsa_vol=%d\n", 
		__func__, alsa_vol);

	if (!volume_is_valid(alsa_vol))
		return -EINVAL;

	/* Convert ALSA 1dB steps to hardware 0.5dB steps and invert */
	hw_vol = TAS5805M_VOLUME_MIN - (alsa_vol * 2);

	mutex_lock(&tas5805m->lock);
	if (tas5805m->vol != hw_vol) {
		int db_value = 24 - (hw_vol / 2);  /* Calculate dB: 0x00=+24dB, each step is 0.5dB */
		tas5805m->vol = hw_vol;
		dev_dbg(component->dev, "%s: set vol=%d (hw_reg=0x%02x, %ddB, is_powered=%d)\n",
			__func__, alsa_vol, hw_vol, db_value, tas5805m->is_powered);
		if (tas5805m->is_powered)
			tas5805m_refresh(tas5805m);
		else
			dev_dbg(component->dev, "%s: volume change deferred until power-up\n", 
				__func__);
		ret = 1;
	}
	mutex_unlock(&tas5805m->lock);

	return ret;
}

static int tas5805m_again_info(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = TAS5805M_AGAIN_MAX;
	uinfo->value.integer.max = TAS5805M_AGAIN_MIN;
	return 0;
}

static int tas5805m_again_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct tas5805m_priv *tas5805m =
		snd_soc_component_get_drvdata(component);

	mutex_lock(&tas5805m->lock);
	/* Invert: register TAS5805M_AGAIN_MAX (0dB) -> control 31, register TAS5805M_AGAIN_MIN (-15.5dB) -> control 0 */
	ucontrol->value.integer.value[0] = TAS5805M_AGAIN_MIN - (tas5805m->gain & TAS5805M_AGAIN_MIN);
	dev_dbg(component->dev, "get analog gain control=%ld (reg=0x%02x)\n",
		ucontrol->value.integer.value[0], tas5805m->gain & TAS5805M_AGAIN_MIN);
	mutex_unlock(&tas5805m->lock);

	return 0;
}

static int tas5805m_again_put(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct tas5805m_priv *tas5805m =
		snd_soc_component_get_drvdata(component);
	unsigned int control_value = ucontrol->value.integer.value[0];
	unsigned int reg_value;
	int ret = 0;

	dev_dbg(component->dev, "%s(control_value=%u) entered\n", __func__, control_value);

	if (control_value > TAS5805M_AGAIN_MIN)
		return -EINVAL;

	/* Invert: control 31 (0dB) -> register TAS5805M_AGAIN_MAX, control 0 (-15.5dB) -> register TAS5805M_AGAIN_MIN */
	reg_value = TAS5805M_AGAIN_MIN - control_value;

	mutex_lock(&tas5805m->lock);
	
	if (tas5805m->gain != reg_value) {
		tas5805m->gain = reg_value;
		dev_dbg(component->dev, "%s: set gain control=%u (hw_reg=0x%02x, is_powered=%d)\n",
			__func__, control_value, reg_value, tas5805m->is_powered);
		if (tas5805m->is_powered)
			tas5805m_refresh(tas5805m);
		else
			dev_dbg(component->dev, "%s: gain change deferred until power-up\n",
				__func__);
		ret = 1;
	}

	mutex_unlock(&tas5805m->lock);
	return ret;
}

/* TLV for analog gain control: -15.5dB to 0dB in 0.5dB steps (32 steps, 0-31) */
static const SNDRV_CTL_TLVD_DECLARE_DB_SCALE(tas5805m_again_tlv, -1550, 50, 0);

static const struct snd_kcontrol_new tas5805m_snd_controls[] = {
	{
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Digital Volume",
		.access	= SNDRV_CTL_ELEM_ACCESS_TLV_READ |
			  SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info	= tas5805m_vol_info,
		.get	= tas5805m_vol_get,
		.put	= tas5805m_vol_put,
	},
	{
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Analog Gain",
		.access	= SNDRV_CTL_ELEM_ACCESS_TLV_READ |
			  SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info	= tas5805m_again_info,
		.get	= tas5805m_again_get,
		.put	= tas5805m_again_put,
		.tlv.p	= tas5805m_again_tlv,
	},
};

static void send_cfg(struct regmap *rm,
		     const uint8_t *s, unsigned int len)
{
	unsigned int i;

	pr_debug("%s: len=%u\n", 
		__func__, len);
	for (i = 0; i + 1 < len; i += 2)
		regmap_write(rm, s[i], s[i + 1]);
}

/* The TAS5805M DSP can't be configured until the I2S clock has been
 * present and stable for 5ms, or else it won't boot and we get no
 * sound.
 */
static int tas5805m_trigger(struct snd_pcm_substream *substream, int cmd,
			    struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct tas5805m_priv *tas5805m =
		snd_soc_component_get_drvdata(component);

	dev_dbg(component->dev, "%s: cmd=%d\n", 
		__func__, cmd);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		dev_dbg(component->dev, "%s: clock start\n", __func__);
		schedule_work(&tas5805m->work);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static void do_work(struct work_struct *work)
{
	struct tas5805m_priv *tas5805m =
	       container_of(work, struct tas5805m_priv, work);
	struct regmap *rm = tas5805m->regmap;

	dev_dbg(&tas5805m->i2c->dev, "%s: DSP startup\n", 
		__func__);

	mutex_lock(&tas5805m->lock);
	/* We mustn't issue any I2C transactions until the I2S
	 * clock is stable. Furthermore, we must allow a 5ms
	 * delay after the first set of register writes to
	 * allow the DSP to boot before configuring it.
	 */
	usleep_range(5000, 10000);
	
	/* Only send preboot config once per PDN cycle */
	if (!tas5805m->dsp_initialized) {
		dev_dbg(&tas5805m->i2c->dev, "%s: sending preboot config\n", __func__);
		send_cfg(rm, dsp_cfg_preboot, ARRAY_SIZE(dsp_cfg_preboot));
		if (tas5805m->dsp_cfg_len > 0)
		{
			usleep_range(5000, 15000);
			send_cfg(rm, tas5805m->dsp_cfg_data, tas5805m->dsp_cfg_len);
		}
		tas5805m->dsp_initialized = true;
	} else {
		dev_dbg(&tas5805m->i2c->dev, "%s: DSP already initialized, skipping preboot config\n", __func__);
	}

	tas5805m->is_powered = true;
	tas5805m_refresh(tas5805m);
	mutex_unlock(&tas5805m->lock);
}

static int tas5805m_dac_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct tas5805m_priv *tas5805m =
		snd_soc_component_get_drvdata(component);
	struct regmap *rm = tas5805m->regmap;

	dev_dbg(component->dev, "%s: event=0x%x\n", 
		__func__, event);

	if (event & SND_SOC_DAPM_PRE_PMD) {
		dev_dbg(component->dev, "%s: DSP shutdown\n", __func__);
		cancel_work_sync(&tas5805m->work);

		mutex_lock(&tas5805m->lock);
		if (tas5805m->is_powered) {
			tas5805m->is_powered = false;
			dev_dbg(component->dev, "%s: writing device state 0x%02x\n",
				__func__, TAS5805M_DCTRL2_MODE_HIZ);
			regmap_write(rm, TAS5805M_REG_DEVICE_CTRL_2, TAS5805M_DCTRL2_MODE_HIZ);
		}
		mutex_unlock(&tas5805m->lock);
	}

	return 0;
}

static const struct snd_soc_dapm_route tas5805m_audio_map[] = {
	{ "DAC", NULL, "DAC IN" },
	{ "OUT", NULL, "DAC" },
};

static const struct snd_soc_dapm_widget tas5805m_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("DAC IN", "Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_DAC_E("DAC", NULL, SND_SOC_NOPM, 0, 0,
		tas5805m_dac_event, SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("OUT")
};

static const struct snd_soc_component_driver soc_codec_dev_tas5805m = {
	.controls		= tas5805m_snd_controls,
	.num_controls		= ARRAY_SIZE(tas5805m_snd_controls),
	.dapm_widgets		= tas5805m_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(tas5805m_dapm_widgets),
	.dapm_routes		= tas5805m_audio_map,
	.num_dapm_routes	= ARRAY_SIZE(tas5805m_audio_map),
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static int tas5805m_mute(struct snd_soc_dai *dai, int mute, int direction)
{
	struct snd_soc_component *component = dai->component;
	struct tas5805m_priv *tas5805m =
		snd_soc_component_get_drvdata(component);
		
	mutex_lock(&tas5805m->lock);

	dev_dbg(component->dev, "%s: mute=%d, direction=%d, is_powered=%d\n", 
		__func__, mute, direction, tas5805m->is_powered);

	tas5805m->is_muted = mute;
	if (tas5805m->is_powered)
		tas5805m_refresh(tas5805m);
	else
		dev_dbg(component->dev, "%s: mute change deferred until power-up\n", 
			__func__);
	mutex_unlock(&tas5805m->lock);

	return 0;
}

static const struct snd_soc_dai_ops tas5805m_dai_ops = {
	.trigger		= tas5805m_trigger,
	.mute_stream		= tas5805m_mute,
	.no_capture_mute	= 1,
};

static struct snd_soc_dai_driver tas5805m_dai = {
	.name		= "tas5805m-amplifier",
	.playback	= {
		.stream_name	= "Playback",
		.channels_min	= 2,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_48000,
		.formats	= SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops		= &tas5805m_dai_ops,
};

static const struct regmap_config tas5805m_regmap = {
	.reg_bits	= 8,
	.val_bits	= 8,

	/* We have quite a lot of multi-level bank switching and a
	 * relatively small number of register writes between bank
	 * switches.
	 */
	.cache_type	= REGCACHE_NONE,
};

static int tas5805m_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct regmap *regmap;
	struct tas5805m_priv *tas5805m;
	char filename[128];
	const char *config_name;
	const struct firmware *fw;
	int ret;

	dev_dbg(dev, "%s on %s\n", 
		__func__, dev_name(dev));

	regmap = devm_regmap_init_i2c(i2c, &tas5805m_regmap);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(dev, "%s: unable to allocate register map: %d\n", 
			__func__, ret);
		return ret;
	}

	tas5805m = devm_kzalloc(dev, sizeof(struct tas5805m_priv), GFP_KERNEL);
	if (!tas5805m)
		return -ENOMEM;

	tas5805m->i2c = i2c;
	tas5805m->pvdd = devm_regulator_get(dev, "pvdd");
	if (IS_ERR(tas5805m->pvdd)) {
		dev_err(dev, "%s: failed to get pvdd supply: %ld\n", 
			__func__, PTR_ERR(tas5805m->pvdd));
		return PTR_ERR(tas5805m->pvdd);
	}

	dev_set_drvdata(dev, tas5805m);
	tas5805m->regmap = regmap;
	tas5805m->gpio_pdn_n = devm_gpiod_get(dev, "pdn", GPIOD_OUT_LOW);
	if (IS_ERR(tas5805m->gpio_pdn_n)) {
		dev_err(dev, "%s: error requesting PDN gpio: %ld\n",
			__func__, PTR_ERR(tas5805m->gpio_pdn_n));
		return PTR_ERR(tas5805m->gpio_pdn_n);
	}

	/* This configuration must be generated by PPC3. The file loaded
	 * consists of a sequence of register writes, where bytes at
	 * even indices are register addresses and those at odd indices
	 * are register values.
	 *
	 * The fixed portion of PPC3's output prior to the 5ms delay
	 * should be omitted.
	 *
	 * If the device node does not
	 * provide `ti,dsp-config-name` just warn and continue with an
	 * empty configuration set. If a name is provided, attempt to
	 * load the firmware and fail probe on error.
	 */
	if (device_property_read_string(dev, "ti,dsp-config-name",
					&config_name)) {
		dev_warn(dev, "%s: no ti,dsp-config-name provided; continuing without DSP config\n", 
			__func__);
		config_name = NULL;
	}

	if (config_name) {
		snprintf(filename, sizeof(filename), "tas5805m_dsp_%s.bin",
			 config_name);
		ret = request_firmware(&fw, filename, dev);
		if (ret)
			return ret;

		if ((fw->size < 2) || (fw->size & 1)) {
			dev_err(dev, "%s: firmware is invalid\n", 
				__func__);
			release_firmware(fw);
			return -EINVAL;
		}

		tas5805m->dsp_cfg_len = fw->size;
		tas5805m->dsp_cfg_data = devm_kmemdup(dev, fw->data, fw->size, GFP_KERNEL);
		if (!tas5805m->dsp_cfg_data) {
			release_firmware(fw);
			return -ENOMEM;
		}

		release_firmware(fw);
	} else {
		/* No config provided: initialize empty configset */
		tas5805m->dsp_cfg_len = 0;
		tas5805m->dsp_cfg_data = NULL;
	}

	/* Do the first part of the power-on here, while we can expect
	 * the I2S interface to be quiet. We must raise PDN# and then
	 * wait 5ms before any I2S clock is sent, or else the internal
	 * regulator apparently won't come on.
	 *
	 * Also, we must keep the device in power down for 100ms or so
	 * after PVDD is applied, or else the ADR pin is sampled
	 * incorrectly and the device comes up with an unpredictable I2C
	 * address.
	 */
	tas5805m->vol = TAS5805M_VOLUME_ZERO_DB;
	tas5805m->gain = TAS5805M_AGAIN_MAX; /* 0dB analog gain */

	ret = regulator_enable(tas5805m->pvdd);
	if (ret < 0) {
		dev_err(dev, "%s: failed to enable pvdd: %d\n", 
			__func__, ret);
		return ret;
	}

	usleep_range(100000, 150000);
	gpiod_set_value(tas5805m->gpio_pdn_n, 1);
	usleep_range(10000, 15000);

	INIT_WORK(&tas5805m->work, do_work);
	mutex_init(&tas5805m->lock);

	/* Don't register through devm. We need to be able to unregister
	 * the component prior to deasserting PDN#
	 */
	ret = snd_soc_register_component(dev, &soc_codec_dev_tas5805m,
					 &tas5805m_dai, 1);
	if (ret < 0) {
		dev_err(dev, "%s: unable to register codec: %d\n", 
			__func__, ret);
		gpiod_set_value(tas5805m->gpio_pdn_n, 0);
		regulator_disable(tas5805m->pvdd);
		return ret;
	}

	return 0;
}

static void tas5805m_i2c_remove(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct tas5805m_priv *tas5805m = dev_get_drvdata(dev);

	dev_dbg(dev, "%s on %s\n", 
		__func__, dev_name(dev));

	cancel_work_sync(&tas5805m->work);
	snd_soc_unregister_component(dev);
	mutex_lock(&tas5805m->lock);
	tas5805m->dsp_initialized = false;
	mutex_unlock(&tas5805m->lock);
	gpiod_set_value(tas5805m->gpio_pdn_n, 0);
	usleep_range(10000, 15000);
	regulator_disable(tas5805m->pvdd);
}

static const struct i2c_device_id tas5805m_i2c_id[] = {
	{ "tas5805m", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tas5805m_i2c_id);

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id tas5805m_of_match[] = {
	{ .compatible = "ti,tas5805m", },
	{ }
};
MODULE_DEVICE_TABLE(of, tas5805m_of_match);
#endif

static struct i2c_driver tas5805m_i2c_driver = {
	.probe		= tas5805m_i2c_probe,
	.remove		= tas5805m_i2c_remove,
	.id_table	= tas5805m_i2c_id,
	.driver		= {
		.name		= "tas5805m",
		.of_match_table = of_match_ptr(tas5805m_of_match),
	},
};

module_i2c_driver(tas5805m_i2c_driver);

MODULE_AUTHOR("Andy Liu <andy-liu@ti.com>");
MODULE_AUTHOR("Daniel Beer <daniel.beer@igorinstitute.com>");
MODULE_AUTHOR("Andriy Malyshenko <andriy@sonocotta.com>");
MODULE_DESCRIPTION("TAS5805M Audio Amplifier Driver");
MODULE_LICENSE("GPL v2");
