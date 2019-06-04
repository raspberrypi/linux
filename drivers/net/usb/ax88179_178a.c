/*
 * ASIX AX88179/178A USB 3.0/2.0 to Gigabit Ethernet Devices
 *
 * Copyright (C) 2011-2013 ASIX
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/usb.h>
#include <linux/crc32.h>
#include <linux/if_vlan.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <net/ip6_checksum.h>
#include <linux/usb/cdc.h>
#include <linux/suspend.h>
#include <linux/pm_runtime.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/in.h>
#include <linux/mdio.h>
#include <uapi/linux/mdio.h>

#include "ax88179_178a.h"

/* EEE advertisement is disabled in default setting */
static int bEEE;
module_param(bEEE, int, 0);
MODULE_PARM_DESC(bEEE, "EEE advertisement configuration");

/* Green ethernet advertisement is disabled in default setting */
static int bGETH;
module_param(bGETH, int, 0);
MODULE_PARM_DESC(bGETH, "Green ethernet configuration");

static unsigned int agg_buf_sz_rx = (16 * 1024);
static unsigned int agg_buf_sz_tx = (16 * 1024);

//#define ax_LIMITED_TSO_SIZE	(agg_buf_sz_tx - 8 - ETH_FCS_LEN)

/*-------------------------------------------------------------------------*/
static int __usbnet_read_cmd(struct ax88179 *dev, u8 cmd, u8 reqtype,
			     u16 value, u16 index, void *data, u16 size)
{
	void *buf = NULL;
	int err = -ENOMEM;

	//printk("usbnet_read_cmd cmd=0x%02x reqtype=%02x"
	//		" value=0x%04x index=0x%04x size=%d\n",
	//		cmd, reqtype, value, index, size);

	if (size) {
		buf = kmalloc(size, GFP_KERNEL);
		if (!buf)
			goto out;
	}

	err = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
			      cmd, reqtype, value, index, buf, size,
			      USB_CTRL_GET_TIMEOUT);
	if (err > 0 && err <= size) {
		if (data)
			memcpy(data, buf, err);
		else
			netdev_dbg(dev->netdev,
				   "Huh? Data requested but thrown away.\n");
	}
	kfree(buf);
out:
	return err;
}

static int __usbnet_write_cmd(struct ax88179 *dev, u8 cmd, u8 reqtype,
			      u16 value, u16 index, const void *data,
			      u16 size)
{
	void *buf = NULL;
	int err = -ENOMEM;

	//printk("usbnet_write_cmd cmd=0x%02x reqtype=%02x"
	//	   " value=0x%04x index=0x%04x size=%d\n",
	//	   cmd, reqtype, value, index, size);

	if (data) {
		buf = kmemdup(data, size, GFP_KERNEL);
		if (!buf)
			goto out;
	} else {
		if (size) {
			WARN_ON_ONCE(1);
			err = -EINVAL;
			goto out;
		}
	}

	err = usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0),
			      cmd, reqtype, value, index, buf, size,
			      USB_CTRL_SET_TIMEOUT);
	kfree(buf);

out:
	return err;
}

/* The function can't be called inside suspend/resume callback,
 * otherwise deadlock will be caused.
 */
int axusbnet_read_cmd(struct ax88179 *dev, u8 cmd, u8 reqtype,
		      u16 value, u16 index, void *data, u16 size)
{
	int ret;

	if (usb_autopm_get_interface(dev->intf) < 0)
		return -ENODEV;
	ret = __usbnet_read_cmd(dev, cmd, reqtype, value, index,
				data, size);
	usb_autopm_put_interface(dev->intf);
	return ret;
}

/* The function can't be called inside suspend/resume callback,
 * otherwise deadlock will be caused.
 */
int axusbnet_write_cmd(struct ax88179 *dev, u8 cmd, u8 reqtype,
		       u16 value, u16 index, const void *data, u16 size)
{
	int ret;

	if (usb_autopm_get_interface(dev->intf) < 0)
		return -ENODEV;
	ret = __usbnet_write_cmd(dev, cmd, reqtype, value, index,
				 data, size);
	usb_autopm_put_interface(dev->intf);
	return ret;
}

/* The function can be called inside suspend/resume callback safely
 * and should only be called by suspend/resume callback generally.
 */
int axusbnet_read_cmd_nopm(struct ax88179 *dev, u8 cmd, u8 reqtype,
			   u16 value, u16 index, void *data, u16 size)
{
	return __usbnet_read_cmd(dev, cmd, reqtype, value, index,
				 data, size);
}

/* The function can be called inside suspend/resume callback safely
 * and should only be called by suspend/resume callback generally.
 */
int axusbnet_write_cmd_nopm(struct ax88179 *dev, u8 cmd, u8 reqtype,
			    u16 value, u16 index, const void *data,
			    u16 size)
{
	return __usbnet_write_cmd(dev, cmd, reqtype, value, index,
				  data, size);
}

/*-------------------------------------------------------------------------*/
static int __ax88179_read_cmd(struct ax88179 *dev, u8 cmd, u16 value, u16 index,
			      u16 size, void *data, int in_pm)
{
	int ret;

	int (*fn)(struct ax88179 *, u8, u8, u16, u16, void *, u16);

	if (!in_pm)
		fn = axusbnet_read_cmd;
	else
		fn = axusbnet_read_cmd_nopm;

	ret = fn(dev, cmd, USB_DIR_IN | USB_TYPE_VENDOR |
		 USB_RECIP_DEVICE, value, index, data, size);

	if (unlikely(ret < 0))
		netdev_warn(dev->netdev, "Failed to read reg cmd 0x%04x, value 0x%04x: %d\n",
			    cmd, value, ret);
	return ret;
}

static int __ax88179_write_cmd(struct ax88179 *dev, u8 cmd, u16 value,
			       u16 index, u16 size, void *data,
			       int in_pm)
{
	int ret;

	int (*fn)(struct ax88179 *, u8, u8, u16, u16, const void *, u16);

	if (!in_pm)
		fn = axusbnet_write_cmd;
	else
		fn = axusbnet_write_cmd_nopm;

	ret = fn(dev, cmd, USB_DIR_OUT | USB_TYPE_VENDOR |
		 USB_RECIP_DEVICE, value, index, data, size);

	if (unlikely(ret < 0))
		netdev_warn(dev->netdev, "Failed to write reg cmd 0x%04x, value 0x%04x: %d\n",
			    cmd, value, ret);

	return ret;
}

static int ax88179_read_cmd_nopm(struct ax88179 *dev, u8 cmd, u16 value,
				 u16 index, u16 size, void *data, int eflag)
{
	int ret;

	if (eflag && size == 2) {
		u16 buf = 0;

		ret = __ax88179_read_cmd(dev, cmd, value, index, size, &buf, 1);

		le16_to_cpus(&buf);
		*((u16 *)data) = buf;
	} else if (eflag && (size == 4)) {
		u32 buf = 0;

		ret = __ax88179_read_cmd(dev, cmd, value, index, size, &buf, 1);
		le32_to_cpus(&buf);
		*((u32 *)data) = buf;
	} else {
		ret = __ax88179_read_cmd(dev, cmd, value, index, size, data, 1);
	}

	return ret;
}

static int ax88179_write_cmd_nopm(struct ax88179 *dev, u8 cmd, u16 value,
				  u16 index, u16 size, void *data)
{
	int ret;

	if (size == 2) {
		u16 buf = 0;

		buf = *((u16 *)data);
		cpu_to_le16s(&buf);
		ret = __ax88179_write_cmd(dev, cmd, value, index,
					  size, &buf, 1);
	} else {
		ret = __ax88179_write_cmd(dev, cmd, value, index,
					  size, data, 1);
	}

	return ret;
}

static int ax88179_read_cmd(struct ax88179 *dev, u8 cmd, u16 value, u16 index,
			    u16 size, void *data, int eflag)
{
	int ret;

	if (eflag && size == 2) {
		u16 buf = 0;

		ret = __ax88179_read_cmd(dev, cmd, value, index, size, &buf, 0);
		le16_to_cpus(&buf);
		*((u16 *)data) = buf;
	} else if (eflag && (size == 4)) {
		u32 buf = 0;

		ret = __ax88179_read_cmd(dev, cmd, value, index, size, &buf, 0);
		le32_to_cpus(&buf);
		*((u32 *)data) = buf;
	} else {
		ret = __ax88179_read_cmd(dev, cmd, value, index, size, data, 0);
	}

	return ret;
}

