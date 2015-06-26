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

/* Tags permitted via /dev/vcio_provisioning. */
static const u32 vcio_provisioning_tags[] = {
	0x00038021, /* SET_CUSTOMER_OTP */
	0x00038024, /* SET_USER_OTP */
	0x00038082, /* SET_CUSTOMER_ETHER_MAC_OTP */
	0x00038083, /* SET_CUSTOMER_WIFI_MAC_OTP */
	0x00038084, /* SET_CUSTOMER_BT_MAC_OTP */
	0x00030085, /* TST_MAC_OTP */
	0x00030086, /* SET_CUSTOMER_OTP_LOCK */
};

/* Tags permitted via /dev/vcio_ab. */
static const u32 vcio_ab_tags[] = {
	0x00030048, /* SET_NOTIFY_REBOOT */
	0x00030057, /* SET_GLOBAL_RESET */
	0x00030064, /* GET_REBOOT_FLAGS */
	0x00038064, /* SET_REBOOT_FLAGS */
	0x0003008b, /* GET_REBOOT_ORDER */
	0x0003808b, /* SET_REBOOT_ORDER */
	0x0003008c, /* GET_REBOOT_ARG */
	0x0003808c, /* SET_REBOOT_ARG */
	0x0003008d, /* GET_BOOT_COUNT */
	0x0003808d, /* SET_BOOT_COUNT */
	0x00040011, /* GET_RSTS */
	0x00030096, /* GET_EEPROM_PACKET */
	0x00038096, /* SET_EEPROM_PACKET */
	0x00030097, /* GET_EEPROM_UPDATE_STATUS */
	0x00038097, /* SET_EEPROM_UPDATE_STATUS */
	0x00030098, /* GET_EEPROM_PARTITION */
	0x00038098, /* SET_EEPROM_PARTITION */
	0x00030099, /* GET_EEPROM_AB_PARAMS */
	0x00038099, /* SET_EEPROM_AB_PARAMS */
};

/* Tags permitted via /dev/vcio_gencmd. */
static const u32 vcio_gencmd_tags[] = {
	0x00030080, /* GET_GENCMD_RESULT */
};

/* Tags permitted via /dev/vcio_crypto. */
static const u32 vcio_crypto_tags[] = {
	0x0003008e, /* GET_CRYPTO_LAST_ERROR */
	0x0003008f, /* GET_CRYPTO_NUM_OTP_KEYS */
	0x00030090, /* GET_CRYPTO_KEY_STATUS */
	0x00038090, /* SET_CRYPTO_KEY_STATUS */
	0x00030091, /* GET_CRYPTO_ECDSA_SIGN */
	0x00030092, /* GET_CRYPTO_HMAC_SHA256 */
	0x00030093, /* GET_CRYPTO_PUBLIC_KEY */
	0x00030094, /* GET_CRYPTO_PRIVATE_KEY */
	0x00030095, /* GET_CRYPTO_GEN_ECDSA */
};

struct vcio_group {
	const char *name;
	const u32 *tags;
	size_t num_tags;
};

#define VCIO_GROUP(suffix) {						\
	.name = "vcio_" #suffix,					\
	.tags = vcio_ ## suffix ## _tags,				\
	.num_tags = ARRAY_SIZE(vcio_ ## suffix ## _tags),		\
}

static const struct vcio_group vcio_groups[] = {
	VCIO_GROUP(provisioning),
	VCIO_GROUP(ab),
	VCIO_GROUP(gencmd),
	VCIO_GROUP(crypto),
};

struct vcio_dev {
	struct miscdevice misc_dev;
	struct rpi_firmware *fw;
	const u32 *allowed_tags;
	size_t num_allowed_tags;
};

struct vcio_data {
	struct rpi_firmware *fw;
	struct vcio_dev vcio;
	struct vcio_dev groups[ARRAY_SIZE(vcio_groups)];
};

static bool vcio_tag_allowed(const struct vcio_dev *vdev, u32 tag)
{
	size_t i;

	if (!vdev->allowed_tags)
		return true;

	for (i = 0; i < vdev->num_allowed_tags; i++)
		if (vdev->allowed_tags[i] == tag)
			return true;

	return false;
}

