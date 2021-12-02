// SPDX-License-Identifier: GPL-2.0
/*
 * TM1637 LED driver
 *
 * Author: Sukjin Kong <kongsukjin@beyless.com>
 * Copyright: (C) 2021 Beyless Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#define DEBUG 1
#define pr_fmt(fmt) "tm1367: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/tm1637_ioctl.h>

#include "tm1637.h"

/* global defines */
#ifdef DEBUG
#define bit_dbg(level, dev, format, args...) \
	do { \
		if (tm1637_debug >= level) \
			dev_dbg(dev, format, ##args); \
	} while (0)
#else
#define bit_dbg(level, dev, format, args...) \
	do {} while (0)
#endif /* DEBUG */

/* global variables */
#ifdef DEBUG
static int tm1637_debug = 1;
module_param(tm1637_debug, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(tm1637_debug,
		"debug level - 0 off; 1 normal; 2 verbose; 3 very verbose");
#endif

/* Commands and Functions */
#define CMD_DATA			0x40
#define CMD_DATA_MODE_WR	0
#define CMD_DATA_MODE_RD	BIT(1)
#define CMD_DATA_MODE_INC	0
#define CMD_DATA_MODE_FIX	BIT(2)
#define CMD_DATA_MODE_NORM	0
#define CMD_DATA_MODE_TEST	BIT(3)

#define CMD_CTRL			0x80
#define CMD_CTRL_PW0		0x00
#define CMD_CTRL_PW1		0x01
#define CMD_CTRL_PW2		0x02
#define CMD_CTRL_PW3		0x03
#define CMD_CTRL_PW4		0x04
#define CMD_CTRL_PW5		0x05
#define CMD_CTRL_PW6		0x06
#define CMD_CTRL_PW7		0x07
#define CMD_CTRL_DISP_OFF	0
#define CMD_CTRL_DISP_ON	BIT(3)

#define CMD_ADDR			0xC0
#define CMD_ADDR_CH0		0x00
#define CMD_ADDR_CH1		0x01
#define CMD_ADDR_CH2		0x02
#define CMD_ADDR_CH3		0x03
#define CMD_ADDR_CH4		0x04
#define CMD_ADDR_CH5		0x05

/* Defines */
#define DRIVER_NAME			"tm1637"

#define MAX_LEDS			6

#define MIN_BRIGHTNESS		0x00
#define MAX_BRIGHTNESS		0x07

/*
 * Data for TM1637 Messages
 */
#define TM1637_BLOCK_MAX 	6	/* As specified in SMBus standard */
union tm1637_data {
	u8 byte;
	u8 block[TM1637_BLOCK_MAX + 2];	/* block[0] is used for length */
	/* and one more for user-space compatibility */
};

/* tm1637_xfer read or write markers */
#define TM1637_READ  		1
#define TM1637_WRITE 		0

/* TM1637 transaction types (size parameter in the above functions) */
#define TM1637_BYTE			0
#define TM1637_BYTE_DATA	1
#define TM1637_BLOCK_DATA	2

enum tm1637_pin {
	/* Order doesn't matter due to reading from DT node gpios! */
	PIN_CTRL_CLK,
	PIN_CTRL_DIO,
	PIN_NUM
};

struct tm1637 {
	struct gpio_desc *pins[PIN_NUM];
	struct device *dev;
	struct mutex lock;
	u8 leds[MAX_LEDS];
	u8 key;
	u8 brightness;
	u8 led;

	/* local setting */
	int ndelay;	/* common clock cycle time in ns */
};

struct tm1637_block_data {
	struct list_head node;
	u8 command;
	u8 len;
	u8 block[TM1637_BLOCK_MAX];
};

struct tm1637_priv {
	struct tm1637 tm;
};

/* Low-level gpio access */
static int tm1637_get_dio(struct tm1637 *tm)
{
	return gpiod_get_value_cansleep(tm->pins[PIN_CTRL_DIO]);
}

static void tm1637_set_dio(struct tm1637 *tm, int dio)
{
	gpiod_set_value_cansleep(tm->pins[PIN_CTRL_DIO], dio);
}

static int __always_unused tm1637_get_clk(struct tm1637 *tm)
{
	return gpiod_get_value_cansleep(tm->pins[PIN_CTRL_CLK]);
}

static void tm1637_set_clk(struct tm1637 *tm, int clk)
{
	gpiod_set_value_cansleep(tm->pins[PIN_CTRL_CLK], clk);
}

/* setting states on the bus with the right timing */
static inline void tm1637_dio_lo(struct tm1637 *tm)
{
	tm1637_set_dio(tm, 0);
	ndelay(tm->ndelay);
}

static inline void tm1637_dio_hi(struct tm1637 *tm)
{
	tm1637_set_dio(tm, 1);
	ndelay(tm->ndelay);
}

static inline void tm1637_clk_lo(struct tm1637 *tm)
{
	tm1637_set_clk(tm, 0);
	ndelay(tm->ndelay);
}

static inline void tm1637_clk_hi(struct tm1637 *tm)
{
	tm1637_set_clk(tm, 1);
	ndelay(tm->ndelay);
}

/* other auxiliary functions */
static void tm1637_start(struct tm1637 *tm)
{
	/* assert: sck, dio are high */
	tm1637_dio_lo(tm);
	tm1637_clk_lo(tm);
}

static void tm1637_stop(struct tm1637 *tm)
{
	/* assert: sck is low */
	tm1637_dio_lo(tm);
	tm1637_clk_hi(tm);
	tm1637_dio_hi(tm);
}

/* send a byte without start cond., look for arbitration,
   check ackn. from slave */
/* returns:
 * 1 if the device acknowledged
 * 0 if the device did not ack
 */
static int tm1637_outb(struct tm1637 *tm, unsigned char c)
{
	int i;
	int sb;
	int ack;

	/* assert: clk is low */
	for (i = 0; i < 8; i++) {
		sb = (c >> i) & 1;
		tm1637_set_dio(tm, sb);
		ndelay(tm->ndelay);
		tm1637_clk_hi(tm);
		ndelay(tm->ndelay);
		/* FIXME do arbitration here:
		 * if (sb && !tm1637_get_dio(tm)) -> ouch! Get out of here.
		 *
		 * Report a unique code, so higher level code can retry
		 * the whole (combined) message and *NOT* issue STOP.
		 */
		tm1637_clk_lo(tm);
	};
	tm1637_dio_hi(tm);
	tm1637_clk_hi(tm);

	/* read ack: DIO should be pulled down by slave, or it may
	 * NAK (usually to report problems with the data we wrote).
	 */
	ack = !tm1637_get_dio(tm);
	bit_dbg(1, tm->dev, "tm1637_outb: 0x%02x %s\n", (int)c,
		ack ? "A" : "NA");
	ndelay(tm->ndelay);

	tm1637_clk_lo(tm);
	return ack;
	/* assert: clk is low (dio undef) */
}

static int tm1637_inb(struct tm1637 *tm)
{
	/* read byte via gpio port, with out start/stop sequence */
	/* acknowledge is sent in tm1637_read. */
	int i;
	unsigned char indata = 0;
	int ack;

	/* assert: clk is low */
	for (i = 0; i < 8; i++) {
		indata *= 2;
		if (tm1637_get_dio(tm))
			indata |= 0x01;
		ndelay(tm->ndelay);
		tm1637_clk_hi(tm);
		ndelay(tm->ndelay);
		tm1637_clk_lo(tm);
	}
	tm1637_dio_hi(tm);
	tm1637_clk_hi(tm);

	/* read ack: DIO should be pulled down by slave, or it may
	 * NAK (usually to report problems with the data we read).
	 */
	ack = !tm1637_get_dio(tm);
	bit_dbg(1, tm->dev, "tm1637_inb: 0x%02x %s\n", (int)indata,
		ack ? "A" : "NA");
	ndelay(tm->ndelay);

	/* assert: clk is low */
	return indata;
}

/**
 * tm1367_xfer - execute TM1367 protocol operations
 * @tm: Handle to GPIO port
 * @read_write: TM1367_READ or TM1367_WRITE
 * @command: Byte interpreted by TM1637, for protocols which use such bytes
 * @protocol: TM1367 protocol operation to execute, such as TM1367_BLOCK_DATA
 * @data: Data to be read or written
 *
 * This executes an TM1367 protocol operation, and returns a negative
 * errno code else zero on success.
 */
static s32 tm1637_xfer(struct tm1637 *tm,
				char read_write, u8 command,
				int protocol, union tm1637_data *data)
{
	s32 ret;
	int i, len;

	tm1637_start(tm);
	tm1637_outb(tm, command);

	switch (protocol) {

	case TM1637_BYTE:
		dev_dbg(tm->dev,
			"tm1637 byte - wrote 0x%02x.\n",
			command);

		ret = 0;
		break;		

	case TM1637_BYTE_DATA:
		if (read_write == TM1637_WRITE) {
			tm1637_outb(tm, data->byte);
			dev_dbg(tm->dev,
				"tm1637 byte data - wrote 0x%02x at 0x%02x.\n",
				data->byte, command);
		} else {
			data->byte = tm1637_inb(tm);
			dev_dbg(tm->dev,
				"tm1637 byte data - read 0x%02x at 0x%02x.\n",
				data->byte, command);
		}

		ret = 0;
		break;

	case TM1637_BLOCK_DATA:
		len = data->block[0];
		if (len == 0 || len > TM1637_BLOCK_MAX) {
			ret = -EINVAL;
			break;
		}
		/* Largest write sets read block length */
		for (i = 0; i < len; i++)
			tm1637_outb(tm, data->block[i + 1]);
		dev_dbg(tm->dev,
			"tm1637 block data - wrote %d bytes at 0x%02x.\n",
			len, command);
		
		ret = 0;
		break;

	default:
		dev_dbg(tm->dev, "Unsupported TM1637 command\n");
		ret = -EOPNOTSUPP;
		break;
	} /* switch (size) */

	tm1637_stop(tm);

	return ret;
}
EXPORT_SYMBOL(tm1637_xfer);

/**
 * tm1637_write_byte - TM1637 "send byte" protocol
 * @tm: Handle to GPIO port
 * @value: Byte to be sent
 *
 * This executes the SMBus "send byte" protocol, returning negative errno
 * else zero on success.
 */
s32 tm1637_write_byte(const struct tm1637 *tm, u8 value)
{
	return tm1637_xfer(tm, TM1637_WRITE, value, TM1637_BYTE, NULL);
}
EXPORT_SYMBOL(tm1637_write_byte);

/**
 * tm1637_read_byte_data - TM1637 "receive byte" protocol
 * @tm: Handle to GPIO port
 * @command: Byte interpreted by TM1637
 *
 * THis executes the TM1637 "receive byte" protocol, returning negative errno
 * else the byte received from the device.
 */
s32 tm1637_read_byte_data(const struct tm1637 *tm, u8 command)
{
	union tm1637_data data;
	int status;

	status = tm1637_xfer(tm, TM1637_READ, command, TM1637_BYTE_DATA, &data);

	return (status < 0) ? status : data.byte;
}
EXPORT_SYMBOL(tm1637_read_byte_data);

/**
 * tm1637_write_byte_data - TM1637 "send byte" protocol
 * @tm: Handle to GPIO port
 * @command: Byte interpreted by TM1637
 * @value: Byte to be sent
 *
 * This executes the TM1637 "send byte" protocol, returning negative errno
 * else zero on success.
 */
s32 tm1637_write_byte_data(const struct tm1637 *tm, u8 command, u8 value)
{
	union tm1637_data data;
	data.byte = value;

	return tm1637_xfer(tm, TM1637_WRITE, command, TM1637_BYTE_DATA, &data);
}
EXPORT_SYMBOL(tm1637_write_byte_data);

/**
 * tm1637_write_block_data - TM1637 "block write" protocol
 * @tm: Handle to gpio port
 * @command: Byte interpreted by TM1637
 * @length: Size of data block; TM1637 allows at 6 bytes
 * @values: Byte array which will be written.
 *
 * This executes the TM1637 "block write" protocol, returning negative errno
 * else zero on success.
 */
s32 tm1637_write_block_data(const struct tm1637 *tm, u8 command,
				u8 length, const u8 *values)
{
	union tm1637_data data;

	if (length > TM1637_BLOCK_MAX)
		length = TM1637_BLOCK_MAX;
	data.block[0] = length;
	memcpy(&data.block[1], values, length);

	return tm1637_xfer(tm, TM1637_WRITE, command, TM1637_BLOCK_DATA, &data);
}
EXPORT_SYMBOL(tm1637_write_block_data);

static int tm1637_initialize(struct tm1637_priv *priv)
{
	uint8_t byte;
	int err;
	uint8_t data[MAX_LEDS];

	/* Clear RAM (8 * 6 bits) */
	byte = CMD_DATA | CMD_DATA_MODE_INC;
	err = tm1637_write_byte(&priv->tm, byte);
	if (err)
		return err;

	memset(data, 0, sizeof(data));
	byte = CMD_ADDR | CMD_ADDR_CH0;
	err = tm1637_write_block_data(&priv->tm, byte, sizeof(data), data);
	
	/* Turn on display and Configure pulse width */
	byte = CMD_CTRL | CMD_CTRL_DISP_ON | CMD_CTRL_PW7;
	err = tm1637_write_byte(&priv->tm, byte);
	if (err)
		return err;

	priv->tm.brightness = 7;
	priv->tm.led = 1;

	return 0;
}

static ssize_t tm1637_show_led(struct device *dev,
				struct device_attribute *attr,
				char *buf, int nr)
{
	struct tm1637_priv *priv = dev_get_drvdata(dev);
	struct tm1637 *tm = &priv->tm;

	return scnprintf(buf, PAGE_SIZE, "%d\n", tm->leds[nr]);
}
show_led(0)
show_led(1)
show_led(2)
show_led(3)
show_led(4)
show_led(5)
		
static ssize_t tm1637_store_led(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len, int nr)
{
    struct tm1637_priv *priv = dev_get_drvdata(dev);
	struct tm1637 *tm = &priv->tm;
	uint8_t byte;
	unsigned long data;
	ssize_t ret;

	mutex_lock(&tm->lock);

	if (kstrtoul(buf, 0, &data))
		ret = -EINVAL;
		goto unlock;

	byte = CMD_ADDR | nr;
	ret = tm1637_write_byte_data(tm, byte, (u8)data);
	if (ret < 0)
		goto unlock;

	tm->leds[nr] = data;
	ret = len;
unlock:
	mutex_unlock(&tm->lock);
	return ret;
}
store_led(0)
store_led(1)
store_led(2)
store_led(3)
store_led(4)
store_led(5)

static ssize_t tm1637_show_key(struct device *dev,
				struct device_attribute *attr, char *buf)
{
    struct tm1637_priv *priv = dev_get_drvdata(dev);
	struct tm1637 *tm = &priv->tm;
	uint8_t byte;
	unsigned long data;

	mutex_lock(&tm->lock);

	byte = CMD_DATA | CMD_DATA_MODE_RD;
	data = tm1637_read_byte_data(tm, byte);
	if (data < 0)
		goto unlock;

	tm->key = data;
unlock:
	mutex_unlock(&tm->lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", tm->key);
}

static ssize_t tm1637_show_brightness(struct device *dev,
				struct device_attribute *attr, char *buf)
{
    struct tm1637_priv *priv = dev_get_drvdata(dev);
	struct tm1637 *tm = &priv->tm;

	return scnprintf(buf, PAGE_SIZE, "%d\n", tm->brightness);
}

static ssize_t tm1637_store_brightness(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
    struct tm1637_priv *priv = dev_get_drvdata(dev);
	struct tm1637 *tm = &priv->tm;
	uint8_t byte;
	unsigned long data;
	ssize_t ret;

	mutex_lock(&tm->lock);

	if (kstrtoul(buf, 0, &data))
		ret = -EINVAL;
		goto unlock;

	byte = CMD_CTRL | (tm->led ? CMD_CTRL_DISP_ON : CMD_CTRL_DISP_OFF) | data;
	ret = tm1637_write_byte(tm, byte);
	if (ret < 0)
		goto unlock;

	tm->brightness = data;
	ret = len;
unlock:
	mutex_unlock(&tm->lock);
	return ret;
}

static ssize_t tm1637_show_leds(struct device *dev,
				struct device_attribute *attr, char *buf)
{
    struct tm1637_priv *priv = dev_get_drvdata(dev);
	struct tm1637 *tm = &priv->tm;

	return scnprintf(buf, PAGE_SIZE, "%s\n", tm->led ? "on" : "off");
}

static ssize_t tm1637_store_leds(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
    struct tm1637_priv *priv = dev_get_drvdata(dev);
	struct tm1637 *tm = &priv->tm;
	uint8_t byte;
	unsigned long data;
	ssize_t ret;

	mutex_lock(&tm->lock);

	if (!strncmp(buf, "on", 2))
		data = CMD_CTRL_DISP_ON;
	else if (!strncmp(buf, "off", 3))
		data = CMD_CTRL_DISP_OFF;
	else
		ret = -EINVAL;
		goto unlock;

	byte = CMD_CTRL | data | tm->brightness;
	ret = tm1637_write_byte(tm, byte);
	if (ret < 0)
		goto unlock;

	tm->led = data ? 1 : 0;
	ret = len;
unlock:
	mutex_unlock(&tm->lock);
	return ret;
}

static TM1637_DEV_ATTR_RW(led0, tm1637_show_led0, tm1637_store_led0);
static TM1637_DEV_ATTR_RW(led1, tm1637_show_led1, tm1637_store_led1);
static TM1637_DEV_ATTR_RW(led2, tm1637_show_led2, tm1637_store_led2);
static TM1637_DEV_ATTR_RW(led3, tm1637_show_led3, tm1637_store_led3);
static TM1637_DEV_ATTR_RW(led4, tm1637_show_led4, tm1637_store_led4);
static TM1637_DEV_ATTR_RW(led5, tm1637_show_led5, tm1637_store_led5);
static TM1637_DEV_ATTR_RO(key, tm1637_show_key);
static TM1637_DEV_ATTR_RW(brightness, tm1637_show_brightness, tm1637_store_brightness);
static TM1637_DEV_ATTR_RW(leds, tm1637_show_leds, tm1637_store_leds);

static struct attribute *tm1637_attrs[] = {
	&dev_attr_led0.attr,
	&dev_attr_led1.attr,
	&dev_attr_led2.attr,
	&dev_attr_led3.attr,
	&dev_attr_led4.attr,
	&dev_attr_led5.attr,
	&dev_attr_key.attr,
	&dev_attr_brightness.attr,
	&dev_attr_leds.attr,
	NULL,
};

static const struct attribute_group tm1637_group = {
	.attrs = tm1637_attrs,
};

static int tm1637_open(struct inode *inode, struct file *filp)
{
	return nonseekable_open(inode, filp);
}

static ssize_t tm1637_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
	struct miscdevice *mdev = filp->private_data;
	struct tm1637_priv *priv = dev_get_drvdata(mdev->this_device);
	struct tm1637 *tm = &priv->tm;
	uint8_t byte;
	unsigned char data;
	int ret;

	mutex_lock(&tm->lock);

	byte = CMD_DATA | CMD_DATA_MODE_RD;
	data = tm1637_read_byte_data(tm, byte);
	if (data < 0)
		goto unlock;

	ret = copy_to_user(buf, &data, len);
	if (ret)
		ret = -EFAULT;
		goto unlock;

	tm->key = data;
	ret = len;
unlock:
	mutex_unlock(&tm->lock);
	return ret;
}

static ssize_t tm1637_write(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
	struct miscdevice *mdev = filp->private_data;
	struct tm1637_priv *priv = dev_get_drvdata(mdev->this_device);
	struct tm1637 *tm = &priv->tm;
	uint8_t byte;
	unsigned char data[MAX_LEDS];
	int ret;
	int i;

	mutex_lock(&tm->lock);

	ret = copy_from_user(data, buf, len);
	if (ret)
		ret = -EFAULT;
		goto unlock;

	for (i = 0; i < len; i++) {
		byte = CMD_ADDR | i;
		ret = tm1637_write_byte_data(tm, byte, (u8)data[i]);
		if (ret < 0)
			goto unlock;

		tm->leds[i] = data[i];
	}
	ret = len;
unlock:
	mutex_unlock(&tm->lock);
	return ret;
}

static int ioctl_set_led(struct tm1637 *tm, unsigned int cmd, struct tm1637_ioctl_led_args __user *uargs)
{
	int ret;
	struct tm1637_ioctl_led_args args = {0};
	uint8_t byte;
   	int i;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	ret = copy_from_user(&args, uargs, sizeof(args));
	if (ret)
		return -EFAULT;

	if (cmd == TM1637_IOC_SET_LEDS) {
		for (i = 0; i < MAX_LEDS; i++) {
			byte = CMD_ADDR | i;
			ret = tm1637_write_byte_data(tm, byte, (u8)args.leds[i]);
			if (ret < 0)
				return ret;

			tm->leds[i] = args.leds[i];
		}
	}
	else {
		switch(cmd) {
		case TM1637_IOC_SET_LED0:
			i = CMD_ADDR_CH0;
			break;
		case TM1637_IOC_SET_LED1:
			i = CMD_ADDR_CH1;
			break;
		case TM1637_IOC_SET_LED2:
			i = CMD_ADDR_CH2;
			break;
		case TM1637_IOC_SET_LED3:
			i = CMD_ADDR_CH3;
			break;
		case TM1637_IOC_SET_LED4:
			i = CMD_ADDR_CH4;
			break;
		case TM1637_IOC_SET_LED5:
			i = CMD_ADDR_CH5;
			break;
		default:
			return -EINVAL;
		}

		byte = CMD_ADDR | i;
		ret = tm1637_write_byte_data(tm, byte, (u8)args.leds[i]);
		if (ret < 0)
			return ret;

		tm->leds[i] = args.leds[i];
	}

	return 0;
}

static int ioctl_get_led(struct tm1637 *tm, unsigned int cmd, struct tm1637_ioctl_led_args __user *uargs)
{
	int ret;
	struct tm1637_ioctl_led_args args = {0};
   	int i;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (cmd == TM1637_IOC_GET_LEDS) {
		for (i = 0; i < MAX_LEDS; i++) {
			args.leds[i] = tm->leds[i];
		}
	}
	else {
		switch(cmd) {
		case TM1637_IOC_GET_LED0:
			i = CMD_ADDR_CH0;
			break;
		case TM1637_IOC_GET_LED1:
			i = CMD_ADDR_CH1;
			break;
		case TM1637_IOC_GET_LED2:
			i = CMD_ADDR_CH2;
			break;
		case TM1637_IOC_GET_LED3:
			i = CMD_ADDR_CH3;
			break;
		case TM1637_IOC_GET_LED4:
			i = CMD_ADDR_CH4;
			break;
		case TM1637_IOC_GET_LED5:
			i = CMD_ADDR_CH5;
			break;
		default:
			return -EINVAL;
		}
		
		args.leds[i] = tm->leds[i];
	}

	ret = copy_to_user(uargs, &args, sizeof(args));
	if (ret)
		return -EFAULT;

	return 0;
}

static int ioctl_get_key(struct tm1637 *tm, unsigned int cmd, struct tm1637_ioctl_key_args __user *uargs)
{
	int ret;
	struct tm1637_ioctl_key_args args = {0};
	uint8_t byte;
	unsigned char data;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	byte = CMD_DATA | CMD_DATA_MODE_RD;
	data = tm1637_read_byte_data(tm, byte);
	if (data < 0)
		return ret;

	tm->key = data;
	args.key = tm->key;

	ret = copy_to_user(uargs, &args, sizeof(args));
	if (ret)
		ret = -EFAULT;

	return 0;
}

static int ioctl_set_ctl(struct tm1637 *tm, unsigned int cmd, struct tm1637_ioctl_ctl_args __user *uargs)
{
	int ret;
	struct tm1637_ioctl_ctl_args args = {0};
	uint8_t byte;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	ret = copy_from_user(&args, uargs, sizeof(args));
	if (ret)
		return -EFAULT;

	switch(cmd) {
	case TM1637_IOC_SET_BRIGHTNESS:
		byte = CMD_CTRL | (tm->led ? CMD_CTRL_DISP_ON : CMD_CTRL_DISP_OFF) | args.brightness;
		args.led = tm->led;
		break;
	case TM1637_IOC_SET_LED:
		byte = CMD_CTRL | args.led | tm->brightness;
		args.brightness = tm->brightness;
		break;
	default:
		return -EINVAL;
	}

	ret = tm1637_write_byte(tm, byte);
	if (ret < 0)
		return ret;

	tm->brightness = args.brightness;
	tm->led = args.led;

	return 0;
}

static int ioctl_get_ctl(struct tm1637 *tm, unsigned int cmd, struct tm1637_ioctl_ctl_args __user *uargs)
{
	int ret;
	struct tm1637_ioctl_ctl_args args = {0};

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	switch(cmd) {
	case TM1637_IOC_GET_BRIGHTNESS:
		args.brightness = tm->brightness;
		break;
	case TM1637_IOC_GET_LED:
		args.led = tm->led;
		break;
	default:
		return -EINVAL;
	}

	ret = copy_to_user(uargs, &args, sizeof(args));
	if (ret)
		return -EFAULT;

	return 0;
}

static long tm1637_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct miscdevice *mdev = filp->private_data;
    struct tm1637_priv *priv = dev_get_drvdata(mdev->this_device);
    struct tm1637 *tm = &priv->tm;
	int ret;
	void __user *argp = (void __user *)arg;

	switch(cmd) {
	case TM1637_IOC_SET_LED0:
	case TM1637_IOC_SET_LED1:
	case TM1637_IOC_SET_LED2:
	case TM1637_IOC_SET_LED3:
	case TM1637_IOC_SET_LED4:
	case TM1637_IOC_SET_LED5:
	case TM1637_IOC_SET_LEDS:
		ret = ioctl_set_led(tm, cmd, argp);
		break;
	case TM1637_IOC_GET_LED0:
	case TM1637_IOC_GET_LED1:
	case TM1637_IOC_GET_LED2:
	case TM1637_IOC_GET_LED3:
	case TM1637_IOC_GET_LED4:
	case TM1637_IOC_GET_LED5:
	case TM1637_IOC_GET_LEDS:
		ret = ioctl_get_led(tm, cmd, argp);
		break;
	case TM1637_IOC_GET_KEY:
		ret = ioctl_get_key(tm, cmd, argp);
		break;
	case TM1637_IOC_SET_BRIGHTNESS:
	case TM1637_IOC_SET_LED:
		ret = ioctl_set_ctl(tm, cmd, argp);
		break;
	case TM1637_IOC_GET_BRIGHTNESS:
	case TM1637_IOC_GET_LED:
		ret = ioctl_get_ctl(tm, cmd, argp);
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static long tm1637_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct miscdevice *mdev = filp->private_data;
	struct tm1637_priv *priv = dev_get_drvdata(mdev->this_device);
	struct tm1637 *tm = &priv->tm;
	int ret;

	mutex_lock(&tm->lock);
	ret = tm1637_ioctl(filp, cmd, arg);
	mutex_unlock(&tm->lock);

	return ret;
}

static struct file_operations tm1637_fops = {
	.owner			= THIS_MODULE,
	.open			= tm1637_open,
	.read			= tm1637_read,
	.write			= tm1637_write,
	.unlocked_ioctl	= tm1637_unlocked_ioctl,
	.llseek			= no_llseek,
};

static struct miscdevice tm1637_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DRIVER_NAME,
	.fops = &tm1637_fops,
};

static int tm1637_probe(struct platform_device *pdev)
{
	struct tm1637_priv *priv = platform_get_drvdata(pdev);
	struct tm1637 *tm;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	tm = &priv->tm;
	tm->dev = &pdev->dev;
	tm1637_misc.this_device = &pdev->dev;

	tm->pins[PIN_CTRL_CLK] = devm_gpiod_get(tm->dev, "clk", GPIOD_OUT_HIGH);
	if (IS_ERR(tm->pins[PIN_CTRL_CLK])) {
		ret = PTR_ERR(tm->pins[PIN_CTRL_CLK]);
		goto fail;
	}

	tm->pins[PIN_CTRL_DIO] = devm_gpiod_get(tm->dev, "dio", GPIOD_OUT_HIGH);
	if (IS_ERR(tm->pins[PIN_CTRL_DIO])) {
		ret = PTR_ERR(tm->pins[PIN_CTRL_DIO]);
		goto fail;
	}

	/* Required properties */
	ret = device_property_read_u32(tm->dev, "delay-ns", &tm->ndelay);
	if (ret)
		goto fail;

	ret = sysfs_create_group(&tm->dev->kobj, &tm1637_group);
	if (ret) {
		goto fail;
	}

	ret = misc_register(&tm1637_misc);
	if (!ret) {
		goto fail;
	}

	mutex_init(&tm->lock);

	ret = tm1637_initialize(priv);
	if (ret)
		goto fail;

	platform_set_drvdata(pdev, priv);

	return 0;

fail:
	kfree(priv);
	return ret;
}

static int tm1637_remove(struct platform_device *pdev)
{
	misc_deregister(&tm1637_misc);

	return 0;
}

static const struct of_device_id tm1637_of_match[] = {
    { .compatible = "tm,tm1637" },
	{ }
};
MODULE_DEVICE_TABLE(of, tm1637_of_match);

static struct platform_driver tm1637_driver = {
	.probe = tm1637_probe,
	.remove = tm1637_remove,
	.driver = {
		.name =	"tm1637",
		.of_match_table = tm1637_of_match,
	},
};
module_platform_driver(tm1637_driver);

MODULE_AUTHOR("Sukjin Kong <kongsukjin@beyless.com>");
MODULE_DESCRIPTION("TM1637 LED driver");
MODULE_LICENSE("GPL");