static int ax88179_write_cmd(struct ax88179 *dev, u8 cmd, u16 value, u16 index,
			     u16 size, void *data)
{
	int ret;

	if (size == 4) {
		u16 buf = 0;

		buf = *((u16 *)data);
		cpu_to_le16s(&buf);
		ret = __ax88179_write_cmd(dev, cmd, value, index,
					  size, &buf, 0);
	} else {
		ret = __ax88179_write_cmd(dev, cmd, value, index,
					  size, data, 0);
	}

	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
static void ax88179_async_cmd_callback(struct urb *urb, struct pt_regs *regs)
#else
static void ax88179_async_cmd_callback(struct urb *urb)
#endif
{
	struct ax88179_async_handle *asyncdata;

	asyncdata = (struct ax88179_async_handle *)urb->context;

	if (urb->status < 0)
		printk(KERN_ERR "ax88179_async_cmd_callback() failed with %d",
		       urb->status);

	kfree(asyncdata->req);
	kfree(asyncdata);
	usb_free_urb(urb);
}

static void
ax88179_write_cmd_async(struct ax88179 *dev, u8 cmd, u16 value, u16 index,
			u16 size, void *data)
{
	struct usb_ctrlrequest *req = NULL;
	int status = 0;
	struct urb *urb = NULL;
	void *buf = NULL;
	struct ax88179_async_handle *asyncdata = NULL;

	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (urb == NULL) {
		netdev_err(dev->netdev, "Error allocating URB in write_cmd_async!");
		return;
	}

	req = kmalloc(sizeof(struct usb_ctrlrequest), GFP_ATOMIC);
	if (req == NULL) {
		netdev_err(dev->netdev, "Failed to allocate memory for control request");
		usb_free_urb(urb);
		return;
	}

	asyncdata = (struct ax88179_async_handle *)
		kmalloc(sizeof(struct ax88179_async_handle), GFP_ATOMIC);
	if (asyncdata == NULL) {
		netdev_err(dev->netdev, "Failed to allocate memory for async data");
		kfree(req);
		usb_free_urb(urb);
		return;
	}

	asyncdata->req = req;

	if (size == 2) {
		asyncdata->rxctl = *((u16 *)data);
		cpu_to_le16s(&asyncdata->rxctl);
		buf = &asyncdata->rxctl;
	} else {
		memcpy(asyncdata->m_filter, data, size);
		buf = asyncdata->m_filter;
	}

	req->bRequestType = USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE;
	req->bRequest = cmd;
	req->wValue = cpu_to_le16(value);
	req->wIndex = cpu_to_le16(index);
	req->wLength = cpu_to_le16(size);

	usb_fill_control_urb(urb, dev->udev,
			     usb_sndctrlpipe(dev->udev, 0),
			     (void *)req, buf, size,
			     ax88179_async_cmd_callback, asyncdata);

	status = usb_submit_urb(urb, GFP_ATOMIC);
	if (status < 0) {
		netdev_err(dev->netdev, "Error submitting the control message: status=%d",
			   status);
		kfree(req);
		kfree(asyncdata);
		usb_free_urb(urb);
	}
}

static void ax_set_unplug(struct ax88179 *dev)
{
	if (dev->udev->state == USB_STATE_NOTATTACHED) {
		set_bit(AX88179_UNPLUG, &dev->flags);
		/* Memory barrier
		 */
		smp_mb__after_atomic();
	}
}

static int ax88179_mdio_read(struct net_device *netdev, int phy_id, int reg)
{
	struct ax88179 *dev = netdev_priv(netdev);
	u16 res;

	ax88179_read_cmd(dev, AX_ACCESS_PHY, phy_id, (__u16)reg, 2, &res, 1);

	return res;
}

static
void ax88179_mdio_write(struct net_device *netdev, int phy_id, int reg, int val)
{
	struct ax88179 *dev = netdev_priv(netdev);
	u16 res = (u16)val;

	ax88179_write_cmd(dev, AX_ACCESS_PHY, phy_id, (__u16)reg, 2, &res);
}

static int
ax88179_submit_rx(struct ax88179 *dev, struct rx_agg *agg, gfp_t mem_flags);

static int ax88179_set_mac_addr(struct net_device *net, void *p)
{
	struct ax88179 *dev = netdev_priv(net);
	struct sockaddr *addr = p;

	if (netif_running(net))
		return -EBUSY;
	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(net->dev_addr, addr->sa_data, ETH_ALEN);

	/* Set the MAC address */
	return ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_NODE_ID, ETH_ALEN,
				 ETH_ALEN, net->dev_addr);
}

static inline struct net_device_stats *ax88179_get_stats(struct net_device *dev)
{
	return &dev->stats;
}

static void read_bulk_callback(struct urb *urb)
{
	struct net_device *netdev;
	int status = urb->status;
	struct rx_agg *agg;
	struct ax88179 *dev;

	agg = urb->context;
	if (!agg)
		return;

	dev = agg->context;
	if (!dev)
		return;

	if (test_bit(AX88179_UNPLUG, &dev->flags))
		return;

	if (!test_bit(WORK_ENABLE, &dev->flags))
		return;

	netdev = dev->netdev;

	/* When link down, the driver would cancel all bulks. */
	/* This avoid the re-submitting bulk */
	if (!netif_carrier_ok(netdev))
		return;

	usb_mark_last_busy(dev->udev);

	switch (status) {
	case 0:
		if (urb->actual_length < ETH_ZLEN)
			break;

		spin_lock(&dev->rx_lock);
		list_add_tail(&agg->list, &dev->rx_done);
		spin_unlock(&dev->rx_lock);
		napi_schedule(&dev->napi);
		return;
	case -ESHUTDOWN:
		ax_set_unplug(dev);
		netif_device_detach(dev->netdev);
		return;
	case -ENOENT:
		return;	/* the urb is in unlink state */
	case -ETIME:
		if (net_ratelimit())
			netif_warn(dev, rx_err, netdev,
				   "maybe reset is needed?\n");
		break;
	default:
		if (net_ratelimit())
			netif_warn(dev, rx_err, netdev,
				   "Rx status %d\n", status);
		break;
	}

	ax88179_submit_rx(dev, agg, GFP_ATOMIC);
}

static void write_bulk_callback(struct urb *urb)
{
	struct net_device_stats *stats;
	struct net_device *netdev;
	struct tx_agg *agg;
	struct ax88179 *dev;
	int status = urb->status;

	agg = urb->context;
	if (!agg)
		return;

	dev = agg->context;
	if (!dev)
		return;

	netdev = dev->netdev;
	stats = ax88179_get_stats(netdev);
	if (status) {
		if (net_ratelimit())
			netif_warn(dev, tx_err, netdev,
				   "Tx status %d\n", status);
		stats->tx_errors += agg->skb_num;
	} else {
		stats->tx_packets += agg->skb_num;
		stats->tx_bytes += agg->skb_len;
	}

	spin_lock(&dev->tx_lock);
	list_add_tail(&agg->list, &dev->tx_free);
	spin_unlock(&dev->tx_lock);

	usb_autopm_put_interface_async(dev->intf);

	if (!netif_carrier_ok(netdev))
		return;

	if (!test_bit(WORK_ENABLE, &dev->flags))
		return;

	if (test_bit(AX88179_UNPLUG, &dev->flags))
		return;

	if (!skb_queue_empty(&dev->tx_queue))
		napi_schedule(&dev->napi);
}

static void intr_callback(struct urb *urb)
{
	struct ax88179 *dev;
	struct ax88179_int_data *event = NULL;
	int status = urb->status;
	int res;

	dev = urb->context;
	if (!dev)
		return;

	if (!test_bit(WORK_ENABLE, &dev->flags))
		return;

	if (test_bit(AX88179_UNPLUG, &dev->flags))
		return;

	switch (status) {
	case 0:			/* success */
		break;
	case -ECONNRESET:	/* unlink */
	case -ESHUTDOWN:
		netif_device_detach(dev->netdev);
	case -ENOENT:
	case -EPROTO:
		netif_info(dev, intr, dev->netdev,
			   "Stop submitting intr, status %d\n", status);
		return;
	case -EOVERFLOW:
		netif_info(dev, intr, dev->netdev, "intr status -EOVERFLOW\n");
		goto resubmit;
	/* -EPIPE:  should clear the halt */
	default:
		netif_info(dev, intr, dev->netdev, "intr status %d\n", status);
		goto resubmit;
	}

	event = urb->transfer_buffer;
	dev->link = event->link & AX_INT_PPLS_LINK;

	if (dev->link) {
		if (!netif_carrier_ok(dev->netdev)) { //Link up
			set_bit(AX88179_LINK_CHG, &dev->flags);
			schedule_delayed_work(&dev->schedule, 0);
		}
	} else {
		if (netif_carrier_ok(dev->netdev)) { //Link down
			netif_stop_queue(dev->netdev);
			set_bit(AX88179_LINK_CHG, &dev->flags);
			schedule_delayed_work(&dev->schedule, 0);
		}
	}

resubmit:
	res = usb_submit_urb(urb, GFP_ATOMIC);
	if (res == -ENODEV) {
		ax_set_unplug(dev);
		netif_device_detach(dev->netdev);
	} else if (res) {
		netif_err(dev, intr, dev->netdev,
			  "can't resubmit intr, status %d\n", res);
	}
}

static inline void *rx_agg_align(void *data)
{
	return (void *)ALIGN((uintptr_t)data, RX_ALIGN);
}

static inline void *tx_agg_align(void *data)
{
	return (void *)ALIGN((uintptr_t)data, TX_ALIGN);
}

static void free_all_mem(struct ax88179 *dev)
{
	int i;

	for (i = 0; i < AX88179_MAX_RX; i++) {
		usb_free_urb(dev->rx_info[i].urb);
		dev->rx_info[i].urb = NULL;

		kfree(dev->rx_info[i].buffer);
		dev->rx_info[i].buffer = NULL;
		dev->rx_info[i].head = NULL;
	}

	for (i = 0; i < AX88179_MAX_TX; i++) {
		usb_free_urb(dev->tx_info[i].urb);
		dev->tx_info[i].urb = NULL;

		kfree(dev->tx_info[i].buffer);
		dev->tx_info[i].buffer = NULL;
		dev->tx_info[i].head = NULL;
	}

	usb_free_urb(dev->intr_urb);
	dev->intr_urb = NULL;

	kfree(dev->intr_buff);
	dev->intr_buff = NULL;
}

static int alloc_all_mem(struct ax88179 *dev)
{
	struct net_device *netdev = dev->netdev;
	struct usb_interface *intf = dev->intf;
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct usb_host_endpoint *ep_intr = alt->endpoint;
	struct urb *urb;
	int node, i;
	u8 *buf;

	node = netdev->dev.parent ? dev_to_node(netdev->dev.parent) : -1;

	spin_lock_init(&dev->rx_lock);
	spin_lock_init(&dev->tx_lock);
	INIT_LIST_HEAD(&dev->tx_free);
	INIT_LIST_HEAD(&dev->rx_done);
	skb_queue_head_init(&dev->tx_queue);
	skb_queue_head_init(&dev->rx_queue);
	skb_queue_head_init(&dev->tx_done);

	for (i = 0; i < AX88179_MAX_RX; i++) {
		buf = kmalloc_node(agg_buf_sz_rx, GFP_KERNEL, node);
		if (!buf)
			goto err1;

		if (buf != rx_agg_align(buf)) {
			kfree(buf);
			buf = kmalloc_node(agg_buf_sz_rx + RX_ALIGN, GFP_KERNEL,
					   node);
			if (!buf)
				goto err1;
		}

		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb) {
			kfree(buf);
			goto err1;
		}

		INIT_LIST_HEAD(&dev->rx_info[i].list);
		dev->rx_info[i].context = dev;
		dev->rx_info[i].urb = urb;
		dev->rx_info[i].buffer = buf;
		dev->rx_info[i].head = rx_agg_align(buf);
	}
	for (i = 0; i < AX88179_MAX_TX; i++) {
		buf = kmalloc_node(agg_buf_sz_tx, GFP_KERNEL, node);
		if (!buf)
			goto err1;

		if (buf != tx_agg_align(buf)) {
			kfree(buf);
			buf = kmalloc_node(agg_buf_sz_tx + TX_ALIGN, GFP_KERNEL,
					   node);
			if (!buf)
				goto err1;
		}

		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb) {
			kfree(buf);
			goto err1;
		}

		INIT_LIST_HEAD(&dev->tx_info[i].list);
		dev->tx_info[i].context = dev;
		dev->tx_info[i].urb = urb;
		dev->tx_info[i].buffer = buf;
		dev->tx_info[i].head = tx_agg_align(buf);

		list_add_tail(&dev->tx_info[i].list, &dev->tx_free);
	}

	dev->intr_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->intr_urb)
		goto err1;

	dev->intr_buff = kmalloc(INTBUFSIZE, GFP_KERNEL);
	if (!dev->intr_buff)
		goto err1;

	dev->intr_interval = (int)ep_intr->desc.bInterval;
	usb_fill_int_urb(dev->intr_urb, dev->udev, usb_rcvintpipe(dev->udev, 1),
			 dev->intr_buff, INTBUFSIZE, intr_callback,
			 dev, dev->intr_interval);

	return 0;

