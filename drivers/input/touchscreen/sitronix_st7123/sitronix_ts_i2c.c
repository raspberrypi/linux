// SPDX-License-Identifier: GPL-2.0
/*
 * Sitronix Touchscreen Controller Driver
 *
 * Copyright (C) 2018 Sitronix Technology Co., Ltd.
 *	CT Chen <ct_chen@sitronix.com.tw>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "sitronix_ts.h"
#include "sitronix_st7123.h"
#include <linux/i2c.h>

#define I2C_RETRY_COUNT 1
//#define I2C_MASTER_SEND_RECV

static int sitronix_ts_i2c_parse_dt(struct device *dev,
				    struct sitronix_ts_host_interface *host_if)
{
#ifdef CONFIG_OF
	struct device_node *np = dev->of_node;
	int rc;

	host_if->irq_gpio = of_get_named_gpio(np, "irq-gpio", 0);
	if (host_if->irq_gpio < 0)
		sterr("%s: Failed to read interrupt GPIO!(%d)\n", __func__,
		      host_if->irq_gpio);
	else {
		rc = gpio_request(host_if->irq_gpio, "st_irq_gpio");
		stmsg("%s: Interrupt GPIO = %d. (flag=%d) , request ret = %d\n",
		      __func__, host_if->irq_gpio, host_if->irq_gpio_flags, rc);
	}

	host_if->rst_gpio = of_get_named_gpio(np, "rst-gpio", 0);
	if (host_if->rst_gpio < 0)
		sterr("%s: Failed to read Reset GPIO!(%d)\n", __func__,
		      host_if->rst_gpio);
	else {
		rc = gpio_request(host_if->rst_gpio, "st_rst_gpio");
		stmsg("%s: Reset GPIO = %d. (flag=%d), request ret = %d\n",
		      __func__, host_if->rst_gpio, host_if->rst_gpio_flags, rc);
	}

#endif
	return 0;
}

static int sitronix_ts_i2c_read(uint16_t addr, uint8_t *data, uint16_t length,
				void *if_data)
{
	int ret;
	uint8_t retry;
	unsigned char buf[2];
	struct sitronix_ts_i2c_data *i2c_data =
		(struct sitronix_ts_i2c_data *)if_data;
	struct i2c_client *client = i2c_data->client;

	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 2,
			.buf = buf,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = data,
		},
	};
	buf[0] = (addr >> 8) & 0xFF;
	buf[1] = (addr) & 0xFF;

	for (retry = 1; retry <= I2C_RETRY_COUNT; retry++) {
		if (i2c_transfer(client->adapter, msg, 2) == 2) {
			ret = length;
			break;
		}
		sterr("%s: I2C retry %d\n", __func__, retry);
		msleep(20);
	}

	if (retry > I2C_RETRY_COUNT) {
		sterr("%s: I2C read over retry limit\n", __func__);
		ret = -EIO;
	}

	return ret;
}

static int sitronix_ts_i2c_write(uint16_t addr, uint8_t *data, uint16_t length,
				 void *if_data)
{
	int ret;
	unsigned char retry;
	unsigned char *buf = NULL;
	struct sitronix_ts_i2c_data *i2c_data =
		(struct sitronix_ts_i2c_data *)if_data;
	struct i2c_client *client = i2c_data->client;

	struct i2c_msg msg[] = { {
		.addr = client->addr, .flags = 0, .len = length + 2,
		//.buf = buf,
	} };

	buf = kzalloc((length + 2), GFP_KERNEL);
	if (!buf) {
		sterr("%s: Alloc memory for buf failed!\n", __func__);
		return -ENOMEM;
	}
	msg[0].buf = buf;
	buf[0] = (addr >> 8) & 0xFF;
	buf[1] = (addr) & 0xFF;
	memcpy(&buf[2], &data[0], length);

	for (retry = 1; retry <= I2C_RETRY_COUNT; retry++) {
		if (i2c_transfer(client->adapter, msg, 1) == 1) {
			ret = length;
			break;
		}
		sterr("%s: I2C retry %d\n", __func__, retry);
		usleep_range(8000, 9000);
	}

	if (retry > I2C_RETRY_COUNT) {
		sterr("%s: I2C write over retry limit\n", __func__);
		ret = -EIO;
		goto err_return;
	}

err_return:
	kfree(buf);

	return ret;
}

static int sitronix_ts_i2c_dread(uint8_t *data, uint16_t length, void *if_data)
{
	int ret;
	uint8_t retry;
	struct sitronix_ts_i2c_data *i2c_data =
		(struct sitronix_ts_i2c_data *)if_data;
	struct i2c_client *client = i2c_data->client;

	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = data,
		},
	};

	for (retry = 1; retry <= I2C_RETRY_COUNT; retry++) {
		if (i2c_transfer(client->adapter, msg, 1) == 1) {
			ret = length;
			break;
		}
		sterr("%s: I2C retry %d\n", __func__, retry);
		msleep(20);
	}

	if (retry > I2C_RETRY_COUNT) {
		sterr("%s: I2C read over retry limit\n", __func__);
		ret = -EIO;
	}

	return ret;
}

static int sitronix_ts_i2c_dwrite(uint8_t *data, uint16_t length, void *if_data)
{
	int ret;
	unsigned char retry;
	struct sitronix_ts_i2c_data *i2c_data =
		(struct sitronix_ts_i2c_data *)if_data;
	struct i2c_client *client = i2c_data->client;

	struct i2c_msg msg[] = { {
		.addr = client->addr,
		.flags = 0,
		.len = length,
		.buf = data,
	} };

	for (retry = 1; retry <= I2C_RETRY_COUNT; retry++) {
		if (i2c_transfer(client->adapter, msg, 1) == 1) {
			ret = length;
			break;
		}
		sterr("%s: I2C retry %d\n", __func__, retry);
		msleep(20);
	}

	if (retry > I2C_RETRY_COUNT) {
		sterr("%s: I2C write over retry limit\n", __func__);
		ret = -EIO;
	}

	return ret;
}

static int sitronix_ts_i2c_aread(uint8_t *tx_buf, uint16_t tx_len,
				 uint8_t *rx_buf, uint16_t rx_len,
				 void *if_data)
{
	int ret;
	uint8_t retry;
	struct sitronix_ts_i2c_data *i2c_data =
		(struct sitronix_ts_i2c_data *)if_data;
	struct i2c_client *client = i2c_data->client;

	struct i2c_msg msg[] = {
		{
			.addr = ST_ADDR_MODE_I2C_ADDR,
			.flags = 0,
			.len = tx_len + 1,
			.buf = tx_buf,
		},
		{
			.addr = ST_ADDR_MODE_I2C_ADDR,
			.flags = I2C_M_RD,
			.len = rx_len,
			.buf = rx_buf,
		},
	};

	tx_buf[0] = 0xA0 | (tx_len - 1);

	for (retry = 1; retry <= I2C_RETRY_COUNT; retry++) {
		if (i2c_transfer(client->adapter, msg, 2) == 2) {
			ret = rx_len;
			break;
		}
		sterr("%s: I2C retry %d\n", __func__, retry);
		msleep(20);
	}

	if (retry > I2C_RETRY_COUNT) {
		sterr("%s: I2C read over retry limit\n", __func__);
		ret = -EIO;
	}

	return 0;
}

static int sitronix_ts_i2c_awrite(uint8_t *tx_buf, uint16_t tx_len,
				  void *if_data)
{
	int ret;
	unsigned char retry;
	struct sitronix_ts_i2c_data *i2c_data =
		(struct sitronix_ts_i2c_data *)if_data;
	struct i2c_client *client = i2c_data->client;

	struct i2c_msg msg[] = { {
		.addr = ST_ADDR_MODE_I2C_ADDR,
		.flags = 0,
		.len = tx_len + 1,
		.buf = tx_buf,
	} };

	tx_buf[0] = 0x00;

	for (retry = 1; retry <= I2C_RETRY_COUNT; retry++) {
		if (i2c_transfer(client->adapter, msg, 1) == 1) {
			ret = tx_len;
			break;
		}
		sterr("%s: I2C retry %d\n", __func__, retry);
		msleep(20);
	}

	if (retry > I2C_RETRY_COUNT) {
		sterr("%s: I2C write over retry limit\n", __func__);
		ret = -EIO;
	}

	return ret;
}

static void sitronix_ts_i2c_dev_release(struct device *dev)
{
}

static struct sitronix_ts_i2c_data g_i2c_data;

static struct sitronix_ts_host_interface i2c_host_if = {
	.bus_type = BUS_I2C,
#ifdef SITRONIX_TP_WITH_FLASH
	.is_use_flash = 1,
#else
	.is_use_flash = 0,
#endif
	.if_data = &g_i2c_data,
	.read = sitronix_ts_i2c_read,
	.write = sitronix_ts_i2c_write,
	.dread = sitronix_ts_i2c_dread,
	.dwrite = sitronix_ts_i2c_dwrite,
	.aread = sitronix_ts_i2c_aread,
	.awrite = sitronix_ts_i2c_awrite,
	.sread = sitronix_ts_i2c_aread,
};

static struct platform_device sitronix_ts_i2c_device = {
	.name = SITRONIX_TS_I2C_DRIVER_NAME,
	.id = 0,
	.num_resources = 0,
	.dev = {
		.release = sitronix_ts_i2c_dev_release,
	},
};

static int sitronix_ts_i2c_probe(struct i2c_client *client)
{
	int ret;

	//client->addr = 0x66;	//for debug
	stmsg("i2c addr: 0x%x\n", client->addr);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		sterr("%s: I2C function not supported by host!\n", __func__);
		return -EIO;
	}

	if (client->dev.of_node)
		ret = sitronix_ts_i2c_parse_dt(&client->dev, &i2c_host_if);

	g_i2c_data.client = client;

	sitronix_ts_i2c_device.dev.parent = &client->dev;
	sitronix_ts_i2c_device.dev.platform_data = &i2c_host_if;

	ret = platform_device_register(&sitronix_ts_i2c_device);
	if (ret) {
		sterr("%s: Failed to register platform device\n", __func__);
		return -ENODEV;
	}

	return 0;
}
#ifdef SITRONIX_INTERFACE_I2C
static void sitronix_ts_i2c_remove(struct i2c_client *client)
{
	struct sitronix_ts_host_interface *host_if;

	host_if = (struct sitronix_ts_host_interface *)
			  sitronix_ts_i2c_device.dev.platform_data;

	if (!host_if)
		return;

	if (gpio_is_valid(host_if->irq_gpio))
		gpio_free(host_if->irq_gpio);

	if (gpio_is_valid(host_if->rst_gpio))
		gpio_free(host_if->rst_gpio);

	platform_device_unregister(&sitronix_ts_i2c_device);
}
#endif

static const struct i2c_device_id i2c_id_table[] = {
	{
		SITRONIX_TS_I2C_DRIVER_NAME,
		0,
	},
	{},
};
MODULE_DEVICE_TABLE(i2c, i2c_id_table);

#ifdef CONFIG_OF
static const struct of_device_id i2c_match_table[] = {
	{ .compatible = "sitronix_ts,st7123" },
	{},
};
MODULE_DEVICE_TABLE(of, i2c_match_table);
#else /* CONFIG_OF */
#define i2c_match_table NULL
#endif /* CONFIG_OF */

