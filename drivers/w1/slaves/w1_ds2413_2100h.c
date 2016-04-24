/*
 * w1_ds2413_2100h.c - w1 family 85 (clone DS2413: 3A 2100H ) driver
 * based on w1_ds2413.c by Jean-Francois Dagenais <dagenaisj@sonatest.com>
 *
 * Copyright (c) 2016 Przemyslaw W (saper_2) <sapers.2@gmail.net>
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2. See the file COPYING for more details.
 */

 /*
 * Tested on Raspberry Pi 2 from shell, must be called by root (sudo won't work ("permission denied")).
 * Read IO - for bit values refer to datasheet: DS2313, function: PIO ACCESS READ [F5h]: 
 * root@piv2:/sys/bus/w1/devices/85-xxxxxxxxxxxx# xxd -b state
 * 0000000: 00001111                                               .
 * Write IO - for bit values refer to datasheet: DS2313, function: PIO ACCESS WRITE [5Ah]:
 * root@piv2:/sys/bus/w1/devices/85-xxxxxxxxxxxx# echo -e "\x01"|dd of=/sys/bus/w1/devices/85-1003c073b2be/output bs=1 count=1
 * root@piv2:/sys/bus/w1/devices/85-xxxxxxxxxxxx# echo -e "\x00"|dd of=/sys/bus/w1/devices/85-1003c073b2be/output bs=1 count=1
 * root@piv2:/sys/bus/w1/devices/85-xxxxxxxxxxxx# echo -e "\x02"|dd of=/sys/bus/w1/devices/85-1003c073b2be/output bs=1 count=1
 */
 
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "../w1.h"
#include "../w1_int.h"
#include "../w1_family.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Przemyslaw W (saper_2) <sapers.2@gmail.com>");
MODULE_DESCRIPTION("w1 family 85 driver for clone DS2413 (2100H) 2 Pin IO");
MODULE_ALIAS("w1-family-" __stringify(W1_FAMILY_2100H));

#define W1_F85_RETRIES                     3
#define W1_F85_FUNC_PIO_ACCESS_READ        0xF5
#define W1_F85_FUNC_PIO_ACCESS_WRITE       0x5A
#define W1_F85_SUCCESS_CONFIRM_BYTE        0xAA

static ssize_t state_read(struct file *filp, struct kobject *kobj,
			  struct bin_attribute *bin_attr, char *buf, loff_t off,
			  size_t count)
{
	struct w1_slave *sl = kobj_to_w1_slave(kobj);
	dev_dbg(&sl->dev,
		"Reading %s kobj: %p, off: %0#10x, count: %zu, buff addr: %p",
		bin_attr->attr.name, kobj, (unsigned int)off, count, buf);

	if (off != 0)
		return 0;
	if (!buf)
		return -EINVAL;

	mutex_lock(&sl->master->bus_mutex);
	dev_dbg(&sl->dev, "mutex locked");

	if (w1_reset_select_slave(sl)) {
		mutex_unlock(&sl->master->bus_mutex);
		return -EIO;
	}

	w1_write_8(sl->master, W1_F85_FUNC_PIO_ACCESS_READ);
	*buf = w1_read_8(sl->master);

	mutex_unlock(&sl->master->bus_mutex);
	dev_dbg(&sl->dev, "mutex unlocked");

	/* check for correct complement */
	if ((*buf & 0x0F) != ((~*buf >> 4) & 0x0F))
		return -EIO;
	else
		return 1;
}

static BIN_ATTR_RO(state, 1);

static ssize_t output_write(struct file *filp, struct kobject *kobj,
			    struct bin_attribute *bin_attr, char *buf,
			    loff_t off, size_t count)
{
	struct w1_slave *sl = kobj_to_w1_slave(kobj);
	u8 w1_buf[3];
	unsigned int retries = W1_F85_RETRIES;

	if (count != 1 || off != 0)
		return -EFAULT;

	dev_dbg(&sl->dev, "locking mutex for write_output");
	mutex_lock(&sl->master->bus_mutex);
	dev_dbg(&sl->dev, "mutex locked");

	if (w1_reset_select_slave(sl))
		goto error;

	/* according to the DS2413 datasheet the most significant 6 bits
	   should be set to "1"s, so do it now */
	*buf = *buf | 0xFC;

	while (retries--) {
		w1_buf[0] = W1_F85_FUNC_PIO_ACCESS_WRITE;
		w1_buf[1] = *buf;
		w1_buf[2] = ~(*buf);
		w1_write_block(sl->master, w1_buf, 3);

		if (w1_read_8(sl->master) == W1_F85_SUCCESS_CONFIRM_BYTE) {
			mutex_unlock(&sl->master->bus_mutex);
			dev_dbg(&sl->dev, "mutex unlocked, retries:%d", retries);
			return 1;
		}
		if (w1_reset_resume_command(sl->master))
			goto error;
	}

error:
	mutex_unlock(&sl->master->bus_mutex);
	dev_dbg(&sl->dev, "mutex unlocked in error, retries:%d", retries);
	return -EIO;
}

static BIN_ATTR(output, S_IRUGO | S_IWUSR | S_IWGRP, NULL, output_write, 1);

static struct bin_attribute *w1_f85_bin_attrs[] = {
	&bin_attr_state,
	&bin_attr_output,
	NULL,
};

static const struct attribute_group w1_f85_group = {
	.bin_attrs = w1_f85_bin_attrs,
};

static const struct attribute_group *w1_f85_groups[] = {
	&w1_f85_group,
	NULL,
};

static struct w1_family_ops w1_f85_fops = {
	.groups		= w1_f85_groups,
};

static struct w1_family w1_family_85 = {
	.fid = W1_FAMILY_2100H,
	.fops = &w1_f85_fops,
};

static int __init w1_f85_init(void)
{
	return w1_register_family(&w1_family_85);
}

static void __exit w1_f85_exit(void)
{
	w1_unregister_family(&w1_family_85);
}

module_init(w1_f85_init);
module_exit(w1_f85_exit);
