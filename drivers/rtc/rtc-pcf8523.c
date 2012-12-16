/*
 * An I2C driver for the NXP PCF8523 RTC
 * Copyright 2011 Promwad Innovation Company
 *
 * Author: Yauhen Kharuzhy <yauhen.kharuzhy@promwad.com>
 *	Promwad Innovation Company, http://promwad.com/
 *
 * based on the pcf8563 driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/i2c.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/module.h>

#define DRV_VERSION		"1.0"

#define PCF8523_REG_CTL1	0x00 /* control and status registers */
#define PCF8523_REG_CTL2	0x01
#define PCF8523_REG_CTL3	0x02

#define PCF8523_REG_SC		0x03 /* datetime */
#define PCF8523_REG_MN		0x04
#define PCF8523_REG_HR		0x05
#define PCF8523_REG_DM		0x06
#define PCF8523_REG_DW		0x07
#define PCF8523_REG_MO		0x08
#define PCF8523_REG_YR		0x09

#define PCF8523_REG_AMN		0x0A /* alarm */
#define PCF8523_REG_AHR		0x0B
#define PCF8523_REG_ADM		0x0C
#define PCF8523_REG_ADW		0x0D

#define PCF8523_REG_CLKO	0x0F /* clock out */
#define PCF8523_REG_TMRAC	0x10 /* timer control */
#define PCF8523_REG_TMRA	0x11 /* timer */
#define PCF8523_REG_TMRBC	0x12 /* timer control */
#define PCF8523_REG_TMRB	0x13 /* timer */

#define PCF8523_CTL3_BLF	(1 << 2)

static struct i2c_driver pcf8523_driver;

struct pcf8523 {
	struct rtc_device *rtc;
};

/*
 * In the routines that deal directly with the pcf8523 hardware, we use
 * rtc_time -- month 0-11, hour 0-23, yr = calendar year-epoch.
 */
static int pcf8523_get_datetime(struct i2c_client *client, struct rtc_time *tm)
{
	/* struct pcf8523 *pcf8523 = i2c_get_clientdata(client); */
	unsigned char buf[14] = { PCF8523_REG_CTL1 };

	struct i2c_msg msgs[] = {
		{ client->addr, 0, 1, buf },	/* setup read ptr */
		{ client->addr, I2C_M_RD, 14, buf },	/* read status + date */
	};

	/* read registers */
	if ((i2c_transfer(client->adapter, msgs, 2)) != 2) {
		dev_err(&client->dev, "%s: read error\n", __func__);
		return -EIO;
	}

	if (buf[PCF8523_REG_CTL3] & PCF8523_CTL3_BLF)
		dev_info(&client->dev,
			"low voltage detected, date/time is not reliable.\n");

	dev_dbg(&client->dev,
		"%s: raw data is ctl1=%02x, ctl2=%02x, ctl3=%02x, "
		"sec=%02x, min=%02x, hr=%02x, "
		"mday=%02x, wday=%02x, mon=%02x, year=%02x\n",
		__func__,
		buf[0], buf[1], buf[2], buf[3],
		buf[4], buf[5], buf[6], buf[7],
		buf[8], buf[9]);


	tm->tm_sec = bcd2bin(buf[PCF8523_REG_SC] & 0x7F);
	tm->tm_min = bcd2bin(buf[PCF8523_REG_MN] & 0x7F);
	tm->tm_hour = bcd2bin(buf[PCF8523_REG_HR] & 0x3F); /* rtc hr 0-23 */
	tm->tm_mday = bcd2bin(buf[PCF8523_REG_DM] & 0x3F);
	tm->tm_wday = buf[PCF8523_REG_DW] & 0x07;
	tm->tm_mon = bcd2bin(buf[PCF8523_REG_MO] & 0x1F) - 1; /* rtc mn 1-12 */
	tm->tm_year = bcd2bin(buf[PCF8523_REG_YR]);
	if (tm->tm_year < 70)
		tm->tm_year += 100;	/* assume we are in 1970...2069 */

	dev_dbg(&client->dev, "%s: tm is secs=%d, mins=%d, hours=%d, "
		"mday=%d, mon=%d, year=%d, wday=%d\n",
		__func__,
		tm->tm_sec, tm->tm_min, tm->tm_hour,
		tm->tm_mday, tm->tm_mon, tm->tm_year, tm->tm_wday);

	/* the clock can give out invalid datetime, but we cannot return
	 * -EINVAL otherwise hwclock will refuse to set the time on bootup.
	 */
	if (rtc_valid_tm(tm) < 0)
		dev_err(&client->dev, "retrieved date/time is not valid.\n");

	return 0;
}

