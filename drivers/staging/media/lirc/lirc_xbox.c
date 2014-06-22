/*
 * lirc_xbox - USB remote support for LIRC
 * (supports Microsoft XBOX DVD Dongle)
 *
 * Copyright (C) 2003-2004 Paul Miller <pmiller9@users.sourceforge.net>
 *
 * This driver was derived from:
 *   Vladimir Dergachev <volodya@minspring.com>'s 2002
 *      "USB ATI Remote support" (input device)
 *   Adrian Dewhurst <sailor-lk@sailorfrag.net>'s 2002
 *      "USB StreamZap remote driver" (LIRC)
 *   Artur Lipowski <alipowski@kki.net.pl>'s 2002
 *      "lirc_dev" and "lirc_gpio" LIRC modules
 *   Michael Wojciechowski
 *      initial xbox support
 *   Vassilis Virvilis <vasvir@iit.demokritos.gr> 2006
 *      reworked the patch for lirc submission
 *   Paul Miller's <pmiller9@users.sourceforge.net> 2004
 *      lirc_atiusb - removed all ati remote support
 * $Id: lirc_xbox.c,v 1.88 2011/06/03 11:11:11 jmartin Exp $
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/completion.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/list.h>

#include <media/lirc.h>
#include <media/lirc_dev.h>

#define DRIVER_VERSION		"$Revision: 0.01 $"
#define DRIVER_AUTHOR		"Jason Martin <austinspartan@users.sourceforge.net>"
#define DRIVER_DESC		"XBOX DVD Dongle USB remote driver for LIRC"
#define DRIVER_NAME		"lirc_xbox"

#define CODE_LENGTH		6
#define CODE_MIN_LENGTH		6
#define DECODE_LENGTH		1

/* module parameters */
#ifdef CONFIG_USB_DEBUG
static int debug = 1;
#else
static int debug;
#endif

