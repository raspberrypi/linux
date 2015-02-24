/* drivers/input/touchscreen/ft6x06_ts.c
 *
 * FocalTech ft6x06 TouchScreen driver.
 *
 * Copyright (c) 2010  Focal tech Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <mach/irqs.h>
#include <linux/kernel.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>

#include <linux/input/ft6x06_ts.h>

struct ts_event {
	u16 x;
	u16 y;
	u8 event; /* 0 -- down; 1-- contact; 2 -- contact */
	u8 id;
	u16 pressure;
};

/* ts_event values */
#define FTS_POINT_UP		0x01
#define FTS_POINT_DOWN		0x00
#define FTS_POINT_CONTACT	0x02


struct ft6x06_ts_data {
	unsigned int irq;
	unsigned int x_max;
	unsigned int y_max;
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct ts_event events[CFG_MAX_TOUCH_POINTS];
	int touch_points;
	struct ft6x06_platform_data *pdata;
};

/*
 * This struct is a touchpoint as stored in hardware.  Note that the id,
 * as well as the event, are stored in the upper nybble of the hi byte.
 */
struct ft6x06_touchpoint {
	union {
		u8 xhi;
		u8 event;
	};
	u8 xlo;
	union {
		u8 yhi;
		u8 id;
	};
	u8 ylo;
	u8 unk0;
	u8 unk1;
} __attribute__((__packed__));

/* This packet represents the register map as read from offset 0 */
struct ft6x06_packet {
	u8 unk0;
	u8 unk1;
	u8 touches;
	struct ft6x06_touchpoint points[CFG_MAX_TOUCH_POINTS];
} __attribute__((__packed__));


static int ft6x06_read(struct i2c_client *client, u8 reg, u8 len, void *data)
{
	return i2c_smbus_read_i2c_block_data(client, reg, len, data);
}

static int ft6x06_read_touchdata(struct ft6x06_ts_data *data)
{
	struct ts_event *event = data->events;
	struct ft6x06_packet buf;
	int ret;
	int i;

	ret = ft6x06_read(data->client, 0, sizeof(buf), &buf);
	if (ret < 0) {
		dev_err(&data->client->dev, "%s read touchdata failed.\n",
			__func__);
		return ret;
	}

	dev_dbg(&data->input_dev->dev, "detected %d touch events\n",
			buf.touches);

	data->touch_points = buf.touches;
	if (data->touch_points > CFG_MAX_TOUCH_POINTS) {
		dev_err(&data->input_dev->dev,
			"touchscreen reports %d points, %d are supported\n",
			data->touch_points, CFG_MAX_TOUCH_POINTS);
		data->touch_points = CFG_MAX_TOUCH_POINTS;
	}

	for (i = 0; i < CFG_MAX_TOUCH_POINTS; i++) {
		event[i].x  = ((buf.points[i].xhi & 0xf) << 8);
		event[i].x |= buf.points[i].xlo;
		event[i].y  = ((buf.points[i].yhi & 0xf) << 8);
		event[i].y |= buf.points[i].ylo;
		event[i].event = buf.points[i].event >> 6;
		event[i].id = buf.points[i].id >> 4;
		event[i].pressure = FT_PRESS;
	}

	return 0;
}

static void ft6x06_report_values(struct ft6x06_ts_data *data)
{
	struct ts_event *event = data->events;
	int i = 0;

	for (i = 0; i < data->touch_points; i++) {
		input_report_abs(data->input_dev, ABS_MT_POSITION_X,
				 event[i].x);
		input_report_abs(data->input_dev, ABS_MT_POSITION_Y,
				 event[i].y);
		input_report_abs(data->input_dev, ABS_MT_PRESSURE,
				 event[i].pressure);
		input_report_abs(data->input_dev, ABS_MT_TRACKING_ID,
				 event[i].id);
		if (event[i].event == FTS_POINT_DOWN
		 || event[i].event == FTS_POINT_CONTACT)
			input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR,
					 event[i].pressure);
		else
			input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR,
					 0);
		input_mt_sync(data->input_dev);
	}

	/* Legacy touchscreen emulation */
	if (data->touch_points > 0) {
		input_report_abs(data->input_dev, ABS_X, event[0].x);
		input_report_abs(data->input_dev, ABS_Y, event[0].y);
		input_report_abs(data->input_dev, ABS_PRESSURE, event[0].pressure);
		input_report_key(data->input_dev, BTN_TOUCH, 1);
		input_sync(data->input_dev);
	}

	else if (data->touch_points == 0) {
		input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR, 0);
		input_report_abs(data->input_dev, ABS_PRESSURE, 0);
		input_report_key(data->input_dev, BTN_TOUCH, 0);
		input_sync(data->input_dev);
	}
}

static irqreturn_t ft6x06_ts_interrupt(int irq, void *dev_id)
{
	struct ft6x06_ts_data *ft6x06_ts = dev_id;
	int ret = 0;

	ret = ft6x06_read_touchdata(ft6x06_ts);
	if (ret == 0)
		ft6x06_report_values(ft6x06_ts);

	return IRQ_HANDLED;
}



