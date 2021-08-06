/*
 * drivers/input/touchscreen/tsc2007.c
 *
 * Copyright (c) 2008 MtekVision Co., Ltd.
 *	Kwangwoo Lee <kwlee@mtekvision.com>
 *
 * Using code from:
 *  - ads7846.c
 *	Copyright (c) 2005 David Brownell
 *	Copyright (c) 2006 Nokia Corporation
 *  - corgi_ts.c
 *	Copyright (C) 2004-2005 Richard Purdie
 *  - omap_ts.[hc], ads7846.h, ts_osk.c
 *	Copyright (C) 2002 MontaVista Software
 *	Copyright (C) 2004 Texas Instruments
 *	Copyright (C) 2005 Dirk Behme
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

 //https://github.com/patrickhwood

#include <linux/module.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
# include <linux/earlysuspend.h>
# include <linux/suspend.h>
#endif

#define TS_POLL_DELAY	(50 * 1000 * 1000)	/* ns delay before the first sample */
#define TS_POLL_PERIOD	(50 * 1000 * 1000)	/* ns delay between samples */
#define MAX_X        799
#define MAX_Y        479
#define MAX_PRESSURE 200
#define MT_SUPPORT

#define FINGERNO 5

#define DEVICE_ID_REG      0x02
#define VERSION_ID_REG     0x03
#define EVENT_STATUS       0x79
#define FINGER00_REG       0x7C
#define DEVICE_CHANEL_REG  0x06

/* detection level register settings */
/* see registers 0x33, 0x34, and 0x35 in the Solomon Systech manual */
static unsigned int min_area = 1;
static unsigned int min_level = 0x80;
static unsigned int min_weight = 1;

static unsigned int disable = 0;

module_param(min_area, uint, S_IRUGO);
module_param(min_level, uint, S_IRUGO);
module_param(min_weight, uint, S_IRUGO);
module_param(disable, uint, S_IRUGO);

// keep most recent instance of SSD touchpanel I2C client device around
static struct i2c_client *ssd_i2c_client;

struct ChipSetting
{
	char No;
	char Reg;
	char Data1;
	char Data2;
};

static const struct ChipSetting ssdcfgTable[] = {
	{2,0x06,0x19,0x0E},
	{2,0x28,0x00,0x12},
	{2,0x07,0x00,0xE1},
	{2,0x08,0x00,0xE2},
	{2,0x09,0x00,0xE3},
	{2,0x0A,0x00,0xE4},
	{2,0x0B,0x00,0xE5},
	{2,0x0C,0x00,0xE6},
	{2,0x0D,0x00,0xE7},
	{2,0x0E,0x00,0xE8},
	{2,0x0F,0x00,0xE9},
	{2,0x10,0x00,0xEA},
	{2,0x11,0x00,0xEB},
	{2,0x12,0x00,0xEC},
	{2,0x13,0x00,0xED},
	{2,0x14,0x00,0xEE},
	{2,0x15,0x00,0xEF},
	{2,0x16,0x00,0xF0},
	{2,0x17,0x00,0xF1},
	{2,0x18,0x00,0xF2},
	{2,0x19,0x00,0xF3},
	{2,0x1A,0x00,0xF4},
	{2,0x1B,0x00,0xF5},
	{2,0x1C,0x00,0xF6},
	{2,0x1D,0x00,0xF7},
	{2,0x1E,0x00,0xF8},
	{2,0x1F,0x00,0xF9},
	{2,0x20,0x00,0xFA},
	{2,0xD7,0x00,0x02},
	{2,0xD8,0x00,0x07},
	{2,0xDB,0x00,0x02},
	{2,0x30,0x08,0x0D},
	{2,0x36,0x00,0x1A},
	{2,0x3A,0x00,0x00},
	{2,0x65,0x00,0x05},
	{2,0x66,0x23,0x28},
	{2,0x67,0x23,0x28},
	{2,0x68,0xFF,0x9C},
	{2,0x69,0xFF,0x9C},
	{2,0x7A,0xFF,0xFF},
	{2,0x7B,0x00,0x03},
	{2,0x25,0x00,0x0C},
	{2,0x04,0x00,0x01},
	{2,0xFF,0x00,0xC8},
};

