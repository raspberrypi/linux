/*
 *  Copyright © 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * Defines interfaces for interacting wtih the Raspberry Pi firmware's
 * property channel.
 */

#include <linux/dma-mapping.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_data/mailbox-bcm2708.h>
#include <linux/platform_device.h>
#include <soc/bcm2835/raspberrypi-firmware-property.h>

#define MBOX_MSG(chan, data28)		(((data28) & ~0xf) | ((chan) & 0xf))
#define MBOX_CHAN(msg)			((msg) & 0xf)
#define MBOX_DATA28(msg)		((msg) & ~0xf)
#define MBOX_CHAN_PROPERTY		8

static DEFINE_MUTEX(transaction_lock);

/*
 * Sends a request to the firmware through the BCM2835 mailbox driver,
 * and synchronously waits for the reply.
 */
static int
rpi_firmware_transaction(u32 chan, u32 data)
{
	int ret;

	WARN_ON(data & 0xf);

	mutex_lock(&transaction_lock);
	ret = bcm_mailbox_write(chan, data);
	if (ret == 0) {
		u32 ret;

		ret = bcm_mailbox_read(chan, &ret);
		if (ret != 0)
			pr_err("rpi_firmware_transaction() failed to read\n");

	} else {
		pr_err("rpi_firmware_transaction() failed to write\n");
	}
	mutex_unlock(&transaction_lock);

	return ret;
}

/**
 * rpi_firmware_property_list - Submit firmware property list
 * @of_node:	Pointer to the firmware Device Tree node.
 * @data:	Buffer holding tags.
 * @tag_size:	Size of tags buffer.
 *
 * Submits a set of concatenated tags to the VPU firmware through the
 * mailbox property interface.
 *
 * The buffer header and the ending tag are added by this function and
 * don't need to be supplied, just the actual tags for your operation.
 * See struct rpi_firmware_property_tag_header for the per-tag
 * structure.
 */
int rpi_firmware_property_list(struct device *dev,
			       void *data, size_t tag_size)
{
	size_t size = tag_size + 12;
	u32 *buf;
	dma_addr_t bus_addr;
	int ret = 0;

	/* Packets are processed a dword at a time. */
	if (size & 3)
		return -EINVAL;

	buf = dma_alloc_coherent(dev, PAGE_ALIGN(size), &bus_addr,
				 GFP_ATOMIC);
	if (!buf)
		return -ENOMEM;

	/* The firmware will error out without parsing in this case. */
	WARN_ON(size >= 1024 * 1024);

	buf[0] = size;
	buf[1] = RPI_FIRMWARE_STATUS_REQUEST;
	memcpy(&buf[2], data, tag_size);
	buf[size / 4 - 1] = RPI_FIRMWARE_PROPERTY_END;
	wmb();

	ret = rpi_firmware_transaction(MBOX_CHAN_PROPERTY, bus_addr);

	rmb();
	memcpy(data, &buf[2], tag_size);
	if (ret == 0 && buf[1] != RPI_FIRMWARE_STATUS_SUCCESS) {
		/*
		 * The tag name here might not be the one causing the
		 * error, if there were multiple tags in the request.
		 * But single-tag is the most common, so go with it.
		 */
		dev_err(dev, "Request 0x%08x returned status 0x%08x\n",
			buf[2], buf[1]);
		ret = -EINVAL;
	}

	dma_free_coherent(dev, PAGE_ALIGN(size), buf, bus_addr);

	return ret;
}
EXPORT_SYMBOL_GPL(rpi_firmware_property_list);

/**
 * rpi_firmware_property - Submit single firmware property
 * @of_node:	Pointer to the firmware Device Tree node.
 * @tag:	One of enum_mbox_property_tag.
 * @tag_data:	Tag data buffer.
 * @buf_size:	Buffer size.
 *
 * Submits a single tag to the VPU firmware through the mailbox
 * property interface.
 *
 * This is a convenience wrapper around
 * rpi_firmware_property_list() to avoid some of the
 * boilerplate in property calls.
 */
int rpi_firmware_property(struct device *dev,
			  u32 tag, void *tag_data, size_t buf_size)
{
	/* Single tags are very small (generally 8 bytes), so the
	 * stack should be safe.
	 */
	u8 data[buf_size + sizeof(struct rpi_firmware_property_tag_header)];
	struct rpi_firmware_property_tag_header *header =
		(struct rpi_firmware_property_tag_header *)data;
	int ret;

	header->tag = tag;
	header->buf_size = buf_size;
	header->req_resp_size = 0;
	memcpy(data + sizeof(struct rpi_firmware_property_tag_header),
	       tag_data, buf_size);

	ret = rpi_firmware_property_list(dev, &data, sizeof(data));
	memcpy(tag_data,
	       data + sizeof(struct rpi_firmware_property_tag_header),
	       buf_size);

	return ret;
}
EXPORT_SYMBOL_GPL(rpi_firmware_property);

MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("Raspberry Pi firmware driver");
MODULE_LICENSE("GPL v2");
