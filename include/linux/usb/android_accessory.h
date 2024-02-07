/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2024 Google LLC
 */
#ifndef __ANDROID_ACCESSORY_H
#define __ANDROID_ACCESSORY_H

#include <linux/usb/composite.h>
#include <linux/usb/ch9.h>

#ifdef CONFIG_ANDROID_USB_CONFIGFS_F_ACC

/**
 * android_acc_req_match_composite - used to check if the android accessory
 * driver can handle a usb_ctrlrequest
 * @cdev - the usb_composite_dev instance associated with the incoming ctrl
 * request
 * @ctrl - a usb_ctrlrequest for the accessory driver to check
 *
 * This function should be called in composite_setup() after other req_match
 * checks have failed and the usb_ctrlrequest is still unhandled.
 *
 * The reason this must be implemented instead of the standard req_match
 * interface is that the accessory function does not get bound to a config
 * by userspace until a connected device sends the ACCESSORY_START control
 * request, and therefore the composite driver does not know about f_accessory
 * yet we need to check for the control requests.
 *
 * Returns: true if the accessory driver can handle the request, false if not
 */
bool android_acc_req_match_composite(struct usb_composite_dev *cdev,
		const struct usb_ctrlrequest *ctrl);

/**
 * android_acc_setup_composite - function for the f_accessory driver to handle
 * usb_ctrlrequests
 * @cdev - the usb_composite_dev instance associated with the incoming ctrl
 * request.
 * @ctrl - a usb_ctrlrequest to be handled by the f_accessory driver.
 *
 * This function should be called in composite_setup() after successfully
 * checking for ctrl request support in android_acc_req_match_composite().
 *
 * The reason this additional api must be defined is due to the fact that
 * userspace does not bind the f_accessory instance to a gadget config until
 * after receiving an ACCESSORY_START control request from a connected
 * accessory device, and therefore we have a circular dependency. The addition
 * of this allows compatibility with the existing Android userspace, but is
 * not ideal and should be refactored in the future.
 *
 * Returns: Negative error value upon failure, >=0 upon successful handdling
 * of the usb_ctrlrequest aligned with the standard composite function driver
 * setup() api.
 */
int android_acc_setup_composite(struct usb_composite_dev *cdev,
		const struct usb_ctrlrequest *ctrl);

/**
 * android_acc_disconnect - used to cleanup the accessory function
 * and update the connection state on device disconnection.
 *
 * This should be called in the composite driver's __composite_disconnect()
 * path to notify the accessory function of device disconnect. This is
 * required because the accessory function exists outside of a gadget config
 * and therefore the composite driver's standard cleanup paths may not apply.
 */
void android_acc_disconnect(void);

#else

static inline bool android_acc_req_match_composite(struct usb_composite_dev *cdev,
		const struct usb_ctrlrequest *ctrl)
{
	return false;
}

static inline int android_acc_setup_composite(struct usb_composite_dev *cdev,
		const struct usb_ctrlrequest *ctrl)
{
	return 0;
}

static inline void android_acc_disconnect(void)
{
}
#endif /* CONFIG_ANDROID_USB_CONFIGFS_F_ACC */
#endif /* __ANDROID_ACCESSORY_H */
