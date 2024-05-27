// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2011-2024 Google LLC
 */
#include <linux/usb/android_configfs_uevent.h>
#include <linux/spinlock.h>
#include <linux/err.h>
#include <linux/sysfs.h>
#include <linux/device.h>

#include "android_f_midi_info.h"

int android_set_midi_device_info(struct f_midi_info *ctx, int card_number,
		unsigned int rmidi_device)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->configured) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -EBUSY;
	}
	ctx->card_number = card_number;
	ctx->rmidi_device = rmidi_device;
	ctx->configured = true;
	spin_unlock_irqrestore(&ctx->lock, flags);
	return 0;
}

void android_clear_midi_device_info(struct f_midi_info *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);
	ctx->card_number = 0;
	ctx->rmidi_device = 0;
	ctx->configured = false;
	spin_unlock_irqrestore(&ctx->lock, flags);
}

static ssize_t alsa_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct f_midi_info *ctx = dev_get_drvdata(dev);
	unsigned long flags;
	int ret;

	/*
	 * print PCM card and device numbers or '-1 -1' if unconfigured
	 *
	 * Note: this is a hack and not an appropriate use of sysfs. Sysfs
	 * is intended to be "one value per file," however this API was defined
	 * in a prior version of this driver and therefore we must maintain API
	 * compatibility at this time. This must be changed to upstream.
	 */
	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->configured) {
		ret = sysfs_emit(buf, "%d %d\n",
		ctx->card_number, ctx->rmidi_device);
	} else {
		// This could occur if the sysfs entry is read prior to binding
		dev_dbg(dev, "f_midi: function not configured\n");
		ret = sysfs_emit(buf, "-1 -1\n");
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	return ret;
}
static DEVICE_ATTR_RO(alsa);

static struct attribute *alsa_attrs[] = {
	&dev_attr_alsa.attr,
	NULL
};
ATTRIBUTE_GROUPS(alsa);

int android_create_midi_device(struct f_midi_info *ctx)
{
	struct device *dev;

	spin_lock_init(&ctx->lock);
	ctx->configured = false;

	/*
	 * I believe this limits the creation of multiple f_midi devices within
	 * a single androidN device instance. This is a hack and not the
	 * correct way to do this, however the android userspace expects a
	 * single device named "f_midi" to exist, so maintain this limitation
	 * until we can refactor.
	 */
	dev = android_create_function_device("f_midi", (void *) ctx,
			alsa_groups);
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	ctx->dev = get_device(dev);
	return 0;
}

void android_remove_midi_device(struct f_midi_info *ctx)
{
	unsigned long flags;
	struct device *dev = NULL;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->dev) {
		dev = ctx->dev;
		ctx->dev = NULL;
	}

	ctx->configured = false;
	ctx->card_number = 0;
	ctx->rmidi_device = 0;
	spin_unlock_irqrestore(&ctx->lock, flags);
	if (dev) {
		// Matches with get_device in android_create_midi_device()
		put_device(dev);
		android_remove_function_device(dev);
	}
}
