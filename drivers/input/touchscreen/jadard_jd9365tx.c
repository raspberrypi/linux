// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 VIEWE
 *
 * Jadard JD9365TX in-cell touchscreen (I2C @0x68, polling).
 * Typical DSI FPC has no interrupt line.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#define JD_CHIP_ID			0x9084
#define JD_CHIP_ID_ALT			0x9032
#define JD_SOC_CHIP_ID2			0x40008076
#define JD_ERAM_BASE			0x20011000
#define JD_SECTION_READY_VAL		0xA55A
#define JD_MAX_DSRAM_NUM		25
#define JD_ESRAM_COORD_IDX		1
#define JD_ESRAM_SECTION_NUM		6
#define JD_DEFAULT_COORD_ADDR		0x20011120
#define JD_TOUCH_DATA_SIZE		78
#define JD_REPORT_MIN_SIZE		53
#define JD_FINGER_DATA_SIZE		5
#define JD_FINGER_NUM_ADDR		0
#define JD_TOUCH_COORD_INFO_ADDR	3
#define JD_MAX_POINTS			10
#define JD_DEFAULT_POLL_MS		16

struct jadard_tp {
	struct i2c_client *client;
	struct input_dev *input;
	struct touchscreen_properties prop;
	struct gpio_desc *reset;
	u32 coord_addr;
	u16 touch_data_size;
	u16 max_points;
	unsigned int poll_interval_ms;
};

