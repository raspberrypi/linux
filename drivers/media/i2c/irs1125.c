// SPDX-License-Identifier: GPL-2.0
/*
 * A V4L2 driver for Infineon IRS1125 TOF cameras.
 * Copyright (C) 2018, pieye GmbH
 *
 * Based on V4L2 OmniVision OV5647 Image Sensor driver
 * Copyright (C) 2016 Ramiro Oliveira <roliveir@synopsys.com>
 *
 * DT / fwnode changes, and GPIO control taken from ov5640.c
 * Copyright (C) 2011-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (C) 2014-2017 Mentor Graphics Inc.
 *
 */

#include "irs1125.h"
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-image-sizes.h>
#include <media/v4l2-mediabus.h>

#define CHECK_BIT(val, pos) ((val) & BIT(pos))

#define SENSOR_NAME "irs1125"

#define RESET_ACTIVE_DELAY_MS	 20

#define IRS1125_ALTERNATE_FW "irs1125_af.bin"

#define IRS1125_REG_SAFE_RECONFIG	0xA850
#define IRS1125_REG_CSICFG		0xA882
#define IRS1125_REG_DESIGN_STEP		0xB0AD
#define IRS1125_REG_EFUSEVAL2		0xB09F
#define IRS1125_REG_EFUSEVAL3		0xB0A0
#define IRS1125_REG_EFUSEVAL4		0xB0A1
#define IRS1125_REG_DMEM_SHADOW		0xC320

#define IRS1125_DESIGN_STEP_EXPECTED	0x0a12

#define IRS1125_ROW_START_DEF		0
#define IRS1125_COLUMN_START_DEF	0
#define IRS1125_WINDOW_HEIGHT_DEF	288
#define IRS1125_WINDOW_WIDTH_DEF	352

struct regval_list {
	u16 addr;
	u16 data;
};

struct irs1125 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	/* the parsed DT endpoint info */
	struct v4l2_fwnode_endpoint ep;

	struct clk *xclk;
	struct v4l2_ctrl_handler ctrl_handler;

	/* To serialize asynchronus callbacks */
	struct mutex lock;

	/* image data layout */
	unsigned int num_seq;

	/* reset pin */
	struct gpio_desc *reset;

	/* V4l2 Controls to grab */
	struct v4l2_ctrl *ctrl_modplls;
	struct v4l2_ctrl *ctrl_numseq;

	int power_count;
	bool mod_pll_init;
};

static inline struct irs1125 *to_state(struct v4l2_subdev *sd)
{
	return container_of(sd, struct irs1125, sd);
}

static const char *expo_ctrl_names[IRS1125_NUM_SEQ_ENTRIES] = {
	"safe reconfiguration of exposure of sequence 0",
	"safe reconfiguration of exposure of sequence 1",
	"safe reconfiguration of exposure of sequence 2",
	"safe reconfiguration of exposure of sequence 3",
	"safe reconfiguration of exposure of sequence 4",
	"safe reconfiguration of exposure of sequence 5",
	"safe reconfiguration of exposure of sequence 6",
	"safe reconfiguration of exposure of sequence 7",
	"safe reconfiguration of exposure of sequence 8",
	"safe reconfiguration of exposure of sequence 9",
	"safe reconfiguration of exposure of sequence 10",
	"safe reconfiguration of exposure of sequence 11",
	"safe reconfiguration of exposure of sequence 12",
	"safe reconfiguration of exposure of sequence 13",
	"safe reconfiguration of exposure of sequence 14",
	"safe reconfiguration of exposure of sequence 15",
	"safe reconfiguration of exposure of sequence 16",
	"safe reconfiguration of exposure of sequence 17",
	"safe reconfiguration of exposure of sequence 18",
	"safe reconfiguration of exposure of sequence 19",
};

static const char *frame_ctrl_names[IRS1125_NUM_SEQ_ENTRIES] = {
	"safe reconfiguration of framerate of sequence 0",
	"safe reconfiguration of framerate of sequence 1",
	"safe reconfiguration of framerate of sequence 2",
	"safe reconfiguration of framerate of sequence 3",
	"safe reconfiguration of framerate of sequence 4",
	"safe reconfiguration of framerate of sequence 5",
	"safe reconfiguration of framerate of sequence 6",
	"safe reconfiguration of framerate of sequence 7",
	"safe reconfiguration of framerate of sequence 8",
	"safe reconfiguration of framerate of sequence 9",
	"safe reconfiguration of framerate of sequence 10",
	"safe reconfiguration of framerate of sequence 11",
	"safe reconfiguration of framerate of sequence 12",
	"safe reconfiguration of framerate of sequence 13",
	"safe reconfiguration of framerate of sequence 14",
	"safe reconfiguration of framerate of sequence 15",
	"safe reconfiguration of framerate of sequence 16",
	"safe reconfiguration of framerate of sequence 17",
	"safe reconfiguration of framerate of sequence 18",
	"safe reconfiguration of framerate of sequence 19",
};