err1:
	free_all_mem(dev);
	return -ENOMEM;
}

static struct tx_agg *ax88179_get_tx_agg(struct ax88179 *dev)
{
	struct tx_agg *agg = NULL;
	unsigned long flags;

	if (list_empty(&dev->tx_free))
		return NULL;

	spin_lock_irqsave(&dev->tx_lock, flags);
	if (!list_empty(&dev->tx_free)) {
		struct list_head *cursor;

		cursor = dev->tx_free.next;
		list_del_init(cursor);
		agg = list_entry(cursor, struct tx_agg, list);
	}
	spin_unlock_irqrestore(&dev->tx_lock, flags);

	return agg;
}

static int ax88179_tx_agg_fill(struct ax88179 *dev, struct tx_agg *agg)
{
	struct sk_buff_head skb_head, *tx_queue = &dev->tx_queue;
	int remain, ret;
	u8 *tx_data;

	__skb_queue_head_init(&skb_head);
	spin_lock(&tx_queue->lock);
	skb_queue_splice_init(tx_queue, &skb_head);
	spin_unlock(&tx_queue->lock);

	tx_data = agg->head;
	agg->skb_num = 0;
	agg->skb_len = 0;
	remain = agg_buf_sz_tx;

	while (remain >= ETH_ZLEN + 8) {
		struct sk_buff *skb;
		u32 tx_hdr;

		skb = __skb_dequeue(&skb_head);
		if (!skb)
			break;

		if ((skb->len + 8) > remain) {
			__skb_queue_head(&skb_head, skb);
			break;
		}

		tx_data = tx_agg_align(tx_data);

		tx_hdr = skb->len;
		cpu_to_le32s(&tx_hdr);
		memcpy(tx_data, &tx_hdr, 4);
		tx_data += 4;
		memset(tx_data, 0, 4);
		tx_data += 4;

		if (skb_copy_bits(skb, 0, tx_data, skb->len) < 0) {
			struct net_device_stats *stats = &dev->netdev->stats;

			stats->tx_dropped++;
			dev_kfree_skb_any(skb);
			continue;
		}

		tx_data += skb->len;
		agg->skb_len += skb->len;
		agg->skb_num++;

		dev_kfree_skb_any(skb);

		remain = agg_buf_sz_tx - (int)(tx_agg_align(tx_data)
					       - agg->head);
	}

	if (!skb_queue_empty(&skb_head)) {
		spin_lock(&tx_queue->lock);
		skb_queue_splice(&skb_head, tx_queue);
		spin_unlock(&tx_queue->lock);
	}

	netif_tx_lock(dev->netdev);

	if (netif_queue_stopped(dev->netdev) &&
	    skb_queue_len(&dev->tx_queue) < dev->tx_qlen)
		netif_wake_queue(dev->netdev);

	netif_tx_unlock(dev->netdev);

	ret = usb_autopm_get_interface_async(dev->intf);
	if (ret < 0)
		goto out_tx_fill;

	usb_fill_bulk_urb(agg->urb, dev->udev, usb_sndbulkpipe(dev->udev, 3),
			  agg->head, (int)(tx_data - (u8 *)agg->head),
			  (usb_complete_t)write_bulk_callback, agg);

	ret = usb_submit_urb(agg->urb, GFP_ATOMIC);
	if (ret < 0)
		usb_autopm_put_interface_async(dev->intf);

out_tx_fill:
	return ret;
}

static void
ax88179_rx_checksum(struct sk_buff *skb, u32 *pkt_hdr)
{
	skb->ip_summed = CHECKSUM_NONE;

	/* checksum error bit is set */
	if ((*pkt_hdr & AX_RXHDR_L3CSUM_ERR) ||
	    (*pkt_hdr & AX_RXHDR_L4CSUM_ERR))
		return;

	/* It must be a TCP or UDP packet with a valid checksum */
	if (((*pkt_hdr & AX_RXHDR_L4_TYPE_MASK) == AX_RXHDR_L4_TYPE_TCP) ||
	    ((*pkt_hdr & AX_RXHDR_L4_TYPE_MASK) == AX_RXHDR_L4_TYPE_UDP))
		skb->ip_summed = CHECKSUM_UNNECESSARY;
}

static int rx_bottom(struct ax88179 *dev, int budget)
{
	unsigned long flags;
	struct list_head *cursor, *next, rx_queue;
	int ret = 0, work_done = 0;
	struct napi_struct *napi = &dev->napi;

	if (!skb_queue_empty(&dev->rx_queue)) {
		while (work_done < budget) {
			struct sk_buff *skb = __skb_dequeue(&dev->rx_queue);
			struct net_device *netdev = dev->netdev;
			struct net_device_stats *stats;
			unsigned int pkt_len;

			if (!skb)
				break;

			pkt_len = skb->len;
			stats = ax88179_get_stats(netdev);
			napi_gro_receive(napi, skb);
			work_done++;
			stats->rx_packets++;
			stats->rx_bytes += pkt_len;
		}
	}

	if (list_empty(&dev->rx_done))
		goto out1;

	INIT_LIST_HEAD(&rx_queue);
	spin_lock_irqsave(&dev->rx_lock, flags);
	list_splice_init(&dev->rx_done, &rx_queue);
	spin_unlock_irqrestore(&dev->rx_lock, flags);

	list_for_each_safe(cursor, next, &rx_queue) {
		struct rx_agg *agg;
		struct urb *urb;
		u8 *rx_data;
		u32 rx_hdr = 0, *pkt_hdr = NULL;
		int pkt_cnt = 0;
		u16 hdr_off = 0;

		list_del_init(cursor);

		agg = list_entry(cursor, struct rx_agg, list);
		urb = agg->urb;
		if (urb->actual_length < ETH_ZLEN)
			goto submit;
		/* RX Desc */
		memcpy(&rx_hdr,
		       (agg->head + urb->actual_length - 4), sizeof(rx_hdr));
		le32_to_cpus(&rx_hdr);

		pkt_cnt = (u16)rx_hdr;
		hdr_off = (u16)(rx_hdr >> 16);
		pkt_hdr = (u32 *)(agg->head + hdr_off);

		rx_data = agg->head;

		while (pkt_cnt--) {
			struct net_device *netdev = dev->netdev;
			struct net_device_stats *stats;
			u16 pkt_len;
			struct sk_buff *skb;

			/* limite the skb numbers for rx_queue */
			if (unlikely(skb_queue_len(&dev->rx_queue) >= 1000))
				break;

			le32_to_cpus(pkt_hdr);
			pkt_len = (*pkt_hdr >> 16) & 0x1FFF;

			/* Check CRC or runt packet */
			if ((*pkt_hdr & AX_RXHDR_CRC_ERR) ||
			    (*pkt_hdr & AX_RXHDR_DROP_ERR))
				goto find_next_rx;

			stats = ax88179_get_stats(netdev);

			skb = napi_alloc_skb(napi, pkt_len);
			if (!skb) {
				stats->rx_dropped++;
				goto find_next_rx;
			}

			skb_put(skb, pkt_len);
			memcpy(skb->data, rx_data, pkt_len);

			ax88179_rx_checksum(skb, pkt_hdr);

			skb->protocol = eth_type_trans(skb, netdev);

			if (work_done < budget) {
				napi_gro_receive(napi, skb);
				work_done++;
				stats->rx_packets++;
				stats->rx_bytes += pkt_len;
			} else {
				__skb_queue_tail(&dev->rx_queue, skb);
			}

			if (pkt_cnt == 0)
				break;

find_next_rx:
			rx_data += (pkt_len + 7) & 0xFFF8;
			pkt_hdr++;
		}

submit:
		if (!ret) {
			ret = ax88179_submit_rx(dev, agg, GFP_ATOMIC);
		} else {
			urb->actual_length = 0;
			list_add_tail(&agg->list, next);
		}
	}

	if (!list_empty(&rx_queue)) {
		spin_lock_irqsave(&dev->rx_lock, flags);
		list_splice_tail(&rx_queue, &dev->rx_done);
		spin_unlock_irqrestore(&dev->rx_lock, flags);
	}

out1:
	return work_done;
}

static void tx_bottom(struct ax88179 *dev)
{
	int res;

	do {
		struct tx_agg *agg;

		if (skb_queue_empty(&dev->tx_queue))
			break;

		agg = ax88179_get_tx_agg(dev);
		if (!agg)
			break;

		res = ax88179_tx_agg_fill(dev, agg);
		if (res) {
			struct net_device *netdev = dev->netdev;

			if (res == -ENODEV) {
				ax_set_unplug(dev);
				netif_device_detach(netdev);
			} else {
				struct net_device_stats *stats;
				unsigned long flags;

				stats = ax88179_get_stats(netdev);
				stats->tx_dropped += agg->skb_num;

				spin_lock_irqsave(&dev->tx_lock, flags);
				list_add_tail(&agg->list, &dev->tx_free);
				spin_unlock_irqrestore(&dev->tx_lock, flags);
			}
		}
	} while (res == 0);
}

static void bottom_half(struct ax88179 *dev)
{
	if (test_bit(AX88179_UNPLUG, &dev->flags))
		return;

	if (!test_bit(WORK_ENABLE, &dev->flags))
		return;

	/* When link down, the driver would cancel all bulks. */
	/* This avoid the re-submitting bulk */
	if (!netif_carrier_ok(dev->netdev))
		return;

	clear_bit(SCHEDULE_NAPI, &dev->flags);

	tx_bottom(dev);
}

static inline int __ax88179_poll(struct ax88179 *dev, int budget)
{
	struct napi_struct *napi = &dev->napi;
	int work_done;
	struct sk_buff		*skb;
	struct skb_data		*entry;

	work_done = rx_bottom(dev, budget);
	bottom_half(dev);

	while ((skb = skb_dequeue(&dev->tx_done)) != NULL) {
		entry = (struct skb_data *)skb->cb;
		usb_free_urb(entry->urb);
		dev_kfree_skb(skb);
	}

	if (work_done < budget) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		napi_complete_done(napi, work_done);
#else
		if (!napi_complete_done(napi, work_done))
			goto out;
#endif
		if (!list_empty(&dev->rx_done))
			napi_schedule(napi);
		else if (!skb_queue_empty(&dev->tx_queue) &&
			 !list_empty(&dev->tx_free))
			napi_schedule(napi);
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
out:
#endif
	return work_done;
}

