/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2024 Google LLC
 */
#ifndef __ANDROID_CONFIGFS_UEVENT_H
#define __ANDROID_CONFIGFS_UEVENT_H

#ifdef CONFIG_ANDROID_USB_CONFIGFS_UEVENT
#include <linux/usb/android_configfs_uevent.h>

/**
 * android_class_create - essentially the __init() function for the
 * configfs_uevent library, since it is not a standalone driver.
 *
 * Creates the android_usb class of device
 *
 * Returns: the result of class_register (0 for success, err otherwise)
 */
int android_class_create(void);

/**
 * android_class_destroy - essentially the __exit() function for the
 * configfs_uevent library, since it is not a standalone driver.
 *
 * Removes the android_usb class of devices and performs any necessary
 * cleanup.
 */
void android_class_destroy(void);

/**
 * android_device_create - Creates an android device instance and
 * a state attribute file which can be read to determine the state of the
 * usb gadget.
 * @opts: contextual data for the configfs_uevent library.
 *
 * Note: the state file created by this function mimics the functionaltiy
 * of the UDC driver and is likely redundant, but maintained for legacy
 * support.
 *
 * The state can be one of "DISCONNECTED", "CONNECTED", or "CONFIGURED"
 *
 * Returns: 0 for success, or if an error is encountered during ida_allocation
 * or device_creation, that error is returned.
 */
int android_device_create(struct android_uevent_opts *opts);

/**
 * android_device_destroy - Removes the android device instance and performs
 * any necessary cleanup.
 * @opts: contextual data for the configfs_uevent library.
 */
void android_device_destroy(struct android_uevent_opts *opts);

/**
 * android_set_connected - set the internal state of android_uevent_opts to
 * connected and schedule the work to emit a uevent with this status update.
 * @opts: contextual data for the configfs_uevent library
 *
 * This should be called by the gadget composite driver when a usb_ctrlrequest
 * is received by the gadget driver.
 *
 * This function locks the android specific android_uevent_opts->lock and
 * therefore should not require locking the containing composite device
 * structure as the internal lock is also used in the teardown path of the
 * composite driver in android_device_destroy().
 */
void android_set_connected(struct android_uevent_opts *opts);

/**
 * android_set_disconnected - reset the internal state of android_uevent_opts to
 * disconnected and schedule the work to emit a uevent with this status update.
 * @opts: contextual data for the configfs_uevent library
 *
 * This should be called by the gadget composite driver when the link is
 * disconnected.
 *
 * This function locks the android specific android_uevent_opts->lock and
 * therefore should not require locking the containing composite device
 * structure as the internal lock is also used in the teardown path of the
 * composite driver in android_device_destroy().
 */
void android_set_disconnected(struct android_uevent_opts *opts);

/**
 * android_set_configured - set the internal state of android_uevent_opts to
 * configured and schedule the work to emit a uevent with this status update.
 * @opts: contextual data for the configfs_uevent library
 *
 * This should be called by the gadget composite driver when the configuration
 * is applied to the gadget composite device
 *
 * This function locks the android specific android_uevent_opts->lock and
 * therefore should not require locking the containing composite device
 * structure as the internal lock is also used in the teardown path of the
 * composite driver in android_device_destroy().
 */
void android_set_configured(struct android_uevent_opts *opts);

/**
 * android_set_unconfigured - reset the internal state of android_uevent_opts to
 * unconfigured and schedule the work to emit a uevent with this status update.
 * @opts: contextual data for the configfs_uevent library
 *
 * This should be called by the gadget composite driver when the gadget
 * configuration is torn down.
 *
 * This function locks the android specific android_uevent_opts->lock and
 * therefore should not require locking the containing composite device
 * structure as the internal lock is also used in the teardown path of the
 * composite driver in android_device_destroy().
 */
void android_set_unconfigured(struct android_uevent_opts *opts);

#else

static inline int android_class_create(void)
{
	return 0;
}

static inline void android_class_destroy(void)
{
}

static inline int android_device_create(struct android_uevent_opts *opts)
{
	return 0;
}

static inline void android_device_destroy(struct android_uevent_opts *opts)
{
}

static inline void android_set_connected(struct android_uevent_opts *opts)
{
}

static inline void android_set_disconnected(struct android_uevent_opts *opts)
{
}

static inline void android_set_configured(struct android_uevent_opts *opts)
{
}

static inline void android_set_unconfigured(struct android_uevent_opts *opts)
{
}
#endif /* CONFIG_ANDROID_USB_CONFIGFS_UEVENT */
#endif /* __ANDROID_CONFIGFS_UEVENT_H */