static struct regval_list irs1125_26mhz[] = {
	{0xB017, 0x0413},
	{0xB086, 0x3535},
	{0xB0AE, 0xEF02},
	{0xA000, 0x0004},
	{0xFFFF, 100},

	{0xB062, 0x6383},
	{0xB063, 0x55A8},
	{0xB068, 0x7628},
	{0xB069, 0x03E2},

	{0xFFFF, 100},
	{0xB05A, 0x01C5},
	{0xB05C, 0x0206},
	{0xB05D, 0x01C5},
	{0xB05F, 0x0206},
	{0xB016, 0x1335},
	{0xFFFF, 100},
	{0xA893, 0x8261},
	{0xA894, 0x89d8},
	{0xA895, 0x131d},
	{0xA896, 0x4251},
	{0xA897, 0x9D8A},
	{0xA898, 0x0BD8},
	{0xA899, 0x2245},
	{0xA89A, 0xAB9B},
	{0xA89B, 0x03B9},
	{0xA89C, 0x8041},
	{0xA89D, 0xE07E},
	{0xA89E, 0x0307},
	{0xFFFF, 100},
	{0xA88D, 0x0004},
	{0xA800, 0x0E68},
	{0xA801, 0x0000},
	{0xA802, 0x000C},
	{0xA803, 0x0000},
	{0xA804, 0x0E68},
	{0xA805, 0x0000},
	{0xA806, 0x0440},
	{0xA807, 0x0000},
	{0xA808, 0x0E68},
	{0xA809, 0x0000},
	{0xA80A, 0x0884},
	{0xA80B, 0x0000},
	{0xA80C, 0x0E68},
	{0xA80D, 0x0000},
	{0xA80E, 0x0CC8},
	{0xA80F, 0x0000},
	{0xA810, 0x0E68},
	{0xA811, 0x0000},
	{0xA812, 0x2000},
	{0xA813, 0x0000},
	{0xA882, 0x0081},
	{0xA88C, 0x403A},
	{0xA88F, 0x031E},
	{0xA892, 0x0351},
	{0x9813, 0x13FF},
	{0x981B, 0x7608},

	{0xB008, 0x0000},
	{0xB015, 0x1513},

	{0xFFFF, 100}
};

static struct regval_list irs1125_seq_cfg_init[] = {
	{0xC3A0, 0x823D},
	{0xC3A1, 0xB13B},
	{0xC3A2, 0x0313},
	{0xC3A3, 0x4659},
	{0xC3A4, 0xC4EC},
	{0xC3A5, 0x03CE},
	{0xC3A6, 0x4259},
	{0xC3A7, 0xC4EC},
	{0xC3A8, 0x03CE},
	{0xC3A9, 0x8839},
	{0xC3AA, 0x89D8},
	{0xC3AB, 0x031D},

	{0xC24C, 0x5529},
	{0xC24D, 0x0000},
	{0xC24E, 0x1200},
	{0xC24F, 0x6CB2},
	{0xC250, 0x0000},
	{0xC251, 0x5529},
	{0xC252, 0x42F4},
	{0xC253, 0xD1AF},
	{0xC254, 0x8A18},
	{0xC255, 0x0002},
	{0xC256, 0x5529},
	{0xC257, 0x6276},
	{0xC258, 0x11A7},
	{0xC259, 0xD907},
	{0xC25A, 0x0000},
	{0xC25B, 0x5529},
	{0xC25C, 0x07E0},
	{0xC25D, 0x7BFE},
	{0xC25E, 0x6402},
	{0xC25F, 0x0019},

	{0xC3AC, 0x0007},
	{0xC3AD, 0xED88},
	{0xC320, 0x003E},
	{0xC321, 0x0000},
	{0xC322, 0x2000},
	{0xC323, 0x0000},
	{0xC324, 0x0271},
	{0xC325, 0x0000},
	{0xC326, 0x000C},
	{0xC327, 0x0000},
	{0xC328, 0x0271},
	{0xC329, 0x0000},
	{0xC32A, 0x0440},
	{0xC32B, 0x0000},
	{0xC32C, 0x0271},
	{0xC32D, 0x0000},
	{0xC32E, 0x0884},
	{0xC32F, 0x0000},
	{0xC330, 0x0271},
	{0xC331, 0x0000},
	{0xC332, 0x0CC8},
	{0xC333, 0x0000},
	{0xA88D, 0x0004},