static int ax88179_poll(struct napi_struct *napi, int budget)
{
	struct ax88179 *dev = container_of(napi, struct ax88179, napi);

	return __ax88179_poll(dev, budget);
}

static
int ax88179_submit_rx(struct ax88179 *dev, struct rx_agg *agg, gfp_t mem_flags)
{
	int ret;

	/* The rx would be stopped, so skip submitting */
	if (test_bit(AX88179_UNPLUG, &dev->flags) ||
	    !test_bit(WORK_ENABLE, &dev->flags) ||
	    !netif_carrier_ok(dev->netdev))
		return 0;

	usb_fill_bulk_urb(agg->urb, dev->udev, usb_rcvbulkpipe(dev->udev, 2),
			  agg->head, agg_buf_sz_rx,
			  (usb_complete_t)read_bulk_callback, agg);

	ret = usb_submit_urb(agg->urb, mem_flags);
	if (ret == -ENODEV) {
		ax_set_unplug(dev);
		netif_device_detach(dev->netdev);
	} else if (ret) {
		struct urb *urb = agg->urb;
		unsigned long flags;

		urb->actual_length = 0;
		spin_lock_irqsave(&dev->rx_lock, flags);
		list_add_tail(&agg->list, &dev->rx_done);
		spin_unlock_irqrestore(&dev->rx_lock, flags);

		netif_err(dev, rx_err, dev->netdev,
			  "Couldn't submit rx[%p], ret = %d\n", agg, ret);

		napi_schedule(&dev->napi);
	}

	return ret;
}

static void ax_drop_queued_tx(struct ax88179 *dev)
{
	struct net_device_stats *stats = ax88179_get_stats(dev->netdev);
	struct sk_buff_head skb_head, *tx_queue = &dev->tx_queue;
	struct sk_buff *skb;

	if (skb_queue_empty(tx_queue))
		return;

	__skb_queue_head_init(&skb_head);
	spin_lock_bh(&tx_queue->lock);
	skb_queue_splice_init(tx_queue, &skb_head);
	spin_unlock_bh(&tx_queue->lock);

	while ((skb = __skb_dequeue(&skb_head))) {
		dev_kfree_skb(skb);
		stats->tx_dropped++;
	}
}

static void ax88179_tx_timeout(struct net_device *netdev)
{
	struct ax88179 *dev = netdev_priv(netdev);

	netif_warn(dev, tx_err, netdev, "Tx timeout\n");

	usb_queue_reset_device(dev->intf);
}

static void tx_complete(struct urb *urb)
{
	struct sk_buff		*skb = (struct sk_buff *)urb->context;
	struct skb_data		*entry = (struct skb_data *)skb->cb;
	struct ax88179		*dev = entry->dev;

	if (urb->status == 0) {
		dev->netdev->stats.tx_packets++;
		dev->netdev->stats.tx_bytes += entry->length;
	} else {
		dev->netdev->stats.tx_errors++;
	}

	usb_autopm_put_interface_async(dev->intf);
	skb_queue_tail(&dev->tx_done, skb);
}

static struct sk_buff *
ax88179_tx_fixup(struct ax88179 *dev, struct sk_buff *skb, gfp_t flags)
{
	u32 tx_hdr1 = 0, tx_hdr2 = 0;
	int headroom = 0, tailroom = 0;

	tx_hdr1 = skb->len;
	tx_hdr2 = skb_shinfo(skb)->gso_size;

	if ((dev->netdev->features & NETIF_F_SG) && skb_linearize(skb))
		return NULL;

	headroom = skb_headroom(skb);
	tailroom = skb_tailroom(skb);

	if ((headroom + tailroom) >= 8) {
		if (headroom < 8) {
			skb->data = memmove(skb->head + 8, skb->data, skb->len);
			skb_set_tail_pointer(skb, skb->len);
		}
	} else {
		struct sk_buff *skb2 = NULL;

		skb2 = skb_copy_expand(skb, 8, 0, flags);
		dev_kfree_skb_any(skb);
		skb = skb2;
		if (!skb)
			return NULL;
	}

	skb_push(skb, 4);
	cpu_to_le32s(&tx_hdr2);
	skb_copy_to_linear_data(skb, &tx_hdr2, 4);

	skb_push(skb, 4);
	cpu_to_le32s(&tx_hdr1);
	skb_copy_to_linear_data(skb, &tx_hdr1, 4);

	return skb;
}

netdev_tx_t ax88179_lso_xmit(struct sk_buff *skb,
			     struct net_device *net)
{
	struct ax88179		*dev = netdev_priv(net);
	int			length;
	struct urb		*urb = NULL;
	struct skb_data		*entry;
	int retval;

	skb = ax88179_tx_fixup(dev, skb, GFP_ATOMIC);
	if (!skb)
		goto drop;

	length = skb->len;
	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb) {
		netif_dbg(dev, tx_err, dev->netdev, "no urb\n");
		goto drop;
	}

	entry = (struct skb_data *)skb->cb;
	entry->urb = urb;
	entry->dev = dev;
	entry->length = length;

	usb_fill_bulk_urb(urb, dev->udev, usb_sndbulkpipe(dev->udev, 3),
			  skb->data, skb->len, tx_complete, skb);

	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval < 0)
		usb_autopm_put_interface_async(dev->intf);

	if (retval) {
drop:
		dev->netdev->stats.tx_dropped++;
		if (skb)
			dev_kfree_skb_any(skb);
		if (urb)
			usb_free_urb(urb);
	} else {
		netif_dbg(dev, tx_queued, dev->netdev,
			  "> tx, len %d, type 0x%x\n", length, skb->protocol);
	}
	return NETDEV_TX_OK;
}

static netdev_tx_t ax88179_start_xmit(struct sk_buff *skb,
				      struct net_device *netdev)
{
	struct ax88179 *dev = netdev_priv(netdev);

	if (skb)
		skb_tx_timestamp(skb);

	if (skb_shinfo(skb)->gso_size > 0)
		return ax88179_lso_xmit(skb, netdev);

	skb_queue_tail(&dev->tx_queue, skb);

	if (!list_empty(&dev->tx_free)) {
		usb_mark_last_busy(dev->udev);
		napi_schedule(&dev->napi);
	} else if (skb_queue_len(&dev->tx_queue) > dev->tx_qlen) {
		netif_stop_queue(netdev);
	}

	return NETDEV_TX_OK;
}

static void set_tx_qlen(struct ax88179 *dev)
{
	struct net_device *netdev = dev->netdev;

	dev->tx_qlen = agg_buf_sz_tx / (netdev->mtu + ETH_FCS_LEN + 8);
}

static int ax_start_rx(struct ax88179 *dev)
{
	int i, ret = 0;

	INIT_LIST_HEAD(&dev->rx_done);
	for (i = 0; i < AX88179_MAX_RX; i++) {
		INIT_LIST_HEAD(&dev->rx_info[i].list);
		ret = ax88179_submit_rx(dev, &dev->rx_info[i], GFP_KERNEL);
		if (ret)
			break;
	}

	if (ret && ++i < AX88179_MAX_RX) {
		struct list_head rx_queue;
		unsigned long flags;

		INIT_LIST_HEAD(&rx_queue);

		do {
			struct rx_agg *agg = &dev->rx_info[i++];
			struct urb *urb = agg->urb;

			urb->actual_length = 0;
			list_add_tail(&agg->list, &rx_queue);
		} while (i < AX88179_MAX_RX);

		spin_lock_irqsave(&dev->rx_lock, flags);
		list_splice_tail(&rx_queue, &dev->rx_done);
		spin_unlock_irqrestore(&dev->rx_lock, flags);
	}

	return ret;
}

static int ax_stop_rx(struct ax88179 *dev)
{
	int i;

	for (i = 0; i < AX88179_MAX_RX; i++)
		usb_kill_urb(dev->rx_info[i].urb);

	while (!skb_queue_empty(&dev->rx_queue))
		dev_kfree_skb(__skb_dequeue(&dev->rx_queue));

	return 0;
}

static void ax_disable(struct ax88179 *dev)
{
	int i;

	if (test_bit(AX88179_UNPLUG, &dev->flags)) {
		ax_drop_queued_tx(dev);
		return;
	}

	ax_drop_queued_tx(dev);

	for (i = 0; i < AX88179_MAX_TX; i++)
		usb_kill_urb(dev->tx_info[i].urb);

	ax_stop_rx(dev);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 39)
static int
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0)
ax88179_set_features(struct net_device *net, netdev_features_t features)
#else
ax88179_set_features(struct net_device *net, u32 features)
#endif

{
	u8 *tmp8;
	struct ax88179 *dev = netdev_priv(net);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0)
	netdev_features_t changed = net->features ^ features;
#else
	u32 changed = net->features ^ features;
#endif

	tmp8 = kmalloc(1, GFP_KERNEL);
	if (!tmp8)
		return -ENOMEM;

	if (changed & NETIF_F_IP_CSUM) {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_TXCOE_CTL,
				 1, 1, tmp8, 0);
		*tmp8 ^= AX_TXCOE_TCP | AX_TXCOE_UDP;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_TXCOE_CTL, 1, 1, tmp8);
	}

	if (changed & NETIF_F_IPV6_CSUM) {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_TXCOE_CTL,
				 1, 1, tmp8, 0);
		*tmp8 ^= AX_TXCOE_TCPV6 | AX_TXCOE_UDPV6;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_TXCOE_CTL, 1, 1, tmp8);
	}

	if (changed & NETIF_F_RXCSUM) {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_RXCOE_CTL,
				 1, 1, tmp8, 0);
		*tmp8 ^= AX_RXCOE_IP | AX_RXCOE_TCP | AX_RXCOE_UDP |
		       AX_RXCOE_TCPV6 | AX_RXCOE_UDPV6;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RXCOE_CTL, 1, 1, tmp8);
	}

	kfree(tmp8);

	return 0;
}
#endif

