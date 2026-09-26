// SPDX-License-Identifier: GPL-2.0
/*
 * Jadard JD9365TX in-cell touchscreen (I2C @0x68, polling).
 *
 * Typical DSI FPC has no interrupt line. Backlight MCU @0x45 shares the
 * bus; pending brightness writes are flushed after coordinate reads.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "viewe_dsi_i2c_bus.h"

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
#define JD_FINGER_DATA_SIZE		5
#define JD_FINGER_NUM_ADDR		0
#define JD_TOUCH_COORD_INFO_ADDR	3
#define JD_MAX_POINTS			10

#define JD_BUS_RETRY			3
#define JD_PROBE_RETRY_MS		500
#define JD_PROBE_DELAY_MS		2500

struct jadard_tp {
	struct i2c_client *client;
	struct input_dev *input;
	struct touchscreen_properties prop;
	struct delayed_work poll_work;
	struct mutex lock;

	u32 coord_addr;
	u16 touch_data_size;
	u16 max_points;
	u16 abs_x_max;
	u16 abs_y_max;
	unsigned int poll_interval_ms;

	bool ready;
	bool suspended;
	u16 prev_x[JD_MAX_POINTS];
	u16 prev_y[JD_MAX_POINTS];
};

static int jd_bus_write(struct i2c_client *client, const u8 *cmd, u8 cmd_len,
			const u8 *data, u16 data_len)
{
	u8 buf[64];
	struct i2c_msg msg;
	int retry, ret;

	if (cmd_len + data_len > sizeof(buf))
		return -EINVAL;

	memcpy(buf, cmd, cmd_len);
	if (data_len)
		memcpy(buf + cmd_len, data, data_len);

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = cmd_len + data_len;
	msg.buf = buf;

	for (retry = 0; retry < JD_BUS_RETRY; retry++) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1)
			return 0;
		msleep(20);
	}

	return ret < 0 ? ret : -EIO;
}

static int jd_bus_read(struct i2c_client *client, const u8 *cmd, u8 cmd_len,
		       u8 *data, u16 data_len)
{
	struct i2c_msg msg[2];
	int retry, ret;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = cmd_len;
	msg[0].buf = (u8 *)cmd;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = data_len;
	msg[1].buf = data;

	for (retry = 0; retry < JD_BUS_RETRY; retry++) {
		ret = i2c_transfer(client->adapter, msg, 2);
		if (ret == 2)
			return 0;
		msleep(20);
	}

	return ret < 0 ? ret : -EIO;
}

/* Official POR_BACKDOOR: F2 AA F0 0F 55 68 */
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

/*
 * Parse ESRAM section table for coordinate_report address / length.
 * Layout matches official jd9365tx_ChipInfoInit / ReadSectionInfo.
 */
static int jd_resolve_coord_addr(struct jadard_tp *ts)
{
	u32 esram_num_addr;
	u32 esram_sec_addr;
	u8 buf[JD_ESRAM_SECTION_NUM * 8];
	u32 addr, len;
	int ret;

	esram_num_addr = JD_ERAM_BASE + 4 + JD_MAX_DSRAM_NUM * 8;
	esram_sec_addr = esram_num_addr + 4;

	ret = jd_read_reg(ts->client, esram_sec_addr, buf, sizeof(buf));
	if (ret)
		return ret;

	addr = buf[JD_ESRAM_COORD_IDX * 8 + 0] |
	       (buf[JD_ESRAM_COORD_IDX * 8 + 1] << 8) |
	       (buf[JD_ESRAM_COORD_IDX * 8 + 2] << 16) |
	       (buf[JD_ESRAM_COORD_IDX * 8 + 3] << 24);
	len = buf[JD_ESRAM_COORD_IDX * 8 + 4] |
	      (buf[JD_ESRAM_COORD_IDX * 8 + 5] << 8) |
	      (buf[JD_ESRAM_COORD_IDX * 8 + 6] << 16) |
	      (buf[JD_ESRAM_COORD_IDX * 8 + 7] << 24);

	if (addr < JD_ERAM_BASE || addr >= JD_ERAM_BASE + 0x1000 ||
	    len == 0 || len > JD_TOUCH_DATA_SIZE)
		return -EINVAL;

	ts->coord_addr = addr;
	ts->touch_data_size = (u16)len;
	return 0;
}

