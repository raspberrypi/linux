// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2011-2024 Google LLC
 */
#include "android_configfs_uevent.h"
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/err.h>
#include <linux/kdev_t.h>
#include <linux/spinlock.h>
#include <trace/hooks/usb.h>

static struct android_uevent_opts *android_opts;

static DEFINE_SPINLOCK(opts_lock);
static DEFINE_IDA(android_ida);

static void android_work(struct work_struct *data)
{
	struct android_uevent_opts *opts = container_of(data,
			struct android_uevent_opts, work);

	char *disconnected_strs[2] = { "USB_STATE=DISCONNECTED", NULL };
	char *connected_strs[2] = { "USB_STATE=CONNECTED", NULL };
	char *configured_strs[2] = { "USB_STATE=CONFIGURED", NULL };
	unsigned long flags;
	bool disconnected = false;
	bool connected = false;
	bool configured = false;
	bool uevent_sent = false;
	struct device *dev;

	/*
	 * I believe locking is important due to the fact that we are checking
	 * several conditions here, and if the state changes after checking one
	 * we could potentially drop a uevent to userspace. Additionally, we
	 * want to prevent teardown until after events are sent.
	 */
	spin_lock_irqsave(&opts_lock, flags);

	/*
	 * If the device does not exist, it means we were torn down after
	 * scheduling this work, but before the work ran, so return to prevent
	 * use after free.
	 */
	if (!opts->dev) {
		spin_unlock_irqrestore(&opts_lock, flags);
		return;
	}

	/*
	 * Cache the dev pointer in the locked area incase it gets cleared by
	 * android_device_destroy() after we release the lock. The call to
	 * flush_work in the cleanup path ensures we finish our work prior to
	 * destroying the dev which we have cached the pointer to. Ideally,
	 * this would be handled differently (using reference counting), but
	 * for now this should work.
	 */
	dev = opts->dev;

	if (opts->connected != opts->sw_connected) {
		if (opts->connected)
			connected = true;
		else
			disconnected = true;
		opts->sw_connected = opts->connected;
	}
	if (opts->configured)
		configured = true;

	spin_unlock_irqrestore(&opts_lock, flags);

	/*
	 * This is an abuse of uevents, however the android userspace parses
	 * the uevent string for information instead of reading the state from
	 * sysfs entries. This is one of several things about this driver which
	 * would need to change to upstream it. In an attempt to keep the
	 * exising userspace api unmodified until either an upstream solution
	 * is implemented or this functionality is otherwise replaced, leave
	 * the pre-existing logic in place.
	 */
	if (connected) {
		if (kobject_uevent_env(&dev->kobj, KOBJ_CHANGE,
				       connected_strs)) {
			dev_err(dev, "Failed to send connected uevent\n");
		} else {
			dev_dbg(dev, "sent uevent %s\n", connected_strs[0]);
			uevent_sent = true;
		}
	}

	if (configured) {
		if (kobject_uevent_env(&dev->kobj, KOBJ_CHANGE,
					configured_strs)) {
			dev_err(dev, "Failed to send configured uevent\n");
		} else {
			dev_dbg(dev, "sent uevent %s\n", configured_strs[0]);
			uevent_sent = true;
		}
	}

	if (disconnected) {
		if (kobject_uevent_env(&dev->kobj, KOBJ_CHANGE,
					disconnected_strs)) {
			dev_err(dev, "Failed to send disconnected uevent\n");
		} else {
			dev_dbg(dev, "sent uevent %s\n", disconnected_strs[0]);
			uevent_sent = true;
		}
	}

	if (!uevent_sent) {
		/*
		 * This is an odd case, but not necessarily an error- the state
		 * of the device may have changed since the work was scheduled,
		 * and if the state changed, there is likely another scheduled
		 *  work which will send a uevent.
		 */
		dev_dbg(dev, "did not send uevent\n");
	}

	trace_android_vh_configfs_uevent_work(connected,
		disconnected, configured, uevent_sent);
}

static ssize_t state_show(struct device *pdev,
		struct device_attribute *attr,
		char *buf)
{
	struct android_uevent_opts *opts = dev_get_drvdata(pdev);
	char *state = "DISCONNECTED";

	if (opts->configured)
		state = "CONFIGURED";
	else if (opts->connected)
		state = "CONNECTED";

	return sysfs_emit(buf, "%s\n", state);
}
static DEVICE_ATTR_RO(state);

static struct attribute *android_usb_attrs[] = {
	&dev_attr_state.attr,
	NULL,
};

ATTRIBUTE_GROUPS(android_usb);

static struct class android_usb_class = {
	.name = "android_usb",
	.dev_groups = android_usb_groups,
};

int android_class_create(void)
{
	return class_register(&android_usb_class);
}

void android_class_destroy(void)
{
	class_unregister(&android_usb_class);
}

