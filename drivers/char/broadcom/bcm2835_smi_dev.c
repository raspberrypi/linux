/**
 * Character device driver for Broadcom Secondary Memory Interface
 *
 * Written by Luke Wren <luke@raspberrypi.org>
 * Copyright (c) 2015, Raspberry Pi (Trading) Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2, as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/fs.h>

#include <linux/broadcom/bcm2835_smi.h>

#define DEVICE_NAME "bcm2835-smi-dev"
#define DRIVER_NAME "smi-dev-bcm2835"
#define DEVICE_MINOR 0

static struct cdev bcm2835_smi_cdev;
static dev_t bcm2835_smi_devid;
static struct class *bcm2835_smi_class;
static struct device *bcm2835_smi_dev;

struct bcm2835_smi_dev_instance {
	struct device *dev;
};

static struct bcm2835_smi_instance *smi_inst;
static struct bcm2835_smi_dev_instance *inst;

static const char *const ioctl_names[] = {
	"READ_SETTINGS",
	"WRITE_SETTINGS",
	"ADDRESS"
};

/****************************************************************************
*
*   SMI chardev file ops
*
***************************************************************************/
static long
bcm2835_smi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = 0;

	dev_info(inst->dev, "serving ioctl...");

	switch (cmd) {
	case BCM2835_SMI_IOC_GET_SETTINGS:{
		struct smi_settings *settings;

		dev_info(inst->dev, "Reading SMI settings to user.");
		settings = bcm2835_smi_get_settings_from_regs(smi_inst);
		if (copy_to_user((void *)arg, settings,
				 sizeof(struct smi_settings)))
			dev_err(inst->dev, "settings copy failed.");
		break;
	}
	case BCM2835_SMI_IOC_WRITE_SETTINGS:{
		struct smi_settings *settings;

		dev_info(inst->dev, "Setting user's SMI settings.");
		settings = bcm2835_smi_get_settings_from_regs(smi_inst);
		if (copy_from_user(settings, (void *)arg,
				   sizeof(struct smi_settings)))
			dev_err(inst->dev, "settings copy failed.");
		else
			bcm2835_smi_set_regs_from_settings(smi_inst);
		break;
	}
	case BCM2835_SMI_IOC_ADDRESS:
		dev_info(inst->dev, "SMI address set: 0x%02x", (int)arg);
		bcm2835_smi_set_address(smi_inst, arg);
		break;
	default:
		dev_err(inst->dev, "invalid ioctl cmd: %d", cmd);
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static int bcm2835_smi_open(struct inode *inode, struct file *file)
{
	int dev = iminor(inode);

	dev_dbg(inst->dev, "SMI device opened.");

	if (dev != DEVICE_MINOR) {
		dev_err(inst->dev,
			"bcm2835_smi_release: Unknown minor device: %d",
			dev);
		return -ENXIO;
	}

	return 0;
}

static int bcm2835_smi_release(struct inode *inode, struct file *file)
{
	int dev = iminor(inode);

	if (dev != DEVICE_MINOR) {
		dev_err(inst->dev,
			"bcm2835_smi_release: Unknown minor device %d", dev);
		return -ENXIO;
	}

	return 0;
}

static ssize_t dma_bounce_user(
	enum dma_transfer_direction dma_dir,
	char __user *user_ptr,
	size_t count,
	struct bcm2835_smi_bounce_info *bounce)
{
	int chunk_size;
	int chunk_no = 0;
	int count_left = count;

	while (count_left) {
		int rv;
		void *buf;

		/* Wait for current chunk to complete: */
		if (down_timeout(&bounce->callback_sem,
			msecs_to_jiffies(1000))) {
			dev_err(inst->dev, "DMA bounce timed out");
			count -= (count_left);
			break;
		}

		if (bounce->callback_sem.count >= DMA_BOUNCE_BUFFER_COUNT - 1)
			dev_err(inst->dev, "WARNING: Ring buffer overflow");
		chunk_size = count_left > DMA_BOUNCE_BUFFER_SIZE ?
			DMA_BOUNCE_BUFFER_SIZE : count_left;
		buf = bounce->buffer[chunk_no % DMA_BOUNCE_BUFFER_COUNT];
		if (dma_dir == DMA_DEV_TO_MEM)
			rv = copy_to_user(user_ptr, buf, chunk_size);
		else
			rv = copy_from_user(buf, user_ptr, chunk_size);
		if (rv)
			dev_err(inst->dev, "copy_*_user() failed!: %d", rv);
		user_ptr += chunk_size;
		count_left -= chunk_size;
		chunk_no++;
	}
	return count;
}

static ssize_t
bcm2835_read_file(struct file *f, char __user *user_ptr,
		  size_t count, loff_t *offs)
{
	int odd_bytes;
	size_t count_check;

	dev_dbg(inst->dev, "User reading %zu bytes from SMI.", count);
	/* We don't want to DMA a number of bytes % 4 != 0 (32 bit FIFO) */
	if (count > DMA_THRESHOLD_BYTES)
		odd_bytes = count & 0x3;
	else
		odd_bytes = count;
	count -= odd_bytes;
	count_check = count;
	if (count) {
		struct bcm2835_smi_bounce_info *bounce;

		count = bcm2835_smi_user_dma(smi_inst,
			DMA_DEV_TO_MEM, user_ptr, count,
			&bounce);
		if (count)
			count = dma_bounce_user(DMA_DEV_TO_MEM, user_ptr,
				count, bounce);
	}
	if (odd_bytes && (count == count_check)) {
		/* Read from FIFO directly if not using DMA */
		uint8_t buf[DMA_THRESHOLD_BYTES];
		unsigned long bytes_not_transferred;

		bcm2835_smi_read_buf(smi_inst, buf, odd_bytes);
		bytes_not_transferred = copy_to_user(user_ptr + count, buf, odd_bytes);
		if (bytes_not_transferred)
			dev_err(inst->dev, "copy_to_user() failed.");
		count += odd_bytes - bytes_not_transferred;
	}
	return count;
}

static ssize_t
bcm2835_write_file(struct file *f, const char __user *user_ptr,
		   size_t count, loff_t *offs)
{
	int odd_bytes;
	size_t count_check;

	dev_dbg(inst->dev, "User writing %zu bytes to SMI.", count);
	if (count > DMA_THRESHOLD_BYTES)
		odd_bytes = count & 0x3;
	else
		odd_bytes = count;
	count -= odd_bytes;
	count_check = count;
	if (count) {
		struct bcm2835_smi_bounce_info *bounce;

		count = bcm2835_smi_user_dma(smi_inst,
			DMA_MEM_TO_DEV, (char __user *)user_ptr, count,
			&bounce);
		if (count)
			count = dma_bounce_user(DMA_MEM_TO_DEV,
				(char __user *)user_ptr,
				count, bounce);
	}
	if (odd_bytes && (count == count_check)) {
		uint8_t buf[DMA_THRESHOLD_BYTES];
		unsigned long bytes_not_transferred;

		bytes_not_transferred = copy_from_user(buf, user_ptr + count, odd_bytes);
		if (bytes_not_transferred)
			dev_err(inst->dev, "copy_from_user() failed.");
		else
			bcm2835_smi_write_buf(smi_inst, buf, odd_bytes);
		count += odd_bytes - bytes_not_transferred;
	}
	return count;
}

static const struct file_operations
bcm2835_smi_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = bcm2835_smi_ioctl,
	.open = bcm2835_smi_open,
	.release = bcm2835_smi_release,
	.read = bcm2835_read_file,
	.write = bcm2835_write_file,
};