#define dprintk(fmt, args...)					\
	do {							\
		if (debug)					\
			printk(KERN_DEBUG fmt, ## args);	\
	} while (0)

/*
 * USB_BUFF_LEN must be the maximum value of the code_length array.
 * It is used for static arrays.
 */
#define USB_BUFF_LEN 6

static int mask = 0xFFFF;	/* channel acceptance bit mask */
static int unique;		/* enable channel-specific codes */
static int repeat = 10;		/* repeat time in 1/100 sec */
static unsigned long repeat_jiffies; /* repeat timeout */

/* get hi and low bytes of a 16-bits int */
#define HI(a)			((unsigned char)((a) >> 8))
#define LO(a)			((unsigned char)((a) & 0xff))

/* general constants */
#define SEND_FLAG_IN_PROGRESS	1
#define SEND_FLAG_COMPLETE	2
#define FREE_ALL		0xFF

/* endpoints */
#define EP_KEYS			0
#define EP_MOUSE		1
#define EP_MOUSE_ADDR		0x81
#define EP_KEYS_ADDR		0x82

/* USB vendor ids for XBOX DVD Dongles */
#define VENDOR_MS1		0x040b
#define VENDOR_MS2		0x045e
#define VENDOR_MS3		0xFFFF

static struct usb_device_id usb_remote_table[] = {
	/* Gamester Xbox DVD Movie Playback Kit IR */
	{ USB_DEVICE(VENDOR_MS1, 0x6521) },

	/* Microsoft Xbox DVD Movie Playback Kit IR */
	{ USB_DEVICE(VENDOR_MS2, 0x0284) },

	/*
	 * Some Chinese manufacturer -- conflicts with the joystick from the
	 * same manufacturer
	 */
	{ USB_DEVICE(VENDOR_MS3, 0xFFFF) },

	/* Terminating entry */
	{ }
};

/* init strings */
#define USB_OUTLEN		7

static char init1[] = {0x01, 0x00, 0x20, 0x14};
static char init2[] = {0x01, 0x00, 0x20, 0x14, 0x20, 0x20, 0x20};

struct in_endpt {
	/* inner link in list of endpoints for the remote specified by ir */
	struct list_head iep_list_link;
	struct xbox_dev *ir;
	struct urb *urb;
	struct usb_endpoint_descriptor *ep;

	/* buffers and dma */
	unsigned char *buf;
	unsigned int len;
	dma_addr_t dma;

	/* handle repeats */
	unsigned char old[USB_BUFF_LEN];
	unsigned long old_jiffies;
};

struct out_endpt {
	struct xbox_dev *ir;
	struct urb *urb;
	struct usb_endpoint_descriptor *ep;

	/* buffers and dma */
	unsigned char *buf;
	dma_addr_t dma;

	/* handle sending (init strings) */
	int send_flags;
	wait_queue_head_t wait;
};


/* data structure for each usb remote */
struct xbox_dev {
	/* inner link in list of all remotes managed by this module */
	struct list_head remote_list_link;
	/* Number of usb interfaces associated with this device */
	int dev_refcount;

	/* usb */
	struct usb_device *usbdev;
	/* Head link to list of all inbound endpoints in this remote */
	struct list_head iep_listhead;
	struct out_endpt *out_init;
	int devnum;

	/* lirc */
	struct lirc_driver *d;
	int connected;

	/* locking */
	struct mutex lock;
};

/* list of all registered devices via the remote_list_link in xbox_dev */
static struct list_head remote_list;

/*
 * Convenience macros to retrieve a pointer to the surrounding struct from
 * the given list_head reference within, pointed at by link.
 */
#define get_iep_from_link(link) \
		list_entry((link), struct in_endpt, iep_list_link);
#define get_irctl_from_link(link) \
		list_entry((link), struct xbox_dev, remote_list_link);

/* send packet - used to initialize remote */
static void send_packet(struct out_endpt *oep, u16 cmd, unsigned char *data)
{
	struct xbox_dev *ir = oep->ir;
	DECLARE_WAITQUEUE(wait, current);
	int timeout = HZ; /* 1 second */
	unsigned char buf[USB_OUTLEN];

	dprintk(DRIVER_NAME "[%d]: send called (%#x)\n", ir->devnum, cmd);

	mutex_lock(&ir->lock);
	oep->urb->transfer_buffer_length = LO(cmd) + 1;
	oep->urb->dev = oep->ir->usbdev;
	oep->send_flags = SEND_FLAG_IN_PROGRESS;

	memcpy(buf+1, data, LO(cmd));
	buf[0] = HI(cmd);
	memcpy(oep->buf, buf, LO(cmd)+1);

	set_current_state(TASK_INTERRUPTIBLE);
	add_wait_queue(&oep->wait, &wait);

	if (usb_submit_urb(oep->urb, GFP_ATOMIC)) {
		set_current_state(TASK_RUNNING);
		remove_wait_queue(&oep->wait, &wait);
		mutex_unlock(&ir->lock);
		return;
	}
	mutex_unlock(&ir->lock);

	while (timeout && (oep->urb->status == -EINPROGRESS)
	       && !(oep->send_flags & SEND_FLAG_COMPLETE)) {
		timeout = schedule_timeout(timeout);
		rmb();
	}

	dprintk(DRIVER_NAME "[%d]: send complete (%#x)\n", ir->devnum, cmd);

	set_current_state(TASK_RUNNING);
	remove_wait_queue(&oep->wait, &wait);
	usb_unlink_urb(oep->urb);
}

static int unregister_from_lirc(struct xbox_dev *ir)
{
	struct lirc_driver *d = ir->d;
	int devnum;

	devnum = ir->devnum;
	dprintk(DRIVER_NAME "[%d]: unregister from lirc called\n", devnum);

	lirc_unregister_driver(d->minor);

	printk(DRIVER_NAME "[%d]: usb remote disconnected\n", devnum);
	return 0;
}

static int set_use_inc(void *data)
{
	struct xbox_dev *ir = data;
	struct list_head *pos, *n;
	struct in_endpt *iep;
	int rtn;

	if (!ir) {
		printk(DRIVER_NAME "[?]: set_use_inc called with no context\n");
		return -EIO;
	}
	dprintk(DRIVER_NAME "[%d]: set use inc\n", ir->devnum);

	mutex_lock(&ir->lock);
	if (!ir->connected) {
		if (!ir->usbdev) {
			mutex_unlock(&ir->lock);
			dprintk(DRIVER_NAME "[%d]: !ir->usbdev\n", ir->devnum);
			return -ENOENT;
		}

		/* Iterate through the inbound endpoints */
		list_for_each_safe(pos, n, &ir->iep_listhead) {
			/* extract the current in_endpt */
			iep = get_iep_from_link(pos);
			iep->urb->dev = ir->usbdev;
			dprintk(DRIVER_NAME "[%d]: linking iep 0x%02x (%p)\n",
				ir->devnum, iep->ep->bEndpointAddress, iep);
			rtn = usb_submit_urb(iep->urb, GFP_ATOMIC);
			if (rtn) {
				printk(DRIVER_NAME "[%d]: open result = %d "
				       "error submitting urb\n",
				       ir->devnum, rtn);
				mutex_unlock(&ir->lock);
				return -EIO;
			}
		}
		ir->connected = 1;
	}
	mutex_unlock(&ir->lock);

	return 0;
}

static void set_use_dec(void *data)
{
	struct xbox_dev *ir = data;
	struct list_head *pos, *n;
	struct in_endpt *iep;

	if (!ir) {
		printk(DRIVER_NAME "[?]: set_use_dec called with no context\n");
		return;
	}
	dprintk(DRIVER_NAME "[%d]: set use dec\n", ir->devnum);

	mutex_lock(&ir->lock);
	if (ir->connected) {
		/* Free inbound usb urbs */
		list_for_each_safe(pos, n, &ir->iep_listhead) {
			iep = get_iep_from_link(pos);
			dprintk(DRIVER_NAME "[%d]: unlinking iep 0x%02x (%p)\n",
				ir->devnum, iep->ep->bEndpointAddress, iep);
			usb_kill_urb(iep->urb);
		}
		ir->connected = 0;
	}
	mutex_unlock(&ir->lock);
}

static void print_data(struct in_endpt *iep, char *buf, int len)
{
        const int clen = CODE_LENGTH;
	char codes[clen * 3 + 1];
	int i;

	if (len <= 0)
		return;

	for (i = 0; i < len && i < clen; i++)
		snprintf(codes+i*3, 4, "%02x ", buf[i] & 0xFF);
	printk(DRIVER_NAME "[%d]: data received %s (ep=0x%x length=%d)\n",
		iep->ir->devnum, codes, iep->ep->bEndpointAddress, len);
}

static int code_check_xbox(struct in_endpt *iep, int len)
{
  //	struct xbox_dev *ir = iep->ir;
	const int clen = CODE_LENGTH;

	if (len != clen) {
		dprintk(DRIVER_NAME ": We got %d instead of %d bytes from xbox "
			"ir.. ?\n", len, clen);
		return -1;
	}

	/* check for repeats */
	if (memcmp(iep->old, iep->buf, len) == 0) {
		if (iep->old_jiffies + repeat_jiffies > jiffies)
			return -1;
	} else {
		/*
		 * the third byte of xbox ir packet seems to contain key info
		 * the last two bytes are.. some kind of clock?
		 */
		iep->buf[0] = iep->buf[2];
		memset(iep->buf + 1, 0, len - 1);
		memcpy(iep->old, iep->buf, len);
	}
	iep->old_jiffies = jiffies;

	return 0;
}

static void usb_remote_recv(struct urb *urb)
{
	struct in_endpt *iep;
	int len, result = -1;

	if (!urb)
		return;
	iep = urb->context;
	if (!iep) {
		usb_unlink_urb(urb);
		return;
	}
	if (!iep->ir->usbdev)
		return;

	len = urb->actual_length;
	if (debug)
		print_data(iep, urb->transfer_buffer, len);

	switch (urb->status) {

	case 0:
	        result = code_check_xbox(iep, len);
	  
	        if (result < 0)
	        break;

		lirc_buffer_write(iep->ir->d->rbuf, iep->buf);
		wake_up(&iep->ir->d->rbuf->wait_poll);
		break;

	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		usb_unlink_urb(urb);
		return;

	case -EPIPE:
	default:
		break;
	}

	usb_submit_urb(urb, GFP_ATOMIC);
}

static void usb_remote_send(struct urb *urb)
{
	struct out_endpt *oep;

	if (!urb)
		return;
	oep = urb->context;
	if (!oep) {
		usb_unlink_urb(urb);
		return;
	}
	if (!oep->ir->usbdev)
		return;

	dprintk(DRIVER_NAME "[%d]: usb out called\n", oep->ir->devnum);

	if (urb->status)
		return;

	oep->send_flags |= SEND_FLAG_COMPLETE;
	wmb();
	if (waitqueue_active(&oep->wait))
		wake_up(&oep->wait);
}


/*
 * Initialization and removal
 */

/*
 * Free iep according to mem_failure which specifies a checkpoint into the
 * initialization sequence for rollback recovery.
 */
static void free_in_endpt(struct in_endpt *iep, int mem_failure)
{
	struct xbox_dev *ir;
	dprintk(DRIVER_NAME ": free_in_endpt(%p, %d)\n", iep, mem_failure);
	if (!iep)
		return;

	ir = iep->ir;
	if (!ir) {
		dprintk(DRIVER_NAME ": free_in_endpt: WARNING! null ir\n");
		return;
	}
	mutex_lock(&ir->lock);
	switch (mem_failure) {
	case FREE_ALL:
	case 5:
		list_del(&iep->iep_list_link);
		dprintk(DRIVER_NAME "[%d]: free_in_endpt removing ep=0x%0x "
			"from list\n", ir->devnum, iep->ep->bEndpointAddress);
	case 4:
		if (iep->urb) {
			usb_unlink_urb(iep->urb);
			usb_free_urb(iep->urb);
			iep->urb = 0;
		} else
			dprintk(DRIVER_NAME "[%d]: free_in_endpt null urb!\n",
				ir->devnum);
	case 3:
		usb_free_coherent(iep->ir->usbdev, iep->len, iep->buf, iep->dma);
		iep->buf = 0;
	case 2:
		kfree(iep);
	}
	mutex_unlock(&ir->lock);
}

/*
 * Construct a new inbound endpoint for this remote, and add it to the list of
 * in_epts in ir.
 */
static struct in_endpt *new_in_endpt(struct xbox_dev *ir,
				     struct usb_endpoint_descriptor *ep)
{
	struct usb_device *dev = ir->usbdev;
	struct in_endpt *iep;
	int pipe, maxp, len, addr;
	int mem_failure;

	addr = ep->bEndpointAddress;
	pipe = usb_rcvintpipe(dev, addr);
	maxp = usb_maxpacket(dev, pipe, usb_pipeout(pipe));

/*	len = (maxp > USB_BUFLEN) ? USB_BUFLEN : maxp;
 *	len -= (len % CODE_LENGTH); */
	len = CODE_LENGTH;

	dprintk(DRIVER_NAME "[%d]: acceptable inbound endpoint (0x%x) found "
		"(maxp=%d len=%d)\n", ir->devnum, addr, maxp, len);

	mem_failure = 0;
	iep = kzalloc(sizeof(*iep), GFP_KERNEL);
	if (!iep) {
		mem_failure = 1;
		goto new_in_endpt_failure_check;
	}
	iep->ir = ir;
	iep->ep = ep;
	iep->len = len;

	iep->buf = usb_alloc_coherent(dev, len, GFP_ATOMIC, &iep->dma);
	if (!iep->buf) {
		mem_failure = 2;
		goto new_in_endpt_failure_check;
	}

	iep->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!iep->urb)
		mem_failure = 3;

new_in_endpt_failure_check:

	if (mem_failure) {
		free_in_endpt(iep, mem_failure);
		printk(DRIVER_NAME "[%d]: ep=0x%x out of memory (code=%d)\n",
		       ir->devnum, addr, mem_failure);
		return NULL;
	}
	list_add_tail(&iep->iep_list_link, &ir->iep_listhead);
	dprintk(DRIVER_NAME "[%d]: adding ep=0x%0x to list\n",
		ir->devnum, iep->ep->bEndpointAddress);
	return iep;
}

