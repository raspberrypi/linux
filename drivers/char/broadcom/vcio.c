/*
 *  Copyright (C) 2010 Broadcom
 *  Copyright (C) 2015 Noralf Trønnes
 *  Copyright (C) 2021 Raspberry Pi (Trading) Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <linux/miscdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define MODULE_NAME "vcio"
#define VCIO_IOC_MAGIC 100
#define IOCTL_MBOX_PROPERTY _IOWR(VCIO_IOC_MAGIC, 0, char *)
#ifdef CONFIG_COMPAT
#define IOCTL_MBOX_PROPERTY32 _IOWR(VCIO_IOC_MAGIC, 0, compat_uptr_t)
#endif

struct vcio_data {
	struct rpi_firmware *fw;
	struct miscdevice misc_dev;
};

static int vcio_user_property_list(struct vcio_data *vcio, void *user)
{
	u32 *buf, size;
	int ret;

	/* The first 32-bit is the size of the buffer */
	if (copy_from_user(&size, user, sizeof(size)))
		return -EFAULT;

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, user, size)) {
		kfree(buf);
		return -EFAULT;
	}

	/* Strip off protocol encapsulation */
	ret = rpi_firmware_property_list(vcio->fw, &buf[2], size - 12);
	if (ret) {
		kfree(buf);
		return ret;
	}

	buf[1] = RPI_FIRMWARE_STATUS_SUCCESS;
	if (copy_to_user(user, buf, size))
		ret = -EFAULT;

	kfree(buf);

	return ret;
}

static int vcio_device_open(struct inode *inode, struct file *file)
{
	try_module_get(THIS_MODULE);

	return 0;
}

static int vcio_device_release(struct inode *inode, struct file *file)
{
	module_put(THIS_MODULE);

	return 0;
}

static long vcio_device_ioctl(struct file *file, unsigned int ioctl_num,
			      unsigned long ioctl_param)
{
	struct vcio_data *vcio = container_of(file->private_data,
					      struct vcio_data, misc_dev);

	switch (ioctl_num) {
	case IOCTL_MBOX_PROPERTY:
		return vcio_user_property_list(vcio, (void *)ioctl_param);
	default:
		pr_err("unknown ioctl: %x\n", ioctl_num);
		return -EINVAL;
	}
}

#ifdef CONFIG_COMPAT
static long vcio_device_compat_ioctl(struct file *file, unsigned int ioctl_num,
				     unsigned long ioctl_param)
{
	struct vcio_data *vcio = container_of(file->private_data,
					      struct vcio_data, misc_dev);

	switch (ioctl_num) {
	case IOCTL_MBOX_PROPERTY32:
		return vcio_user_property_list(vcio, compat_ptr(ioctl_param));
	default:
		pr_err("unknown ioctl: %x\n", ioctl_num);
		return -EINVAL;
	}
}
#endif

const struct file_operations vcio_fops = {
	.unlocked_ioctl = vcio_device_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = vcio_device_compat_ioctl,
#endif
	.open = vcio_device_open,
	.release = vcio_device_release,
};

static int vcio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *fw_node;
	struct rpi_firmware *fw;
	struct vcio_data *vcio;

	fw_node = of_get_parent(np);
	if (!fw_node) {
		dev_err(dev, "Missing firmware node\n");
		return -ENOENT;
	}

	fw = rpi_firmware_get(fw_node);
	of_node_put(fw_node);
	if (!fw)
		return -EPROBE_DEFER;

	vcio = devm_kzalloc(dev, sizeof(struct vcio_data), GFP_KERNEL);
	if (!vcio)
		return -ENOMEM;

	vcio->fw = fw;
	vcio->misc_dev.fops = &vcio_fops;
	vcio->misc_dev.minor = MISC_DYNAMIC_MINOR;
	vcio->misc_dev.name = "vcio";
	vcio->misc_dev.parent = dev;

	return misc_register(&vcio->misc_dev);
}

static int vcio_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	misc_deregister(dev_get_drvdata(dev));
	return 0;
}

static const struct of_device_id vcio_ids[] = {
	{ .compatible = "raspberrypi,vcio" },
	{ }
};
MODULE_DEVICE_TABLE(of, vcio_ids);

static struct platform_driver vcio_driver = {
	.driver	= {
		.name		= MODULE_NAME,
		.of_match_table	= of_match_ptr(vcio_ids),
	},
	.probe	= vcio_probe,
	.remove = vcio_remove,
};

module_platform_driver(vcio_driver);

MODULE_AUTHOR("Gray Girling");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_DESCRIPTION("Mailbox userspace access");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:rpi-vcio");