static int jd_bus_write(struct i2c_client *client, const u8 *cmd, u8 cmd_len,
			const u8 *data, u16 data_len)
{
	u8 buf[64];
	struct i2c_msg msg;
	int ret;

	if (cmd_len + data_len > sizeof(buf))
		return -EINVAL;

	memcpy(buf, cmd, cmd_len);
	if (data_len)
		memcpy(buf + cmd_len, data, data_len);

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = cmd_len + data_len;
	msg.buf = buf;

	ret = i2c_transfer(client->adapter, &msg, 1);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int jd_bus_read(struct i2c_client *client, const u8 *cmd, u8 cmd_len,
		       u8 *data, u16 data_len)
{
	struct i2c_msg msg[2];
	int ret;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = cmd_len;
	msg[0].buf = (u8 *)cmd;
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = data_len;
	msg[1].buf = data;

	ret = i2c_transfer(client->adapter, msg, 2);
	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int jd_enter_backdoor(struct i2c_client *client)
{
	const u8 cmd[5] = { 0xF2, 0xAA, 0xF0, 0x0F, 0x55 };
	const u8 data = 0x68;

	return jd_bus_write(client, cmd, sizeof(cmd), &data, 1);
}

static int jd_read_reg(struct i2c_client *client, u32 addr, u8 *rdata, u16 rlen)
{
	u8 cmd[6];

	cmd[0] = 0xF3;
	cmd[1] = (addr >> 24) & 0xFF;
	cmd[2] = (addr >> 16) & 0xFF;
	cmd[3] = (addr >> 8) & 0xFF;
	cmd[4] = addr & 0xFF;
	cmd[5] = 0x03;

	return jd_bus_read(client, cmd, sizeof(cmd), rdata, rlen);
}

static int jd_get_chip_id(struct i2c_client *client, u16 *id)
{
	u8 rbuf[2];
	int ret;

	ret = jd_read_reg(client, JD_SOC_CHIP_ID2, rbuf, sizeof(rbuf));
	if (ret)
		return ret;

	*id = (u16)((rbuf[1] << 8) | rbuf[0]);
	return 0;
}

static bool jd_section_ready(struct i2c_client *client)
{
	u8 rbuf[2];
	unsigned int waited = 0;

	do {
		msleep(2);
		if (jd_read_reg(client, JD_ERAM_BASE, rbuf, sizeof(rbuf)))
			return false;
		waited += 2;
		if (((rbuf[1] << 8) | rbuf[0]) == JD_SECTION_READY_VAL)
			return true;
	} while (waited < 100);

	return false;
}

static int jd_resolve_coord_addr(struct jadard_tp *ts)
{
	u32 esram_num_addr = JD_ERAM_BASE + 4 + JD_MAX_DSRAM_NUM * 8;
	u32 esram_sec_addr = esram_num_addr + 4;
	u8 buf[JD_ESRAM_SECTION_NUM * 8];
	u32 addr, len;
	int ret;

	ret = jd_read_reg(ts->client, esram_sec_addr, buf, sizeof(buf));
	if (ret)
		return ret;

	addr = get_unaligned_le32(&buf[JD_ESRAM_COORD_IDX * 8]);
	len = get_unaligned_le32(&buf[JD_ESRAM_COORD_IDX * 8 + 4]);

	if (len < JD_REPORT_MIN_SIZE || len > JD_TOUCH_DATA_SIZE ||
	    addr < JD_ERAM_BASE ||
	    addr > JD_ERAM_BASE + 0x1000 - len)
		return -EINVAL;

	ts->coord_addr = addr;
	ts->touch_data_size = (u16)len;
	return 0;
}

static int jd_hw_init(struct jadard_tp *ts)
{
	u16 chip_id = 0;
	int ret;

	ret = jd_enter_backdoor(ts->client);
	if (ret)
		return ret;

	ret = jd_get_chip_id(ts->client, &chip_id);
	if (ret)
		return ret;

	if (chip_id != JD_CHIP_ID && chip_id != JD_CHIP_ID_ALT)
		return -ENODEV;

	if (jd_section_ready(ts->client) && !jd_resolve_coord_addr(ts))
		return 0;

	ts->coord_addr = JD_DEFAULT_COORD_ADDR;
	ts->touch_data_size = JD_TOUCH_DATA_SIZE;
	return 0;
}

static void jd_report_touches(struct jadard_tp *ts, const u8 *buf)
{
	u8 finger_num = buf[JD_FINGER_NUM_ADDR];
	unsigned int i;

	/* Discard obviously invalid headers instead of clamping to max. */
	if (finger_num > ts->max_points)
		return;

	for (i = 0; i < ts->max_points; i++) {
		const u8 *c = buf + JD_TOUCH_COORD_INFO_ADDR +
			      i * JD_FINGER_DATA_SIZE;
		u16 x = get_unaligned_be16(c);
		u16 y = get_unaligned_be16(c + 2);
		u8 w = c[4];
		bool down;

		/*
		 * Slots are fixed; inactive contacts report 0xFFFF/0xFFFF.
		 * Do not gate on finger_num, or a lower slot lift drops a
		 * still-active higher slot.
		 */
		down = x != 0xFFFF && y != 0xFFFF &&
		       x <= ts->prop.max_x && y <= ts->prop.max_y;

		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, down);

		if (down) {
			touchscreen_report_pos(ts->input, &ts->prop, x, y, true);
			input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, w);
		}
	}

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static void jd_poll(struct input_dev *input)
{
	struct jadard_tp *ts = input_get_drvdata(input);
	u8 buf[JD_TOUCH_DATA_SIZE];
	int ret;

	ret = jd_read_reg(ts->client, ts->coord_addr, buf, ts->touch_data_size);
	if (ret) {
		/* Panel/power cycle drops backdoor mode; re-open it. */
		if (jd_hw_init(ts))
			return;
		ret = jd_read_reg(ts->client, ts->coord_addr, buf,
				  ts->touch_data_size);
		if (ret)
			return;
	}

	jd_report_touches(ts, buf);
}

static int jadard_tp_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct jadard_tp *ts;
	struct input_dev *input;
	static const char * const supplies[] = { "vdd", "iovcc" };
	u32 val;
	int error;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	ts->max_points = JD_MAX_POINTS;
	ts->poll_interval_ms = JD_DEFAULT_POLL_MS;
	ts->touch_data_size = JD_TOUCH_DATA_SIZE;
	ts->coord_addr = JD_DEFAULT_COORD_ADDR;

	error = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(supplies),
					       supplies);
	if (error)
		return error;

	ts->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ts->reset))
		return PTR_ERR(ts->reset);

	if (ts->reset) {
		gpiod_set_value_cansleep(ts->reset, 1);
		msleep(10);
		gpiod_set_value_cansleep(ts->reset, 0);
		msleep(20);
	}

	error = jd_hw_init(ts);
	if (error)
		return error;

	if (!device_property_read_u32(dev, "poll-interval", &val) &&
	    val >= 8 && val <= 100)
		ts->poll_interval_ms = val;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = "Jadard JD9365TX Touchscreen";
	input->id.bustype = BUS_I2C;

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, 719, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, 1279, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	touchscreen_parse_properties(input, true, &ts->prop);

	error = input_mt_init_slots(input, ts->max_points, INPUT_MT_DIRECT);
	if (error)
		return error;

	input_set_drvdata(input, ts);
	ts->input = input;
	i2c_set_clientdata(client, ts);

	error = input_setup_polling(input, jd_poll);
	if (error)
		return error;

	input_set_poll_interval(input, ts->poll_interval_ms);

	error = input_register_device(input);
	if (error)
		return error;

	return 0;
}

static const struct of_device_id jadard_tp_of_match[] = {
	{ .compatible = "jadard,jd9365tx" },
	{ }
};
MODULE_DEVICE_TABLE(of, jadard_tp_of_match);

static const struct i2c_device_id jadard_tp_id[] = {
	{ .name = "jd9365tx" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, jadard_tp_id);

static struct i2c_driver jadard_tp_driver = {
	.driver = {
		.name = "jadard-tp-jd9365tx",
		.of_match_table = jadard_tp_of_match,
	},
	.probe = jadard_tp_probe,
	.id_table = jadard_tp_id,
};
module_i2c_driver(jadard_tp_driver);

MODULE_AUTHOR("Hefei <3066883572@qq.com>");
MODULE_DESCRIPTION("Jadard JD9365TX in-cell touchscreen");
MODULE_LICENSE("GPL");