static void free_out_endpt(struct out_endpt *oep, int mem_failure)
{
	struct xbox_dev *ir;
	dprintk(DRIVER_NAME ": free_out_endpt(%p, %d)\n", oep, mem_failure);
	if (!oep)
		return;

	wake_up_all(&oep->wait);

	ir = oep->ir;
	if (!ir) {
		dprintk(DRIVER_NAME ": free_out_endpt: WARNING! null ir\n");
		return;
	}
	mutex_lock(&ir->lock);
	switch (mem_failure) {
	case FREE_ALL:
	case 4:
		if (oep->urb) {
			usb_unlink_urb(oep->urb);
			usb_free_urb(oep->urb);
			oep->urb = 0;
		} else {
			dprintk(DRIVER_NAME "[%d]: free_out_endpt: null urb!\n",
				ir->devnum);
		}
	case 3:
		usb_free_coherent(oep->ir->usbdev, USB_OUTLEN,
				  oep->buf, oep->dma);
		oep->buf = 0;
	case 2:
		kfree(oep);
	}
	mutex_unlock(&ir->lock);
}

static struct out_endpt *new_out_endpt(struct xbox_dev *ir,
				       struct usb_endpoint_descriptor *ep)
{
	struct usb_device *dev = ir->usbdev;
	struct out_endpt *oep;
	int mem_failure;