#ifdef CONFIG_OF

static int request_one_gpio(struct device *dev,
	const char *name, int index, int *gpiop)
{
	struct device_node *node = dev->of_node;
	int gpio, flags, ret = 0;
	enum of_gpio_flags of_flags;
	if (of_find_property(node, name, NULL)) {
	  gpio = of_get_named_gpio_flags(node, name, index, &of_flags);

	  if (gpio == -ENOENT)
	    return 0;
	  if (gpio == -EPROBE_DEFER)
	    return gpio;
	  if (gpio < 0) {
	    dev_err(dev, "failed to get '%s' from DT\n", name);
	    return gpio;
	  }
	  /* active low translates to initially low */
	  flags = (of_flags & OF_GPIO_ACTIVE_LOW) ? GPIOF_OUT_INIT_LOW : GPIOF_OUT_INIT_HIGH;
	  ret = devm_gpio_request_one(dev, gpio, flags,
	  dev->driver->name);
	  if (ret) {
	    dev_err(dev, "gpio_request_one('%s'=%d) failed with %d\n", name, gpio, ret);
	    return ret;
	  }
	  if (gpiop)
		*gpiop = gpio;
	}
	return ret;
}


static int ft6x06_parse_dt(struct device *dev,
			struct ft6x06_platform_data *pdata)
{
	struct device_node *np = dev->of_node;

/*
	pdata->reset_gpio = request_one_gpio(dev, "reset-gpio",
		0, &pdata->reset_gpio);
*/
	/* reset, irq gpio info */
	pdata->reset_gpio = of_get_named_gpio_flags(np, "reset-gpio",
				0, &pdata->reset_gpio);

//	printk(KERN_ALERT "RST GPIO = %d\n", pdata->reset_gpio);

//	if (pdata->reset_gpio < 0)
//		return pdata->reset_gpio;

	pdata->irq_gpio = of_get_named_gpio_flags(np, "irq-gpio",
				0, &pdata->irq_gpio);

/*
	pdata->irq_gpio = request_one_gpio(dev, "irq-gpio",
		0, &pdata->irq_gpio);
*/

	if (pdata->irq_gpio < 0)
		return pdata->irq_gpio;


	return 0;
}
#else
static int ft6x06_parse_dt(struct device *dev,
			struct ft6x06_ts_platform_data *pdata)
{
	return -ENODEV;
}
#endif

static int ft6x06_ts_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct ft6x06_platform_data *pdata;
	struct ft6x06_ts_data *ft6x06_ts;
	struct input_dev *input_dev;
	int err = 0;

	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
			sizeof(struct ft6x06_platform_data), GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}
		err = ft6x06_parse_dt(&client->dev, pdata);
		if (err)
			return err;
	} else {
		pdata = (struct ft6x06_platform_data *)client->dev.platform_data;
	}

	if (!pdata) {
		dev_err(&client->dev, "Invalid pdata\n");
		return -EINVAL;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		err = -ENODEV;
		goto exit_check_functionality_failed;
	}

	ft6x06_ts = kzalloc(sizeof(struct ft6x06_ts_data), GFP_KERNEL);

	if (!ft6x06_ts) {
		err = -ENOMEM;
		goto exit_alloc_data_failed;
	}

	i2c_set_clientdata(client, ft6x06_ts);
	ft6x06_ts->client = client;
	ft6x06_ts->pdata = pdata;
	ft6x06_ts->x_max = 4095;
	ft6x06_ts->y_max = 4095;

#ifdef CONFIG_PM
	if (gpio_is_valid(pdata->reset_gpio)) {
		err = devm_gpio_request(&client->dev, pdata->reset_gpio,
					"ft6x06 reset");
		if (err < 0) {
			dev_err(&client->dev, "%s: failed to set gpio reset.\n",
				__func__);
			goto exit_request_reset;
		}
	}
#endif