	{0xA890, 0x0000},
	{0xC219, 0x0002},
	{0xC21A, 0x0000},
	{0xC21B, 0x0000},
	{0xC21C, 0x00CD},
	{0xC21D, 0x0009},
	{0xC21E, 0x00CD},
	{0xC21F, 0x0009},

	{0xA87C, 0x0000},
	{0xC032, 0x0001},
	{0xC034, 0x0000},
	{0xC035, 0x0001},
	{0xC039, 0x0000},
	{0xC401, 0x0002},

	{0xFFFF, 1}
};

static int irs1125_write(struct v4l2_subdev *sd, u16 reg, u16 val)
{
	int ret;
	unsigned char data[4] = { reg >> 8, reg & 0xff, val >> 8, val & 0xff};
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	ret = i2c_master_send(client, data, 4);
	if (ret < 0)
		dev_err(&client->dev, "%s: i2c write error, reg: %x\n",
			__func__, reg);

	dev_dbg(&client->dev, "write addr 0x%04x, val 0x%04x\n", reg, val);
	return ret;
}

static int irs1125_read(struct v4l2_subdev *sd, u16 reg, u16 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct i2c_msg msgs[2];
	u8 addr_buf[2] = { reg >> 8, reg & 0xff };
	u8 data_buf[2] = { 0, };
	int ret;

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 2;
	msgs[1].buf = data_buf;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs)) {
		if (ret >= 0)
			ret = -EIO;
		return ret;
	}

	*val = data_buf[1] | (data_buf[0] << 8);

	return 0;
}

static int irs1125_write_array(struct v4l2_subdev *sd,
			       struct regval_list *regs, int array_size)
{
	int i, ret;