	dprintk(DRIVER_NAME "[%d]: acceptable outbound endpoint (0x%x) found\n",
		ir->devnum, ep->bEndpointAddress);

	mem_failure = 0;
	oep = kzalloc(sizeof(*oep), GFP_KERNEL);
	if (!oep)
		mem_failure = 1;
	else {
		oep->ir = ir;
		oep->ep = ep;
		init_waitqueue_head(&oep->wait);

		oep->buf = usb_alloc_coherent(dev, USB_OUTLEN,
					      GFP_ATOMIC, &oep->dma);
		if (!oep->buf)
			mem_failure = 2;
		else {
			oep->urb = usb_alloc_urb(0, GFP_KERNEL);
			if (!oep->urb)
				mem_failure = 3;
		}
	}
	if (mem_failure) {
		free_out_endpt(oep, mem_failure);
		printk(DRIVER_NAME "[%d]: ep=0x%x out of memory (code=%d)\n",
		       ir->devnum, ep->bEndpointAddress, mem_failure);
		return NULL;
	}
	return oep;
}

static void free_irctl(struct xbox_dev *ir, int mem_failure)
{
	struct list_head *pos, *n;
	struct in_endpt *in;
	dprintk(DRIVER_NAME ": free_irctl(%p, %d)\n", ir, mem_failure);

	if (!ir)
		return;

	list_for_each_safe(pos, n, &ir->iep_listhead) {
		in = get_iep_from_link(pos);
		free_in_endpt(in, FREE_ALL);
	}
	if (ir->out_init) {
		free_out_endpt(ir->out_init, FREE_ALL);
		ir->out_init = NULL;
	}

	mutex_lock(&ir->lock);
	switch (mem_failure) {
	case FREE_ALL:
	case 6:
		if (!--ir->dev_refcount) {
			list_del(&ir->remote_list_link);
			dprintk(DRIVER_NAME "[%d]: free_irctl: removing "
				"remote from list\n", ir->devnum);
		} else {
			dprintk(DRIVER_NAME "[%d]: free_irctl: refcount at %d,"
				"aborting free_irctl\n",
				ir->devnum, ir->dev_refcount);
			mutex_unlock(&ir->lock);
			return;
		}
	case 5:
	case 4:
	case 3:
		if (ir->d) {
			switch (mem_failure) {
			case 5:
				lirc_buffer_free(ir->d->rbuf);
			case 4:
				kfree(ir->d->rbuf);
			case 3:
				kfree(ir->d);
			}
		} else
			printk(DRIVER_NAME "[%d]: ir->d is a null pointer!\n",
			       ir->devnum);
	case 2:
		mutex_unlock(&ir->lock);
		kfree(ir);
		return;
	}
	mutex_unlock(&ir->lock);
}

