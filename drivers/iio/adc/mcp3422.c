/*
 * mcp3422.c - driver for the Microchip mcp3422/3/4 chip family
 *
 * Copyright (C) 2013, Angelo Compagnucci
 * Author: Angelo Compagnucci <angelo.compagnucci@gmail.com>
 *
 * Datasheet: http://ww1.microchip.com/downloads/en/devicedoc/22088b.pdf
 *
 * This driver exports the value of analog input voltage to sysfs, the
 * voltage unit is nV.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/sysfs.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

/* Masks */
#define MCP3422_CHANNEL_MASK	0x60
#define MCP3422_PGA_MASK	0x03
#define MCP3422_SRATE_MASK	0x0C
#define MCP3422_SRATE_240	0x0
#define MCP3422_SRATE_60	0x1
#define MCP3422_SRATE_15	0x2
#define MCP3422_SRATE_3	0x3
#define MCP3422_PGA_1	0
#define MCP3422_PGA_2	1
#define MCP3422_PGA_4	2
#define MCP3422_PGA_8	3
#define MCP3422_CONT_SAMPLING	0x10

#define MCP3422_CHANNEL(config)	(((config) & MCP3422_CHANNEL_MASK) >> 5)
#define MCP3422_PGA(config)	((config) & MCP3422_PGA_MASK)
#define MCP3422_SAMPLE_RATE(config)	(((config) & MCP3422_SRATE_MASK) >> 2)

#define MCP3422_CHANNEL_VALUE(value) (((value) << 5) & MCP3422_CHANNEL_MASK)
#define MCP3422_PGA_VALUE(value) ((value) & MCP3422_PGA_MASK)
#define MCP3422_SAMPLE_RATE_VALUE(value) ((value << 2) & MCP3422_SRATE_MASK)

#define MCP3422_CHAN(_index) \
	{ \
		.type = IIO_VOLTAGE, \
		.indexed = 1, \
		.channel = _index, \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) \
				| BIT(IIO_CHAN_INFO_SCALE), \
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	}

/* LSB is in nV to eliminate floating point */
static const u32 rates_to_lsb[] = {1000000, 250000, 62500, 15625};

/* Client data (each client gets its own) */
struct mcp3422 {
	struct i2c_client *i2c;
	u8 config;
	u8 pga[4];
	struct mutex lock;
};

static int mcp3422_update_config(struct mcp3422 *adc, u8 newconfig)
{
	int ret;

	mutex_lock(&adc->lock);

	ret = i2c_master_send(adc->i2c, &newconfig, 1);
	if (ret > 0)
		adc->config = newconfig;

	mutex_unlock(&adc->lock);

	return ret;
}

static int mcp3422_read(struct mcp3422 *adc, int *value, u8 *config)
{
	int ret = 0;
	u8 sample_rate = MCP3422_SAMPLE_RATE(adc->config);
	u8 buf[4] = {0, 0, 0, 0};
	u32 temp;

	if (sample_rate == MCP3422_SRATE_3) {
		ret = i2c_master_recv(adc->i2c, buf, 4);
		temp = buf[0] << 16 | buf[1] << 8 | buf[2];
		*config = buf[3];
	} else {
		ret = i2c_master_recv(adc->i2c, buf, 3);
		temp = buf[0] << 8 | buf[1];
		*config = buf[2];
	}

	switch (sample_rate) {
	case MCP3422_SRATE_240:
		*value = sign_extend32(temp, 12);
		break;
	case MCP3422_SRATE_60:
		*value = sign_extend32(temp, 14);
		break;
	case MCP3422_SRATE_15:
		*value = sign_extend32(temp, 16);
		break;
	case MCP3422_SRATE_3:
		*value = sign_extend32(temp, 18);
		break;
	}

	return ret;
}

static int mcp3422_read_channel(struct mcp3422 *adc,
				struct iio_chan_spec const *channel, int *value)
{
	int mtime = 0;
	u8 config = 0;
	u8 req_channel = channel->channel;