static int ax88179_link_reset(struct ax88179 *dev)
{
	u8 reg8[5], link_sts;
	u16 mode, reg16, delay = 10 * HZ;
	u32 reg32;
	unsigned long jtimeout = 0;

	mode = AX_MEDIUM_TXFLOW_CTRLEN | AX_MEDIUM_RXFLOW_CTRLEN;

	ax88179_read_cmd(dev, AX_ACCESS_MAC, PHYSICAL_LINK_STATUS,
			 1, 1, &link_sts, 0);

	jtimeout = jiffies + delay;
	while (time_before(jiffies, jtimeout)) {
		ax88179_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				 GMII_PHY_PHYSR, 2, &reg16, 1);

		if (reg16 & GMII_PHY_PHYSR_LINK)
			break;
	}

	if (!(reg16 & GMII_PHY_PHYSR_LINK)) {
		return 0;
	} else if (GMII_PHY_PHYSR_GIGA == (reg16 & GMII_PHY_PHYSR_SMASK)) {
		mode |= AX_MEDIUM_GIGAMODE;
		if (dev->netdev->mtu > 1500)
			mode |= AX_MEDIUM_JUMBO_EN;

		if (link_sts & AX_USB_SS)
			memcpy(reg8, &AX88179_BULKIN_SIZE[0], 5);
		else if (link_sts & AX_USB_HS)
			memcpy(reg8, &AX88179_BULKIN_SIZE[1], 5);
		else
			memcpy(reg8, &AX88179_BULKIN_SIZE[3], 5);
	} else if (GMII_PHY_PHYSR_100 == (reg16 & GMII_PHY_PHYSR_SMASK)) {
		mode |= AX_MEDIUM_PS;	/* Bit 9 : PS */
		if (link_sts & (AX_USB_SS | AX_USB_HS))
			memcpy(reg8, &AX88179_BULKIN_SIZE[2], 5);
		else
			memcpy(reg8, &AX88179_BULKIN_SIZE[3], 5);
	} else {
		memcpy(reg8, &AX88179_BULKIN_SIZE[3], 5);
	}
	/* RX bulk configuration */
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, 5, reg8);

	if (reg16 & GMII_PHY_PHYSR_FULL)
		mode |= AX_MEDIUM_FULL_DUPLEX;	/* Bit 1 : FD */
	netdev_info(dev->netdev, "Write medium type: 0x%04x\n", mode);

	ax88179_read_cmd(dev, 0x81, 0x8c, 0, 4, &reg32, 1);
	delay = HZ / 2;
	if (reg32 & 0x40000000) {
		u16 temp16 = 0;

		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &temp16);

		/* Configure default medium type => giga */
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
				  2, 2, &mode);

		jtimeout = jiffies + delay;

		while (time_before(jiffies, jtimeout)) {
			ax88179_read_cmd(dev, 0x81, 0x8c, 0, 4, &reg32, 1);

			if (!(reg32 & 0x40000000))
				break;

			reg32 = 0x80000000;
			ax88179_write_cmd(dev, 0x81, 0x8c, 0, 4, &reg32);
		}

		temp16 = AX_RX_CTL_DROPCRCERR | AX_RX_CTL_START | AX_RX_CTL_AP |
			AX_RX_CTL_AMALL | AX_RX_CTL_AB;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL,
				  2, 2, &temp16);
	}

	mode |= AX_MEDIUM_RECEIVE_EN;

	/* Configure default medium type => giga */
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
			  2, 2, &mode);
	mii_check_media(&dev->mii, 1, 1);

	return 0;
}

static void set_carrier(struct ax88179 *dev)
{
	struct net_device *netdev = dev->netdev;
	struct napi_struct *napi = &dev->napi;

	if (dev->link) {
		if (!netif_carrier_ok(netdev)) {
			ax88179_link_reset(dev);

			netif_stop_queue(netdev);
			napi_disable(napi);
			netif_carrier_on(netdev);
			ax_start_rx(dev);
			napi_enable(napi);
			netif_wake_queue(netdev);
		} else if (netif_queue_stopped(netdev) &&
			   skb_queue_len(&dev->tx_queue) < dev->tx_qlen) {
			netif_wake_queue(netdev);
		}
	} else {
		if (netif_carrier_ok(netdev)) {
			netif_carrier_off(netdev);
			napi_disable(napi);
			ax_disable(dev);
			napi_enable(napi);
			netif_info(dev, link, netdev, "link down\n");
		}
	}
}

static inline void __ax_work_func(struct ax88179 *dev)
{
	/* If the device is unplugged or !netif_running(), the workqueue
	 * doesn't need to wake the device, and could return directly.
	 */
	if (test_bit(AX88179_UNPLUG, &dev->flags) ||
	    !netif_running(dev->netdev))
		return;

	if (usb_autopm_get_interface(dev->intf) < 0)
		return;

	if (!test_bit(WORK_ENABLE, &dev->flags))
		goto out1;

	if (!mutex_trylock(&dev->control)) {
		schedule_delayed_work(&dev->schedule, 0);
		goto out1;
	}

	if (test_and_clear_bit(AX88179_LINK_CHG, &dev->flags))
		set_carrier(dev);

	/* don't schedule napi before linking */
	if (test_and_clear_bit(SCHEDULE_NAPI, &dev->flags) &&
	    netif_carrier_ok(dev->netdev))
		napi_schedule(&dev->napi);

	mutex_unlock(&dev->control);

out1:
	usb_autopm_put_interface(dev->intf);
}

static void ax_work_func_t(struct work_struct *work)
{
	struct ax88179 *dev = container_of(work, struct ax88179, schedule.work);

	__ax_work_func(dev);
}

static int ax88179_bind(struct ax88179 *dev);

static int ax88179_open(struct net_device *netdev)
{
	struct ax88179 *dev = netdev_priv(netdev);
	int res = 0;

	res = alloc_all_mem(dev);
	if (res)
		goto out;

	res = usb_autopm_get_interface(dev->intf);
	if (res < 0)
		goto out_free;

	mutex_lock(&dev->control);

	res = ax88179_bind(dev);
	if (res < 0)
		goto out_free;

	netif_carrier_off(netdev);
	netif_start_queue(netdev);
	/* Memory barrier
	 */
	smp_mb__before_atomic();
	set_bit(WORK_ENABLE, &dev->flags);
	/* Memory barrier
	 */
	smp_mb__after_atomic();

	set_tx_qlen(dev);

	res = usb_submit_urb(dev->intr_urb, GFP_KERNEL);
	if (res) {
		if (res == -ENODEV)
			netif_device_detach(dev->netdev);
		netif_warn(dev, ifup, netdev, "intr_urb submit failed: %d\n",
			   res);
		goto out_unlock;
	}
	napi_enable(&dev->napi);

	mutex_unlock(&dev->control);

	usb_autopm_put_interface(dev->intf);

	return 0;

out_unlock:
	mutex_unlock(&dev->control);
	usb_autopm_put_interface(dev->intf);
out_free:
	free_all_mem(dev);
out:
	return res;
}

static int ax88179_close(struct net_device *netdev)
{
	struct ax88179 *dev = netdev_priv(netdev);
	u16 reg16;
	int res = 0;

	netif_carrier_off(netdev);

	ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
			 2, 2, &reg16, 1);
	reg16 &= ~AX_MEDIUM_RECEIVE_EN;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
			  2, 2, &reg16);

	napi_disable(&dev->napi);
	/* Memory barrier
	 */
	smp_mb__before_atomic();
	clear_bit(WORK_ENABLE, &dev->flags);
	/* Memory barrier
	 */
	smp_mb__after_atomic();
	usb_kill_urb(dev->intr_urb);
	cancel_delayed_work_sync(&dev->schedule);
	netif_stop_queue(netdev);

	res = usb_autopm_get_interface(dev->intf);
	if (res < 0 || test_bit(AX88179_UNPLUG, &dev->flags)) {
		ax_drop_queued_tx(dev);
		ax_stop_rx(dev);
	}

	ax_disable(dev);

	free_all_mem(dev);

	return res;
}

static int ax88179_pre_reset(struct usb_interface *intf)
{
	struct ax88179 *dev = usb_get_intfdata(intf);
	struct net_device *netdev;

	if (!dev)
		return 0;

	netdev = dev->netdev;
	if (!netif_running(netdev))
		return 0;

	netif_stop_queue(netdev);
	napi_disable(&dev->napi);
	/* Memory barrier
	 */
	smp_mb__before_atomic();
	clear_bit(WORK_ENABLE, &dev->flags);
	/* Memory barrier
	 */
	smp_mb__after_atomic();
	usb_kill_urb(dev->intr_urb);
	cancel_delayed_work_sync(&dev->schedule);

	return 0;
}

static int ax88179_post_reset(struct usb_interface *intf)
{
	struct ax88179 *dev = usb_get_intfdata(intf);
	struct net_device *netdev;

	if (!dev)
		return 0;

	netdev = dev->netdev;
	if (!netif_running(netdev))
		return 0;
	/* Memory barrier
	 */
	smp_mb__before_atomic();
	set_bit(WORK_ENABLE, &dev->flags);
	/* Memory barrier
	 */
	smp_mb__after_atomic();
	if (netif_carrier_ok(netdev)) {
		mutex_lock(&dev->control);
		ax_start_rx(dev);
		mutex_unlock(&dev->control);
	}

	napi_enable(&dev->napi);
	netif_wake_queue(netdev);
	usb_submit_urb(dev->intr_urb, GFP_KERNEL);

	if (!list_empty(&dev->rx_done))
		napi_schedule(&dev->napi);

	return 0;
}

static int ax88179_system_resume(struct ax88179 *dev)
{
	struct net_device *netdev = dev->netdev;

	netif_device_attach(netdev);

	if (netif_running(netdev) && (netdev->flags & IFF_UP)) {
		u16 reg16;
		u8 reg8;

		netif_carrier_off(netdev);

		/* Power up ethernet PHY */
		reg16 = 0;
		ax88179_write_cmd_nopm(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL,
				       2, 2, &reg16);
		usleep_range(1000, 2000);
		reg16 = AX_PHYPWR_RSTCTL_IPRL;
		ax88179_write_cmd_nopm(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL,
				       2, 2, &reg16);
		msleep(200);

		/* change clock */
		ax88179_read_cmd_nopm(dev, AX_ACCESS_MAC,  AX_CLK_SELECT,
				      1, 1, &reg8, 0);
		reg8 |= AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS;
		ax88179_write_cmd_nopm(dev, AX_ACCESS_MAC,
				       AX_CLK_SELECT, 1, 1, &reg8);
		msleep(100);

		/* Configure RX control register => start operation */
		reg16 = AX_RX_CTL_DROPCRCERR | AX_RX_CTL_START | AX_RX_CTL_AP |
			 AX_RX_CTL_AMALL | AX_RX_CTL_AB;
		ax88179_write_cmd_nopm(dev, AX_ACCESS_MAC,
				       AX_RX_CTL, 2, 2, &reg16);
		/* memory Barrier
		 */
		smp_mb__before_atomic();
		set_bit(WORK_ENABLE, &dev->flags);
		/* Memory Barrier
		 */
		smp_mb__after_atomic();

		usb_submit_urb(dev->intr_urb, GFP_NOIO);
	}

	return 0;
}