static struct xbox_dev *new_irctl(struct usb_interface *intf)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct xbox_dev *ir;
	struct lirc_driver *driver;
	int devnum, dclen;
	int mem_failure;

	devnum = dev->devnum;

	dprintk(DRIVER_NAME "[%d]: remote type = XBOX DVD Dongle\n", devnum);

	mem_failure = 0;
	ir = kzalloc(sizeof(*ir), GFP_KERNEL);
	if (!ir) {
		mem_failure = 1;
		goto new_irctl_failure_check;
	}

	dclen = DECODE_LENGTH;

	/*
	 * add this infrared remote struct to remote_list, keeping track
	 * of the number of drivers registered.
	 */
	dprintk(DRIVER_NAME "[%d]: adding remote to list\n", devnum);
	list_add_tail(&ir->remote_list_link, &remote_list);
	ir->dev_refcount = 1;

	driver = kzalloc(sizeof(*driver), GFP_KERNEL);
	if (!driver) {
		mem_failure = 2;
		goto new_irctl_failure_check;
	}

	ir->d = driver;
	driver->rbuf = kmalloc(sizeof(*(driver->rbuf)), GFP_KERNEL);
	if (!driver->rbuf) {
		mem_failure = 3;
		goto new_irctl_failure_check;
	}

	if (lirc_buffer_init(driver->rbuf, dclen, 2)) {
		mem_failure = 4;
		goto new_irctl_failure_check;
	}

	strcpy(driver->name, DRIVER_NAME " ");
	driver->minor = -1;
	driver->code_length = dclen * 8;
	driver->features = LIRC_CAN_REC_LIRCCODE;
	driver->data = ir;
	driver->set_use_inc = &set_use_inc;
	driver->set_use_dec = &set_use_dec;
	driver->dev = &intf->dev;
	driver->owner = THIS_MODULE;
	ir->usbdev = dev;
	ir->devnum = devnum;

	mutex_init(&ir->lock);
	INIT_LIST_HEAD(&ir->iep_listhead);