static int jd_hw_init(struct jadard_tp *ts)
{
	struct device *dev = &ts->client->dev;
	u16 chip_id = 0;
	int ret, attempt;

	for (attempt = 0; attempt < 3; attempt++) {
		ret = jd_enter_backdoor(ts->client);
		if (ret) {
			dev_dbg(dev, "EnterBackDoor fail (%d), retry\n", ret);
			msleep(200);
			continue;
		}

		ret = jd_get_chip_id(ts->client, &chip_id);
		if (ret) {
			msleep(200);
			continue;
		}

		if (chip_id == JD_CHIP_ID || chip_id == JD_CHIP_ID_ALT)
			break;

		dev_info(dev, "chip id 0x%04x (expected 0x%04x/0x%04x)\n",
			 chip_id, JD_CHIP_ID, JD_CHIP_ID_ALT);
		/* unknown non-zero id still usable */
		if (chip_id != 0 && chip_id != 0xFFFF)
			break;

		msleep(200);
	}

	if (ret || chip_id == 0 || chip_id == 0xFFFF)
		return ret ? ret : -ENODEV;

	dev_info(dev, "JD9365TX TP chip id 0x%04x\n", chip_id);

	/*
	 * Do NOT soft-reset: incell shares JD9365TX with LCD.
	 * Soft reset belongs to FW upgrade / recovery only.
	 */
	jd_enter_backdoor(ts->client);

	if (jd_section_ready(ts->client) && !jd_resolve_coord_addr(ts)) {
		dev_info(dev, "coord report @ 0x%08x len %u\n",
			 ts->coord_addr, ts->touch_data_size);
	} else {
		if (!ts->coord_addr)
			ts->coord_addr = JD_DEFAULT_COORD_ADDR;
		if (!ts->touch_data_size)
			ts->touch_data_size = JD_TOUCH_DATA_SIZE;
		dev_info(dev, "using coord fallback @ 0x%08x len %u\n",
			 ts->coord_addr, ts->touch_data_size);
	}

	return 0;
}

static void jd_report_touches(struct jadard_tp *ts, const u8 *buf)
{
	u8 finger_num = buf[JD_FINGER_NUM_ADDR];
	unsigned int i;
	bool any = false;

	if (finger_num > ts->max_points)
		finger_num = ts->max_points;

	for (i = 0; i < ts->max_points; i++) {
		const u8 *c = buf + JD_TOUCH_COORD_INFO_ADDR +
			      i * JD_FINGER_DATA_SIZE;
		u16 x = (c[0] << 8) | c[1];
		u16 y = (c[2] << 8) | c[3];
		u8 w = c[4];
		bool down = false;

		if (i < finger_num &&
		    x <= ts->abs_x_max && y <= ts->abs_y_max &&
		    x != 0xFFFF && y != 0xFFFF)
			down = true;

		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, down);

		if (down) {
			touchscreen_report_pos(ts->input, &ts->prop, x, y, true);
			input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, w);
			ts->prev_x[i] = x;
			ts->prev_y[i] = y;
			any = true;
		} else {
			ts->prev_x[i] = 0xFFFF;
			ts->prev_y[i] = 0xFFFF;
		}
	}

	input_mt_sync_frame(ts->input);
	input_report_key(ts->input, BTN_TOUCH, any);
	input_sync(ts->input);
}

static void jd_poll_work(struct work_struct *work)
{
	struct jadard_tp *ts = container_of(to_delayed_work(work),
					    struct jadard_tp, poll_work);
	u8 buf[JD_TOUCH_DATA_SIZE];
	int ret;

	if (ts->suspended)
		return;

	mutex_lock(&ts->lock);

	if (!ts->ready) {
		ret = jd_hw_init(ts);
		if (ret) {
			mutex_unlock(&ts->lock);
			schedule_delayed_work(&ts->poll_work,
					      msecs_to_jiffies(JD_PROBE_RETRY_MS));
			return;
		}
		ts->ready = true;
		dev_info(&ts->client->dev,
			 "TP ready — will flush queued I2C backlight after polls\n");
	}

	ret = jd_read_reg(ts->client, ts->coord_addr, buf, ts->touch_data_size);
	/* touch report first, then flush backlight write */
	if (ts->ready)
		viewe_dsi_bl_flush(ts->client->adapter);

	if (!ret)
		jd_report_touches(ts, buf);
	else
		dev_dbg_ratelimited(&ts->client->dev, "coord read fail %d\n", ret);

	mutex_unlock(&ts->lock);

	schedule_delayed_work(&ts->poll_work,
			      msecs_to_jiffies(ts->poll_interval_ms));
}

