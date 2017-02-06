/*
 * Defines interfaces for interacting wtih the Raspberry Pi firmware's
 * property channel.
 *
 * Copyright © 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/dma-mapping.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define MBOX_MSG(chan, data28)		(((data28) & ~0xf) | ((chan) & 0xf))
#define MBOX_CHAN(msg)			((msg) & 0xf)
#define MBOX_DATA28(msg)		((msg) & ~0xf)
#define MBOX_CHAN_PROPERTY		8

struct rpi_firmware {
	struct mbox_client cl;
	struct mbox_chan *chan; /* The property channel. */
	struct completion c;
	u32 enabled;

	struct vc4_dev *vc4;
	int (*vc4_qpu_execute)(struct vc4_dev *vc4,
			       u32 num_qpu,
			       u32 control,
			       u32 noflush,
			       u32 timeout);
};

static struct platform_device *g_pdev;

static DEFINE_MUTEX(transaction_lock);

static void response_callback(struct mbox_client *cl, void *msg)
{
	struct rpi_firmware *fw = container_of(cl, struct rpi_firmware, cl);
	complete(&fw->c);
}

/*
 * Sends a request to the firmware through the BCM2835 mailbox driver,
 * and synchronously waits for the reply.
 */
int
rpi_firmware_transaction(struct rpi_firmware *fw, u32 chan, u32 data)
{
	u32 message = MBOX_MSG(chan, data);
	int ret;

	WARN_ON(data & 0xf);

	mutex_lock(&transaction_lock);
	reinit_completion(&fw->c);
	ret = mbox_send_message(fw->chan, &message);
	if (ret >= 0) {
		wait_for_completion(&fw->c);
		ret = 0;
	} else {
		dev_err(fw->cl.dev, "mbox_send_message returned %d\n", ret);
	}
	mutex_unlock(&transaction_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(rpi_firmware_transaction);

/**
 * Peeks at the property request to see if it's something that we
 * should pass off to vc4 instead.
 */
static int
vc4_filter_property(struct rpi_firmware *fw, uint32_t *data, size_t tag_size)
{
	uint32_t tag = data[0];
	int ret;

	if (!fw->vc4)
		return -ENOENT;

	switch (tag) {
	case RPI_FIRMWARE_EXECUTE_QPU: {
		struct qpu_execute_packet {
			u32 tag;
			u32 bufsize;
			u32 size;
			u32 num_qpu;
			u32 control;
			u32 noflush;
			u32 timeout_ms;
		} *packet = (void *)data;

		ret = fw->vc4_qpu_execute(fw->vc4,
					  packet->num_qpu,
					  packet->control,
					  packet->noflush,
					  packet->timeout_ms);

		packet->num_qpu = (ret != 0);

		return 0;
	}

	case RPI_FIRMWARE_SET_ENABLE_QPU: {
		struct qpu_enable_packet {
			u32 tag;
			u32 bufsize;
			u32 size;
			u32 enable;
		} *packet = (void *)data;
		/* If vc4 is present, userspace doesn't get to control
		 * when the QPUs are off or on.  Just hand back the
		 * return value indicating success.
		 */
		packet->enable = 0;

		return 0;
	}

	default:
		return -ENOENT;
	}
}

/**
 * rpi_firmware_property_list - Submit firmware property list
 * @fw:		Pointer to firmware structure from rpi_firmware_get().
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
int rpi_firmware_property_list(struct rpi_firmware *fw,
			       void *data, size_t tag_size)
{
	size_t size = tag_size + 12;
	u32 *buf;
	dma_addr_t bus_addr;
	int ret;

	/* NOTE: We're only handling filtering on the first property
	 * here, and if it gets filtered then we skip the rest of
	 * them.  This is enough for hello_fft.
	 */
	ret = vc4_filter_property(fw, data, tag_size);
	if (ret != -ENOENT)
		return ret;

	/* Packets are processed a dword at a time. */
	if (size & 3)
		return -EINVAL;

	buf = dma_alloc_coherent(fw->cl.dev, PAGE_ALIGN(size), &bus_addr,
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

	ret = rpi_firmware_transaction(fw, MBOX_CHAN_PROPERTY, bus_addr);

	rmb();
	memcpy(data, &buf[2], tag_size);
	if (ret == 0 && buf[1] != RPI_FIRMWARE_STATUS_SUCCESS) {
		/*
		 * The tag name here might not be the one causing the
		 * error, if there were multiple tags in the request.
		 * But single-tag is the most common, so go with it.
		 */
		dev_err(fw->cl.dev, "Request 0x%08x returned status 0x%08x\n",
			buf[2], buf[1]);
		ret = -EINVAL;
	}

	dma_free_coherent(fw->cl.dev, PAGE_ALIGN(size), buf, bus_addr);

	return ret;
}
EXPORT_SYMBOL_GPL(rpi_firmware_property_list);

/**
 * rpi_firmware_property - Submit single firmware property
 * @fw:		Pointer to firmware structure from rpi_firmware_get().
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
int rpi_firmware_property(struct rpi_firmware *fw,
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

	ret = rpi_firmware_property_list(fw, &data, sizeof(data));
	memcpy(tag_data,
	       data + sizeof(struct rpi_firmware_property_tag_header),
	       buf_size);

	return ret;
}
EXPORT_SYMBOL_GPL(rpi_firmware_property);

static void
rpi_firmware_print_firmware_revision(struct rpi_firmware *fw)
{
	u32 packet;
	int ret = rpi_firmware_property(fw,
					RPI_FIRMWARE_GET_FIRMWARE_REVISION,
					&packet, sizeof(packet));

	if (ret == 0) {
		struct tm tm;

		time_to_tm(packet, 0, &tm);

		dev_info(fw->cl.dev,
			 "Attached to firmware from %04ld-%02d-%02d %02d:%02d\n",
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			 tm.tm_hour, tm.tm_min);
	}
}

static int raspberrypi_firmware_set_power(struct rpi_firmware *fw,
					  u32 domain, bool on)
{
	struct {
		u32 domain;
		u32 on;
	} packet;
	int ret;

	packet.domain = domain;
	packet.on = on;
	ret = rpi_firmware_property(fw, RPI_FIRMWARE_SET_POWER_STATE,
				    &packet, sizeof(packet));
	if (!ret && packet.on != on)
		ret = -EINVAL;

	return ret;
}

static int rpi_firmware_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rpi_firmware *fw;

	fw = devm_kzalloc(dev, sizeof(*fw), GFP_KERNEL);
	if (!fw)
		return -ENOMEM;

	fw->cl.dev = dev;
	fw->cl.rx_callback = response_callback;
	fw->cl.tx_block = true;

	fw->chan = mbox_request_channel(&fw->cl, 0);
	if (IS_ERR(fw->chan)) {
		int ret = PTR_ERR(fw->chan);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get mbox channel: %d\n", ret);
		return ret;
	}

	init_completion(&fw->c);

	platform_set_drvdata(pdev, fw);
	g_pdev = pdev;

	rpi_firmware_print_firmware_revision(fw);

	if (raspberrypi_firmware_set_power(fw, 3, true))
		dev_err(dev, "failed to turn on USB power\n");

	return 0;
}

static int rpi_firmware_remove(struct platform_device *pdev)
{
	struct rpi_firmware *fw = platform_get_drvdata(pdev);

	mbox_free_channel(fw->chan);
	g_pdev = NULL;

	return 0;
}

/**
 * rpi_firmware_get - Get pointer to rpi_firmware structure.
 * @firmware_node:    Pointer to the firmware Device Tree node.
 *
 * Returns NULL is the firmware device is not ready.
 */
struct rpi_firmware *rpi_firmware_get(struct device_node *firmware_node)
{
	struct platform_device *pdev = g_pdev;

	if (!pdev)
		return NULL;

	return platform_get_drvdata(pdev);
}
EXPORT_SYMBOL_GPL(rpi_firmware_get);

/**
 * Called by the vc4 driver at its probe time, to request that QPU
 * execution requests be redirected to it.
 */
void rpi_firmware_register_vc4(struct rpi_firmware *fw, struct vc4_dev *vc4,
			       int (*qpu_execute)(struct vc4_dev *vc4,
						  u32 num_qpu,
						  u32 control,
						  u32 noflush,
						  u32 timeout))
{
	fw->vc4 = vc4;
	fw->vc4_qpu_execute = qpu_execute;
}
EXPORT_SYMBOL_GPL(rpi_firmware_register_vc4);

static const struct of_device_id rpi_firmware_of_match[] = {
	{ .compatible = "raspberrypi,bcm2835-firmware", },
	{},
};
MODULE_DEVICE_TABLE(of, rpi_firmware_of_match);

static struct platform_driver rpi_firmware_driver = {
	.driver = {
		.name = "raspberrypi-firmware",
		.of_match_table = rpi_firmware_of_match,
	},
	.probe		= rpi_firmware_probe,
	.remove		= rpi_firmware_remove,
};

static int __init rpi_firmware_init(void)
{
	return platform_driver_register(&rpi_firmware_driver);
}
subsys_initcall(rpi_firmware_init);

static void __init rpi_firmware_exit(void)
{
	platform_driver_unregister(&rpi_firmware_driver);
}
module_exit(rpi_firmware_exit);

MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("Raspberry Pi firmware driver");
MODULE_LICENSE("GPL v2");