new_irctl_failure_check:

	if (mem_failure) {
		free_irctl(ir, mem_failure);
		printk(DRIVER_NAME "[%d]: out of memory (code=%d)\n",
		       devnum, mem_failure);
		return NULL;
	}
	return ir;
}

/*
 * Scan the global list of remotes to see if the device listed is one of them.
 * If it is, the corresponding xbox_dev is returned, with its dev_refcount
 * incremented.  Otherwise, returns null.
 */
static struct xbox_dev *get_prior_reg_ir(struct usb_device *dev)
{
	struct list_head *pos;
	struct xbox_dev *ir = NULL;

	dprintk(DRIVER_NAME "[%d]: scanning remote_list...\n", dev->devnum);
	list_for_each(pos, &remote_list) {
		ir = get_irctl_from_link(pos);
		if (ir->usbdev != dev) {
			dprintk(DRIVER_NAME "[%d]: device %d isn't it...",
				dev->devnum, ir->devnum);
		    ir = NULL;
		} else {
			dprintk(DRIVER_NAME "[%d]: prior instance found.\n",
				dev->devnum);
			ir->dev_refcount++;
			break;
		}
	}
	return ir;
}

/*
 * If the USB interface has an out endpoint for control. 
 */
static void send_outbound_init(struct xbox_dev *ir)
{
	if (ir->out_init) {
		struct out_endpt *oep = ir->out_init;
		dprintk(DRIVER_NAME "[%d]: usb_remote_probe: initializing "
			"outbound ep\n", ir->devnum);
		usb_fill_int_urb(oep->urb, ir->usbdev,
			usb_sndintpipe(ir->usbdev, oep->ep->bEndpointAddress),
			oep->buf, USB_OUTLEN, usb_remote_send,
			oep, oep->ep->bInterval);
		oep->urb->transfer_dma = oep->dma;
		oep->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

		send_packet(oep, 0x8004, init1);
		send_packet(oep, 0x8007, init2);
	}
}

/* Log driver and usb info */
static void log_usb_dev_info(struct usb_device *dev)
{
	char buf[63], name[128] = "";

	if (dev->descriptor.iManufacturer
	    && usb_string(dev, dev->descriptor.iManufacturer,
			  buf, sizeof(buf)) > 0)
		strlcpy(name, buf, sizeof(name));
	if (dev->descriptor.iProduct
	    && usb_string(dev, dev->descriptor.iProduct, buf, sizeof(buf)) > 0)
		snprintf(name + strlen(name), sizeof(name) - strlen(name),
			 " %s", buf);
	printk(DRIVER_NAME "[%d]: %s on usb%d:%d\n", dev->devnum, name,
	       dev->bus->busnum, dev->devnum);
}