	for (i = 0; i < array_size; i++) {
		if (regs[i].addr == 0xFFFF) {
			msleep(regs[i].data);
		} else {
			ret = irs1125_write(sd, regs[i].addr, regs[i].data);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

static int irs1125_stream_on(struct v4l2_subdev *sd)
{
	int ret;
	struct irs1125 *irs1125 = to_state(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	v4l2_ctrl_grab(irs1125->ctrl_numseq, 1);
	v4l2_ctrl_grab(irs1125->ctrl_modplls, 1);

	ret = irs1125_write(sd, 0xC400, 0x0001);
	if (ret < 0) {
		dev_err(&client->dev, "error enabling firmware: %d", ret);
		return ret;
	}

	msleep(100);

	return irs1125_write(sd, 0xA87C, 0x0001);
}

static int irs1125_stream_off(struct v4l2_subdev *sd)
{
	int ret;
	struct irs1125 *irs1125 = to_state(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	v4l2_ctrl_grab(irs1125->ctrl_numseq, 0);
	v4l2_ctrl_grab(irs1125->ctrl_modplls, 0);

	ret = irs1125_write(sd, 0xA87C, 0x0000);
	if (ret < 0) {
		dev_err(&client->dev, "error disabling trigger: %d", ret);
		return ret;
	}

	msleep(100);

	return irs1125_write(sd, 0xC400, 0x0002);
}

static int __sensor_init(struct v4l2_subdev *sd)
{
	unsigned int cnt, idx;
	int ret;
	u16 val;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct irs1125 *irs1125 = to_state(sd);
	const struct firmware *fw;
	struct regval_list *reg_data;

	cnt = 0;
	while (1) {
		ret = irs1125_read(sd, 0xC40F, &val);
		if (ret < 0) {
			dev_err(&client->dev, "read register 0xC40F failed\n");
			return ret;
		}
		if (CHECK_BIT(val, 14) == 0)
			break;

		if (cnt >= 5) {
			dev_err(&client->dev, "timeout waiting for 0xC40F\n");
			return -EAGAIN;
		}

		cnt++;
	}

	ret = irs1125_write_array(sd, irs1125_26mhz,
				  ARRAY_SIZE(irs1125_26mhz));
	if (ret < 0) {
		dev_err(&client->dev, "write sensor default regs error\n");
		return ret;
	}

	/* set CSI-2 number of data lanes */
	if (irs1125->ep.bus.mipi_csi2.num_data_lanes == 1) {
		val = 0x0001;
	} else if (irs1125->ep.bus.mipi_csi2.num_data_lanes == 2) {
		val = 0x0081;
	} else {
		dev_err(&client->dev, "invalid number of data lanes %d\n",
			irs1125->ep.bus.mipi_csi2.num_data_lanes);
		return -EINVAL;
	}

	ret = irs1125_write(sd, IRS1125_REG_CSICFG, val);
	if (ret < 0) {
		dev_err(&client->dev, "write sensor csi2 config error\n");
		return ret;
	}

	/* request the firmware, this will block and timeout */
	ret = request_firmware(&fw, IRS1125_ALTERNATE_FW, &client->dev);
	if (ret) {
		dev_err(&client->dev,
			"did not find the firmware file '%s' (status %d)\n",
			IRS1125_ALTERNATE_FW, ret);
		return ret;
	}

	if (fw->size % 4) {
		dev_err(&client->dev, "firmware file '%s' invalid\n",
			IRS1125_ALTERNATE_FW);
		release_firmware(fw);
		return -EINVAL;
	}

	for (idx = 0; idx < fw->size; idx += 4)	{
		reg_data = (struct regval_list *)&fw->data[idx];
		ret = irs1125_write(sd, reg_data->addr, reg_data->data);
		if (ret < 0) {
			dev_err(&client->dev, "firmware write error\n");
			release_firmware(fw);
			return ret;
		}
	}
	release_firmware(fw);

	ret = irs1125_write_array(sd, irs1125_seq_cfg_init,
				  ARRAY_SIZE(irs1125_seq_cfg_init));
	if (ret < 0) {
		dev_err(&client->dev, "write default sequence failed\n");
		return ret;
	}

	irs1125->mod_pll_init = true;
	v4l2_ctrl_handler_setup(&irs1125->ctrl_handler);
	irs1125->mod_pll_init = false;

	return irs1125_write(sd, 0xA87C, 0x0001);
}

static int irs1125_sensor_power(struct v4l2_subdev *sd, int on)
{
	int ret = 0;
	struct irs1125 *irs1125 = to_state(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	mutex_lock(&irs1125->lock);

	if (on && !irs1125->power_count) {
		gpiod_set_value_cansleep(irs1125->reset, 1);
		msleep(RESET_ACTIVE_DELAY_MS);

		ret = clk_prepare_enable(irs1125->xclk);
		if (ret < 0) {
			dev_err(&client->dev, "clk prepare enable failed\n");
			goto out;
		}

		ret = __sensor_init(sd);
		if (ret < 0) {
			clk_disable_unprepare(irs1125->xclk);
			dev_err(&client->dev,
				"Camera not available, check Power\n");
			goto out;
		}
	} else if (!on && irs1125->power_count == 1) {
		gpiod_set_value_cansleep(irs1125->reset, 0);
	}

	/* Update the power count. */
	irs1125->power_count += on ? 1 : -1;
	WARN_ON(irs1125->power_count < 0);

out:
	mutex_unlock(&irs1125->lock);

	return ret;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int irs1125_sensor_get_register(struct v4l2_subdev *sd,
				       struct v4l2_dbg_register *reg)
{
	u16 val;
	int ret;

	ret = irs1125_read(sd, reg->reg & 0xffff, &val);
	if (ret < 0)
		return ret;

	reg->val = val;
	reg->size = 1;

	return 0;
}

static int irs1125_sensor_set_register(struct v4l2_subdev *sd,
				       const struct v4l2_dbg_register *reg)
{
	return irs1125_write(sd, reg->reg & 0xffff, reg->val & 0xffff);
}
#endif

static const struct v4l2_subdev_core_ops irs1125_subdev_core_ops = {
	.s_power = irs1125_sensor_power,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register = irs1125_sensor_get_register,
	.s_register = irs1125_sensor_set_register,
#endif
};

static int irs1125_s_stream(struct v4l2_subdev *sd, int enable)
{
	if (enable)
		return irs1125_stream_on(sd);
	else
		return irs1125_stream_off(sd);
}

static const struct v4l2_subdev_video_ops irs1125_subdev_video_ops = {
	.s_stream = irs1125_s_stream,
};

static int irs1125_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
	struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_Y12_1X12;

	return 0;
}

static int irs1125_set_get_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_pad_config *cfg,
			       struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct irs1125 *irs1125 = to_state(sd);

	if (format->pad != 0)
		return -EINVAL;

	/* Only one format is supported, so return that */
	memset(fmt, 0, sizeof(*fmt));
	fmt->code = MEDIA_BUS_FMT_Y12_1X12;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->field = V4L2_FIELD_NONE;
	fmt->width = IRS1125_WINDOW_WIDTH_DEF;
	fmt->height = IRS1125_WINDOW_HEIGHT_DEF * irs1125->num_seq;

	return 0;
}

static const struct v4l2_subdev_pad_ops irs1125_subdev_pad_ops = {
	.enum_mbus_code = irs1125_enum_mbus_code,
	.set_fmt = irs1125_set_get_fmt,
	.get_fmt = irs1125_set_get_fmt,
};

static const struct v4l2_subdev_ops irs1125_subdev_ops = {
	.core = &irs1125_subdev_core_ops,
	.video = &irs1125_subdev_video_ops,
	.pad = &irs1125_subdev_pad_ops,
};

static int irs1125_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct irs1125 *dev = container_of(ctrl->handler,
					struct irs1125, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&dev->sd);
	int err = 0, i;

	switch (ctrl->id) {
	case IRS1125_CID_SAFE_RECONFIG_S0_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S0_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S1_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S1_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S2_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S2_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S3_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S3_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S4_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S4_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S5_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S5_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S6_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S6_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S7_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S7_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S8_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S8_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S9_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S9_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S10_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S10_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S11_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S11_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S12_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S12_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S13_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S13_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S14_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S14_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S15_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S15_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S16_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S16_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S17_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S17_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S18_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S18_FRAME:
	case IRS1125_CID_SAFE_RECONFIG_S19_EXPO:
	case IRS1125_CID_SAFE_RECONFIG_S19_FRAME: {
		unsigned int offset = ctrl->id -
			IRS1125_CID_SAFE_RECONFIG_S0_EXPO;

		err = irs1125_write(&dev->sd,
				    IRS1125_REG_SAFE_RECONFIG + offset,
				    ctrl->val);
		break;
	}
	case IRS1125_CID_MOD_PLL: {
		struct irs1125_mod_pll *mod_new;

		if (dev->mod_pll_init)
			break;

		mod_new = (struct irs1125_mod_pll *)ctrl->p_new.p;
		for (i = 0; i < IRS1125_NUM_MOD_PLLS; i++) {
			unsigned int pll_offset, ssc_offset;

			pll_offset = i * 3;
			ssc_offset = i * 5;

			err = irs1125_write(&dev->sd, 0xC3A0 + pll_offset,
					    mod_new[i].pllcfg1);
			if (err < 0)
				break;

			err = irs1125_write(&dev->sd, 0xC3A1 + pll_offset,
					    mod_new[i].pllcfg2);
			if (err < 0)
				break;

			err = irs1125_write(&dev->sd, 0xC3A2 + pll_offset,
					    mod_new[i].pllcfg3);
			if (err < 0)
				break;

			err = irs1125_write(&dev->sd, 0xC24C + ssc_offset,
					    mod_new[i].pllcfg4);
			if (err < 0)
				break;

			err = irs1125_write(&dev->sd, 0xC24D + ssc_offset,
					    mod_new[i].pllcfg5);
			if (err < 0)
				break;

			err = irs1125_write(&dev->sd, 0xC24E + ssc_offset,
					    mod_new[i].pllcfg6);
			if (err < 0)
				break;

			err = irs1125_write(&dev->sd, 0xC24F + ssc_offset,
					    mod_new[i].pllcfg7);
			if (err < 0)
				break;

			err = irs1125_write(&dev->sd, 0xC250 + ssc_offset,
					    mod_new[i].pllcfg8);
			if (err < 0)
				break;
		}
		break;
	}
	case IRS1125_CID_SEQ_CONFIG: {
		struct irs1125_seq_cfg *cfg_new;

		cfg_new = (struct irs1125_seq_cfg *)ctrl->p_new.p;
		for (i = 0; i < IRS1125_NUM_SEQ_ENTRIES; i++) {
			unsigned int seq_offset = i * 4;
			u16 addr, val;

			addr = IRS1125_REG_DMEM_SHADOW + seq_offset;
			val = cfg_new[i].exposure;
			err = irs1125_write(&dev->sd, addr, val);
			if (err < 0)
				break;

			addr = IRS1125_REG_DMEM_SHADOW + 1 + seq_offset;
			val = cfg_new[i].framerate;
			err = irs1125_write(&dev->sd, addr, val);
			if (err < 0)
				break;

			addr = IRS1125_REG_DMEM_SHADOW + 2 + seq_offset;
			val = cfg_new[i].ps;
			err = irs1125_write(&dev->sd, addr, val);
			if (err < 0)
				break;

			addr = IRS1125_REG_DMEM_SHADOW + 3 + seq_offset;
			val = cfg_new[i].pll;
			err = irs1125_write(&dev->sd, addr, val);
			if (err < 0)
				break;
		}
		break;
	}
	case IRS1125_CID_NUM_SEQS:
		err = irs1125_write(&dev->sd, 0xA88D, ctrl->val - 1);
		if (err >= 0)
			dev->num_seq = ctrl->val;
		break;
	case IRS1125_CID_CONTINUOUS_TRIG:
		if (ctrl->val == 0)
			err = irs1125_write(&dev->sd, 0xA87C, 0);
		else
			err = irs1125_write(&dev->sd, 0xA87C, 1);
		break;
	case IRS1125_CID_TRIGGER:
		if (ctrl->val != 0) {
			err = irs1125_write(&dev->sd, 0xA87C, 1);
			if (err >= 0)
				err = irs1125_write(&dev->sd, 0xA87C, 0);
		}
		break;
	case IRS1125_CID_RECONFIG:
		if (ctrl->val != 0)
			err = irs1125_write(&dev->sd, 0xA87A, 1);
		break;
	case IRS1125_CID_ILLU_ON:
		if (ctrl->val == 0)
			err = irs1125_write(&dev->sd, 0xA892, 0x377);
		else
			err = irs1125_write(&dev->sd, 0xA892, 0x355);
		break;
	default:
		break;
	}

	if (err < 0)
		dev_err(&client->dev, "Error executing control ID: %d, val %d, err %d",
			ctrl->id, ctrl->val, err);
	else
		err = 0;

	return err;
}

static const struct v4l2_ctrl_ops irs1125_ctrl_ops = {
	.s_ctrl = irs1125_s_ctrl,
};

static const struct v4l2_ctrl_config irs1125_custom_ctrls[] = {
	{
		.ops = &irs1125_ctrl_ops,
		.id = IRS1125_CID_NUM_SEQS,
		.name = "Change number of sequences",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_MODIFY_LAYOUT,
		.min = 1,
		.max = 20,
		.step = 1,
		.def = 5,
	}, {
		.ops = &irs1125_ctrl_ops,
		.id = IRS1125_CID_MOD_PLL,
		.name = "Reconfigure modulation PLLs",
		.type = V4L2_CTRL_TYPE_U16,
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD,
		.min = 0,
		.max = U16_MAX,
		.step = 1,
		.def = 0,
		.elem_size = sizeof(u16),
		.dims = {sizeof(struct irs1125_mod_pll) / sizeof(u16),
			IRS1125_NUM_MOD_PLLS}
	}, {
		.ops = &irs1125_ctrl_ops,
		.id = IRS1125_CID_SEQ_CONFIG,
		.name = "Change sequence settings",
		.type = V4L2_CTRL_TYPE_U16,
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD,
		.min = 0,
		.max = U16_MAX,
		.step = 1,
		.def = 0,
		.elem_size = sizeof(u16),
		.dims = {sizeof(struct irs1125_seq_cfg) / sizeof(u16),
			IRS1125_NUM_SEQ_ENTRIES}
	}, {
		.ops = &irs1125_ctrl_ops,
		.id = IRS1125_CID_CONTINUOUS_TRIG,
		.name = "Enable/disable continuous trigger",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
		.min = 0,
		.max = 1,
		.step = 1,
		.def = 0
	}, {
		.ops = &irs1125_ctrl_ops,
		.id = IRS1125_CID_TRIGGER,
		.name = "Capture a single sequence",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
		.min = 0,
		.max = 1,
		.step = 1,
		.def = 0
	}, {
		.ops = &irs1125_ctrl_ops,
		.id = IRS1125_CID_RECONFIG,
		.name = "Trigger imager reconfiguration",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
		.min = 0,
		.max = 1,
		.step = 1,
		.def = 0
	}, {
		.ops = &irs1125_ctrl_ops,
		.id = IRS1125_CID_ILLU_ON,
		.name = "Turn illu on or off",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
		.min = 0,
		.max = 1,
		.step = 1,
		.def = 1
	}, {
		.ops = &irs1125_ctrl_ops,
		.id = IRS1125_CID_IDENT0,
		.name = "Get ident 0 information",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
		.min = S32_MIN,
		.max = S32_MAX,
		.step = 1,
		.def = 0
	}, {
		.ops = &irs1125_ctrl_ops,
		.id = IRS1125_CID_IDENT1,
		.name = "Get ident 1 information",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
		.min = S32_MIN,
		.max = S32_MAX,
		.step = 1,
		.def = 0
	}, {
		.ops = &irs1125_ctrl_ops,
		.id = IRS1125_CID_IDENT2,
		.name = "Get ident 2 information",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
		.min = S32_MIN,
		.max = S32_MAX,
		.step = 1,
		.def = 0
	}
};

static int irs1125_detect(struct v4l2_subdev *sd)
{
	u16 read;
	int ret;
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	ret = irs1125_read(sd, IRS1125_REG_DESIGN_STEP, &read);
	if (ret < 0) {
		dev_err(&client->dev, "error reading from i2c\n");
		return ret;
	}

	if (read != IRS1125_DESIGN_STEP_EXPECTED) {
		dev_err(&client->dev, "Design step expected 0x%x got 0x%x",
			IRS1125_DESIGN_STEP_EXPECTED, read);
		return -ENODEV;
	}

	return 0;
}

static int irs1125_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct v4l2_mbus_framefmt *format =
	v4l2_subdev_get_try_format(sd, fh->pad, 0);

	format->code = MEDIA_BUS_FMT_Y12_1X12;
	format->width = IRS1125_WINDOW_WIDTH_DEF;
	format->height = IRS1125_WINDOW_HEIGHT_DEF;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;

	return 0;
}

static const struct v4l2_subdev_internal_ops irs1125_subdev_internal_ops = {
	.open = irs1125_open,
};

static int irs1125_ctrls_init(struct irs1125 *sensor, struct device *dev)
{
	struct v4l2_ctrl *ctrl;
	int err, i;
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	struct v4l2_ctrl_config ctrl_cfg = {
		.ops = &irs1125_ctrl_ops,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0,
		.max = U16_MAX,
		.step = 1,
		.def = 0x1000
	};

	v4l2_ctrl_handler_init(hdl, ARRAY_SIZE(irs1125_custom_ctrls));

	for (i = 0; i < ARRAY_SIZE(irs1125_custom_ctrls); i++)	{
		ctrl = v4l2_ctrl_new_custom(hdl, &irs1125_custom_ctrls[i],
					    NULL);
		if (!ctrl)
			dev_err(dev, "Failed to init custom control %s\n",
				irs1125_custom_ctrls[i].name);
		else if (irs1125_custom_ctrls[i].id == IRS1125_CID_NUM_SEQS)
			sensor->ctrl_numseq = ctrl;
		else if (irs1125_custom_ctrls[i].id == IRS1125_CID_MOD_PLL)
			sensor->ctrl_modplls = ctrl;
	}

	if (hdl->error)	{
		dev_err(dev, "Error %d adding controls\n", hdl->error);
		err = hdl->error;
		goto error_ctrls;
	}

	for (i = 0; i < IRS1125_NUM_SEQ_ENTRIES; i++) {
		ctrl_cfg.name = expo_ctrl_names[i];
		ctrl_cfg.id = IRS1125_CID_SAFE_RECONFIG_S0_EXPO + i * 2;
		ctrl = v4l2_ctrl_new_custom(hdl, &ctrl_cfg,
					    NULL);
		if (!ctrl)
			dev_err(dev, "Failed to init exposure control %s\n",
				ctrl_cfg.name);
	}

	ctrl_cfg.def = 0;
	for (i = 0; i < IRS1125_NUM_SEQ_ENTRIES; i++) {
		ctrl_cfg.name = frame_ctrl_names[i];
		ctrl_cfg.id = IRS1125_CID_SAFE_RECONFIG_S0_FRAME + i * 2;
		ctrl = v4l2_ctrl_new_custom(hdl, &ctrl_cfg,
					    NULL);
		if (!ctrl)
			dev_err(dev, "Failed to init framerate control %s\n",
				ctrl_cfg.name);
	}

	sensor->sd.ctrl_handler = hdl;
	return 0;

error_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	return -err;
}

static int irs1125_ident_setup(struct irs1125 *sensor, struct device *dev)
{
	int ret;
	struct v4l2_ctrl *ctrl;
	struct v4l2_subdev *sd;
	u16 read;

	sd = &sensor->sd;

	ctrl = v4l2_ctrl_find(&sensor->ctrl_handler, IRS1125_CID_IDENT0);
	if (!ctrl) {
		dev_err(dev, "could not find device ctrl.\n");
		return -EINVAL;
	}

	ret = irs1125_read(sd, IRS1125_REG_EFUSEVAL2, &read);
	if (ret < 0) {
		dev_err(dev, "error reading from i2c\n");
		return -EIO;
	}

	v4l2_ctrl_s_ctrl(ctrl, read);

	ctrl = v4l2_ctrl_find(&sensor->ctrl_handler, IRS1125_CID_IDENT1);
	if (!ctrl) {
		dev_err(dev, "could not find device ctrl.\n");
		return -EINVAL;
	}

	ret = irs1125_read(sd, IRS1125_REG_EFUSEVAL3, &read);
	if (ret < 0) {
		dev_err(dev, "error reading from i2c\n");
		return -EIO;
	}

	v4l2_ctrl_s_ctrl(ctrl, read);

	ctrl = v4l2_ctrl_find(&sensor->ctrl_handler, IRS1125_CID_IDENT2);
	if (!ctrl) {
		dev_err(dev, "could not find device ctrl.\n");
		return -EINVAL;
	}

	ret = irs1125_read(sd, IRS1125_REG_EFUSEVAL4, &read);
	if (ret < 0) {
		dev_err(dev, "error reading from i2c\n");
		return -EIO;
	}
	v4l2_ctrl_s_ctrl(ctrl, read & 0xFFFC);

	return 0;
}

static int irs1125_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct irs1125 *sensor;
	int ret;
	struct fwnode_handle *endpoint;
	u32 xclk_freq;
	int gpio_num;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&sensor->sd, client, &irs1125_subdev_ops);

	/* Get CSI2 bus config */
	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev),
						  NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(endpoint, &sensor->ep);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(dev, "Could not parse endpoint\n");
		return ret;
	}

	/* get system clock (xclk) */
	sensor->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk)) {
		dev_err(dev, "could not get xclk");
		return PTR_ERR(sensor->xclk);
	}

	xclk_freq = clk_get_rate(sensor->xclk);
	if (xclk_freq != 26000000) {
		dev_err(dev, "Unsupported clock frequency: %u\n", xclk_freq);
		return -EINVAL;
	}

	sensor->num_seq = 5;

	/* Request the power down GPIO */
	sensor->reset = devm_gpiod_get(&client->dev, "pwdn",
				       GPIOD_OUT_LOW);

	if (IS_ERR(sensor->reset)) {
		dev_err(dev, "could not get reset");
		return PTR_ERR(sensor->reset);
	}

	gpio_num = desc_to_gpio(sensor->reset);
	dev_dbg(&client->dev, "reset on GPIO num %d\n", gpio_num);

	mutex_init(&sensor->lock);

	ret = irs1125_ctrls_init(sensor, dev);
	if (ret < 0)
		goto mutex_remove;

	sensor->sd.internal_ops = &irs1125_subdev_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret < 0)
		goto mutex_remove;

	gpiod_set_value_cansleep(sensor->reset, 1);
	msleep(RESET_ACTIVE_DELAY_MS);

	ret = irs1125_detect(&sensor->sd);
	if (ret < 0)
		goto error;

	ret = irs1125_ident_setup(sensor, dev);
	if (ret < 0)
		goto error;

	gpiod_set_value_cansleep(sensor->reset, 0);

	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret < 0)
		goto error;

	dev_dbg(dev, "Infineon IRS1125 camera driver probed\n");

	return 0;

error:
	media_entity_cleanup(&sensor->sd.entity);
mutex_remove:
	mutex_destroy(&sensor->lock);
	return ret;
}

static int irs1125_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct irs1125 *irs1125 = to_state(sd);

	v4l2_async_unregister_subdev(&irs1125->sd);
	media_entity_cleanup(&irs1125->sd.entity);
	v4l2_device_unregister_subdev(sd);
	mutex_destroy(&irs1125->lock);
	v4l2_ctrl_handler_free(&irs1125->ctrl_handler);

	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id irs1125_of_match[] = {
	{ .compatible = "infineon,irs1125" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, irs1125_of_match);
#endif

static struct i2c_driver irs1125_driver = {
	.driver = {
		.of_match_table = of_match_ptr(irs1125_of_match),
		.name	 = SENSOR_NAME,
	},
	.probe		= irs1125_probe,
	.remove		= irs1125_remove,
};

module_i2c_driver(irs1125_driver);

MODULE_AUTHOR("Markus Proeller <markus.proeller@pieye.org>");
MODULE_DESCRIPTION("Infineon irs1125 sensor driver");
MODULE_LICENSE("GPL v2");