static const struct ChipSetting Resume[]={
	// { 2,0x04,0x00,0x01},	// exit sleep mode
	{2,0x25,0x00,0x0C},	// reset scan rate
};

static const struct ChipSetting Suspend[] ={
	// { 2,0x05,0x00,0x01},	// enter sleep mode
	{2,0x25,0x00,0x64},	// reduce scan rate to 100 msec
};

struct ssl_ts_priv {
	struct input_dev	*input;
#ifdef SSD_POLL
	struct hrtimer		timer;
#endif
	struct work_struct  ssl_work;
	struct i2c_client	*client;
	spinlock_t			lock;
	int					irq;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
	bool				suspended;
#endif
};

static struct workqueue_struct *ssd2543_wq;
static int preEventStatus;

static int ssd_i2c_transfer(struct i2c_client *client, struct i2c_msg *msgs, int cnt)
{
	int ret, count=3;
	while(count >= 0)
	{
		count-= 1;
		ret = i2c_transfer(client->adapter, msgs, cnt);
		if(ret < 0)
		{
			msleep(50);
			continue;
		}
		break;
	}
	return ret;
}

static int ssd_i2c_read(struct i2c_client *client, uint8_t cmd, uint8_t *data, int length)
{
	int ret;
	struct i2c_msg msgs[] =
	{
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &cmd,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = data,
		}
	};

	ret = ssd_i2c_transfer(client, msgs, 2);
	if(ret < 0)
	{
		dev_err(&client->dev, "%s, i2c read error, ret %d\n", __func__, ret);
	}
	return ret;
}

static int ssd_i2c_write(struct i2c_client *client, uint8_t cmd, uint8_t *data, int length)
{
	int ret;
	int i;
	unsigned char buf[9]={0};
	struct i2c_msg msgs[] =
	{
		{
			.addr = client->addr,
			.flags = 0,
			.len = length+1,
			.buf = buf,
		},
	};

	if (cmd == 0xFF)
	{
		mdelay(data[0]*256 + data[1]);
		return 0;
	}

	buf[0] = cmd;
	for(i = 0; i < length; i++)
	{
		buf[1+i] = data[i];
	}

	ret = ssd_i2c_transfer(client, msgs, 1);
	if(ret < 0)
	{
		dev_err(&client->dev, "%s, i2c write error, ret %d\n", __func__, ret);
	}
	return ret;
}

static int ssd_i2c_read_tp_info(struct ssl_ts_priv *ts)
{
	unsigned char buf[32]={0};
	int __maybe_unused i;

	// read firmware version
	if(ssd_i2c_read(ts->client, DEVICE_ID_REG, buf, 2) < 0)
	{
		return -1;
	}

	dev_info(&ts->client->dev, "%s, chip ID %X%X\n", __func__, buf[0], buf[1]);

	// read firmware version
	if(ssd_i2c_read(ts->client, VERSION_ID_REG, buf, 2) < 0)
	{
		return -1;
	}

	dev_info(&ts->client->dev, "%s, version ID %X:%X\n", __func__, buf[0], buf[1]);

#ifdef VERBOSE_DEBUG
/* dump all register values from ssdcfgTable to verify settings */
	for (i = 0; i < sizeof(ssdcfgTable)/sizeof(ssdcfgTable[0]); i++)
	{
		if (ssdcfgTable[i].Reg <= 4)
			/* write-only registers */
			continue;
		if (ssdcfgTable[i].Reg == 0xFF) {
			/* internal sleep command */
			mdelay(ssdcfgTable[i].Data1*256 + ssdcfgTable[i].Data2);
			continue;
		}

		if (ssd_i2c_read(ts->client,ssdcfgTable[i].Reg, buf, 2) < 0)
			dev_vdbg(&ts->client->dev, "%s, reg %X: read error\n", __func__, ssdcfgTable[i].Reg);
		else
			dev_vdbg(&ts->client->dev, "%s, reg %X %X:%X\n", __func__, ssdcfgTable[i].Reg, buf[0], buf[1]);
	}

	if(ssd_i2c_read(ts->client, DEVICE_CHANEL_REG, buf, 2) < 0)
	{
		return -1;
	}

	dev_vdbg(&ts->client->dev, "%s, Drive:%d Sense:%d\n", __func__, buf[0], buf[1]);
#endif /* VERBOSE_DEBUG */

	return 0;
}

