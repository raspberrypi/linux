/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2024 Google LLC
 */

#ifndef __ANDROID_F_MIDI_INFO_H
#define __ANDROID_F_MIDI_INFO_H

#ifdef CONFIG_ANDROID_USB_F_MIDI_INFO
#include <linux/device.h>
#include <linux/spinlock.h>

struct f_midi_info {
	/*
	 * the device created within the android_usb class for the f_midi
	 * gadget instance
	 */
	struct device *dev;
	/*
	 * the number field of the struct snd_card object created by the
	 * f_midi gadget driver
	 */
	int card_number;
	/*
	 * the number field of the struct snd_rawmidi object configured by the
	 * f_midi gadget driver
	 */
	u32 rmidi_device;
	/*
	 * flag indicating that the card_number and rmidi_device fields have
	 * been set during the f_midi gadget initialization
	 */
	bool configured;
	/*
	 * lock protecting the card_number and rmidi_device fields from being
	 * changed while being accessed
	 */
	spinlock_t lock;
};

/**
 * android_set_midi_device_info - used to update the internal data of
 * f_midi_info with the necessary data to pass to userspace.
 * @ctx: contextual data for the android_f_midi_info library
 * @card_number: the number field of the struct snd_card object created
 * by the f_midi driver
 * @rmidi_device: the device field of the struct snd_rawmidi object configured
 * by the f_midi driver
 *
 * This function should be called in the f_midi driver after creating the
 * necessary snd_ objects.
 *
 * Returns: 0 for success, -EBUSY if the device has already been configured.
 */
int android_set_midi_device_info(struct f_midi_info *ctx, int card_number,
	       unsigned int rmidi_device);

/**
 * android_clear_midi_device_info - used to unconfigure the internal data of
 * f_midi_info when the f_midi device is being torn down.
 * @ctx: contextual data for the android_f_midi_info library
 *
 * This function should be called in the f_midi driver prior to removing the
 * necessary snd_card and snd_rawmidi objects. May be called several times in
 * the cleanup process due to separate lifecycles for the snd_card and
 * snd_rawmidi objects, however, both objects must exist for userspace, so if
 * one goes away, clear both.
 */
void android_clear_midi_device_info(struct f_midi_info *ctx);

/**
 * android_create_midi_device - performs the necessary initialization for the
 * android_f_midi_info library and registers a f_midi device with the android_usb
 * class.
 * @ctx: contextual data for the android_f_midi_info library
 *
 * Returns: 0 for success, relevant error on failure.
 */
int android_create_midi_device(struct f_midi_info *ctx);

/**
 * android_remove_midi_device - used to remove the device created by
 * android_create_midi_device() and clear the internal data of
 * f_midi_info when the related objects are being removed.
 * @ctx: contextual data for the android_f_midi_info library
 *
 * This function should be called in the f_midi driver prior to freeing the
 * midi function instance.
 */
void android_remove_midi_device(struct f_midi_info *ctx);

#else
struct f_midi_info {};

static inline int android_set_midi_device_info(struct f_midi_info *ctx,
		int card_number, unsigned int rmidi_device)
{
	return 0;
}

static inline void android_clear_midi_device_info(struct f_midi_info *ctx)
{
}

static inline int android_create_midi_device(struct f_midi_info *ctx)
{
	return 0;
}

static inline void android_remove_midi_device(struct f_midi_info *ctx)
{
}
#endif /* CONFIG_ANDROID_USB_F_MIDI_INFO */
#endif /* __ANDROID_F_MIDI_INFO_H */