static int ax88179_system_suspend(struct ax88179 *dev)
{
	struct net_device *netdev = dev->netdev;
	int ret = 0;

	netif_device_detach(netdev);

	if (netif_running(netdev) && test_bit(WORK_ENABLE, &dev->flags)) {
		struct napi_struct *napi = &dev->napi;
		u16 reg16;
		u8 reg8;

		/* Memory barrier
		 */
		smp_mb__before_atomic();
		clear_bit(WORK_ENABLE, &dev->flags);
		/* Memory barrier
		 */
		smp_mb__after_atomic();
		usb_kill_urb(dev->intr_urb);
		ax_disable(dev);

		/* Disable RX path */
		ax88179_read_cmd_nopm(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
				      2, 2, &reg16, 1);
		reg16 &= ~AX_MEDIUM_RECEIVE_EN;
		ax88179_write_cmd_nopm(dev, AX_ACCESS_MAC,
				       AX_MEDIUM_STATUS_MODE, 2, 2, &reg16);

		ax88179_read_cmd_nopm(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL,
				      2, 2, &reg16, 1);
		reg16 |= AX_PHYPWR_RSTCTL_IPRL;
		ax88179_write_cmd_nopm(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL,
				       2, 2, &reg16);

		/* change clock */
		reg8 = 0;
		ax88179_write_cmd_nopm(dev, AX_ACCESS_MAC, AX_CLK_SELECT,
				       1, 1, &reg8);

		/* Configure RX control register => stop operation */
		reg16 = AX_RX_CTL_STOP;
		ax88179_write_cmd_nopm(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2,
				       &reg16);

		napi_disable(napi);
		cancel_delayed_work_sync(&dev->schedule);
		napi_enable(napi);
	}

	return ret;
}

static int ax88179_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct ax88179 *dev = usb_get_intfdata(intf);
	int ret;

	mutex_lock(&dev->control);

	if (PMSG_IS_AUTO(message))
		return -EBUSY;
	else
		ret = ax88179_system_suspend(dev);

	mutex_unlock(&dev->control);
	return 0;
}

static int ax88179_resume(struct usb_interface *intf)
{
	struct ax88179 *dev = usb_get_intfdata(intf);
	int ret;

	mutex_lock(&dev->control);

	ret = ax88179_system_resume(dev);

	mutex_unlock(&dev->control);

	return ret;
}

static void
ax88179_get_wol(struct net_device *net, struct ethtool_wolinfo *wolinfo)
{
	struct ax88179 *dev = netdev_priv(net);
	u8 reg8;

	if (ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MONITOR_MODE,
			     1, 1, &reg8, 0) < 0) {
		wolinfo->supported = 0;
		wolinfo->wolopts = 0;
		return;
	}

	wolinfo->supported = WAKE_PHY | WAKE_MAGIC;

	if (reg8 & AX_MONITOR_MODE_RWLC)
		wolinfo->wolopts |= WAKE_PHY;
	if (reg8 & AX_MONITOR_MODE_RWMP)
		wolinfo->wolopts |= WAKE_MAGIC;
}

static int
ax88179_set_wol(struct net_device *net, struct ethtool_wolinfo *wolinfo)
{
	struct ax88179 *dev = netdev_priv(net);
	u8 reg8 = 0;

	if (wolinfo->wolopts & WAKE_PHY)
		reg8 |= AX_MONITOR_MODE_RWLC;
	else
		reg8 &= ~AX_MONITOR_MODE_RWLC;

	if (wolinfo->wolopts & WAKE_MAGIC)
		reg8 |= AX_MONITOR_MODE_RWMP;
	else
		reg8 &= ~AX_MONITOR_MODE_RWMP;

	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MONITOR_MODE, 1, 1, &reg8);

	return 0;
}

static u32 ax88179_get_msglevel(struct net_device *netdev)
{
	struct ax88179 *dev = netdev_priv(netdev);

	return dev->msg_enable;
}

static void ax88179_set_msglevel(struct net_device *netdev, u32 value)
{
	struct ax88179 *dev = netdev_priv(netdev);

	dev->msg_enable = value;
}

static void ax88179_get_drvinfo(struct net_device *net,
				struct ethtool_drvinfo *info)
{
	struct ax88179 *dev = netdev_priv(net);

	strlcpy(info->driver, MODULENAME, sizeof(info->driver));
	strlcpy(info->version, DRIVER_VERSION, sizeof(info->version));
	usb_make_path(dev->udev, info->bus_info, sizeof(info->bus_info));

	info->eedump_len = 0x3e;
}

static int ax88179_get_settings(struct net_device *net, struct ethtool_cmd *cmd)
{
	struct ax88179 *dev = netdev_priv(net);

	return mii_ethtool_gset(&dev->mii, cmd);
}

static int ax88179_set_settings(struct net_device *net, struct ethtool_cmd *cmd)
{
	struct ax88179 *dev = netdev_priv(net);

	return mii_ethtool_sset(&dev->mii, cmd);
}

static const struct ethtool_ops ops = {
	.get_drvinfo	= ax88179_get_drvinfo,
	.get_settings	= ax88179_get_settings,
	.set_settings	= ax88179_set_settings,
	.get_link	= ethtool_op_get_link,
	.get_msglevel	= ax88179_get_msglevel,
	.set_msglevel	= ax88179_set_msglevel,
	.get_wol	= ax88179_get_wol,
	.set_wol	= ax88179_set_wol,
};

static int ax88179_change_mtu(struct net_device *net, int new_mtu)
{
	struct ax88179 *dev = netdev_priv(net);
	u16 reg16;

	if (new_mtu <= 0 || new_mtu > 4088)
		return -EINVAL;

	net->mtu = new_mtu;

	if (net->mtu > 1500) {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
				 2, 2, &reg16, 1);
		reg16 |= AX_MEDIUM_JUMBO_EN;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
				  2, 2, &reg16);
	} else {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
				 2, 2, &reg16, 1);
		reg16 &= ~AX_MEDIUM_JUMBO_EN;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
				  2, 2, &reg16);
	}

	return 0;
}

static void ax88179_set_multicast(struct net_device *net)
{
	struct ax88179 *dev = netdev_priv(net);
	u8 *m_filter = dev->m_filter;
	int mc_count = 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
	mc_count = net->mc_count;
#else
	mc_count = netdev_mc_count(net);
#endif

	dev->rxctl = (AX_RX_CTL_START | AX_RX_CTL_AB);

	if (net->flags & IFF_PROMISC) {
		dev->rxctl |= AX_RX_CTL_PRO;
	} else if (net->flags & IFF_ALLMULTI || mc_count > AX_MAX_MCAST) {
		dev->rxctl |= AX_RX_CTL_AMALL;
	} else if (mc_count == 0) {
		/* just broadcast and directed */
	} else {
		/* We use the 20 byte dev->data
		 * for our 8 byte filter buffer
		 * to avoid allocating memory that
		 * is tricky to free later
		 */
		u32 crc_bits = 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
		struct dev_mc_list *mc_list = net->mc_list;
		int i = 0;

		memset(m_filter, 0, AX_MCAST_FILTER_SIZE);

		/* Build the multicast hash filter. */
		for (i = 0; i < net->mc_count; i++) {
			crc_bits =
			    ether_crc(ETH_ALEN,
				      mc_list->dmi_addr) >> 26;
			*(m_filter + (crc_bits >> 3)) |=
				1 << (crc_bits & 7);
			mc_list = mc_list->next;
		}
#else
		struct netdev_hw_addr *ha = NULL;

		memset(m_filter, 0, AX_MCAST_FILTER_SIZE);
		netdev_for_each_mc_addr(ha, net) {
			crc_bits = ether_crc(ETH_ALEN, ha->addr) >> 26;
			*(m_filter + (crc_bits >> 3)) |=
				1 << (crc_bits & 7);
		}
#endif
		ax88179_write_cmd_async(dev, AX_ACCESS_MAC,
					AX_MULTI_FILTER_ARRY,
					AX_MCAST_FILTER_SIZE,
					AX_MCAST_FILTER_SIZE, m_filter);

		dev->rxctl |= AX_RX_CTL_AM;
	}

	ax88179_write_cmd_async(dev, AX_ACCESS_MAC, AX_RX_CTL,
				2, 2, &dev->rxctl);
}

static int ax88179_ioctl(struct net_device *net, struct ifreq *rq, int cmd)
{
	struct ax88179 *dev = netdev_priv(net);

	return  generic_mii_ioctl(&dev->mii, if_mii(rq), cmd, NULL);
}

static const struct net_device_ops ax88179_netdev_ops = {
	.ndo_open		= ax88179_open,
	.ndo_stop		= ax88179_close,
	.ndo_do_ioctl		= ax88179_ioctl,
	.ndo_start_xmit		= ax88179_start_xmit,
	.ndo_tx_timeout		= ax88179_tx_timeout,
	.ndo_set_features	= ax88179_set_features,
	.ndo_set_rx_mode	= ax88179_set_multicast,
	.ndo_set_mac_address	= ax88179_set_mac_addr,
	.ndo_change_mtu		= ax88179_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
};