#ifdef SITRONIX_I2C_ADDRESS_DETECT
static int sitronix_ts_i2c_detect(struct i2c_client *client,
				  struct i2c_board_info *info)
{
	uint8_t data[8];
	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = sizeof(data),
			.buf = data,
		},
	};
	stmsg("%s:%d bus = %d, addr = 0x%02X: ", __func__, __LINE__,
	      client->adapter->nr, client->addr);
	if (i2c_transfer(client->adapter, msg, 1) == 1) {
		stmsg("Detected.\n");
	} else {
		stdbg("Not detected.\n");
		return -ENODEV;
	}

	return 0;
}

const unsigned short sitronix_i2c_address_list[] = { 0x55, 0x66, 0x38, 0x70,
						     I2C_CLIENT_END };
#endif /* SITRONIX_I2C_ADDRESS_DETECT */

static struct i2c_driver sitronix_ts_i2c_driver = {
	.driver = {
		.name = SITRONIX_TS_I2C_DRIVER_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = i2c_match_table,
#endif
	},
	.probe = sitronix_ts_i2c_probe,
#ifdef SITRONIX_INTERFACE_I2C
	/*.remove = __exit_p(sitronix_ts_i2c_remove),*/
	.remove = sitronix_ts_i2c_remove,
#endif
	.id_table = i2c_id_table,
#ifdef SITRONIX_I2C_ADDRESS_DETECT
	.address_list = sitronix_i2c_address_list,
	.detect = sitronix_ts_i2c_detect,
#endif /* SITRONIX_I2C_ADDRESS_DETECT */
};

int sitronix_ts_i2c_init(void)
{
	return i2c_add_driver(&sitronix_ts_i2c_driver);
}
EXPORT_SYMBOL(sitronix_ts_i2c_init);

void sitronix_ts_i2c_exit(void)
{
	i2c_del_driver(&sitronix_ts_i2c_driver);
}
EXPORT_SYMBOL(sitronix_ts_i2c_exit);

MODULE_AUTHOR("Sitronix Technology Co., Ltd.");
MODULE_DESCRIPTION("Sitronix Touchscreen Controller Driver");
MODULE_LICENSE("GPL");