int android_device_create(struct android_uevent_opts *opts)
{
	unsigned long flags;
	struct device *dev;

	spin_lock_irqsave(&opts_lock, flags);
	INIT_WORK(&opts->work, android_work);

	opts->device_id = ida_alloc(&android_ida, GFP_ATOMIC);
	//Unlock prior to calling device_create() since it may sleep
	spin_unlock_irqrestore(&opts_lock, flags);
	if (opts->device_id < 0)
		return opts->device_id;

	dev = device_create(&android_usb_class, NULL, MKDEV(0, 0),
			       opts, "android%d", opts->device_id);

	spin_lock_irqsave(&opts_lock, flags);
	if (IS_ERR(dev)) {
		ida_free(&android_ida, opts->device_id);
		opts->device_id = -1;
		spin_unlock_irqrestore(&opts_lock, flags);
		return PTR_ERR(dev);
	}
	opts->dev = dev;
	ida_init(&opts->function_ida);
	if (!android_opts)
		android_opts = opts;
	spin_unlock_irqrestore(&opts_lock, flags);

	return 0;
}

void android_device_destroy(struct android_uevent_opts *opts)
{
	unsigned long flags;
	struct device *dev;

	/*
	 * This scheme is used to safely cleanup any remaining work. Once
	 * opts->dev is set to NULL, any newly scheduled work will return
	 * after getting the lock and checking for NULL. Any currently
	 * running work finishes with the flush_work (the worker caches
	 * opts->dev so it can continue), before we free the device.
	 *
	 * Ideally, this cleanup would be handled via reference counting, but
	 * there are nuances around device destroy (or the fact that we are
	 * currently statically allocating opts) which prevent this from
	 * being implemented without a significant refactor.
	 */
	spin_lock_irqsave(&opts_lock, flags);
	dev = opts->dev;
	opts->dev = NULL;
	spin_unlock_irqrestore(&opts_lock, flags);

	flush_work(&opts->work);

	spin_lock_irqsave(&opts_lock, flags);
	if (opts->device_id >= 0)
		ida_free(&android_ida, opts->device_id);

	android_opts = NULL;
	ida_destroy(&opts->function_ida);
	device_destroy(dev->class, dev->devt);
	spin_unlock_irqrestore(&opts_lock, flags);
}

void __android_set_connected(struct android_uevent_opts *opts,
		bool connected)
{
	unsigned long flags;

	spin_lock_irqsave(&opts_lock, flags);
	// Don't send the uevent if connected state is not changed
	if (opts->connected != connected) {
		opts->connected = connected;
		schedule_work(&opts->work);
	}
	spin_unlock_irqrestore(&opts_lock, flags);
}

void __android_set_configured(struct android_uevent_opts *opts,
		bool configured)
{
	unsigned long flags;

	spin_lock_irqsave(&opts_lock, flags);
	// Don't send the uevent if configure state is not changed
	if (opts->configured != configured) {
		opts->configured = configured;
		schedule_work(&opts->work);
	}
	spin_unlock_irqrestore(&opts_lock, flags);
}

void android_set_connected(struct android_uevent_opts *opts)
{
	__android_set_connected(opts, true);
}

void android_set_disconnected(struct android_uevent_opts *opts)
{
	__android_set_connected(opts, false);
}

void android_set_configured(struct android_uevent_opts *opts)
{
	__android_set_configured(opts, true);
}

void android_set_unconfigured(struct android_uevent_opts *opts)
{
	__android_set_configured(opts, false);
}

struct device *android_create_function_device(char *name, void *drvdata,
	       const struct attribute_group **groups)
{
	struct android_uevent_opts *opts;
	struct device *dev;
	unsigned long flags;
	int id;

	spin_lock_irqsave(&opts_lock, flags);
	opts = android_opts;
	if (IS_ERR_OR_NULL(opts) || IS_ERR_OR_NULL(opts->dev)) {
		spin_unlock_irqrestore(&opts_lock, flags);
		return ERR_PTR(-ENODEV);
	}

	id = ida_alloc(&opts->function_ida, GFP_ATOMIC);
	if (id < 0) {
		spin_unlock_irqrestore(&opts_lock, flags);
		return ERR_PTR(id);
	}
	// device_create_with_groups can sleep, so we must unlock first
	spin_unlock_irqrestore(&opts_lock, flags);
	dev = device_create_with_groups(&android_usb_class, opts->dev,
	       MKDEV(0, id), drvdata, groups, name);
	return dev;
}
EXPORT_SYMBOL_GPL(android_create_function_device);

void android_remove_function_device(struct device *dev)
{
	struct android_uevent_opts *opts;
	unsigned long flags;

	device_destroy(&android_usb_class, dev->devt);

	spin_lock_irqsave(&opts_lock, flags);
	opts = android_opts;
	if (IS_ERR_OR_NULL(opts)) {
		spin_unlock_irqrestore(&opts_lock, flags);
		return;
	}

	ida_free(&opts->function_ida, MINOR(dev->devt));
	spin_unlock_irqrestore(&opts_lock, flags);
}
EXPORT_SYMBOL_GPL(android_remove_function_device);