static int access_eeprom_mac(struct ax88179 *dev, u8 *buf, u8 offset, int wflag)
{
	int ret = 0, i;
	u16 *tmp = (u16 *)buf;

	for (i = 0; i < (ETH_ALEN >> 1); i++) {
		if (wflag) {
			u16 tmp16;

			tmp16 = cpu_to_le16(*(tmp + i));
			ret = ax88179_write_cmd(dev, AX_ACCESS_EEPROM,
						offset + i, 1, 2, &tmp16);
			if (ret < 0)
				break;

			mdelay(15);

		} else {
			ret = ax88179_read_cmd(dev, AX_ACCESS_EEPROM,
					       offset + i, 1, 2, tmp + i, 0);
			if (ret < 0)
				break;
		}
	}

	if (!wflag) {
		if (ret < 0) {
			netdev_dbg(dev->netdev,
				   "Failed to read MAC from EEPROM: %d\n", ret);
			return ret;
		}
		memcpy(dev->netdev->dev_addr, buf, ETH_ALEN);

	} else {
		/* reload eeprom data */
		ret = ax88179_write_cmd(dev,
					AX_RELOAD_EEPROM_EFUSE, 0, 0, 0, 0);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int ax88179_check_ether_addr(struct ax88179 *dev)
{
	unsigned char *tmp = (unsigned char *)dev->netdev->dev_addr;
	u8 default_mac[6] = {0, 0x0e, 0xc6, 0x81, 0x79, 0x01};
	u8 default_mac_178a[6] = {0, 0x0e, 0xc6, 0x81, 0x78, 0x01};

	if (((*((u8 *)tmp) == 0) && (*((u8 *)tmp + 1) == 0) &&
	     (*((u8 *)tmp + 2) == 0)) || !is_valid_ether_addr((u8 *)tmp) ||
	     !memcmp(dev->netdev->dev_addr, default_mac, ETH_ALEN) ||
	     !memcmp(dev->netdev->dev_addr, default_mac_178a, ETH_ALEN)) {
		int i;

		printk(KERN_WARNING "Found invalid EEPROM MAC address value ");

		for (i = 0; i < ETH_ALEN; i++) {
			printk(KERN_WARNING "%02X", *((u8 *)tmp + i));
			if (i != 5)
			printk(KERN_WARNING "-");
		}
		printk(KERN_WARNING "\n");
		eth_hw_addr_random(dev->netdev);

		*tmp = 0;
		*(tmp + 1) = 0x0E;
		*(tmp + 2) = 0xC6;
		*(tmp + 3) = 0x8E;

		return -EADDRNOTAVAIL;
	}
	return 0;
}

static int ax88179_get_mac(struct ax88179 *dev, u8 *buf)
{
	int ret, i;

	ret = access_eeprom_mac(dev, buf, 0x0, 0);
	if (ret < 0)
		goto out;

	if (ax88179_check_ether_addr(dev)) {
		ret = access_eeprom_mac(dev, dev->netdev->dev_addr, 0x0, 1);
		if (ret < 0) {
			netdev_err(dev->netdev,
				   "Failed to write MAC to EEPROM: %d", ret);
			goto out;
		}

		msleep(5);

		ret = ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_NODE_ID,
				       ETH_ALEN, ETH_ALEN, buf, 0);
		if (ret < 0) {
			netdev_err(dev->netdev,
				   "Failed to read MAC address: %d", ret);
			goto out;
		}

		for (i = 0; i < ETH_ALEN; i++)
			if (*(dev->netdev->dev_addr + i) != *((u8 *)buf + i)) {
				netdev_warn(dev->netdev, "Found invalid EEPROM part or non-EEPROM");
				break;
			}
	}

	memcpy(dev->netdev->perm_addr, dev->netdev->dev_addr, ETH_ALEN);

	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_NODE_ID, ETH_ALEN,
			  ETH_ALEN, dev->netdev->dev_addr);

	if (ret < 0) {
		netdev_err(dev->netdev, "Failed to write MAC address: %d", ret);
		goto out;
	}

	return 0;
out:
	return ret;
}

static int ax88179_check_eeprom(struct ax88179 *dev)
{
	u8 i = 0;
	u8 buf[2];
	u8 eeprom[20];
	u16 csum = 0, delay = HZ / 10;
	unsigned long jtimeout = 0;

	/* Read EEPROM content */
	for (i = 0 ; i < 6; i++) {
		buf[0] = i;
		if (ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_SROM_ADDR,
				      1, 1, buf) < 0)
			return -EINVAL;

		buf[0] = EEP_RD;
		if (ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_SROM_CMD,
				      1, 1, buf) < 0)
			return -EINVAL;

		jtimeout = jiffies + delay;
		do {
			ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_SROM_CMD,
					 1, 1, buf, 0);

			if (time_after(jiffies, jtimeout))
				return -EINVAL;
		} while (buf[0] & EEP_BUSY);

		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_SROM_DATA_LOW,
				 2, 2, &eeprom[i * 2], 0);

		if (i == 0 && eeprom[0] == 0xFF)
			return -EINVAL;
	}

	csum = eeprom[6] + eeprom[7] + eeprom[8] + eeprom[9];
	csum = (csum >> 8) + (csum & 0xff);

	if ((csum + eeprom[10]) == 0xff)
		return AX_EEP_EFUSE_CORRECT;
	else
		return -EINVAL;

	return AX_EEP_EFUSE_CORRECT;
}

static int ax88179_check_efuse(struct ax88179 *dev, void *ledmode)
{
	u8	i = 0;
	u16	csum = 0;
	u8	efuse[64];

	if (ax88179_read_cmd(dev, AX_ACCESS_EFUSE, 0, 64, 64, efuse, 0) < 0)
		return -EINVAL;

	if (efuse[0] == 0xFF)
		return -EINVAL;

	for (i = 0; i < 64; i++)
		csum = csum + efuse[i];

	while (csum > 255)
		csum = (csum & 0x00FF) + ((csum >> 8) & 0x00FF);

	if (csum == 0xFF) {
		memcpy((u8 *)ledmode, &efuse[51], 2);
		return AX_EEP_EFUSE_CORRECT;
	} else {
		return -EINVAL;
	}

	return AX_EEP_EFUSE_CORRECT;
}

static int ax88179_convert_old_led(struct ax88179 *dev,
				   u8 efuse, void *ledvalue)
{
	u8 ledmode = 0;
	u16 reg16;
	u16 led = 0;

	/* loaded the old eFuse LED Mode */
	if (efuse) {
		if (ax88179_read_cmd(dev, AX_ACCESS_EFUSE, 0x18,
				     1, 2, &reg16, 1) < 0)
			return -EINVAL;
		ledmode = (u8)(reg16 & 0xFF);
	} else { /* loaded the old EEprom LED Mode */
		if (ax88179_read_cmd(dev, AX_ACCESS_EEPROM, 0x3C,
				     1, 2, &reg16, 1) < 0)
			return -EINVAL;
		ledmode = (u8)(reg16 >> 8);
	}
	netdev_dbg(dev->netdev, "Old LED Mode = %02X\n", ledmode);

	switch (ledmode) {
	case 0xFF:
		led = LED0_ACTIVE | LED1_LINK_10 | LED1_LINK_100 |
		      LED1_LINK_1000 | LED2_ACTIVE | LED2_LINK_10 |
		      LED2_LINK_100 | LED2_LINK_1000 | LED_VALID;
		break;
	case 0xFE:
		led = LED0_ACTIVE | LED1_LINK_1000 | LED2_LINK_100 | LED_VALID;
		break;
	case 0xFD:
		led = LED0_ACTIVE | LED1_LINK_1000 | LED2_LINK_100 |
		      LED2_LINK_10 | LED_VALID;
		break;
	case 0xFC:
		led = LED0_ACTIVE | LED1_ACTIVE | LED1_LINK_1000 | LED2_ACTIVE |
		      LED2_LINK_100 | LED2_LINK_10 | LED_VALID;
		break;
	default:
		led = LED0_ACTIVE | LED1_LINK_10 | LED1_LINK_100 |
		      LED1_LINK_1000 | LED2_ACTIVE | LED2_LINK_10 |
		      LED2_LINK_100 | LED2_LINK_1000 | LED_VALID;
		break;
	}

	memcpy((u8 *)ledvalue, &led, 2);

	return 0;
}

static int ax88179_led_setting(struct ax88179 *dev)
{
	u16 ledvalue = 0, delay = HZ / 10;
	u16 ledact, ledlink;
	u16 reg16;
	u8 value;
	unsigned long jtimeout = 0;

	/* Check AX88179 version. UA1 or UA2 */
	ax88179_read_cmd(dev, AX_ACCESS_MAC, GENERAL_STATUS, 1, 1, &value, 0);

	/* UA1 */
	if (!(value & AX_SECLD)) {
		value = AX_GPIO_CTRL_GPIO3EN | AX_GPIO_CTRL_GPIO2EN |
			AX_GPIO_CTRL_GPIO1EN;
		if (ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_GPIO_CTRL,
				      1, 1, &value) < 0)
			return -EINVAL;
	}

	/* check EEprom */
	if (ax88179_check_eeprom(dev) == AX_EEP_EFUSE_CORRECT) {
		value = 0x42;
		if (ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_SROM_ADDR,
				      1, 1, &value) < 0)
			return -EINVAL;

		value = EEP_RD;
		if (ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_SROM_CMD,
				      1, 1, &value) < 0)
			return -EINVAL;

		jtimeout = jiffies + delay;
		do {
			ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_SROM_CMD,
					 1, 1, &value, 0);

			ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_SROM_CMD,
					 1, 1, &value, 0);

			if (time_after(jiffies, jtimeout))
				return -EINVAL;
		} while (value & EEP_BUSY);

		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_SROM_DATA_HIGH,
				 1, 1, &value, 0);
		ledvalue = (value << 8);
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_SROM_DATA_LOW,
				 1, 1, &value, 0);
		ledvalue |= value;

		/* load internal ROM for defaule setting */
		if (ledvalue == 0xFFFF || ((ledvalue & LED_VALID) == 0))
			ax88179_convert_old_led(dev, 0, &ledvalue);

	} else if (ax88179_check_efuse(dev, &ledvalue) ==
				       AX_EEP_EFUSE_CORRECT) { /* check efuse */
		if (ledvalue == 0xFFFF || ((ledvalue & LED_VALID) == 0))
			ax88179_convert_old_led(dev, 0, &ledvalue);
	} else {
		ax88179_convert_old_led(dev, 0, &ledvalue);
	}

	reg16 = GMII_PHY_PAGE_SELECT_EXT;
	ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			  GMII_PHY_PAGE_SELECT, 2, &reg16);

	reg16 = 0x2c;
	ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			  GMII_PHYPAGE, 2, &reg16);

	ax88179_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			 GMII_LED_ACTIVE, 2, &ledact, 1);

	ax88179_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			 GMII_LED_LINK, 2, &ledlink, 1);

	ledact &= GMII_LED_ACTIVE_MASK;
	ledlink &= GMII_LED_LINK_MASK;

	if (ledvalue & LED0_ACTIVE)
		ledact |= GMII_LED0_ACTIVE;
	if (ledvalue & LED1_ACTIVE)
		ledact |= GMII_LED1_ACTIVE;
	if (ledvalue & LED2_ACTIVE)
		ledact |= GMII_LED2_ACTIVE;

	if (ledvalue & LED0_LINK_10)
		ledlink |= GMII_LED0_LINK_10;
	if (ledvalue & LED1_LINK_10)
		ledlink |= GMII_LED1_LINK_10;
	if (ledvalue & LED2_LINK_10)
		ledlink |= GMII_LED2_LINK_10;

	if (ledvalue & LED0_LINK_100)
		ledlink |= GMII_LED0_LINK_100;
	if (ledvalue & LED1_LINK_100)
		ledlink |= GMII_LED1_LINK_100;
	if (ledvalue & LED2_LINK_100)
		ledlink |= GMII_LED2_LINK_100;

	if (ledvalue & LED0_LINK_1000)
		ledlink |= GMII_LED0_LINK_1000;
	if (ledvalue & LED1_LINK_1000)
		ledlink |= GMII_LED1_LINK_1000;
	if (ledvalue & LED2_LINK_1000)
		ledlink |= GMII_LED2_LINK_1000;

	ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			  GMII_LED_ACTIVE, 2, &ledact);

	ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			  GMII_LED_LINK, 2, &ledlink);

	reg16 = GMII_PHY_PAGE_SELECT_PAGE0;
	ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			  GMII_PHY_PAGE_SELECT, 2, &reg16);

	/* LED full duplex setting */
	reg16 = 0;
	if (ledvalue & LED0_FD)
		reg16 |= 0x01;
	else if ((ledvalue & LED0_USB3_MASK) == 0)
		reg16 |= 0x02;

	if (ledvalue & LED1_FD)
		reg16 |= 0x04;
	else if ((ledvalue & LED1_USB3_MASK) == 0)
		reg16 |= 0x08;

	if (ledvalue & LED2_FD) /* LED2_FD */
		reg16 |= 0x10;
	else if ((ledvalue & LED2_USB3_MASK) == 0) /* LED2_USB3 */
		reg16 |= 0x20;

	ax88179_write_cmd(dev, AX_ACCESS_MAC, 0x73, 1, 1, &reg16);

	return 0;
}

