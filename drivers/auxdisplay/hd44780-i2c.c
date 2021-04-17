// SPDX-License-Identifier: GPL-2.0+
/*
 * HD44780-over-I2C Character LCD driver for Linux
 *
 * Copyright (C) 2021 House Gordon Software Company LTD.
 */

//Uncomment DEBUG to enable 'pr_debug' messages
//#define DEBUG

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>

#include "charlcd.h"
#include "hd44780_common.h"

/*
  I2C/hd88470 bits in 4-bit GPIO mode:
  Each I2C written byte contains:
     bit 0: register select: 0=HD44780-COMMADS  1=DATA/characters
     bit 1: read/write (0=write)
     bit 2: EN/enable (acting as clock?)
     bit 3: backlight (1=on)
     bit 4-7: data bits (low or high nibble)

   To send 8bit of command or data, split into 2 nibbles
   and send as above, high nibble first.
*/
#define REGISTER_SELECT_COMMAND 0x00
#define REGISTER_SELECT_DATA 0x01
#define READ_OPERATION 0x02
#define ENABLE 0x04
#define BACKLIGHT 0x08

enum hd44780_i2c_reg_select {
	COMMAND = 0,
	DATA = REGISTER_SELECT_DATA,
};

enum hd44780_i2c_backlight {
	BACKLIGHT_OFF = 0,
	BACKLIGHT_ON = BACKLIGHT,
};

struct hd44780_i2c {
	struct i2c_client *client;
	struct hd44780_common *hd_common;
};

static void hd44780_i2c_backlight(struct charlcd *lcd, enum charlcd_onoff on)
{
	//FIXME: implement
	pr_err("backlight: not implemented (on = %d)", on);
}

static const struct charlcd_ops hd44780_i2c_ops = {
	.backlight = hd44780_i2c_backlight,
	.print = hd44780_common_print,
	.gotoxy = hd44780_common_gotoxy,
	.home = hd44780_common_home,
	.clear_display = hd44780_common_clear_display,
	.init_display = hd44780_common_init_display,
	.shift_cursor = hd44780_common_shift_cursor,
	.shift_display = hd44780_common_shift_display,
	.display = hd44780_common_display,
	.cursor = hd44780_common_cursor,
	.blink = hd44780_common_blink,
	.fontsize = hd44780_common_fontsize,
	.lines = hd44780_common_lines,
	.redefine_char = hd44780_common_redefine_char,
};

/* Send a command to the LCD panel in 8 bit GPIO mode */
static void hd44780_i2c_write_cmd_gpio8(struct hd44780_common *hdc, int cmd)
{
	pr_err("write_cmd_gpio8: not implemented (cmd = 0x%02x)", cmd);
}

/* Send data to the LCD panel in 8 bit GPIO mode */
static void hd44780_i2c_write_data_gpio8(struct hd44780_common *hdc, int data)
{
	pr_err("write_data_gpio8: not implemented (data = 0x%02x)", data);
}

/* Write 4bits of data to the HD44780 through i2c bus.
   Three i2c 'write' commands are issues, toggling the "EN/ENALBLE" bit.
   (acting as a clock?)

   'data' should be in the format mentioned at the top:
     bit 0: command (0) / data (1)
     bit 1: R/W: must be zero
     bit 2: ENABLE: must be zero (will be toggled here)
     bit 3: backlight on (1) / off(0)
     bit 4-7: nibble data
*/
static int hd44780_i2c_write_gpio4_nibble(struct hd44780_common *hdc, u8 data)
{
	int ret;

	struct hd44780_i2c *hd = hdc->hd44780;

	pr_debug("nibble: sending: %x", data);

	ret = i2c_smbus_write_byte(hd->client, data);
	if (ret) {
		pr_err("write_gpio4_nibble, part 1: " \
			"i2c_smbus_write_byte failed: err=%d data=0x%02x)",
			ret, data);
		return ret;
	}

	ret = i2c_smbus_write_byte(hd->client, data | ENABLE);
	if (ret) {
		pr_err("write_gpio4_nibble, part 2: " \
			"i2c_smbus_write_byte failed: err=%d data=0x%02x)",
			ret, data | ENABLE);
		return ret;
	}

	ret = i2c_smbus_write_byte(hd->client, data);
	if (ret) {
		pr_err("write_gpio4_nibble, part 3: " \
			"i2c_smbus_write_byte failed: err=%d data=0x%02x)",
			ret, data);
		return ret;
	}

	return ret;
}

/* writes an octet (8bit) to the hd44780 through the I2C bus,
 by splitting the octet to two nibbles and sending them. */
static int hd44780_i2c_write_gpio4_byte(struct hd44780_common *hdc, u8 data,
					enum hd44780_i2c_reg_select rs,
					enum hd44780_i2c_backlight bl)
{
	int ret;

	pr_debug("byte: sending: 0x%02x  rs=%x  bl=%x", data, rs, bl);

	ret = hd44780_i2c_write_gpio4_nibble(hdc, (data & 0xF0)|rs|bl);
	if (ret)
		return ret;

	ret = hd44780_i2c_write_gpio4_nibble(hdc, (data & 0x0F)<<4|rs|bl);
	return ret;
}

