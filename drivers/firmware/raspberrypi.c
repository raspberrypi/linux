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
#include <linux/workqueue.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define MBOX_MSG(chan, data28)		(((data28) & ~0xf) | ((chan) & 0xf))
#define MBOX_CHAN(msg)			((msg) & 0xf)
#define MBOX_DATA28(msg)		((msg) & ~0xf)
#define MBOX_CHAN_PROPERTY		8

#define UNDERVOLTAGE_BIT		BIT(0)


/*
 * This section defines some rate limited logging that prevent
 * repeated messages at much lower Hz than the default kernel settings.
 * It's usually 5s, this is 5 minutes.
 * Burst 3 means you may get three messages 'quickly', before
 * the ratelimiting kicks in.
 */
#define LOCAL_RATELIMIT_INTERVAL (5 * 60 * HZ)
#define LOCAL_RATELIMIT_BURST 3

#ifdef CONFIG_PRINTK
#define printk_ratelimited_local(fmt, ...)	\
({						\
	static DEFINE_RATELIMIT_STATE(_rs,	\
		LOCAL_RATELIMIT_INTERVAL,	\
		LOCAL_RATELIMIT_BURST);		\
						\
	if (__ratelimit(&_rs))			\
		printk(fmt, ##__VA_ARGS__);	\
})
#else
#define printk_ratelimited_local(fmt, ...)	\
	no_printk(fmt, ##__VA_ARGS__)
#endif

#define pr_crit_ratelimited_local(fmt, ...)              \
	printk_ratelimited_local(KERN_CRIT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info_ratelimited_local(fmt, ...)              \
	printk_ratelimited_local(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)


struct rpi_firmware {
	struct mbox_client cl;
	struct mbox_chan *chan; /* The property channel. */
	struct completion c;
	u32 enabled;
	struct delayed_work get_throttled_poll_work;
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
		dev_dbg(fw->cl.dev, "Request 0x%08x returned status 0x%08x\n",
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

static int rpi_firmware_get_throttled(struct rpi_firmware *fw, u32 *value)
{
	static int old_firmware;
	static ktime_t old_timestamp;
	static u32 old_value;
	u32 new_sticky, old_sticky, new_uv, old_uv;
	ktime_t new_timestamp;
	s64 elapsed_ms;
	int ret;

	if (!fw)
		return -EBUSY;

	if (old_firmware)
		return -EINVAL;

	/*
	 * We can't run faster than the sticky shift (100ms) since we get
	 * flipping in the sticky bits that are cleared.
	 * This happens on polling, so just return the previous value.
	 */
	new_timestamp = ktime_get();
	elapsed_ms = ktime_ms_delta(new_timestamp, old_timestamp);
	if (elapsed_ms < 150) {
		*value = old_value;
		return 0;
	}
	old_timestamp = new_timestamp;

	/* Clear sticky bits */
	*value = 0xffff;

	ret = rpi_firmware_property(fw, RPI_FIRMWARE_GET_THROTTLED,
				    value, sizeof(*value));

	if (ret) {
		/* If the mailbox call fails once, then it will continue to
		 * fail in the future, so no point in continuing to call it
		 * Usual failure reason is older firmware
		 */
		old_firmware = 1;
		dev_err(fw->cl.dev, "Get Throttled mailbox call failed");

		return ret;
	}

	new_sticky = *value >> 16;
	old_sticky = old_value >> 16;
	old_value = *value;

	/* Only notify about changes in the sticky bits */
	if (new_sticky == old_sticky)
		return 0;

	new_uv = new_sticky & UNDERVOLTAGE_BIT;
	old_uv = old_sticky & UNDERVOLTAGE_BIT;

	if (new_uv != old_uv) {
		if (new_uv)
			pr_crit_ratelimited_local(
				"Under-voltage detected! (0x%08x)\n",
				 *value);
		else
			pr_info_ratelimited_local(
				"Voltage normalised (0x%08x)\n",
				 *value);
	}

	sysfs_notify(&fw->cl.dev->kobj, NULL, "get_throttled");

	return 0;
}

static void get_throttled_poll(struct work_struct *work)
{
	struct rpi_firmware *fw = container_of(work, struct rpi_firmware,
					       get_throttled_poll_work.work);
	u32 dummy;
	int ret;

	ret = rpi_firmware_get_throttled(fw, &dummy);

	/* Only reschedule if we are getting valid responses */
	if (!ret)
		schedule_delayed_work(&fw->get_throttled_poll_work, 2 * HZ);
}

static ssize_t get_throttled_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct rpi_firmware *fw = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = rpi_firmware_get_throttled(fw, &value);
	if (ret)
		return ret;

	return sprintf(buf, "%x\n", value);
}

static DEVICE_ATTR_RO(get_throttled);

static struct attribute *rpi_firmware_dev_attrs[] = {
	&dev_attr_get_throttled.attr,
	NULL,
};

static const struct attribute_group rpi_firmware_dev_group = {
	.attrs = rpi_firmware_dev_attrs,
};

static void
rpi_firmware_print_firmware_revision(struct rpi_firmware *fw)
{
	u32 packet;
	int ret = rpi_firmware_property(fw,
					RPI_FIRMWARE_GET_FIRMWARE_REVISION,
					&packet, sizeof(packet));

	if (ret == 0) {
		struct tm tm;

		time64_to_tm(packet, 0, &tm);

		dev_info(fw->cl.dev,
			 "Attached to firmware from %04ld-%02d-%02d %02d:%02d\n",
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			 tm.tm_hour, tm.tm_min);
	}
}

static int rpi_firmware_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rpi_firmware *fw;
	int ret;

	ret = devm_device_add_group(dev, &rpi_firmware_dev_group);
	if (ret)
		return ret;

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
	INIT_DELAYED_WORK(&fw->get_throttled_poll_work, get_throttled_poll);

	platform_set_drvdata(pdev, fw);
	g_pdev = pdev;

	rpi_firmware_print_firmware_revision(fw);

	schedule_delayed_work(&fw->get_throttled_poll_work, 0);

	return 0;
}

static int rpi_firmware_remove(struct platform_device *pdev)
{
	struct rpi_firmware *fw = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&fw->get_throttled_poll_work);
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