/****************************************************************************
*
*   bcm2835_smi_probe - called when the driver is loaded.
*
***************************************************************************/

static int bcm2835_smi_dev_probe(struct platform_device *pdev)
{
	int err;
	void *ptr_err;
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node, *smi_node;

	if (!node) {
		dev_err(dev, "No device tree node supplied!");
		return -EINVAL;
	}

	smi_node = of_parse_phandle(node, "smi_handle", 0);

	if (!smi_node) {
		dev_err(dev, "No such property: smi_handle");
		return -ENXIO;
	}

	smi_inst = bcm2835_smi_get(smi_node);

	if (!smi_inst)
		return -EPROBE_DEFER;

	/* Allocate buffers and instance data */

	inst = devm_kzalloc(dev, sizeof(*inst), GFP_KERNEL);

	if (!inst)
		return -ENOMEM;

	inst->dev = dev;

	/* Create character device entries */

	err = alloc_chrdev_region(&bcm2835_smi_devid,
				  DEVICE_MINOR, 1, DEVICE_NAME);
	if (err != 0) {
		dev_err(inst->dev, "unable to allocate device number");
		return -ENOMEM;
	}
	cdev_init(&bcm2835_smi_cdev, &bcm2835_smi_fops);
	bcm2835_smi_cdev.owner = THIS_MODULE;
	err = cdev_add(&bcm2835_smi_cdev, bcm2835_smi_devid, 1);
	if (err != 0) {
		dev_err(inst->dev, "unable to register device");
		err = -ENOMEM;
		goto failed_cdev_add;
	}

	/* Create sysfs entries */

	bcm2835_smi_class = class_create(THIS_MODULE, DEVICE_NAME);
	ptr_err = bcm2835_smi_class;
	if (IS_ERR(ptr_err))
		goto failed_class_create;

	bcm2835_smi_dev = device_create(bcm2835_smi_class, NULL,
					bcm2835_smi_devid, NULL,
					"smi");
	ptr_err = bcm2835_smi_dev;
	if (IS_ERR(ptr_err))
		goto failed_device_create;

	dev_info(inst->dev, "initialised");

	return 0;

failed_device_create:
	class_destroy(bcm2835_smi_class);
failed_class_create:
	cdev_del(&bcm2835_smi_cdev);
	err = PTR_ERR(ptr_err);
failed_cdev_add:
	unregister_chrdev_region(bcm2835_smi_devid, 1);
	dev_err(dev, "could not load bcm2835_smi_dev");
	return err;
}

/****************************************************************************
*
*   bcm2835_smi_remove - called when the driver is unloaded.
*
***************************************************************************/

static int bcm2835_smi_dev_remove(struct platform_device *pdev)
{
	device_destroy(bcm2835_smi_class, bcm2835_smi_devid);
	class_destroy(bcm2835_smi_class);
	cdev_del(&bcm2835_smi_cdev);
	unregister_chrdev_region(bcm2835_smi_devid, 1);

	dev_info(inst->dev, "SMI character dev removed - OK");
	return 0;
}

/****************************************************************************
*
*   Register the driver with device tree
*
***************************************************************************/

static const struct of_device_id bcm2835_smi_dev_of_match[] = {
	{.compatible = "brcm,bcm2835-smi-dev",},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, bcm2835_smi_dev_of_match);

static struct platform_driver bcm2835_smi_dev_driver = {
	.probe = bcm2835_smi_dev_probe,
	.remove = bcm2835_smi_dev_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = bcm2835_smi_dev_of_match,
		   },
};

module_platform_driver(bcm2835_smi_dev_driver);

MODULE_ALIAS("platform:smi-dev-bcm2835");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(
	"Character device driver for BCM2835's secondary memory interface");
MODULE_AUTHOR("Luke Wren <luke@raspberrypi.org>");