static int ssd_tp_init(struct ssl_ts_priv *ts)
{
	unsigned char buf[4]={0};
	int i;
	const struct ChipSetting detect_table[3] = {
		{2,0x33,0x00,min_area & 0xff},
		{2,0x34,0x00,min_level & 0xff},
		{2,0x35,min_weight >> 8,min_weight & 0xff},
	};

	dev_err(&ts->client->dev, "%s    \n", __func__);

	//init chip config
	// write out detection threshold registers
	for (i = 0; i < sizeof(detect_table)/sizeof(detect_table[0]); i++)
	{
		buf[0] = detect_table[i].Data1;
		buf[1] = detect_table[i].Data2;
		if (ssd_i2c_write(ts->client, detect_table[i].Reg, buf, detect_table[i].No) < 0)
			return -1;
	}
	// write out constant register initializations
	for (i = 0; i < sizeof(ssdcfgTable)/sizeof(ssdcfgTable[0]); i++)
	{
		buf[0] = ssdcfgTable[i].Data1;
		buf[1] = ssdcfgTable[i].Data2;
		if (ssd_i2c_write(ts->client, ssdcfgTable[i].Reg, buf, ssdcfgTable[i].No) < 0)
			return -1;
	}

	msleep(50);

	return 0;
}

static void ssd_ts_work(struct work_struct *work)
{
	struct ssl_ts_priv *ts = container_of(work,struct ssl_ts_priv,ssl_work);

	unsigned char buf[9]={0};
	int send_report = 0;
	int i;
	unsigned short xpos = 0, ypos = 0, width = 0;
	int EventStatus,EventChange;
	int FingerX[FINGERNO];
	int FingerY[FINGERNO];
	int FingerP[FINGERNO];
	int ret;

#ifdef CONFIG_HAS_EARLYSUSPEND
	if (ts->suspended) {
		extern void request_suspend_state(suspend_state_t state);

		request_suspend_state(PM_SUSPEND_ON);
		input_report_key(ts->input, KEY_INFO, 1);
		input_report_key(ts->input, KEY_INFO, 0);
		input_sync(ts->input);
		ts->suspended = false;
	}
#endif

	// read i2c data from device
	ret = ssd_i2c_read(ts->client, EVENT_STATUS, buf, 2);
	if(ret < 0)
	{
		return;
	}

	EventStatus = ((int)buf[0] << 8 | buf[1]) >> 4;

	// dev_dbg(&ts->client->dev, "%s, STATUS%X buf[0]%X buf[1]%X\n", __func__, EventStatus, buf[0], buf[1]);

	for (i = 0; i < FINGERNO; i++)
	{
		if ((EventStatus >> i) & 0x1)
		{
			ret = ssd_i2c_read(ts->client, FINGER00_REG+i, buf, 4);

			if(ret < 0)
			{
				return;
			}

			if(disable)
				continue;

			xpos = ((buf[2] & 0xf0) << 4) | buf[0];
			ypos = ((buf[2] & 0x0f) << 8) | buf[1];
			width = buf[3];
		}
		else
		{
			xpos = ypos = 0xFFF;
			width = 0;
		}
		FingerX[i] = xpos;
		FingerY[i] = ypos;
		FingerP[i] = width;
	}

	for (i = 0; i < FINGERNO; i++)
	{
		xpos = FingerX[i];
		ypos = FingerY[i];
		width = FingerP[i];

		EventChange = ((preEventStatus ^ EventStatus)>> i)&0x0001;

		if(EventChange)							//touch event changed
		{
			preEventStatus ^= (0x0001 << i) ;	//update pre event status
		}
		#ifdef MT_SUPPORT
		if (xpos != 0xFFF)					// touch down , report
		{
			send_report = 1;
			input_report_abs(ts->input, ABS_MT_TRACKING_ID, i);
			input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, 1);
			input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR, width);
			input_report_abs(ts->input, ABS_MT_POSITION_X, xpos);
			input_report_abs(ts->input, ABS_MT_POSITION_Y, ypos);
			input_report_abs(ts->input, ABS_MT_PRESSURE, width);
			input_report_key(ts->input, BTN_TOUCH, 1);
			input_mt_sync(ts->input);
			dev_dbg(&ts->client->dev, "%s, ID:%d X:%d Y:%d Z:%d\n", __func__, i, xpos, ypos,width);
		}
		else if (EventChange)				// touch up
		{
			send_report = 1;
			input_report_abs(ts->input, ABS_MT_TRACKING_ID, i);
			input_report_key(ts->input, BTN_TOUCH, 0);
			input_mt_sync(ts->input);
			dev_dbg(&ts->client->dev, "%s, ID:%d X:%d Y:%d Z:%d\n", __func__, i, xpos, ypos,width);
		}

		#else	// MT_SUPPORT
		if(i==0)				//only report finger 0
		{
			if (xpos != 0xFFF)					// touch down , report
			{
				input_report_abs(ts->input, ABS_X, xpos);
				input_report_abs(ts->input, ABS_Y, ypos);
				input_report_abs(ts->input, ABS_PRESSURE, width);
				input_report_key(ts->input, BTN_TOUCH, 1);
				send_report = 1;
				dev_dbg(&ts->client->dev, "%s, ID:%d X:%d Y:%d Z:%d\n", __func__, i, xpos, ypos,width);

			}
			else if (EventChange)				// touch up/down change
			{
				xpos = 0;
				ypos = 0;
				input_report_key(ts->input, BTN_TOUCH, 0);
				send_report = 1;
				dev_dbg(&ts->client->dev, "%s, ID:%d X:%d Y:%d Z:%d\n", __func__, i, xpos, ypos,width);
			}
		}
		#endif	// MT_SUPPORT
	}

	if(send_report==1)
		input_sync(ts->input);

