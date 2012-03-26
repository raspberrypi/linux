/*****************************************************************************
* Copyright 2011 Broadcom Corporation.  All rights reserved.
*
* Unless you and Broadcom execute a separate written software license
* agreement governing use of this software, this software is licensed to you
* under the terms of the GNU General Public License version 2, available at
* http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
*	
* Notwithstanding the above, under no circumstances may you combine this
* software in any way with any other Broadcom software provided under a
* license other than the GPL, without Broadcom's express prior written
* consent.
*****************************************************************************/

#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>

#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/rawmidi.h>
#include <sound/initval.h>
#include <sound/tlv.h>

#include "bcm2835.h"

/* volume maximum and minimum in terms of 0.01dB */
#define CTRL_VOL_MAX 400
#define CTRL_VOL_MIN -10239 /* originally -10240 */


static int snd_bcm2835_ctl_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	audio_info(" ... IN\n");
	if (kcontrol->private_value == PCM_PLAYBACK_VOLUME) {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		uinfo->count = 1;
		uinfo->value.integer.min = CTRL_VOL_MIN;
		uinfo->value.integer.max = CTRL_VOL_MAX;      /* 2303 */
	} else if (kcontrol->private_value == PCM_PLAYBACK_MUTE) {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
		uinfo->count = 1;
		uinfo->value.integer.min = 0;
		uinfo->value.integer.max = 1;
	} else if (kcontrol->private_value == PCM_PLAYBACK_DEVICE) {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		uinfo->count = 1;
		uinfo->value.integer.min = 0;
		uinfo->value.integer.max = AUDIO_DEST_MAX-1;
	}
	audio_info(" ... OUT\n");
	return 0;
}

/* toggles mute on or off depending on the value of nmute, and returns
 * 1 if the mute value was changed, otherwise 0
 */
static int toggle_mute(struct bcm2835_chip *chip, int nmute)
{
	/* if settings are ok, just return 0 */
	if(chip->mute == nmute)
		return 0;

	/* if the sound is muted then we need to unmute */
	if(chip->mute == CTRL_VOL_MUTE)
	{
		chip->volume = chip->old_volume; /* copy the old volume back */
		audio_info("Unmuting, old_volume = %d, volume = %d ...\n", chip->old_volume, chip->volume);
	}
	else /* otherwise we mute */
	{
		chip->old_volume = chip->volume;
		chip->volume = 26214; /* set volume to minimum level AKA mute */
		audio_info("Muting, old_volume = %d, volume = %d ...\n", chip->old_volume, chip->volume);
	}

	chip->mute = nmute;
	return 1;
}

static int snd_bcm2835_ctl_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct bcm2835_chip *chip = snd_kcontrol_chip(kcontrol);

	BUG_ON(!chip && !(chip->avail_substreams & AVAIL_SUBSTREAMS_MASK));

	if (kcontrol->private_value == PCM_PLAYBACK_VOLUME)
		ucontrol->value.integer.value[0] = chip2alsa(chip->volume);
	else if (kcontrol->private_value == PCM_PLAYBACK_MUTE)
		ucontrol->value.integer.value[0] = chip->mute;
	else if (kcontrol->private_value == PCM_PLAYBACK_DEVICE)
		ucontrol->value.integer.value[0] = chip->dest;

	return 0;
}

static int snd_bcm2835_ctl_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct bcm2835_chip *chip = snd_kcontrol_chip(kcontrol);
	int changed = 0;

	if (kcontrol->private_value == PCM_PLAYBACK_VOLUME) {
		audio_info("Volume change attempted.. volume = %d new_volume = %d\n", chip->volume, (int)ucontrol->value.integer.value[0]);
		if (chip->mute == CTRL_VOL_MUTE) {
			/* changed = toggle_mute(chip, CTRL_VOL_UNMUTE); */
			return 1; /* should return 0 to signify no change but the mixer takes this as the opposite sign (no idea why) */
		}
		if (changed
		    || (ucontrol->value.integer.value[0] != chip2alsa(chip->volume))) {

			chip->volume = alsa2chip(ucontrol->value.integer.value[0]);
			changed = 1;
		}

	} else if (kcontrol->private_value == PCM_PLAYBACK_MUTE) {
		/* Now implemented */
		audio_info(" Mute attempted\n");
		changed = toggle_mute(chip, ucontrol->value.integer.value[0]);

	} else if (kcontrol->private_value == PCM_PLAYBACK_DEVICE) {
		if (ucontrol->value.integer.value[0] != chip->dest) {
			chip->dest = ucontrol->value.integer.value[0];
			changed = 1;
		}
	}

	if (changed) {
		if (bcm2835_audio_set_ctls(chip))
			printk(KERN_ERR "Failed to set ALSA controls..\n");
	}

	return changed;
}

static DECLARE_TLV_DB_SCALE(snd_bcm2835_db_scale, CTRL_VOL_MIN, 1, 1);

static struct snd_kcontrol_new snd_bcm2835_ctl[] = {
	{
	 .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	 .name = "PCM Playback Volume",
	 .index = 0,
	 .access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	 .private_value = PCM_PLAYBACK_VOLUME,
	 .info = snd_bcm2835_ctl_info,
	 .get = snd_bcm2835_ctl_get,
	 .put = snd_bcm2835_ctl_put,
	 .count = 1,
	 .tlv = {.p = snd_bcm2835_db_scale}
	},
	{
	 .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	 .name = "PCM Playback Switch",
	 .index = 0,
	 .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	 .private_value = PCM_PLAYBACK_MUTE,
	 .info = snd_bcm2835_ctl_info,
	 .get = snd_bcm2835_ctl_get,
	 .put = snd_bcm2835_ctl_put,
	 .count = 1,
	 },
	{
	 .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	 .name = "PCM Playback Route",
	 .index = 0,
	 .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	 .private_value = PCM_PLAYBACK_DEVICE,
	 .info = snd_bcm2835_ctl_info,
	 .get = snd_bcm2835_ctl_get,
	 .put = snd_bcm2835_ctl_put,
	 .count = 1,
	},
};

int snd_bcm2835_new_ctl(bcm2835_chip_t * chip)
{
	int err;
	unsigned int idx;

	strcpy(chip->card->mixername, "Broadcom Mixer");
	for (idx = 0; idx < ARRAY_SIZE(snd_bcm2835_ctl); idx++) {
		err =
		    snd_ctl_add(chip->card,
				snd_ctl_new1(&snd_bcm2835_ctl[idx], chip));
		if (err < 0)
			return err;
	}
	return 0;
}
