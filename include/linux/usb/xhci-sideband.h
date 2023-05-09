/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xHCI host controller sideband support
 *
 * Copyright (c) 2023, Intel Corporation.
 *
 * Author: Mathias Nyman <mathias.nyman@linux.intel.com>
 */

#ifndef __LINUX_XHCI_SIDEBAND_H
#define __LINUX_XHCI_SIDEBAND_H

#include <linux/scatterlist.h>
#include <linux/usb.h>

#define	EP_CTX_PER_DEV		31	/* FIMXME defined twice, from xhci.h */

struct xhci_sideband;

/**
 * struct xhci_sideband - representation of a sideband accessed usb device.
 * @xhci: The xhci host controller the usb device is connected to
 * @vdev: the usb device accessed via sideband
 * @eps: array of endpoints controlled via sideband
 * @ir: event handling and buffer for sideband accessed device
 *
 * FIXME usb device accessed via sideband Keeping track of sideband accessed usb devices.
 */

struct xhci_sideband {
	struct xhci_hcd                 *xhci;
	struct xhci_virt_device         *vdev;
	struct xhci_virt_ep             *eps[EP_CTX_PER_DEV];
	struct xhci_interrupter         *ir;
	struct mutex			mutex;
};

struct xhci_sideband *
xhci_sideband_register(struct usb_device *udev);
void
xhci_sideband_unregister(struct xhci_sideband *sb);
int
xhci_sideband_add_endpoint(struct xhci_sideband *sb,
			   struct usb_host_endpoint *host_ep);
int
xhci_sideband_remove_endpoint(struct xhci_sideband *sb,
			      struct usb_host_endpoint *host_ep);
int
xhci_sideband_stop_endpoint(struct xhci_sideband *sb,
			    struct usb_host_endpoint *host_ep);
struct sg_table *
xhci_sideband_get_endpoint_buffer(struct xhci_sideband *sb,
				  struct usb_host_endpoint *host_ep);
struct sg_table *
xhci_sideband_get_event_buffer(struct xhci_sideband *sb);

int xhci_sideband_enable_interrupt(struct xhci_sideband *sb, u32 imod_interval);

int
xhci_sideband_create_interrupter(struct xhci_sideband *sb, int num_seg,
				 int intr_num, bool ip_autoclear);

void
xhci_sideband_remove_interrupter(struct xhci_sideband *sb);

int
xhci_sideband_interrupter_id(struct xhci_sideband *sb);

#endif /* __LINUX_XHCI_SIDEBAND_H */