static int jadard_tp_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct jadard_tp *ts;
	struct input_dev *input;
	u32 val;
	int ret, i;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	mutex_init(&ts->lock);
	ts->max_points = JD_MAX_POINTS;
	ts->abs_x_max = 719;
	ts->abs_y_max = 1279;
	ts->poll_interval_ms = 16;
	ts->coord_addr = 0;
	ts->touch_data_size = JD_TOUCH_DATA_SIZE;

	if (!of_property_read_u32(dev->of_node, "jadard,poll-interval-ms", &val) &&
	    val >= 8 && val <= 100)
		ts->poll_interval_ms = val;

	if (!of_property_read_u32(dev->of_node, "jadard,max-touch-number", &val) &&
	    val >= 1 && val <= JD_MAX_POINTS)
		ts->max_points = val;

	if (!of_property_read_u32(dev->of_node, "touchscreen-size-x", &val) && val)
		ts->abs_x_max = val - 1;

	if (!of_property_read_u32(dev->of_node, "touchscreen-size-y", &val) && val)
		ts->abs_y_max = val - 1;

	if (!of_property_read_u32(dev->of_node, "jadard,coord-addr", &val) && val)
		ts->coord_addr = val;

	for (i = 0; i < JD_MAX_POINTS; i++) {
		ts->prev_x[i] = 0xFFFF;
		ts->prev_y[i] = 0xFFFF;
	}

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = "Jadard JD9365TX Touchscreen";
	input->id.bustype = BUS_I2C;
	input->dev.parent = dev;

	__set_bit(EV_ABS, input->evbit);
	__set_bit(EV_KEY, input->evbit);
	__set_bit(BTN_TOUCH, input->keybit);

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, ts->abs_x_max, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, ts->abs_y_max, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	touchscreen_parse_properties(input, true, &ts->prop);

	ret = input_mt_init_slots(input, ts->max_points, INPUT_MT_DIRECT);
	if (ret)
		return ret;

	ret = input_register_device(input);
	if (ret)
		return ret;

	ts->input = input;
	i2c_set_clientdata(client, ts);

	INIT_DELAYED_WORK(&ts->poll_work, jd_poll_work);
	/* wait for panel firmware before first I2C access */
	schedule_delayed_work(&ts->poll_work,
			      msecs_to_jiffies(JD_PROBE_DELAY_MS));

	dev_info(dev, "JD9365TX TP @%ums (flushes viewe_bl I2C queue)\n",
		 ts->poll_interval_ms);
	return 0;
}

static void jadard_tp_remove(struct i2c_client *client)
{
	struct jadard_tp *ts = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&ts->poll_work);
}

static int jadard_tp_suspend(struct device *dev)
{
	struct jadard_tp *ts = i2c_get_clientdata(to_i2c_client(dev));
	int i;

	ts->suspended = true;
	cancel_delayed_work_sync(&ts->poll_work);

	for (i = 0; i < ts->max_points; i++) {
		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
		ts->prev_x[i] = 0xFFFF;
		ts->prev_y[i] = 0xFFFF;
	}
	input_mt_sync_frame(ts->input);
	input_report_key(ts->input, BTN_TOUCH, 0);
	input_sync(ts->input);

	return 0;
}

static int jadard_tp_resume(struct device *dev)
{
	struct jadard_tp *ts = i2c_get_clientdata(to_i2c_client(dev));

	ts->suspended = false;
	ts->ready = false;
	schedule_delayed_work(&ts->poll_work,
			      msecs_to_jiffies(JD_PROBE_DELAY_MS));
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(jadard_tp_pm_ops,
				jadard_tp_suspend, jadard_tp_resume);

static const struct of_device_id jadard_tp_of_match[] = {
	{ .compatible = "jadard,jd9365tx-tp" },
	{ }
};
MODULE_DEVICE_TABLE(of, jadard_tp_of_match);

static const struct i2c_device_id jadard_tp_id[] = {
	{ "jd9365tx-tp", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, jadard_tp_id);

static struct i2c_driver jadard_tp_driver = {
	.driver = {
		.name = "jadard-tp-jd9365tx",
		.of_match_table = jadard_tp_of_match,
		.pm = pm_sleep_ptr(&jadard_tp_pm_ops),
	},
	.probe = jadard_tp_probe,
	.remove = jadard_tp_remove,
	.id_table = jadard_tp_id,
};
module_i2c_driver(jadard_tp_driver);

MODULE_AUTHOR("HKC / Raspberry Pi port");
MODULE_DESCRIPTION("Jadard JD9365TX in-cell touchscreen");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: viewe_dsi_i2c_bus");
