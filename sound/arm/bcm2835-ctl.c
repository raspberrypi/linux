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
#include <sound/asoundef.h>

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

static int snd_bcm2835_spdif_default_info(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_bcm2835_spdif_default_get(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	struct bcm2835_chip *chip = snd_kcontrol_chip(kcontrol);
	int i;

	for (i = 0; i < 4; i++)
		ucontrol->value.iec958.status[i] =
			(chip->spdif_status >> (i * 8)) && 0xff;

	return 0;
}

static int snd_bcm2835_spdif_default_put(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	struct bcm2835_chip *chip = snd_kcontrol_chip(kcontrol);
	unsigned int val = 0;
	int i, change;

	for (i = 0; i < 4; i++)
		val |= (unsigned int)ucontrol->value.iec958.status[i] << (i * 8);

	change = val != chip->spdif_status;
	chip->spdif_status = val;

	return change;
}

static int snd_bcm2835_spdif_mask_info(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_bcm2835_spdif_mask_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	/* bcm2835 supports only consumer mode and sets all other format flags
	 * automatically. So the only thing left is signalling non-audio
	 * content */
	ucontrol->value.iec958.status[0] = IEC958_AES0_NONAUDIO;
	return 0;
}

static int snd_bcm2835_spdif_stream_info(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int snd_bcm2835_spdif_stream_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct bcm2835_chip *chip = snd_kcontrol_chip(kcontrol);
	int i;

	for (i = 0; i < 4; i++)
		ucontrol->value.iec958.status[i] =
			(chip->spdif_status >> (i * 8)) & 0xff;
	return 0;
}

static int snd_bcm2835_spdif_stream_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct bcm2835_chip *chip = snd_kcontrol_chip(kcontrol);
	unsigned int val = 0;
	int i, change;

	for (i = 0; i < 4; i++)
		val |= (unsigned int)ucontrol->value.iec958.status[i] << (i * 8);
	change = val != chip->spdif_status;
	chip->spdif_status = val;

	return change;
}

static struct snd_kcontrol_new snd_bcm2835_spdif[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_PCM,
		.name = SNDRV_CTL_NAME_IEC958("", PLAYBACK, DEFAULT),
		.info = snd_bcm2835_spdif_default_info,
		.get = snd_bcm2835_spdif_default_get,
		.put = snd_bcm2835_spdif_default_put
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface = SNDRV_CTL_ELEM_IFACE_PCM,
		.name = SNDRV_CTL_NAME_IEC958("", PLAYBACK, CON_MASK),
		.info = snd_bcm2835_spdif_mask_info,
		.get = snd_bcm2835_spdif_mask_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			SNDRV_CTL_ELEM_ACCESS_INACTIVE,
		.iface = SNDRV_CTL_ELEM_IFACE_PCM,
		.name = SNDRV_CTL_NAME_IEC958("", PLAYBACK, PCM_STREAM),
		.info = snd_bcm2835_spdif_stream_info,
		.get = snd_bcm2835_spdif_stream_get,
		.put = snd_bcm2835_spdif_stream_put,
	},
};

struct cea_channel_speaker_allocation {
	int ca_index;
	int speakers[8];
};

#define FL	SNDRV_CHMAP_FL
#define FR	SNDRV_CHMAP_FR
#define RL	SNDRV_CHMAP_RL
#define RR	SNDRV_CHMAP_RR
#define LFE	SNDRV_CHMAP_LFE
#define FC	SNDRV_CHMAP_FC
#define RLC	SNDRV_CHMAP_RLC
#define RRC	SNDRV_CHMAP_RRC
#define RC	SNDRV_CHMAP_RC
#define FLC	SNDRV_CHMAP_FLC
#define FRC	SNDRV_CHMAP_FRC
#define FLH	SNDRV_CHMAP_TFL
#define FRH	SNDRV_CHMAP_TFR
#define FLW	SNDRV_CHMAP_FLW
#define FRW	SNDRV_CHMAP_FRW
#define TC	SNDRV_CHMAP_TC
#define FCH	SNDRV_CHMAP_TFC

/*
 * CEA-861 channel maps
 *
 * Stolen from sound/pci/hda/patch_hdmi.c
 * (unlike the source, this uses SNDRV_* constants directly, as by the
 *  map_tables array in patch_hdmi.c)
 * Unknown entries use 0, which unfortunately is SNDRV_CHMAP_UNKNOWN instead
 * of SNDRV_CHMAP_NA.
 */