	if (req_channel != MCP3422_CHANNEL(adc->config)) {
		config = adc->config;
		config &= ~MCP3422_CHANNEL_MASK;
		config |= MCP3422_CHANNEL_VALUE(req_channel);
		config &= ~MCP3422_PGA_MASK;
		config |= MCP3422_PGA_VALUE(adc->pga[req_channel]);
		mcp3422_update_config(adc, config);
		switch (MCP3422_SAMPLE_RATE(config)) {
		case MCP3422_SRATE_240:
			mtime = 1000 / 240;
			break;
		case MCP3422_SRATE_60:
			mtime = 1000 / 60;
			break;
		case MCP3422_SRATE_15:
			mtime = 1000 / 15;
			break;
		case MCP3422_SRATE_3:
			mtime = 1000 / 3;
			break;
		}
		msleep(mtime);
	}

	return mcp3422_read(adc, value, &config);
}

static int mcp3422_read_raw(struct iio_dev *iio,
			struct iio_chan_spec const *channel, int *val1,
			int *val2, long mask)
{
	struct mcp3422 *adc = iio_priv(iio);
	int err, temp = 0;

	u8 sample_rate = MCP3422_SAMPLE_RATE(adc->config);
	u8 pga		 = MCP3422_PGA(adc->config);

	err = mcp3422_read_channel(adc, channel, &temp);
	if (err < 0)
		return err;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		*val1 = temp;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:

		temp = (temp * rates_to_lsb[sample_rate])
			/ (pga ? 1 << pga : 1);

		*val1 = temp / 1000000000;
		*val2 = abs(temp % 1000000000);
		return IIO_VAL_INT_PLUS_NANO;

	case IIO_CHAN_INFO_SAMP_FREQ:
		switch (MCP3422_SAMPLE_RATE(adc->config)) {
		case MCP3422_SRATE_240:
			*val1 = 240;
			break;
		case MCP3422_SRATE_60:
			*val1 = 60;
			break;
		case MCP3422_SRATE_15:
			*val1 = 15;
			break;
		case MCP3422_SRATE_3:
			*val1 = 3;
			break;
		}
		return IIO_VAL_INT;

	default:
		break;
	}

	return -EINVAL;
}

static int mcp3422_write_raw(struct iio_dev *iio,
			struct iio_chan_spec const *channel, int val1,
			int val2, long mask)
{
	struct mcp3422 *adc = iio_priv(iio);
	u8 temp;
	u8 config = adc->config;
	u8 req_channel = channel->channel;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		switch (val1) {
		case 1:
			temp = MCP3422_PGA_1;
			break;
		case 2:
			temp = MCP3422_PGA_2;
			break;
		case 4:
			temp = MCP3422_PGA_4;
			break;
		case 8:
			temp = MCP3422_PGA_8;
			break;
		default:
			return -EINVAL;
		}

		adc->pga[req_channel] = temp;

		config &= ~MCP3422_CHANNEL_MASK;
		config |= MCP3422_CHANNEL_VALUE(req_channel);
		config &= ~MCP3422_PGA_MASK;
		config |= MCP3422_PGA_VALUE(adc->pga[req_channel]);
		mcp3422_update_config(adc, config);
		return 0;

	case IIO_CHAN_INFO_SAMP_FREQ:
		switch (val1) {
		case 240:
			temp = MCP3422_SRATE_240;
			break;
		case 60:
			temp = MCP3422_SRATE_60;
			break;
		case 15:
			temp = MCP3422_SRATE_15;
			break;
		case 3:
			temp = MCP3422_SRATE_3;
			break;
		default:
			return -EINVAL;
		}

		config &= ~MCP3422_CHANNEL_MASK;
		config |= MCP3422_CHANNEL_VALUE(req_channel);
		config &= ~MCP3422_SRATE_MASK;
		config |= MCP3422_SAMPLE_RATE_VALUE(temp);

		mcp3422_update_config(adc, config);
		return 0;
	default:
		break;
	}

	return -EINVAL;
}