#ifdef SSD_POLL
	hrtimer_start(&ts->timer, ktime_set(0, TS_POLL_PERIOD), HRTIMER_MODE_REL);
#endif

	return;
}

static irqreturn_t ssd_ts_irq(int irq, void *handle)
{
	struct ssl_ts_priv *ts = handle;

	dev_dbg(&ts->client->dev, "%s\n", __func__);
	queue_work(ssd2543_wq, &ts->ssl_work);

	return IRQ_HANDLED;
}

#ifdef SSD_POLL
static enum hrtimer_restart ssd_ts_timer(struct hrtimer *timer)
{
	struct ssl_ts_priv *ts = container_of(timer, struct ssl_ts_priv, timer);
	// dev_dbg(ts->client->dev, "%s\n",__func__);

	queue_work(ssd2543_wq, &ts->ssl_work);
	return HRTIMER_NORESTART;
}
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND

#define UIB_LCD_LED_EN          IMX_GPIO_NR(7, 12)
#define UIB_LCD_PWR_INH IMX_GPIO_NR(3, 20)
#define UIB_LCD_STBYB IMX_GPIO_NR(3, 25)
#define UIB_LCD_RESET IMX_GPIO_NR(3, 27)

static void ssd2543_ts_late_resume(struct early_suspend *early_s)
{
	unsigned char buf[4]={0};
	int i;
	struct ssl_ts_priv *ts = container_of(early_s, struct ssl_ts_priv, early_suspend);

	dev_info(&ts->client->dev, "%s\n", __func__);

#ifdef CONFIG_MX6DL_UIB_REV_2
	// power up LCD panel
	gpio_set_value(UIB_LCD_PWR_INH, 0);
	gpio_set_value(UIB_LCD_STBYB, 1);
	msleep(135);
	gpio_set_value(UIB_LCD_LED_EN, 1);
#endif

	disable_irq_wake(ts->irq);

	// write Resume commands to touch IIC
	for (i = 0; i < sizeof(Resume)/sizeof(Resume[0]); i++)
	{
		buf[0] = Resume[i].Data1;
		buf[1] = Resume[i].Data2;
		ssd_i2c_write(ts->client, Resume[i].Reg, buf, Resume[i].No);
	}
}
static void ssd2543_ts_early_suspend(struct early_suspend *early_s)
{
	unsigned char buf[4]={0};
	int i;
	struct ssl_ts_priv *ts = container_of(early_s, struct ssl_ts_priv, early_suspend);

	dev_info(&ts->client->dev, "%s\n", __func__);
	ts->suspended = true;

#ifdef CONFIG_MX6DL_UIB_REV_2
	// power off LCD panel
	gpio_set_value(UIB_LCD_PWR_INH, 1);
	gpio_set_value(UIB_LCD_STBYB, 0);
#endif

	// write Suspend commands to touch IIC
	for (i = 0; i < sizeof(Suspend)/sizeof(Suspend[0]); i++)
	{
		buf[0] = Suspend[i].Data1;
		buf[1] = Suspend[i].Data2;
		ssd_i2c_write(ts->client, Suspend[i].Reg, buf, Suspend[i].No);
	}

	// enable system wakeup on the touch panel's IRQ
	enable_irq_wake(ts->irq);
}
#endif /* CONFIG_HAS_EARLYSUSPEND */