static int usb_remote_probe(struct usb_interface *intf,
				const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_host_interface *idesc;
	struct usb_endpoint_descriptor *ep;
	struct in_endpt *iep;
	struct xbox_dev *ir;
	int i;

	dprintk(DRIVER_NAME "[%d]: usb_remote_probe: dev:%p, intf:%p, id:%p)\n",
		dev->devnum, dev, intf, id);

	idesc = intf->cur_altsetting;

	/* Check if a usb remote has already been registered for this device */
	ir = get_prior_reg_ir(dev);

	if (!ir) {
		ir = new_irctl(intf);
		if (!ir)
			return -ENOMEM;
	}

	/*
	 * step through the endpoints to find first in and first out endpoint
	 * of type interrupt transfer
	 */
	for (i = 0; i < idesc->desc.bNumEndpoints; ++i) {
		ep = &idesc->endpoint[i].desc;
		dprintk(DRIVER_NAME "[%d]: processing endpoint %d\n",
			dev->devnum, i);
		if (((ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) ==
		     USB_DIR_IN) &&
		     ((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
		      USB_ENDPOINT_XFER_INT)) {

			iep = new_in_endpt(ir, ep);
			if (iep)
			{
				usb_fill_int_urb(iep->urb, dev,
					usb_rcvintpipe(dev,
						iep->ep->bEndpointAddress),
					iep->buf, iep->len, usb_remote_recv,
					iep, iep->ep->bInterval);
				iep->urb->transfer_dma = iep->dma;
				iep->urb->transfer_flags |=
					URB_NO_TRANSFER_DMA_MAP;
			}
		}

		if (((ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) ==
		     USB_DIR_OUT) &&
		     ((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
		      USB_ENDPOINT_XFER_INT) &&
		      (ir->out_init == NULL))
			ir->out_init = new_out_endpt(ir, ep);
	}
	if (list_empty(&ir->iep_listhead)) {
		printk(DRIVER_NAME "[%d]: inbound endpoint not found\n",
		       ir->devnum);
		free_irctl(ir, FREE_ALL);
		return -ENODEV;
	}
	if (ir->dev_refcount == 1) {
		ir->d->minor = lirc_register_driver(ir->d);
		if (ir->d->minor < 0) {
			free_irctl(ir, FREE_ALL);
			return -ENODEV;
		}

		/* Note new driver registration in kernel logs */
		log_usb_dev_info(dev);

		/* outbound data (initialization) */
		send_outbound_init(ir);
	}

	usb_set_intfdata(intf, ir);
	return 0;
}

static void usb_remote_disconnect(struct usb_interface *intf)
{
	/* struct usb_device *dev = interface_to_usbdev(intf); */
	struct xbox_dev *ir = usb_get_intfdata(intf);
	usb_set_intfdata(intf, NULL);

	dprintk(DRIVER_NAME ": disconnecting remote %d:\n",
		(ir ? ir->devnum : -1));
	if (!ir || !ir->d)
		return;

	if (ir->usbdev) {
		/* Only unregister once */
		ir->usbdev = NULL;
		unregister_from_lirc(ir);
	}

	/* This also removes the current remote from remote_list */
	free_irctl(ir, FREE_ALL);
}

static struct usb_driver usb_remote_driver = {
	.name		= DRIVER_NAME,
	.probe		= usb_remote_probe,
	.disconnect	= usb_remote_disconnect,
	.id_table	= usb_remote_table
};

static int __init usb_remote_init(void)
{
	int i;

	INIT_LIST_HEAD(&remote_list);

	printk(KERN_INFO "\n" DRIVER_NAME ": " DRIVER_DESC " "
	       DRIVER_VERSION "\n");
	printk(DRIVER_NAME ": " DRIVER_AUTHOR "\n");
	dprintk(DRIVER_NAME ": debug mode enabled: "
		"$Id: lirc_xbox.c,v 1.88 2011/06/05 11:11:11 jmartin Exp $\n");

	repeat_jiffies = repeat*HZ/100;

	i = usb_register(&usb_remote_driver);
	if (i) {
		printk(DRIVER_NAME ": usb register failed, result = %d\n", i);
		return -ENODEV;
	}

	return 0;
}

static void __exit usb_remote_exit(void)
{
	usb_deregister(&usb_remote_driver);
}

module_init(usb_remote_init);
module_exit(usb_remote_exit);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(usb, usb_remote_table);

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug enabled or not (default: 0)");

module_param(mask, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(mask, "Set channel acceptance bit mask (default: 0xFFFF)");

module_param(unique, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(unique, "Enable channel-specific codes (default: 0)");

module_param(repeat, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(repeat, "Repeat timeout (1/100 sec) (default: 10)");