static IIO_CONST_ATTR_SAMP_FREQ_AVAIL("240 60 15 3");
static IIO_CONST_ATTR(in_voltage_scale_available, "1 2 4 8");

static struct attribute *mcp3422_attributes[] = {
	&iio_const_attr_sampling_frequency_available.dev_attr.attr,
	&iio_const_attr_in_voltage_scale_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group mcp3422_attribute_group = {
	.attrs = mcp3422_attributes,
};

static const struct iio_chan_spec mcp3422_channels[] = {
	MCP3422_CHAN(0),
	MCP3422_CHAN(1),
};

static const struct iio_chan_spec mcp3424_channels[] = {
	MCP3422_CHAN(0),
	MCP3422_CHAN(1),
	MCP3422_CHAN(2),
	MCP3422_CHAN(3),
};

static const struct iio_info mcp3422_info = {
	.read_raw = mcp3422_read_raw,
	.write_raw = mcp3422_write_raw,
	.attrs = &mcp3422_attribute_group,
	.driver_module = THIS_MODULE,
};

static int mcp3422_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct iio_dev *iio;
	struct mcp3422 *adc;
	int err;
	u8 config;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	iio = iio_device_alloc(sizeof(*adc));
	if (!iio)
		return -ENOMEM;

	adc = iio_priv(iio);
	adc->i2c = client;

	mutex_init(&adc->lock);

	iio->dev.parent = &client->dev;
	iio->name = dev_name(&client->dev);
	iio->modes = INDIO_DIRECT_MODE;
	iio->info = &mcp3422_info;

	switch ((unsigned int)(id->driver_data)) {
	case 2:
	case 3:
		iio->channels = mcp3422_channels;
		iio->num_channels = ARRAY_SIZE(mcp3422_channels);
		break;
	case 4:
		iio->channels = mcp3424_channels;
		iio->num_channels = ARRAY_SIZE(mcp3424_channels);
		break;
	}

	/* meaningful default configuration */
	config = (MCP3422_CONT_SAMPLING
		| MCP3422_CHANNEL_VALUE(1)
		| MCP3422_PGA_VALUE(MCP3422_PGA_1)
		| MCP3422_SAMPLE_RATE_VALUE(MCP3422_SRATE_240));
	mcp3422_update_config(adc, config);

	err = iio_device_register(iio);
	if (err < 0)
		goto iio_free;

	i2c_set_clientdata(client, iio);

	return 0;

iio_free:
	iio_device_free(iio);

	return err;
}

static int mcp3422_remove(struct i2c_client *client)
{
	struct iio_dev *iio = i2c_get_clientdata(client);

	iio_device_unregister(iio);
	iio_device_free(iio);

	return 0;
}

static const struct i2c_device_id mcp3422_id[] = {
	{ "mcp3422", 2 },
	{ "mcp3423", 3 },
	{ "mcp3424", 4 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mcp3422_id);

#ifdef CONFIG_OF
static const struct of_device_id mcp3422_of_match[] = {
	{ .compatible = "mcp3422" },
	{ }
};
MODULE_DEVICE_TABLE(of, mcp3422_of_match);
#endif

static struct i2c_driver mcp3422_driver = {
	.driver = {
		.name = "mcp3422",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(mcp3422_of_match),
	},
	.probe = mcp3422_probe,
	.remove = mcp3422_remove,
	.id_table = mcp3422_id,
};
module_i2c_driver(mcp3422_driver);

MODULE_AUTHOR("Angelo Compagnucci <angelo.compagnucci@gmail.com>");
MODULE_DESCRIPTION("Microchip mcp3422/3/4 driver");
MODULE_LICENSE("GPL v2");