/*
 * /sys/bus/i2c/drivers/ssd2543
 *   /min_area     read-write minimum touch area
 *   /min_level    read-write maximum touch sensitivity level
 *   /min_weight   read-write minimum touch weight
 *   /disable      read-write disable touch panel
 */

static ssize_t ssd2543_get_min_area(struct device_driver *dev, char *buf)
{
	return sprintf(buf, "%u\n", min_area);
}

static ssize_t ssd2543_get_min_level(struct device_driver *dev, char *buf)
{
	return sprintf(buf, "%u\n", min_level);
}

static ssize_t ssd2543_get_min_weight(struct device_driver *dev, char *buf)
{
	return sprintf(buf, "%u\n", min_weight);
}

static ssize_t ssd2543_get_disable(struct device_driver *dev, char *buf)
{
	return sprintf(buf, "%u\n", disable);
}

static ssize_t ssd2543_set_min_area(struct device_driver *dev, const char *buf, size_t count)
{
	long area;
	int err;

	err = kstrtoul(buf, 10, &area);
	if (err || area < 1)
		return -EINVAL;
	min_area = (unsigned int) area;

	if (ssd_i2c_client) {
		// perform an update if user changes setting via sysfs
		struct ssl_ts_priv *ts = i2c_get_clientdata(ssd_i2c_client);
		ssd_tp_init(ts);
	}

	return count;
}

static ssize_t ssd2543_set_min_level(struct device_driver *dev, const char *buf, size_t count)
{
	long level;
	int err;

	err = kstrtoul(buf, 10, &level);
	if (err || level < 1)
		return -EINVAL;
	min_level = (unsigned int) level;

	if (ssd_i2c_client) {
		// perform an update if user changes setting via sysfs
		struct ssl_ts_priv *ts = i2c_get_clientdata(ssd_i2c_client);
		ssd_tp_init(ts);
	}

	return count;
}

static ssize_t ssd2543_set_min_weight(struct device_driver *dev, const char *buf, size_t count)
{
	long weight;
	int err;

	err = kstrtoul(buf, 10, &weight);
	if (err || weight < 1)
		return -EINVAL;
	min_weight = (unsigned int) weight;

	if (ssd_i2c_client) {
		// perform an update if user changes setting via sysfs
		struct ssl_ts_priv *ts = i2c_get_clientdata(ssd_i2c_client);
		ssd_tp_init(ts);
	}

	return count;
}