/* ???
	err = devm_gpio_request_one(&client->dev, pdata->irq_gpio,
				GPIOF_DIR_IN, "ft6x06 irq");
	if (err) {
		dev_err(&client->dev, "failed to request IRQ GPIO: %d\n", err);
		goto exit_request_reset;
	}
*/
	ft6x06_ts->irq = gpio_to_irq(pdata->irq_gpio);

	err = devm_request_threaded_irq(&client->dev, ft6x06_ts->irq,
					NULL, ft6x06_ts_interrupt,
					IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
					client->dev.driver->name, ft6x06_ts);
	if (err < 0) {
		dev_err(&client->dev, "%s: request irq failed\n", __func__);
		goto exit_irq_request_failed;
	}

	input_dev = input_allocate_device();
	if (!input_dev) {
		err = -ENOMEM;
		dev_err(&client->dev, "failed to allocate input device\n");
		goto exit_input_dev_alloc_failed;
	}

	ft6x06_ts->input_dev = input_dev;

	input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);

	set_bit(ABS_MT_TOUCH_MAJOR, input_dev->absbit);
	set_bit(ABS_MT_POSITION_X, input_dev->absbit);
	set_bit(ABS_MT_POSITION_Y, input_dev->absbit);
	set_bit(ABS_MT_PRESSURE, input_dev->absbit);

	input_set_abs_params(input_dev,
			     ABS_MT_POSITION_X, 0, ft6x06_ts->x_max, 0, 0);
	input_set_abs_params(input_dev,
			     ABS_MT_POSITION_Y, 0, ft6x06_ts->y_max, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, PRESS_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, PRESS_MAX, 0, 0);
	input_set_abs_params(input_dev,
			     ABS_MT_TRACKING_ID, 0, CFG_MAX_TOUCH_POINTS, 0, 0);
	input_set_abs_params(input_dev, ABS_X, 0, ft6x06_ts->x_max, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, ft6x06_ts->y_max, 0, 0);
	input_set_abs_params(input_dev, ABS_PRESSURE, 0, PRESS_MAX, 0, 0);

	set_bit(EV_KEY, input_dev->evbit);
	set_bit(EV_ABS, input_dev->evbit);

	input_dev->name = FT6X06_NAME;
	err = input_register_device(input_dev);
	if (err) {
		dev_err(&client->dev,
			"ft6x06_ts_probe: failed to register input device: %s\n",
			dev_name(&client->dev));
		goto exit_input_register_device_failed;
	}

	/* allow touch panel controller to boot, before querying it */
	msleep(150);

#ifdef DEBUG
	{
		u8 val;
		ft6x06_read(client, FT6x06_REG_FW_VER, 1, &val);
		dev_dbg(&client->dev, "[FTS] Firmware version = 0x%x\n", val);

		ft6x06_read(client, FT6x06_REG_POINT_RATE, 1, &val);
		dev_dbg(&client->dev, "[FTS] report rate is %dHz.\n",
			val * 10);

		ft6x06_read(client, FT6x06_REG_THGROUP, 1, &val);
		dev_dbg(&client->dev, "[FTS] touch threshold is %d.\n",
			val * 4);
#endif

	return 0;

exit_input_register_device_failed:
	input_free_device(input_dev);

exit_input_dev_alloc_failed:
exit_request_reset:
exit_irq_request_failed:
	i2c_set_clientdata(client, NULL);
	kfree(ft6x06_ts);

exit_alloc_data_failed:
exit_check_functionality_failed:
	return err;
}

#ifdef CONFIG_PM
static int ft6x06_ts_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct ft6x06_ts_data *ts = i2c_get_clientdata(client);
	dev_dbg(&ts->client->dev, "[FTS]ft6x06 suspend\n");
	disable_irq(ts->pdata->irq_gpio);
	return 0;
}

static int ft6x06_ts_resume(struct i2c_client *client)
{
	struct ft6x06_ts_data *ts = i2c_get_clientdata(client);
	dev_dbg(&ts->client->dev, "[FTS]ft6x06 resume.\n");
	if (gpio_is_valid(ts->pdata->reset_gpio)) {
		gpio_set_value(ts->pdata->reset_gpio, 0);
		msleep(20);
		gpio_set_value(ts->pdata->reset_gpio, 1);
	}
	enable_irq(ts->pdata->irq_gpio);
	return 0;
}
#else
#define ft6x06_ts_suspend	NULL
#define ft6x06_ts_resume		NULL
#endif

static int ft6x06_ts_remove(struct i2c_client *client)
{
	struct ft6x06_ts_data *ft6x06_ts;
	ft6x06_ts = i2c_get_clientdata(client);
	input_unregister_device(ft6x06_ts->input_dev);
	kfree(ft6x06_ts);
	i2c_set_clientdata(client, NULL);
	return 0;
}

static const struct i2c_device_id ft6x06_ts_id[] = {
	{FT6X06_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, ft6x06_ts_id);

#ifdef CONFIG_OF
static struct of_device_id ft6x06_match_table[] = {
	{ .compatible = "focaltech,ft6x06",},
	{ },
};
#else
#define ft6x06_match_table NULL
#endif


static struct i2c_driver ft6x06_ts_driver = {
	.probe = ft6x06_ts_probe,
	.remove = ft6x06_ts_remove,
	.id_table = ft6x06_ts_id,
	.suspend = ft6x06_ts_suspend,
	.resume = ft6x06_ts_resume,
	.driver = {
		.name = FT6X06_NAME,
		.owner = THIS_MODULE,
		.of_match_table=ft6x06_match_table,
	},
};

static int __init ft6x06_ts_init(void)
{
	int ret;
	ret = i2c_add_driver(&ft6x06_ts_driver);
	if (ret)
		pr_err("Adding ft6x06 driver failed (errno = %d)\n", ret);

	return ret;
}

static void __exit ft6x06_ts_exit(void)
{
	i2c_del_driver(&ft6x06_ts_driver);
}

module_init(ft6x06_ts_init);
module_exit(ft6x06_ts_exit);

MODULE_AUTHOR("Sean Cross <xobs@kosagi.com>");
MODULE_DESCRIPTION("FocalTech ft6x06 TouchScreen driver");
MODULE_LICENSE("GPL");