static void ax88179_EEE_setting(struct ax88179 *dev)
{
	u16 reg16;

	if (bEEE) {
		// Enable EEE
		reg16 = 0x07;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  GMII_PHY_MACR, 2, &reg16);

		reg16 = 0x3c;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  GMII_PHY_MAADR, 2, &reg16);

		reg16 = 0x4007;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  GMII_PHY_MACR, 2, &reg16);

		reg16 = 0x06;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  GMII_PHY_MAADR, 2, &reg16);
	} else {
		// Disable EEE
		reg16 = 0x07;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  GMII_PHY_MACR, 2, &reg16);

		reg16 = 0x3c;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  GMII_PHY_MAADR, 2, &reg16);

		reg16 = 0x4007;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  GMII_PHY_MACR, 2, &reg16);

		reg16 = 0x00;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  GMII_PHY_MAADR, 2, &reg16);
	}
}

static void ax88179_Gether_setting(struct ax88179 *dev)
{
	u16 reg16;

	if (bGETH) {
		// Enable Green Ethernet
		reg16 = 0x03;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  31, 2, &reg16);

		reg16 = 0x3247;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  25, 2, &reg16);

		reg16 = 0x05;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  31, 2, &reg16);

		reg16 = 0x0680;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  1, 2, &reg16);

		reg16 = 0;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  31, 2, &reg16);
	} else {
		// Disable Green Ethernet
		reg16 = 0x03;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  31, 2, &reg16);

		reg16 = 0x3246;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  25, 2, &reg16);

		reg16 = 0;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  31, 2, &reg16);
	}
}

static int ax88179_bind(struct ax88179 *dev)
{
	u16 reg16 = 0;
	u8 buf[6] = {0};
	u8 reg8 = 0;
	int ret;

	/* Power up ethernet PHY */
	reg16 = 0;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 2, &reg16);
	reg16 = AX_PHYPWR_RSTCTL_IPRL;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 2, &reg16);
	msleep(200);

	reg8 = AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_CLK_SELECT, 1, 1, &reg8);
	msleep(100);

	/* Get the MAC address */
	memset(buf, 0, ETH_ALEN);
	ret = ax88179_get_mac(dev, buf);
	if (ret)
		goto out;
	netdev_dbg(dev->netdev, "MAC [%02x-%02x-%02x-%02x-%02x-%02x]\n",
		   dev->netdev->dev_addr[0], dev->netdev->dev_addr[1],
		   dev->netdev->dev_addr[2], dev->netdev->dev_addr[3],
		   dev->netdev->dev_addr[4], dev->netdev->dev_addr[5]);

	/* RX bulk configuration, default for USB3.0 to Giga*/
	memcpy(buf, &AX88179_BULKIN_SIZE[0], 5);
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, 5, buf);

	reg8 = 0x34;
	ax88179_write_cmd(dev, AX_ACCESS_MAC,
			  AX_PAUSE_WATERLVL_LOW, 1, 1, &reg8);

	reg8 = 0x52;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_HIGH,
			  1, 1, &reg8);

	/* Disable auto-power-OFF GigaPHY after ethx down*/
	ax88179_write_cmd(dev, 0x91, 0, 0, 0, NULL);

	/* Enable checksum offload */
	reg8 = AX_RXCOE_IP | AX_RXCOE_TCP | AX_RXCOE_UDP |
	       AX_RXCOE_TCPV6 | AX_RXCOE_UDPV6;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RXCOE_CTL, 1, 1, &reg8);

	reg8 = AX_TXCOE_IP | AX_TXCOE_TCP | AX_TXCOE_UDP |
	       AX_TXCOE_TCPV6 | AX_TXCOE_UDPV6;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_TXCOE_CTL, 1, 1, &reg8);

	/* Configure RX control register => start operation */
	reg16 = AX_RX_CTL_DROPCRCERR | AX_RX_CTL_START | AX_RX_CTL_AP |
		 AX_RX_CTL_AMALL | AX_RX_CTL_AB;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &reg16);

	reg8 = AX_MONITOR_MODE_PMETYPE | AX_MONITOR_MODE_PMEPOL |
						AX_MONITOR_MODE_RWMP;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MONITOR_MODE, 1, 1, &reg8);

	/* Configure default medium type => giga */
	reg16 = AX_MEDIUM_RECEIVE_EN	 | AX_MEDIUM_TXFLOW_CTRLEN |
		 AX_MEDIUM_RXFLOW_CTRLEN | AX_MEDIUM_FULL_DUPLEX   |
		 AX_MEDIUM_GIGAMODE;

	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
			  2, 2, &reg16);

	ax88179_led_setting(dev);

	ax88179_EEE_setting(dev);

	ax88179_Gether_setting(dev);

	/* Restart autoneg */
	mii_nway_restart(&dev->mii);

	netif_carrier_off(dev->netdev);
	return 0;

out:
	return ret;
}

static int ax88179_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_driver *driver = to_usb_driver(intf->dev.driver);
	struct ax88179 *dev;
	struct net_device *netdev;
	int ret;

	if (!driver->supports_autosuspend) {
		driver->supports_autosuspend = 1;
		pm_runtime_enable(&intf->dev);
	}

	netdev = alloc_etherdev(sizeof(struct ax88179));
	if (!netdev) {
		dev_err(&intf->dev, "Out of memory\n");
		return -ENOMEM;
	}

	SET_NETDEV_DEV(netdev, &intf->dev);
	dev = netdev_priv(netdev);
	dev->msg_enable = 0x7FFF;

	dev->udev = udev;
	dev->netdev = netdev;
	dev->intf = intf;

	mutex_init(&dev->control);
	INIT_DELAYED_WORK(&dev->schedule, ax_work_func_t);

	netdev->netdev_ops = &ax88179_netdev_ops;

	netdev->watchdog_timeo = AX88179_TX_TIMEOUT;

	netdev->features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM |
			      NETIF_F_SG | NETIF_F_TSO;
	//netdev->features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;

	netdev->hw_features = NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM |
			      NETIF_F_SG | NETIF_F_TSO;
	//netdev->hw_features = NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;

	//netif_set_gso_max_size(netdev, ax_LIMITED_TSO_SIZE);

	netdev->ethtool_ops = &ops;

	dev->mii.supports_gmii = 1;
	dev->mii.dev = netdev;
	dev->mii.mdio_read = ax88179_mdio_read;
	dev->mii.mdio_write = ax88179_mdio_write;
	dev->mii.phy_id_mask = 0xff;
	dev->mii.reg_num_mask = 0xff;
	dev->mii.phy_id = AX88179_PHY_ID;
	dev->mii.force_media = 0;
	dev->mii.advertising = ADVERTISE_10HALF | ADVERTISE_10FULL |
			      ADVERTISE_100HALF | ADVERTISE_100FULL;

	dev->autoneg = AUTONEG_ENABLE;
	dev->advertising = ADVERTISED_10baseT_Half | ADVERTISED_10baseT_Full |
			  ADVERTISED_100baseT_Half | ADVERTISED_100baseT_Full |
			  ADVERTISED_1000baseT_Full;
	dev->speed = SPEED_1000;
	dev->duplex = DUPLEX_FULL;
	intf->needs_remote_wakeup = 1;

	ret = ax88179_bind(dev);
	if (ret < 0)
		goto out;

	usb_set_intfdata(intf, dev);

	netif_napi_add(netdev, &dev->napi, ax88179_poll, AX88179_NAPI_WEIGHT);

	netif_device_attach(netdev);

	ret = register_netdev(netdev);
	if (ret != 0) {
		netif_err(dev, probe, netdev, "couldn't register the device\n");
		goto out;
	}

	return 0;

out:
	netif_napi_del(&dev->napi);
	usb_set_intfdata(intf, NULL);
	free_netdev(netdev);
	return ret;
}

static void ax88179_disconnect(struct usb_interface *intf)
{
	struct ax88179 *dev = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (dev) {
		u16 reg16;
		u8 reg8;

		ax_set_unplug(dev);
		netif_napi_del(&dev->napi);
		unregister_netdev(dev->netdev);

		/* Configure RX control register => stop operation */
		reg16 = AX_RX_CTL_STOP;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &reg16);

		reg8 = 0x0;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_CLK_SELECT,
				  1, 1, &reg8);

		/* Power down ethernet PHY */
		reg16 = 0;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL,
				  2, 2, &reg16);
		msleep(200);

		free_netdev(dev->netdev);
	}
}

/* table of devices that work with this driver */
static const struct usb_device_id ax88179_table[] = {
{
	USB_DEVICE(0x0b95, 0x1790),
},
	{},	/* END */
};

MODULE_DEVICE_TABLE(usb, ax88179_table);

static struct usb_driver ax88179_driver = {
	.name =		MODULENAME,
	.id_table =	ax88179_table,
	.probe =	ax88179_probe,
	.disconnect =	ax88179_disconnect,
	.suspend =	ax88179_suspend,
	.resume =	ax88179_resume,
	.pre_reset =	ax88179_pre_reset,
	.post_reset =	ax88179_post_reset,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	.disable_hub_initiated_lpm = 1,
#endif
};

module_usb_driver(ax88179_driver);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