static ssize_t ssd2543_set_disable(struct device_driver *dev, const char *buf, size_t count)
{
	long dis;
	int err;

	err = kstrtoul(buf, 10, &dis);
	if (err || dis < 0)
		return -EINVAL;
	disable = (unsigned int) dis;

	return count;
}

static DRIVER_ATTR(min_area, S_IWUSR | S_IRUGO, ssd2543_get_min_area, ssd2543_set_min_area);
static DRIVER_ATTR(min_level, S_IWUSR | S_IRUGO, ssd2543_get_min_level, ssd2543_set_min_level);
static DRIVER_ATTR(min_weight, S_IWUSR | S_IRUGO, ssd2543_get_min_weight, ssd2543_set_min_weight);
static DRIVER_ATTR(disable, S_IWUSR | S_IRUGO, ssd2543_get_disable, ssd2543_set_disable);

static struct attribute *ssd2543_attrs[] = {
	&driver_attr_min_area.attr,
	&driver_attr_min_level.attr,
	&driver_attr_min_weight.attr,
	&driver_attr_disable.attr,
	NULL
};

static const struct attribute_group ssd2543_sysfs_files = {
	.attrs	= ssd2543_attrs,
};

static const struct attribute_group *ssd2543_sysfs_attr_groups[] = {
    &ssd2543_sysfs_files,
    NULL
};

static int ssd2543_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct ssl_ts_priv *ts;
	struct input_dev *input_dev;
	int err = 0;
	/* reset GPIO NR passed in dev.platform_data */
	unsigned int *SSD_gpios = (unsigned int *) client->dev.platform_data;

	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	dev_err(&client->dev, "%s:\n",__func__);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	ssd_i2c_client = NULL;
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	/* reset the SSD chip */
	if (SSD_gpios != NULL)
	{
	gpio_direction_output(SSD_gpios[0], 1);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	mdelay(5);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	gpio_set_value(SSD_gpios[0], 0);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	mdelay(5);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	gpio_set_value(SSD_gpios[0], 1);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	mdelay(25);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	}
	if (!i2c_check_functionality(client->adapter,
					 I2C_FUNC_SMBUS_READ_WORD_DATA)) {
		dev_err(&client->dev, "%s: i2c_check_functionality failed\n", __func__);
	printk(KERN_ERR "FIXME: < %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
		return -EIO;
	}

	ts = kzalloc(sizeof(struct ssl_ts_priv), GFP_KERNEL);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	input_dev = input_allocate_device();
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	if (!ts || !input_dev) {
		dev_err(&client->dev, "%s: kzalloc failed\n", __func__);
		err = -ENOMEM;
		goto err_free_mem;
	}
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);

	ts->client = client;
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	i2c_set_clientdata(client, ts);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);

	ts->input = input_dev;
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);