static int vcio_check_tags(const struct vcio_dev *vdev, const u32 *buf,
			   size_t size)
{
	size_t offset = 8; /* skip total size and status words */

	if (!vdev->allowed_tags)
		return 0;

	while (offset + 12 <= size) {
		u32 tag = buf[offset / 4];
		u32 val_size;

		if (tag == 0)
			return 0;

		if (!vcio_tag_allowed(vdev, tag)) {
			pr_warn_ratelimited("%s: tag 0x%08x not permitted\n",
					    vdev->misc_dev.name, tag);
			return -EPERM;
		}

		val_size = buf[offset / 4 + 1];
		if (val_size > size - offset - 12)
			return -EINVAL;
		offset += 12 + ALIGN(val_size, 4);

		if (offset > size)
			return -EINVAL;
	}

	return 0;
}

static int vcio_user_property_list(struct vcio_dev *vdev, void *user)
{
	u32 *buf, size;
	int ret;

	/* The first 32-bit is the size of the buffer */
	if (copy_from_user(&size, user, sizeof(size)))
		return -EFAULT;

	if (size < 12)
		return -EINVAL;

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, user, size)) {
		kfree(buf);
		return -EFAULT;
	}

	ret = vcio_check_tags(vdev, buf, size);
	if (ret) {
		kfree(buf);
		return ret;
	}

	/* Strip off protocol encapsulation */
	ret = rpi_firmware_property_list(vdev->fw, &buf[2], size - 12);
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
	struct vcio_dev *vdev = container_of(file->private_data,
					     struct vcio_dev, misc_dev);

	switch (ioctl_num) {
	case IOCTL_MBOX_PROPERTY:
		return vcio_user_property_list(vdev, (void *)ioctl_param);
	default:
		pr_err("unknown ioctl: %x\n", ioctl_num);
		return -EINVAL;
	}
}

#ifdef CONFIG_COMPAT
static long vcio_device_compat_ioctl(struct file *file, unsigned int ioctl_num,
				     unsigned long ioctl_param)
{
	struct vcio_dev *vdev = container_of(file->private_data,
					     struct vcio_dev, misc_dev);

	switch (ioctl_num) {
	case IOCTL_MBOX_PROPERTY32:
		return vcio_user_property_list(vdev, compat_ptr(ioctl_param));
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

static int vcio_register(struct device *dev, struct vcio_dev *vdev,
			 struct rpi_firmware *fw, const char *name,
			 const u32 *allowed_tags, size_t num_allowed_tags)
{
	vdev->fw = fw;
	vdev->allowed_tags = allowed_tags;
	vdev->num_allowed_tags = num_allowed_tags;
	vdev->misc_dev.fops = &vcio_fops;
	vdev->misc_dev.minor = MISC_DYNAMIC_MINOR;
	vdev->misc_dev.name = name;
	vdev->misc_dev.parent = dev;

	return misc_register(&vdev->misc_dev);
}

static int vcio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *fw_node;
	struct rpi_firmware *fw;
	struct vcio_data *vcio;
	size_t i;
	int ret;

	fw_node = of_get_parent(np);
	if (!fw_node) {
		dev_err(dev, "Missing firmware node\n");
		return -ENOENT;
	}

	fw = rpi_firmware_get(fw_node);
	of_node_put(fw_node);
	if (!fw)
		return -EPROBE_DEFER;

	vcio = devm_kzalloc(dev, sizeof(*vcio), GFP_KERNEL);
	if (!vcio)
		return -ENOMEM;

	vcio->fw = fw;
	platform_set_drvdata(pdev, vcio);

	ret = vcio_register(dev, &vcio->vcio, fw, "vcio", NULL, 0);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(vcio_groups); i++) {
		const struct vcio_group *g = &vcio_groups[i];

		ret = vcio_register(dev, &vcio->groups[i], fw, g->name,
				    g->tags, g->num_tags);
		if (ret)
			goto err_groups;
	}

	return 0;

err_groups:
	while (i--)
		misc_deregister(&vcio->groups[i].misc_dev);
	misc_deregister(&vcio->vcio.misc_dev);
	return ret;
}

static void vcio_remove(struct platform_device *pdev)
{
	struct vcio_data *vcio = platform_get_drvdata(pdev);
	size_t i = ARRAY_SIZE(vcio_groups);

	while (i--)
		misc_deregister(&vcio->groups[i].misc_dev);
	misc_deregister(&vcio->vcio.misc_dev);
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