static int pcf8523_set_datetime(struct i2c_client *client, struct rtc_time *tm)
{
	/* struct pcf8523 *pcf8523 = i2c_get_clientdata(client); */
	int i, err;
	unsigned char buf[10];

	dev_dbg(&client->dev, "%s: secs=%d, mins=%d, hours=%d, "
		"mday=%d, mon=%d, year=%d, wday=%d\n",
		__func__,
		tm->tm_sec, tm->tm_min, tm->tm_hour,
		tm->tm_mday, tm->tm_mon, tm->tm_year, tm->tm_wday);

	/* hours, minutes and seconds */
	buf[PCF8523_REG_SC] = bin2bcd(tm->tm_sec);
	buf[PCF8523_REG_MN] = bin2bcd(tm->tm_min);
	buf[PCF8523_REG_HR] = bin2bcd(tm->tm_hour);

	buf[PCF8523_REG_DM] = bin2bcd(tm->tm_mday);

	/* month, 1 - 12 */
	buf[PCF8523_REG_MO] = bin2bcd(tm->tm_mon + 1);

	/* year and century */
	buf[PCF8523_REG_YR] = bin2bcd(tm->tm_year % 100);

	buf[PCF8523_REG_DW] = tm->tm_wday & 0x07;

	/* write register's data */
	for (i = 0; i < 7; i++) {
		unsigned char data[2] = { PCF8523_REG_SC + i,
						buf[PCF8523_REG_SC + i] };

		err = i2c_master_send(client, data, sizeof(data));
		if (err != sizeof(data)) {
			dev_err(&client->dev,
				"%s: err=%d addr=%02x, data=%02x\n",
				__func__, err, data[0], data[1]);
			return -EIO;
		}
	};

	return 0;
}

static int pcf8523_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	return pcf8523_get_datetime(to_i2c_client(dev), tm);
}

static int pcf8523_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	return pcf8523_set_datetime(to_i2c_client(dev), tm);
}

static const struct rtc_class_ops pcf8523_rtc_ops = {
	.read_time	= pcf8523_rtc_read_time,
	.set_time	= pcf8523_rtc_set_time,
};

static int pcf8523_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct pcf8523 *pcf8523;

	int err = 0;

	dev_dbg(&client->dev, "%s\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	pcf8523 = kzalloc(sizeof(struct pcf8523), GFP_KERNEL);
	if (!pcf8523)
		return -ENOMEM;

	dev_info(&client->dev, "chip found, driver version " DRV_VERSION "\n");

	i2c_set_clientdata(client, pcf8523);

	pcf8523->rtc = rtc_device_register(pcf8523_driver.driver.name,
				&client->dev, &pcf8523_rtc_ops, THIS_MODULE);

	if (IS_ERR(pcf8523->rtc)) {
		err = PTR_ERR(pcf8523->rtc);
		goto exit_kfree;
	}

	return 0;

exit_kfree:
	kfree(pcf8523);

	return err;
}

static int pcf8523_remove(struct i2c_client *client)
{
	struct pcf8523 *pcf8523 = i2c_get_clientdata(client);

	if (pcf8523->rtc)
		rtc_device_unregister(pcf8523->rtc);

	kfree(pcf8523);

	return 0;
}

static const struct i2c_device_id pcf8523_id[] = {
	{ "pcf8523", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pcf8523_id);

static struct i2c_driver pcf8523_driver = {
	.driver		= {
		.name	= "rtc-pcf8523",
	},
	.probe		= pcf8523_probe,
	.remove		= pcf8523_remove,
	.id_table	= pcf8523_id,
};

static int __init pcf8523_init(void)
{
	return i2c_add_driver(&pcf8523_driver);
}

static void __exit pcf8523_exit(void)
{
	i2c_del_driver(&pcf8523_driver);
}

MODULE_AUTHOR("Yauhen Kharuzhy <yauhen.kharuzhy@promwad.com>");
MODULE_DESCRIPTION("NXP PCF8523");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

module_init(pcf8523_init);
module_exit(pcf8523_exit);