#ifdef SSD_POLL
	hrtimer_init(&ts->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	ts->timer.function = ssd_ts_timer ;			//ssd2543_timer;
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
#endif

	spin_lock_init(&ts->lock);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);

	input_dev->name = "SSD2543 Touch Screen";
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	input_dev->id.bustype = BUS_I2C;
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);

	input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	input_dev->keybit[BIT_WORD(KEY_INFO)] |= BIT_MASK(KEY_INFO);
	printk(KERN_ERR "FIXME: > %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);

#ifdef MT_SUPPORT
	input_set_abs_params(input_dev, ABS_MT_POSITION_X,  0, MAX_X, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y,  0, MAX_Y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, MAX_PRESSURE, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 1, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TRACKING_ID, 0, FINGERNO-1, 0, 0);
#else
	input_set_abs_params(input_dev, ABS_X, 0, MAX_X, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, MAX_Y, 0, 0);
	input_set_abs_params(input_dev, ABS_PRESSURE, 0, MAX_PRESSURE, 0, 0);
#endif

	if (ssd_i2c_read_tp_info(ts) < 0) {
		err = -ENODEV;
		goto err_free_mem;
	}
	if (ssd_tp_init(ts) < 0) {
		err = -ENODEV;
		goto err_free_mem;
	}
	if (ssd_i2c_read_tp_info(ts) < 0) {
		err = -ENODEV;
		goto err_free_mem;
	}

	INIT_WORK(&ts->ssl_work, ssd_ts_work);	// Intialize the work queue

#ifdef SSD_POLL
	hrtimer_start(&ts->timer, ktime_set(0, TS_POLL_PERIOD), HRTIMER_MODE_REL);
	disable_irq_nosync(ts->irq);
#else
	ts->irq = client->irq;
	if (ts->irq < 0)
	{
		dev_err(&client->dev, "%s: request irq pin failed\n", __func__);
		err = -ENODEV;
		goto err_free_mem;
	}

	err = request_irq(ts->irq, ssd_ts_irq, IRQF_TRIGGER_FALLING, client->name, ts);
	if (err < 0){
		dev_err(&client->dev, "%s: request IRQ failed\n", __func__);
		goto err_free_mem;
	}

	dev_warn(&client->dev, "registered with irq (%d)\n", ts->irq);
#endif

	err = input_register_device(input_dev);
	if (err) {
		dev_err(&client->dev, "%s: input_register_device failed\n", __func__);
		goto err_free_irq;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	ts->suspended = false;
	ts->early_suspend.suspend = ssd2543_ts_early_suspend;
	ts->early_suspend.resume  = ssd2543_ts_late_resume;
	ts->early_suspend.level   = EARLY_SUSPEND_LEVEL_BLANK_SCREEN-2;
	register_early_suspend(&ts->early_suspend);
#endif

	ssd_i2c_client = client;
	printk(KERN_ERR "FIXME: < %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	return 0;

 err_free_irq:
#ifdef SSD_POLL
	hrtimer_cancel(&ts->timer);
#else
	free_irq(ts->irq, ts);
#endif
 err_free_mem:
	if (ssd2543_wq)
		destroy_workqueue(ssd2543_wq);
	ssd2543_wq = NULL;
	input_free_device(input_dev);
	kfree(ts);
	dev_err(&client->dev, "%s: failed, err = %d\n", __func__, err);
	printk(KERN_ERR "FIXME: < %s %s %d \n", __FILE__, __FUNCTION__, __LINE__);
	return err;
}

static int ssd2543_remove(struct i2c_client *client)
{
	struct ssl_ts_priv	*ts = i2c_get_clientdata(client);

#ifdef SSD_POLL
	hrtimer_cancel(&ts->timer);
#else
	free_irq(ts->irq, ts);
#endif
	if (ssd2543_wq)
		destroy_workqueue(ssd2543_wq);
	ssd2543_wq = NULL;
	input_unregister_device(ts->input);
	kfree(ts);

	return 0;
}

static struct i2c_device_id ssd2543_idtable[] = {
	{ "ssd2543", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, ssd2543_idtable);

static struct i2c_driver ssd2543_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.groups	= ssd2543_sysfs_attr_groups,
		.name	= "ssd2543"
	},
	.id_table	= ssd2543_idtable,
	.probe		= ssd2543_probe,
	.remove		= ssd2543_remove,
};

static int __init ssd2543_init(void)
{
	ssd2543_wq = create_singlethread_workqueue("ssd2543_wq");
	if (!ssd2543_wq){
		return -ENOMEM;
	}
	else{
	}
	return i2c_add_driver(&ssd2543_driver);
}

static void __exit ssd2543_exit(void)
{
	i2c_del_driver(&ssd2543_driver);
	if (ssd2543_wq) destroy_workqueue(ssd2543_wq);
}

module_init(ssd2543_init);
module_exit(ssd2543_exit);

MODULE_AUTHOR("Kwangwoo Lee <kwlee@mtekvision.com>");
MODULE_DESCRIPTION("TouchScreen Driver");
MODULE_LICENSE("GPL");