static struct cea_channel_speaker_allocation channel_allocations[] = {
/*			  channel:   7     6    5    4    3     2    1    0  */
{ .ca_index = 0x00,  .speakers = {   0,    0,   0,   0,   0,    0,  FR,  FL } },
				 /* 2.1 */
{ .ca_index = 0x01,  .speakers = {   0,    0,   0,   0,   0,  LFE,  FR,  FL } },
				 /* Dolby Surround */
{ .ca_index = 0x02,  .speakers = {   0,    0,   0,   0,  FC,    0,  FR,  FL } },
				 /* surround40 */
{ .ca_index = 0x08,  .speakers = {   0,    0,  RR,  RL,   0,    0,  FR,  FL } },
				 /* surround41 */
{ .ca_index = 0x09,  .speakers = {   0,    0,  RR,  RL,   0,  LFE,  FR,  FL } },
				 /* surround50 */
{ .ca_index = 0x0a,  .speakers = {   0,    0,  RR,  RL,  FC,    0,  FR,  FL } },
				 /* surround51 */
{ .ca_index = 0x0b,  .speakers = {   0,    0,  RR,  RL,  FC,  LFE,  FR,  FL } },
				 /* 6.1 */
{ .ca_index = 0x0f,  .speakers = {   0,   RC,  RR,  RL,  FC,  LFE,  FR,  FL } },
				 /* surround71 */
{ .ca_index = 0x13,  .speakers = { RRC,  RLC,  RR,  RL,  FC,  LFE,  FR,  FL } },

{ .ca_index = 0x03,  .speakers = {   0,    0,   0,   0,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x04,  .speakers = {   0,    0,   0,  RC,   0,    0,  FR,  FL } },
{ .ca_index = 0x05,  .speakers = {   0,    0,   0,  RC,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x06,  .speakers = {   0,    0,   0,  RC,  FC,    0,  FR,  FL } },
{ .ca_index = 0x07,  .speakers = {   0,    0,   0,  RC,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x0c,  .speakers = {   0,   RC,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_index = 0x0d,  .speakers = {   0,   RC,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x0e,  .speakers = {   0,   RC,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x10,  .speakers = { RRC,  RLC,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_index = 0x11,  .speakers = { RRC,  RLC,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x12,  .speakers = { RRC,  RLC,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x14,  .speakers = { FRC,  FLC,   0,   0,   0,    0,  FR,  FL } },
{ .ca_index = 0x15,  .speakers = { FRC,  FLC,   0,   0,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x16,  .speakers = { FRC,  FLC,   0,   0,  FC,    0,  FR,  FL } },
{ .ca_index = 0x17,  .speakers = { FRC,  FLC,   0,   0,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x18,  .speakers = { FRC,  FLC,   0,  RC,   0,    0,  FR,  FL } },
{ .ca_index = 0x19,  .speakers = { FRC,  FLC,   0,  RC,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x1a,  .speakers = { FRC,  FLC,   0,  RC,  FC,    0,  FR,  FL } },
{ .ca_index = 0x1b,  .speakers = { FRC,  FLC,   0,  RC,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x1c,  .speakers = { FRC,  FLC,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_index = 0x1d,  .speakers = { FRC,  FLC,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x1e,  .speakers = { FRC,  FLC,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x1f,  .speakers = { FRC,  FLC,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x20,  .speakers = {   0,  FCH,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x21,  .speakers = {   0,  FCH,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x22,  .speakers = {  TC,    0,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x23,  .speakers = {  TC,    0,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x24,  .speakers = { FRH,  FLH,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_index = 0x25,  .speakers = { FRH,  FLH,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x26,  .speakers = { FRW,  FLW,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_index = 0x27,  .speakers = { FRW,  FLW,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x28,  .speakers = {  TC,   RC,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x29,  .speakers = {  TC,   RC,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x2a,  .speakers = { FCH,   RC,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x2b,  .speakers = { FCH,   RC,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x2c,  .speakers = {  TC,  FCH,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x2d,  .speakers = {  TC,  FCH,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x2e,  .speakers = { FRH,  FLH,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x2f,  .speakers = { FRH,  FLH,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x30,  .speakers = { FRW,  FLW,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x31,  .speakers = { FRW,  FLW,  RR,  RL,  FC,  LFE,  FR,  FL } },
};

static int snd_bcm2835_chmap_ctl_tlv(struct snd_kcontrol *kcontrol, int op_flag,
				     unsigned int size, unsigned int __user *tlv)
{
	unsigned int __user *dst;
	int count = 0;
	int i;

	if (size < 8)
		return -ENOMEM;
	if (put_user(SNDRV_CTL_TLVT_CONTAINER, tlv))
		return -EFAULT;
	size -= 8;
	dst = tlv + 2;
	for (i = 0; i < ARRAY_SIZE(channel_allocations); i++) {
		struct cea_channel_speaker_allocation *ch = &channel_allocations[i];
		int num_chs = 0;
		int chs_bytes;
		int c;

		for (c = 0; c < 8; c++) {
			if (ch->speakers[c])
				num_chs++;
		}

		chs_bytes = num_chs * 4;
		if (size < 8)
			return -ENOMEM;
		if (put_user(SNDRV_CTL_TLVT_CHMAP_FIXED, dst) ||
		    put_user(chs_bytes, dst + 1))
			return -EFAULT;
		dst += 2;
		size -= 8;
		count += 8;
		if (size < chs_bytes)
			return -ENOMEM;
		size -= chs_bytes;
		count += chs_bytes;
		for (c = 0; c < 8; c++) {
			int sp = ch->speakers[7 - c];
			if (sp) {
				if (put_user(sp, dst))
					return -EFAULT;
				dst++;
			}
		}
	}
	if (put_user(count, tlv + 1))
		return -EFAULT;
	return 0;
}

static int snd_bcm2835_chmap_ctl_get(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_pcm_chmap *info = snd_kcontrol_chip(kcontrol);
	bcm2835_chip_t *chip = info->private_data;
	unsigned int idx = snd_ctl_get_ioffidx(kcontrol, &ucontrol->id);
	struct snd_pcm_substream *substream = snd_pcm_chmap_substream(info, idx);
	struct cea_channel_speaker_allocation *ch = NULL;
	int cur = 0;
	int i;

	if (!substream || !substream->runtime)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(channel_allocations); i++) {
		if (channel_allocations[i].ca_index == chip->cea_chmap)
			ch = &channel_allocations[i];
	}

	/* If no layout was set yet, return a dummy. Apparently the userspace
	 * API will be confused if we don't. */
	if (!ch)
		ch = &channel_allocations[0];

	for (i = 0; i < 8; i++) {
		if (ch->speakers[7 - i])
			ucontrol->value.integer.value[cur++] = ch->speakers[7 - i];
	}
	while (cur < 8)
		ucontrol->value.integer.value[cur++] = SNDRV_CHMAP_NA;
	return 0;
}

static int snd_bcm2835_chmap_ctl_put(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_pcm_chmap *info = snd_kcontrol_chip(kcontrol);
	bcm2835_chip_t *chip = info->private_data;
	unsigned int idx = snd_ctl_get_ioffidx(kcontrol, &ucontrol->id);
	struct snd_pcm_substream *substream = snd_pcm_chmap_substream(info, idx);
	int i, prepared = 0, cea_chmap = -1;
	int remap[8];

	if (!substream || !substream->runtime)
		return -ENODEV;

	switch (substream->runtime->status->state) {
	case SNDRV_PCM_STATE_OPEN:
	case SNDRV_PCM_STATE_SETUP:
		break;
	case SNDRV_PCM_STATE_PREPARED:
		prepared = 1;
		break;
	default:
		return -EBUSY;
	}

	for (i = 0; i < ARRAY_SIZE(channel_allocations); i++) {
		struct cea_channel_speaker_allocation *ch = &channel_allocations[i];
		int matches = 1;
		int cur = 0;
		int x;
		memset(remap, 0, sizeof(remap));
		for (x = 0; x < substream->runtime->channels; x++) {
			int sp = ucontrol->value.integer.value[x];
			while (cur < 8 && !ch->speakers[7 - cur])
				cur++;
			if (cur >= 8) {
				/* user has more channels than ch */
				matches = 0;
				break;
			}
			if (ch->speakers[7 - cur] != sp) {
				matches = 0;
				break;
			}
			remap[x] = cur;
			cur++;
		}
		for (x = cur; x < 8; x++) {
			if (ch->speakers[7 - x]) {
				/* ch has more channels than user */
				matches = 0;
				break;
			}
		}
		if (matches) {
			cea_chmap = ch->ca_index;
			break;
		}
	}

	if (cea_chmap < 0)
		return -EINVAL;

	/* don't change the layout if another substream is active */
	if (chip->opened != (1 << substream->number) && chip->cea_chmap != cea_chmap)
		return -EBUSY; /* unsure whether this is a good error code */

	chip->cea_chmap = cea_chmap;
	for (i = 0; i < 8; i++)
		chip->map_channels[i] = remap[i];
	if (prepared)
		snd_bcm2835_pcm_prepare_again(substream);
	return 0;
}

static int snd_bcm2835_add_chmap_ctl(bcm2835_chip_t * chip)
{
	struct snd_pcm_chmap *chmap;
	struct snd_kcontrol *kctl;
	int err, i;

	err = snd_pcm_add_chmap_ctls(chip->pcm,
				     SNDRV_PCM_STREAM_PLAYBACK,
				     NULL, 8, 0, &chmap);
	if (err < 0)
		return err;
	/* override handlers */
	chmap->private_data = chip;
	kctl = chmap->kctl;
	for (i = 0; i < kctl->count; i++)
		kctl->vd[i].access |= SNDRV_CTL_ELEM_ACCESS_WRITE;
	kctl->get = snd_bcm2835_chmap_ctl_get;
	kctl->put = snd_bcm2835_chmap_ctl_put;
	kctl->tlv.c = snd_bcm2835_chmap_ctl_tlv;
	return 0;
}

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
	snd_bcm2835_add_chmap_ctl(chip);
	for (idx = 0; idx < ARRAY_SIZE(snd_bcm2835_spdif); idx++) {
		err = snd_ctl_add(chip->card,
				snd_ctl_new1(&snd_bcm2835_spdif[idx], chip));
		if (err < 0)
			return err;
	}
	return 0;
}