/* Send a command to the LCD panel in 4 bit GPIO mode */
static void hd44780_i2c_write_cmd_gpio4(struct hd44780_common *hdc, int cmd)
{
	pr_debug("cmd: sending: 0x%02x", cmd);

	int ret = hd44780_i2c_write_gpio4_byte(hdc, cmd,
						COMMAND, BACKLIGHT_ON);
	if (ret)
		pr_err("write_cmd_gpio4: failed to send 0x%02x, ret = %d",
			cmd, ret);
}

/* Send 4-bits of a command to the LCD panel in raw 4 bit GPIO mode */
static void hd44780_i2c_write_cmd_raw_gpio4(struct hd44780_common *hdc,
						int cmd)
{
	pr_debug("cmd_raw: sending: 0x%01x", cmd);

	int ret = hd44780_i2c_write_gpio4_nibble(hdc, cmd << 4 | COMMAND |
							BACKLIGHT_ON);
	if (ret)
		pr_err("write_cmd_raw_gpio4: failed to send 0x%02x, ret = %d",
			cmd, ret);
}

/* Send data to the LCD panel in 4 bit GPIO mode */
static void hd44780_i2c_write_data_gpio4(struct hd44780_common *hdc, int data)
{
	pr_debug("data: sending: 0x%02x", data);

	int ret = hd44780_i2c_write_gpio4_byte(hdc, data, DATA, BACKLIGHT_ON);
	if (ret)
		pr_err("write_data_gpio4: failed to send 0x%02x, ret = %d",
			data, ret);
}

static int hd44780_i2c_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct device_node *node = client->dev.of_node;

	struct charlcd *lcd;
	struct hd44780_common *hdc;
	struct hd44780_i2c *hd;
	int ifwidth, ret;

	pr_debug("hd48870_i2c - driver loaded");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "i2c_check_functionality error\n");
		return -EIO;
	}

	hdc = hd44780_common_alloc();
	if (!hdc)
		return -ENOMEM;

	lcd = charlcd_alloc();
	if (!lcd)
		goto fail1;

	hd = devm_kzalloc(&client->dev, sizeof(struct hd44780_i2c), GFP_KERNEL);
	if (!hd)
		goto fail2;

	hd->hd_common = hdc;
	hd->client = client;
	hdc->hd44780 = hd;
	lcd->drvdata = hdc;
	i2c_set_clientdata(client, lcd);

	/* Required properties */
	ret = of_property_read_u32(node, "display-height-chars", &lcd->height);
	if (ret)
		goto fail3;
	ret = of_property_read_u32(node, "display-width-chars", &lcd->width);
	if (ret)
		goto fail3;

	pr_debug("hd44780-i2c: width: %d, height: %d",
		lcd->width, lcd->height);

	/*
	 * On displays with more than two rows, the internal buffer width is
	 * usually equal to the display width
	 */
	if (lcd->height > 2)
		hdc->bwidth = lcd->width;

	ifwidth = 4;
	hdc->ifwidth = ifwidth;
	lcd->ops = &hd44780_i2c_ops;

	if (ifwidth == 8) {
		hdc->write_data = hd44780_i2c_write_data_gpio8;
		hdc->write_cmd = hd44780_i2c_write_cmd_gpio8;
	} else {
		hdc->write_data = hd44780_i2c_write_data_gpio4;
		hdc->write_cmd = hd44780_i2c_write_cmd_gpio4;
		hdc->write_cmd_raw4 = hd44780_i2c_write_cmd_raw_gpio4;
	}

	ret = charlcd_register(lcd);
	if (ret)
		goto fail3;

	pr_debug("hd4478-i2c: init complete");
	return 0;

fail3:
	pr_err("hd44780-i2c: invalid/missing property value (width/height)");
fail2:
	charlcd_free(lcd);
	pr_err("hd44780-i2c: kzalloc failed");
fail1:
	pr_err("hd44780-i2c: charlcd_alloc failed");
	kfree(hdc);
	return ret;
}

static int hd44780_i2c_remove(struct i2c_client *client)
{
	struct charlcd *lcd = i2c_get_clientdata(client);
	struct hd44780_common *hdc = lcd->drvdata;

	pr_debug("hd44780-i2c: unloading driver");

	kfree(hdc);
	charlcd_unregister(lcd);
	charlcd_free(lcd);

	return 0;
}

static const struct i2c_device_id hd44780_i2c_match[] = { { "hd44780_i2c", 0 },
							  {} };
MODULE_DEVICE_TABLE(i2c, hd44780_i2c_match);

static const struct of_device_id hd44780_i2c_of_match[] = {
	{
		.compatible = "hit,hd44780_i2c",
	},
	{}
};
MODULE_DEVICE_TABLE(of, hd44780_i2c_of_match);

static struct i2c_driver hd44780_i2c_driver = {
	.probe		= hd44780_i2c_probe,
	.remove		= hd44780_i2c_remove,
	.driver		= {
		.name		= "hd44780_i2c",
		.of_match_table	= of_match_ptr(hd44780_i2c_of_match),
	},
	.id_table = hd44780_i2c_match,
};
module_i2c_driver(hd44780_i2c_driver);

MODULE_DESCRIPTION("HD44780-I2C Character LCD driver");
MODULE_AUTHOR("Assaf Gordon <kernel@housegordon.com>");
MODULE_LICENSE("GPL");
