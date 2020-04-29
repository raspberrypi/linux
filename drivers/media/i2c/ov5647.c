/*
 * A V4L2 driver for OmniVision OV5647 cameras.
 *
 * Based on Samsung S5K6AAFX SXGA 1/6" 1.3M CMOS Image Sensor driver
 * Copyright (C) 2011 Sylwester Nawrocki <s.nawrocki@samsung.com>
 *
 * Based on Omnivision OV7670 Camera Driver
 * Copyright (C) 2006-7 Jonathan Corbet <corbet@lwn.net>
 *
 * Copyright (C) 2016, Synopsys, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed .as is. WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-image-sizes.h>
#include <media/v4l2-mediabus.h>


#define SENSOR_NAME "ov5647"

/*
 * From the datasheet, "20ms after PWDN goes low or 20ms after RESETB goes
 * high if reset is inserted after PWDN goes high, host can access sensor's
 * SCCB to initialize sensor."
 */
#define PWDN_ACTIVE_DELAY_MS	20

#define MIPI_CTRL00_CLOCK_LANE_GATE		BIT(5)
#define MIPI_CTRL00_LINE_SYNC_ENABLE		BIT(4)
#define MIPI_CTRL00_BUS_IDLE			BIT(2)
#define MIPI_CTRL00_CLOCK_LANE_DISABLE		BIT(0)

#define OV5647_SW_STANDBY		0x0100
#define OV5647_SW_RESET			0x0103
#define OV5647_REG_CHIPID_H		0x300A
#define OV5647_REG_CHIPID_L		0x300B
#define OV5640_REG_PAD_OUT		0x300D
#define OV5647_REG_EXP_HI		0x3500
#define OV5647_REG_EXP_MID		0x3501
#define OV5647_REG_EXP_LO		0x3502
#define OV5647_REG_AEC_AGC		0x3503
#define OV5647_REG_GAIN_HI		0x350A
#define OV5647_REG_GAIN_LO		0x350B
#define OV5647_REG_FRAME_OFF_NUMBER	0x4202
#define OV5647_REG_MIPI_CTRL00		0x4800
#define OV5647_REG_MIPI_CTRL14		0x4814
#define OV5647_REG_AWB			0x5001

#define REG_TERM 0xfffe
#define VAL_TERM 0xfe
#define REG_DLY  0xffff

/* OV5647 native and active pixel array size */
#define OV5647_NATIVE_WIDTH		2624U
#define OV5647_NATIVE_HEIGHT		1956U

#define OV5647_PIXEL_ARRAY_LEFT		16U
#define OV5647_PIXEL_ARRAY_TOP		16U
#define OV5647_PIXEL_ARRAY_WIDTH	2592U
#define OV5647_PIXEL_ARRAY_HEIGHT	1944U

struct regval_list {
	u16 addr;
	u8 data;
};

struct ov5647_mode {
	struct v4l2_mbus_framefmt	format;
	/* Analog crop rectangle. */
	struct v4l2_rect crop;

	u64 pixel_rate;

	struct regval_list		*reg_list;
	unsigned int			num_regs;
};

struct ov5647 {
	struct v4l2_subdev		sd;
	struct media_pad		pad;
	struct mutex			lock;
	const struct ov5647_mode	*mode;
	int				power_count;
	struct clk			*xclk;
	struct gpio_desc		*pwdn;
	unsigned int			flags;
	struct v4l2_ctrl_handler	ctrls;
	struct v4l2_ctrl		*pixel_rate;
	bool				write_mode_regs;
};

static inline struct ov5647 *to_state(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov5647, sd);
}

static struct regval_list sensor_oe_disable_regs[] = {
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
};

static struct regval_list sensor_oe_enable_regs[] = {
	{0x3000, 0x0f},
	{0x3001, 0xff},
	{0x3002, 0xe4},
};

static struct regval_list ov5647_640x480_8bit[] = {
	{0x0100, 0x00},
	{0x0103, 0x01},
	{0x3034, 0x08},
	{0x3035, 0x21},
	{0x3036, 0x46},
	{0x303c, 0x11},
	{0x3106, 0xf5},
	{0x3821, 0x07},
	{0x3820, 0x41},
	{0x3827, 0xec},
	{0x370c, 0x0f},
	{0x3612, 0x59},
	{0x3618, 0x00},
	{0x5000, 0x06},
	{0x5002, 0x41},
	{0x5003, 0x08},
	{0x5a00, 0x08},
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x3016, 0x08},
	{0x3017, 0xe0},
	{0x3018, 0x44},
	{0x301c, 0xf8},
	{0x301d, 0xf0},
	{0x3a18, 0x00},
	{0x3a19, 0xf8},
	{0x3c01, 0x80},
	{0x3b07, 0x0c},
	{0x380c, 0x07},
	{0x380d, 0x68},
	{0x380e, 0x03},
	{0x380f, 0xd8},
	{0x3814, 0x31},
	{0x3815, 0x31},
	{0x3708, 0x64},
	{0x3709, 0x52},
	{0x3808, 0x02},
	{0x3809, 0x80},
	{0x380a, 0x01},
	{0x380b, 0xE0},
	{0x3801, 0x00},
	{0x3802, 0x00},
	{0x3803, 0x00},
	{0x3804, 0x0a},
	{0x3805, 0x3f},
	{0x3806, 0x07},
	{0x3807, 0xa1},
	{0x3811, 0x08},
	{0x3813, 0x02},
	{0x3630, 0x2e},
	{0x3632, 0xe2},
	{0x3633, 0x23},
	{0x3634, 0x44},
	{0x3636, 0x06},
	{0x3620, 0x64},
	{0x3621, 0xe0},
	{0x3600, 0x37},
	{0x3704, 0xa0},
	{0x3703, 0x5a},
	{0x3715, 0x78},
	{0x3717, 0x01},
	{0x3731, 0x02},
	{0x370b, 0x60},
	{0x3705, 0x1a},
	{0x3f05, 0x02},
	{0x3f06, 0x10},
	{0x3f01, 0x0a},
	{0x3a08, 0x01},
	{0x3a09, 0x27},
	{0x3a0a, 0x00},
	{0x3a0b, 0xf6},
	{0x3a0d, 0x04},
	{0x3a0e, 0x03},
	{0x3a0f, 0x58},
	{0x3a10, 0x50},
	{0x3a1b, 0x58},
	{0x3a1e, 0x50},
	{0x3a11, 0x60},
	{0x3a1f, 0x28},
	{0x4001, 0x02},
	{0x4004, 0x02},
	{0x4000, 0x09},
	{0x4837, 0x24},
	{0x4050, 0x6e},
	{0x4051, 0x8f},
	{0x0100, 0x01},
};

static struct regval_list ov5647_2592x1944_10bit[] = {
	{0x0100, 0x00},
	{0x0103, 0x01},
	{0x3034, 0x1a},
	{0x3035, 0x21},
	{0x3036, 0x69},
	{0x303c, 0x11},
	{0x3106, 0xf5},
	{0x3821, 0x06},
	{0x3820, 0x00},
	{0x3827, 0xec},
	{0x370c, 0x03},
	{0x3612, 0x5b},
	{0x3618, 0x04},
	{0x5000, 0x06},
	{0x5002, 0x41},
	{0x5003, 0x08},
	{0x5a00, 0x08},
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x3016, 0x08},
	{0x3017, 0xe0},
	{0x3018, 0x44},
	{0x301c, 0xf8},
	{0x301d, 0xf0},
	{0x3a18, 0x00},
	{0x3a19, 0xf8},
	{0x3c01, 0x80},
	{0x3b07, 0x0c},
	{0x380c, 0x0b},
	{0x380d, 0x1c},
	{0x380e, 0x07},
	{0x380f, 0xb0},
	{0x3814, 0x11},
	{0x3815, 0x11},
	{0x3708, 0x64},
	{0x3709, 0x12},
	{0x3808, 0x0a},
	{0x3809, 0x20},
	{0x380a, 0x07},
	{0x380b, 0x98},
	{0x3800, 0x00},
	{0x3801, 0x00},
	{0x3802, 0x00},
	{0x3803, 0x00},
	{0x3804, 0x0a},
	{0x3805, 0x3f},
	{0x3806, 0x07},
	{0x3807, 0xa3},
	{0x3811, 0x10},
	{0x3813, 0x06},
	{0x3630, 0x2e},
	{0x3632, 0xe2},
	{0x3633, 0x23},
	{0x3634, 0x44},
	{0x3636, 0x06},
	{0x3620, 0x64},
	{0x3621, 0xe0},
	{0x3600, 0x37},
	{0x3704, 0xa0},
	{0x3703, 0x5a},
	{0x3715, 0x78},
	{0x3717, 0x01},
	{0x3731, 0x02},
	{0x370b, 0x60},
	{0x3705, 0x1a},
	{0x3f05, 0x02},
	{0x3f06, 0x10},
	{0x3f01, 0x0a},
	{0x3a08, 0x01},
	{0x3a09, 0x28},
	{0x3a0a, 0x00},
	{0x3a0b, 0xf6},
	{0x3a0d, 0x08},
	{0x3a0e, 0x06},
	{0x3a0f, 0x58},
	{0x3a10, 0x50},
	{0x3a1b, 0x58},
	{0x3a1e, 0x50},
	{0x3a11, 0x60},
	{0x3a1f, 0x28},
	{0x4001, 0x02},
	{0x4004, 0x04},
	{0x4000, 0x09},
	{0x4837, 0x19},
	{0x4800, 0x24},
	{0x3503, 0x03},
	{0x0100, 0x01},
};

static struct regval_list ov5647_1080p30_10bit[] = {
	{0x0100, 0x00},
	{0x0103, 0x01},
	{0x3034, 0x1a},
	{0x3035, 0x21},
	{0x3036, 0x62},
	{0x303c, 0x11},
	{0x3106, 0xf5},
	{0x3821, 0x06},
	{0x3820, 0x00},
	{0x3827, 0xec},
	{0x370c, 0x03},
	{0x3612, 0x5b},
	{0x3618, 0x04},
	{0x5000, 0x06},
	{0x5002, 0x41},
	{0x5003, 0x08},
	{0x5a00, 0x08},
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x3016, 0x08},
	{0x3017, 0xe0},
	{0x3018, 0x44},
	{0x301c, 0xf8},
	{0x301d, 0xf0},
	{0x3a18, 0x00},
	{0x3a19, 0xf8},
	{0x3c01, 0x80},
	{0x3b07, 0x0c},
	{0x380c, 0x09},
	{0x380d, 0x70},
	{0x380e, 0x04},
	{0x380f, 0x50},
	{0x3814, 0x11},
	{0x3815, 0x11},
	{0x3708, 0x64},
	{0x3709, 0x12},
	{0x3808, 0x07},
	{0x3809, 0x80},
	{0x380a, 0x04},
	{0x380b, 0x38},
	{0x3800, 0x01},
	{0x3801, 0x5c},
	{0x3802, 0x01},
	{0x3803, 0xb2},
	{0x3804, 0x08},
	{0x3805, 0xe3},
	{0x3806, 0x05},
	{0x3807, 0xf1},
	{0x3811, 0x04},
	{0x3813, 0x02},
	{0x3630, 0x2e},
	{0x3632, 0xe2},
	{0x3633, 0x23},
	{0x3634, 0x44},
	{0x3636, 0x06},
	{0x3620, 0x64},
	{0x3621, 0xe0},
	{0x3600, 0x37},
	{0x3704, 0xa0},
	{0x3703, 0x5a},
	{0x3715, 0x78},
	{0x3717, 0x01},
	{0x3731, 0x02},
	{0x370b, 0x60},
	{0x3705, 0x1a},
	{0x3f05, 0x02},
	{0x3f06, 0x10},
	{0x3f01, 0x0a},
	{0x3a08, 0x01},
	{0x3a09, 0x4b},
	{0x3a0a, 0x01},
	{0x3a0b, 0x13},
	{0x3a0d, 0x04},
	{0x3a0e, 0x03},
	{0x3a0f, 0x58},
	{0x3a10, 0x50},
	{0x3a1b, 0x58},
	{0x3a1e, 0x50},
	{0x3a11, 0x60},
	{0x3a1f, 0x28},
	{0x4001, 0x02},
	{0x4004, 0x04},
	{0x4000, 0x09},
	{0x4837, 0x19},
	{0x4800, 0x34},
	{0x3503, 0x03},
	{0x0100, 0x01},
};

static struct regval_list ov5647_2x2binned_10bit[] = {
	{0x0100, 0x00},
	{0x0103, 0x01},
	{0x3034, 0x1A},
	{0x3035, 0x21},
	{0x3036, 0x62},
	{0x303C, 0x11},
	{0x3106, 0xF5},
	{0x3827, 0xEC},
	{0x370C, 0x03},
	{0x3612, 0x59},
	{0x3618, 0x00},
	{0x5000, 0x06},
	{0x5002, 0x41},
	{0x5003, 0x08},
	{0x5A00, 0x08},
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x3016, 0x08},
	{0x3017, 0xE0},
	{0x3018, 0x44},
	{0x301C, 0xF8},
	{0x301D, 0xF0},
	{0x3A18, 0x00},
	{0x3A19, 0xF8},
	{0x3C01, 0x80},
	{0x3B07, 0x0C},
	{0x3800, 0x00},
	{0x3801, 0x00},
	{0x3802, 0x00},
	{0x3803, 0x00},
	{0x3804, 0x0A},
	{0x3805, 0x3F},
	{0x3806, 0x07},
	{0x3807, 0xA3},
	{0x3808, 0x05},
	{0x3809, 0x10},
	{0x380A, 0x03},
	{0x380B, 0xCC},
	{0x380C, 0x07},
	{0x380D, 0x68},
	{0x3811, 0x0c},
	{0x3813, 0x06},
	{0x3814, 0x31},
	{0x3815, 0x31},
	{0x3630, 0x2E},
	{0x3632, 0xE2},
	{0x3633, 0x23},
	{0x3634, 0x44},
	{0x3636, 0x06},
	{0x3620, 0x64},
	{0x3621, 0xE0},
	{0x3600, 0x37},
	{0x3704, 0xA0},
	{0x3703, 0x5A},
	{0x3715, 0x78},
	{0x3717, 0x01},
	{0x3731, 0x02},
	{0x370B, 0x60},
	{0x3705, 0x1A},
	{0x3F05, 0x02},
	{0x3F06, 0x10},
	{0x3F01, 0x0A},
	{0x3A08, 0x01},
	{0x3A09, 0x28},
	{0x3A0A, 0x00},
	{0x3A0B, 0xF6},
	{0x3A0D, 0x08},
	{0x3A0E, 0x06},
	{0x3A0F, 0x58},
	{0x3A10, 0x50},
	{0x3A1B, 0x58},
	{0x3A1E, 0x50},
	{0x3A11, 0x60},
	{0x3A1F, 0x28},
	{0x4001, 0x02},
	{0x4004, 0x04},
	{0x4000, 0x09},
	{0x4837, 0x16},
	{0x4800, 0x24},
	{0x3503, 0x03},
	{0x3820, 0x41},
	{0x3821, 0x07},
	{0x380E, 0x05},
	{0x380F, 0x9B},
	{0x350A, 0x00},
	{0x350B, 0x10},
	{0x3500, 0x00},
	{0x3501, 0x1A},
	{0x3502, 0xF0},
	{0x3212, 0xA0},
	{0x0100, 0x01},
};

static struct regval_list ov5647_640x480_10bit[] = {
	{0x0100, 0x00},
	{0x0103, 0x01},
	{0x3035, 0x11},
	{0x3036, 0x46},
	{0x303c, 0x11},
	{0x3821, 0x07},
	{0x3820, 0x41},
	{0x370c, 0x03},
	{0x3612, 0x59},
	{0x3618, 0x00},
	{0x5000, 0x06},
	{0x5003, 0x08},
	{0x5a00, 0x08},
	{0x3000, 0xff},
	{0x3001, 0xff},
	{0x3002, 0xff},
	{0x301d, 0xf0},
	{0x3a18, 0x00},
	{0x3a19, 0xf8},
	{0x3c01, 0x80},
	{0x3b07, 0x0c},
	{0x380c, 0x07},
	{0x380d, 0x3c},
	{0x380e, 0x01},
	{0x380f, 0xf8},
	{0x3814, 0x35},
	{0x3815, 0x35},
	{0x3708, 0x64},
	{0x3709, 0x52},
	{0x3808, 0x02},
	{0x3809, 0x80},
	{0x380a, 0x01},
	{0x380b, 0xe0},
	{0x3800, 0x00},
	{0x3801, 0x10},
	{0x3802, 0x00},
	{0x3803, 0x00},
	{0x3804, 0x0a},
	{0x3805, 0x2f},
	{0x3806, 0x07},
	{0x3807, 0x9f},
	{0x3630, 0x2e},
	{0x3632, 0xe2},
	{0x3633, 0x23},
	{0x3634, 0x44},
	{0x3620, 0x64},
	{0x3621, 0xe0},
	{0x3600, 0x37},
	{0x3704, 0xa0},
	{0x3703, 0x5a},
	{0x3715, 0x78},
	{0x3717, 0x01},
	{0x3731, 0x02},
	{0x370b, 0x60},
	{0x3705, 0x1a},
	{0x3f05, 0x02},
	{0x3f06, 0x10},
	{0x3f01, 0x0a},
	{0x3a08, 0x01},
	{0x3a09, 0x2e},
	{0x3a0a, 0x00},
	{0x3a0b, 0xfb},
	{0x3a0d, 0x02},
	{0x3a0e, 0x01},
	{0x3a0f, 0x58},
	{0x3a10, 0x50},
	{0x3a1b, 0x58},
	{0x3a1e, 0x50},
	{0x3a11, 0x60},
	{0x3a1f, 0x28},
	{0x4001, 0x02},
	{0x4004, 0x02},
	{0x4000, 0x09},
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x3017, 0xe0},
	{0x301c, 0xfc},
	{0x3636, 0x06},
	{0x3016, 0x08},
	{0x3827, 0xec},
	{0x3018, 0x44},
	{0x3035, 0x21},
	{0x3106, 0xf5},
	{0x3034, 0x1a},
	{0x301c, 0xf8},
	{0x4800, 0x34},
	{0x3503, 0x03},
	{0x0100, 0x01},
};

static struct ov5647_mode supported_modes_8bit[] = {
	/*
	 * MODE 0: Original 8-bit VGA mode.
	 * Uncentred crop (top left quarter) from 2x2 binned 1296x972 image.
	 */
	{
		{
			.code = MEDIA_BUS_FMT_SBGGR8_1X8,
			.colorspace = V4L2_COLORSPACE_SRGB,
			.field = V4L2_FIELD_NONE,
			.width = 640,
			.height = 480
		},
		.crop = {
			.left = 0,
			.top = 0,
			.width = 1280,
			.height = 960,
		},
		.pixel_rate = 77291670,
		ov5647_640x480_8bit,
		ARRAY_SIZE(ov5647_640x480_8bit)
	},
};

static struct ov5647_mode supported_modes_10bit[] = {
	/*
	 * MODE 0: 2592x1944 full resolution full FOV 10-bit mode.
	 */
	{
		{
			.code = MEDIA_BUS_FMT_SBGGR10_1X10,
			.colorspace = V4L2_COLORSPACE_SRGB,
			.field = V4L2_FIELD_NONE,
			.width = 2592,
			.height = 1944
		},
		.crop = {
			.left = 0,
			.top = 0,
			.width = 2592,
			.height = 1944
		},
		.pixel_rate = 87500000,
		ov5647_2592x1944_10bit,
		ARRAY_SIZE(ov5647_2592x1944_10bit)
	},
	/*
	 * MODE 1: 1080p30 10-bit mode.
	 * Full resolution centre-cropped down to 1080p.
	 */
	{
		{
			.code = MEDIA_BUS_FMT_SBGGR10_1X10,
			.colorspace = V4L2_COLORSPACE_SRGB,
			.field = V4L2_FIELD_NONE,
			.width = 1920,
			.height = 1080
		},
		.crop = {
			.left = 348,
			.top = 434,
			.width = 1928,
			.height = 1080,
		},
		.pixel_rate = 81666700,
		ov5647_1080p30_10bit,
		ARRAY_SIZE(ov5647_1080p30_10bit)
	},
	/*
	 * MODE 2: 2x2 binned full FOV 10-bit mode.
	 */
	{
		{
			.code = MEDIA_BUS_FMT_SBGGR10_1X10,
			.colorspace = V4L2_COLORSPACE_SRGB,
			.field = V4L2_FIELD_NONE,
			.width = 1296,
			.height = 972
		},
		.crop = {
			.left = 0,
			.top = 0,
			.width = 2592,
			.height = 1944,
		},
		.pixel_rate = 81666700,
		ov5647_2x2binned_10bit,
		ARRAY_SIZE(ov5647_2x2binned_10bit)
	},
	/*
	 * MODE 3: 10-bit VGA full FOV mode 60fps.
	 * 2x2 binned and subsampled down to VGA.
	 */
	{
		{
			.code = MEDIA_BUS_FMT_SBGGR10_1X10,
			.colorspace = V4L2_COLORSPACE_SRGB,
			.field = V4L2_FIELD_NONE,
			.width = 640,
			.height = 480
		},
		.crop = {
			.left = 16,
			.top = 0,
			.width = 2560,
			.height = 1920,
		},
		.pixel_rate = 55000000,
		ov5647_640x480_10bit,
		ARRAY_SIZE(ov5647_640x480_10bit)
	},
};

/* Use 2x2 binned 10-bit mode as default. */
#define OV5647_DEFAULT_MODE (&supported_modes_10bit[2])

static int ov5647_write(struct v4l2_subdev *sd, u16 reg, u8 val)
{
	int ret;
	unsigned char data[3] = { reg >> 8, reg & 0xff, val};
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	ret = i2c_master_send(client, data, 3);
	/*
	 * Writing the wrong number of bytes also needs to be flagged as an
	 * error. Success needs to produce a 0 return code.
	 */
	if (ret == 3) {
		ret = 0;
	} else {
		dev_dbg(&client->dev, "%s: i2c write error, reg: %x\n",
				__func__, reg);
		if (ret >= 0)
			ret = -EINVAL;
	}

	return ret;
}

static int ov5647_read(struct v4l2_subdev *sd, u16 reg, u8 *val)
{
	int ret;
	unsigned char data_w[2] = { reg >> 8, reg & 0xff };
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	ret = i2c_master_send(client, data_w, 2);
	/*
	 * A negative return code, or sending the wrong number of bytes, both
	 * count as an error.
	 */
	if (ret != 2) {
		dev_dbg(&client->dev, "%s: i2c write error, reg: %x\n",
			__func__, reg);
		if (ret >= 0)
			ret = -EINVAL;
		return ret;
	}

	ret = i2c_master_recv(client, val, 1);
	/*
	 * The only return value indicating success is 1. Anything else, even
	 * a non-negative value, indicates something went wrong.
	 */
	if (ret == 1) {
		ret = 0;
	} else {
		dev_dbg(&client->dev, "%s: i2c read error, reg: %x\n",
				__func__, reg);
		if (ret >= 0)
			ret = -EINVAL;
	}

	return ret;
}

static int ov5647_write_array(struct v4l2_subdev *sd,
				struct regval_list *regs, int array_size)
{
	int i, ret;

	for (i = 0; i < array_size; i++) {
		ret = ov5647_write(sd, regs[i].addr, regs[i].data);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int ov5647_set_virtual_channel(struct v4l2_subdev *sd, int channel)
{
	u8 channel_id;
	int ret;

	ret = ov5647_read(sd, OV5647_REG_MIPI_CTRL14, &channel_id);
	if (ret < 0)
		return ret;

	channel_id &= ~(3 << 6);
	return ov5647_write(sd, OV5647_REG_MIPI_CTRL14, channel_id | (channel << 6));
}

static int __sensor_init(struct v4l2_subdev *sd)
{
	int ret;
	u8 resetval, rdval;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5647 *state = to_state(sd);

	ret = ov5647_read(sd, OV5647_SW_STANDBY, &rdval);
	if (ret < 0)
		return ret;

	if (state->write_mode_regs) {
		ret = ov5647_write_array(sd, state->mode->reg_list,
					 state->mode->num_regs);
		if (ret < 0) {
			dev_err(&client->dev, "write sensor default regs error\n");
			return ret;
		}
		state->write_mode_regs = false;
	}

	ret = ov5647_set_virtual_channel(sd, 0);
	if (ret < 0)
		return ret;

	ret = ov5647_read(sd, OV5647_SW_STANDBY, &resetval);
	if (ret < 0)
		return ret;

	if (!(resetval & 0x01)) {
		dev_err(&client->dev, "Device was in SW standby");
		ret = ov5647_write(sd, OV5647_SW_STANDBY, 0x01);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int ov5647_stream_on(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5647 *ov5647 = to_state(sd);
	u8 val = MIPI_CTRL00_BUS_IDLE;
	int ret;

	ret = __sensor_init(sd);
	if (ret < 0) {
		dev_err(&client->dev, "sensor_init failed\n");
		return ret;
	}

	/* Apply customized values from user when stream starts */
	ret =  __v4l2_ctrl_handler_setup(sd->ctrl_handler);
	if (ret)
		return ret;

	if (ov5647->flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK)
		val |= MIPI_CTRL00_CLOCK_LANE_GATE |
		       MIPI_CTRL00_LINE_SYNC_ENABLE;

	ret = ov5647_write(sd, OV5647_REG_MIPI_CTRL00, val);
	if (ret < 0)
		return ret;

	ret = ov5647_write(sd, OV5647_REG_FRAME_OFF_NUMBER, 0x00);
	if (ret < 0)
		return ret;

	return ov5647_write(sd, OV5640_REG_PAD_OUT, 0x00);
}

static int ov5647_stream_off(struct v4l2_subdev *sd)
{
	int ret;

	ret = ov5647_write(sd, OV5647_REG_MIPI_CTRL00, MIPI_CTRL00_CLOCK_LANE_GATE
			   | MIPI_CTRL00_BUS_IDLE | MIPI_CTRL00_CLOCK_LANE_DISABLE);
	if (ret < 0)
		return ret;

	ret = ov5647_write(sd, OV5647_REG_FRAME_OFF_NUMBER, 0x0f);
	if (ret < 0)
		return ret;

	return ov5647_write(sd, OV5640_REG_PAD_OUT, 0x01);
}

static int set_sw_standby(struct v4l2_subdev *sd, bool standby)
{
	int ret;
	u8 rdval;

	ret = ov5647_read(sd, OV5647_SW_STANDBY, &rdval);
	if (ret < 0)
		return ret;

	if (standby)
		rdval &= ~0x01;
	else
		rdval |= 0x01;

	return ov5647_write(sd, OV5647_SW_STANDBY, rdval);
}

static int ov5647_sensor_power(struct v4l2_subdev *sd, int on)
{
	int ret = 0;
	struct ov5647 *ov5647 = to_state(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	mutex_lock(&ov5647->lock);

	if (on && !ov5647->power_count)	{
		dev_dbg(&client->dev, "OV5647 power on\n");

		if (ov5647->pwdn) {
			gpiod_set_value_cansleep(ov5647->pwdn, 0);
			msleep(PWDN_ACTIVE_DELAY_MS);
		}

		ret = clk_prepare_enable(ov5647->xclk);
		if (ret < 0) {
			dev_err(&client->dev, "clk prepare enable failed\n");
			goto out;
		}

		ret = ov5647_write_array(sd, sensor_oe_enable_regs,
					 ARRAY_SIZE(sensor_oe_enable_regs));
		if (ret < 0) {
			clk_disable_unprepare(ov5647->xclk);
			dev_err(&client->dev,
				"write sensor_oe_enable_regs error\n");
			goto out;
		}

		/*
		 * Ensure streaming off to make clock lane go into LP-11 state.
		 */
		ret = ov5647_stream_off(sd);
		if (ret < 0) {
			clk_disable_unprepare(ov5647->xclk);
			dev_err(&client->dev,
				"Camera not available, check Power\n");
			goto out;
		}

		/* Write out the register set over I2C on stream-on. */
		ov5647->write_mode_regs = true;
	} else if (!on && ov5647->power_count == 1) {
		dev_dbg(&client->dev, "OV5647 power off\n");

		ret = ov5647_write_array(sd, sensor_oe_disable_regs,
					 ARRAY_SIZE(sensor_oe_disable_regs));

		if (ret < 0)
			dev_dbg(&client->dev, "disable oe failed\n");

		ret = set_sw_standby(sd, true);

		if (ret < 0)
			dev_dbg(&client->dev, "soft stby failed\n");

		clk_disable_unprepare(ov5647->xclk);

		gpiod_set_value_cansleep(ov5647->pwdn, 1);
	}

	/* Update the power count. */
	ov5647->power_count += on ? 1 : -1;
	WARN_ON(ov5647->power_count < 0);

out:
	mutex_unlock(&ov5647->lock);

	return ret;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int ov5647_sensor_get_register(struct v4l2_subdev *sd,
				struct v4l2_dbg_register *reg)
{
	u8 val;
	int ret;

	ret = ov5647_read(sd, reg->reg & 0xff, &val);
	if (ret < 0)
		return ret;

	reg->val = val;
	reg->size = 1;

	return 0;
}

static int ov5647_sensor_set_register(struct v4l2_subdev *sd,
				const struct v4l2_dbg_register *reg)
{
	return ov5647_write(sd, reg->reg & 0xff, reg->val & 0xff);
}
#endif

/*
 * Subdev core operations registration
 */
static const struct v4l2_subdev_core_ops ov5647_subdev_core_ops = {
	.s_power		= ov5647_sensor_power,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register		= ov5647_sensor_get_register,
	.s_register		= ov5647_sensor_set_register,
#endif
};

static const struct v4l2_rect *
__ov5647_get_pad_crop(struct ov5647 *ov5647, struct v4l2_subdev_pad_config *cfg,
		      unsigned int pad, enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_crop(&ov5647->sd, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &ov5647->mode->crop;
	}

	return NULL;
}

static int ov5647_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP: {
		struct ov5647 *state = to_state(sd);

		mutex_lock(&state->lock);
		sel->r = *__ov5647_get_pad_crop(state, cfg, sel->pad,
						sel->which);
		mutex_unlock(&state->lock);

		return 0;
	}

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = OV5647_NATIVE_WIDTH;
		sel->r.height = OV5647_NATIVE_HEIGHT;

		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.top = OV5647_PIXEL_ARRAY_TOP;
		sel->r.left = OV5647_PIXEL_ARRAY_LEFT;
		sel->r.width = OV5647_PIXEL_ARRAY_WIDTH;
		sel->r.height = OV5647_PIXEL_ARRAY_HEIGHT;

		return 0;
	}

	return -EINVAL;
}

static int ov5647_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov5647 *state = to_state(sd);
	int ret = 0;

	mutex_lock(&state->lock);

	if (enable)
		ret = ov5647_stream_on(sd);
	else
		ret = ov5647_stream_off(sd);

	mutex_unlock(&state->lock);

	return ret;
}

static const struct v4l2_subdev_video_ops ov5647_subdev_video_ops = {
	.s_stream =		ov5647_s_stream,
};

static int ov5647_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index == 0 && ARRAY_SIZE(supported_modes_8bit))
		code->code = MEDIA_BUS_FMT_SBGGR8_1X8;
	else if (code->index == 0 && ARRAY_SIZE(supported_modes_8bit) == 0 &&
		 ARRAY_SIZE(supported_modes_10bit))
		code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	else if (code->index == 1 && ARRAY_SIZE(supported_modes_8bit) &&
		 ARRAY_SIZE(supported_modes_10bit))
		code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	else
		return -EINVAL;

	return 0;
}

static int ov5647_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct ov5647_mode *mode = NULL;

	if (fse->code == MEDIA_BUS_FMT_SBGGR8_1X8) {
		if (fse->index >= ARRAY_SIZE(supported_modes_8bit))
			return -EINVAL;
		mode = &supported_modes_8bit[fse->index];
	} else if (fse->code == MEDIA_BUS_FMT_SBGGR10_1X10) {
		if (fse->index >= ARRAY_SIZE(supported_modes_10bit))
			return -EINVAL;
		mode = &supported_modes_10bit[fse->index];
	} else {
		return -EINVAL;
	}

	fse->min_width = mode->format.width;
	fse->max_width = fse->min_width;
	fse->min_height = mode->format.height;
	fse->max_height = fse->min_height;

	return 0;
}

static int ov5647_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct ov5647 *state = to_state(sd);
	struct v4l2_mbus_framefmt *framefmt;
	const struct ov5647_mode *mode_8bit, *mode_10bit, *mode = NULL;

	if (format->pad != 0)
		return -EINVAL;

	mutex_lock(&state->lock);

	/*
	 * Try to respect any given pixel format, otherwise try for a 10-bit
	 * mode.
	 */
	mode_8bit = v4l2_find_nearest_size(supported_modes_8bit,
					   ARRAY_SIZE(supported_modes_8bit),
					   format.width, format.height,
					   format->format.width,
					   format->format.height);
	mode_10bit = v4l2_find_nearest_size(supported_modes_10bit,
					    ARRAY_SIZE(supported_modes_10bit),
					    format.width, format.height,
					    format->format.width,
					    format->format.height);
	if (format->format.code == MEDIA_BUS_FMT_SBGGR8_1X8 && mode_8bit)
		mode = mode_8bit;
	else if (format->format.code == MEDIA_BUS_FMT_SBGGR10_1X10 &&
		 mode_10bit)
		mode = mode_10bit;
	else if (mode_10bit)
		mode = mode_10bit;
	else
		mode = mode_8bit;

	if (!mode) {
		mutex_unlock(&state->lock);
		return -EINVAL;
	}

	*fmt = mode->format;
	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_get_try_format(sd, cfg, format->pad);
		*framefmt = format->format;
	} else {
		/*
		 * If we have changed modes, write the I2C register list on
		 * a stream_on().
		 */
		if (state->mode != mode)
			state->write_mode_regs = true;
		state->mode = mode;

		__v4l2_ctrl_modify_range(state->pixel_rate,
					 mode->pixel_rate,
					 mode->pixel_rate, 1,
					 mode->pixel_rate);
	}

	mutex_unlock(&state->lock);

	return 0;
}

static int ov5647_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct ov5647 *state = to_state(sd);

	if (format->pad != 0)
		return -EINVAL;

	mutex_lock(&state->lock);

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		*fmt = *v4l2_subdev_get_try_format(sd, cfg, format->pad);
	else
		*fmt = state->mode->format;

	mutex_unlock(&state->lock);

	return 0;
}

static const struct v4l2_subdev_pad_ops ov5647_subdev_pad_ops = {
	.enum_mbus_code = ov5647_enum_mbus_code,
	.set_fmt =	  ov5647_set_fmt,
	.get_fmt =	  ov5647_get_fmt,
	.get_selection =  ov5647_get_selection,
	.enum_frame_size = ov5647_enum_frame_size,
};

static const struct v4l2_subdev_ops ov5647_subdev_ops = {
	.core		= &ov5647_subdev_core_ops,
	.video		= &ov5647_subdev_video_ops,
	.pad		= &ov5647_subdev_pad_ops,
};

static int ov5647_detect(struct v4l2_subdev *sd)
{
	u8 read;
	int ret;
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	ret = ov5647_write(sd, OV5647_SW_RESET, 0x01);
	if (ret < 0)
		return ret;

	ret = ov5647_read(sd, OV5647_REG_CHIPID_H, &read);
	if (ret < 0)
		return ret;

	if (read != 0x56) {
		dev_err(&client->dev, "ID High expected 0x56 got %x", read);
		return -ENODEV;
	}

	ret = ov5647_read(sd, OV5647_REG_CHIPID_L, &read);
	if (ret < 0)
		return ret;

	if (read != 0x47) {
		dev_err(&client->dev, "ID Low expected 0x47 got %x", read);
		return -ENODEV;
	}

	return ov5647_write(sd, OV5647_SW_RESET, 0x00);
}

static int ov5647_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct v4l2_mbus_framefmt *format =
				v4l2_subdev_get_try_format(sd, fh->pad, 0);
	struct v4l2_rect *crop =
				v4l2_subdev_get_try_crop(sd, fh->pad, 0);
	struct ov5647 *state = to_state(sd);

	crop->left = OV5647_PIXEL_ARRAY_LEFT;
	crop->top = OV5647_PIXEL_ARRAY_TOP;
	crop->width = OV5647_PIXEL_ARRAY_WIDTH;
	crop->height = OV5647_PIXEL_ARRAY_HEIGHT;

	/* Set the default format to the same as the sensor. */
	*format = state->mode->format;

	return 0;
}

static const struct v4l2_subdev_internal_ops ov5647_subdev_internal_ops = {
	.open = ov5647_open,
};

static int ov5647_parse_dt(struct device_node *np, struct ov5647 *sensor)
{
	struct v4l2_fwnode_endpoint bus_cfg = { .bus_type = 0 };
	struct device_node *ep;

	int ret;

	ep = of_graph_get_next_endpoint(np, NULL);
	if (!ep)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(ep), &bus_cfg);

	if (!ret)
		sensor->flags = bus_cfg.bus.mipi_csi2.flags;

	of_node_put(ep);
	return ret;
}

static int ov5647_s_auto_white_balance(struct v4l2_subdev *sd, u32 val)
{
	/* non-zero turns on AWB */
	return ov5647_write(sd, OV5647_REG_AWB, val ? 1 : 0);
}

static int ov5647_s_autogain(struct v4l2_subdev *sd, u32 val)
{
	int ret;
	u8 reg;

	/* non-zero turns on AGC by clearing bit 1 */
	ret = ov5647_read(sd, OV5647_REG_AEC_AGC, &reg);
	if (ret == 0)
		ret = ov5647_write(sd, OV5647_REG_AEC_AGC,
				   val ? reg & ~2 : reg | 2);

	return ret;
}

static int ov5647_s_exposure_auto(struct v4l2_subdev *sd, u32 val)
{
	int ret;
	u8 reg;

	/* Everything except V4L2_EXPOSURE_MANUAL turns on AEC by
	 * clearing bit 0
	 */
	ret = ov5647_read(sd, OV5647_REG_AEC_AGC, &reg);
	if (ret == 0)
		ret = ov5647_write(sd, OV5647_REG_AEC_AGC,
				   val == V4L2_EXPOSURE_MANUAL ?
				   reg | 1 : reg & ~1);

	return ret;
}

static int ov5647_s_analogue_gain(struct v4l2_subdev *sd, u32 val)
{
	int ret;

	/* 10 bits of gain, 2 in the high register */
	ret = ov5647_write(sd, OV5647_REG_GAIN_HI, (val >> 8) & 3);
	if (ret == 0)
		ret = ov5647_write(sd, OV5647_REG_GAIN_LO, val & 0xff);

	return ret;
}

static int ov5647_s_exposure(struct v4l2_subdev *sd, u32 val)
{
	int ret;

	/* Sensor has 20 bits, but the bottom 4 bits are fractions of a line
	 * which we leave as zero (and don't receive in "val").
	 */
	ret = ov5647_write(sd, OV5647_REG_EXP_HI, (val >> 12) & 0xf);
	if (ret == 0)
		ov5647_write(sd, OV5647_REG_EXP_MID, (val >> 4) & 0xff);
	if (ret == 0)
		ov5647_write(sd, OV5647_REG_EXP_LO, (val & 0xf) << 4);

	return ret;
}

static int ov5647_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov5647 *state = container_of(ctrl->handler,
					     struct ov5647, ctrls);
	struct v4l2_subdev *sd = &state->sd;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	/* v4l2_ctrl_lock() locks our own mutex */

	/*
	 * If the device is not powered up by the host driver do
	 * not apply any controls to H/W at this time. Instead
	 * the controls will be restored right after power-up.
	 */
	if (state->power_count == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_AUTO_WHITE_BALANCE:
		ret = ov5647_s_auto_white_balance(sd, ctrl->val);
		break;
	case V4L2_CID_AUTOGAIN:
		ret = ov5647_s_autogain(sd, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE_AUTO:
		ret = ov5647_s_exposure_auto(sd, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ov5647_s_analogue_gain(sd, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		ret = ov5647_s_exposure(sd, ctrl->val);
		break;
	case V4L2_CID_PIXEL_RATE:
		/* Read-only, but we adjust it based on mode. */
		break;
	default:
		dev_info(&client->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops ov5647_ctrl_ops = {
	.s_ctrl = ov5647_s_ctrl,
};

static int ov5647_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ov5647 *sensor;
	int ret;
	struct v4l2_subdev *sd;
	struct device_node *np = client->dev.of_node;
	u32 xclk_freq;
	struct v4l2_ctrl *ctrl;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	if (IS_ENABLED(CONFIG_OF) && np) {
		ret = ov5647_parse_dt(np, sensor);
		if (ret) {
			dev_err(dev, "DT parsing error: %d\n", ret);
			return ret;
		}
	}

	/* get system clock (xclk) */
	sensor->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk)) {
		dev_err(dev, "could not get xclk");
		return PTR_ERR(sensor->xclk);
	}

	xclk_freq = clk_get_rate(sensor->xclk);
	if (xclk_freq != 25000000) {
		dev_err(dev, "Unsupported clock frequency: %u\n", xclk_freq);
		return -EINVAL;
	}

	/* Request the power down GPIO asserted */
	sensor->pwdn = devm_gpiod_get_optional(&client->dev, "pwdn",
					       GPIOD_OUT_HIGH);

	mutex_init(&sensor->lock);

	/* Initialise controls. */
	v4l2_ctrl_handler_init(&sensor->ctrls, 6);
	v4l2_ctrl_new_std(&sensor->ctrls, &ov5647_ctrl_ops,
			  V4L2_CID_AUTOGAIN,
			  0,  /* min */
			  1,  /* max */
			  1,  /* step */
			  0); /* default */
	v4l2_ctrl_new_std(&sensor->ctrls, &ov5647_ctrl_ops,
			  V4L2_CID_AUTO_WHITE_BALANCE,
			  0,  /* min */
			  1,  /* max */
			  1,  /* step */
			  0); /* default */
	v4l2_ctrl_new_std_menu(&sensor->ctrls, &ov5647_ctrl_ops,
			       V4L2_CID_EXPOSURE_AUTO,
			       V4L2_EXPOSURE_MANUAL,  /* max */
			       0,                     /* skip_mask */
			       V4L2_EXPOSURE_MANUAL); /* default */
	ctrl = v4l2_ctrl_new_std(&sensor->ctrls, &ov5647_ctrl_ops,
				 V4L2_CID_EXPOSURE,
				 4,     /* min lines */
				 65535, /* max lines (4+8+4 bits)*/
				 1,     /* step */
				 1000); /* default number of lines */
	ctrl->flags |= V4L2_CTRL_FLAG_EXECUTE_ON_WRITE;
	ctrl = v4l2_ctrl_new_std(&sensor->ctrls, &ov5647_ctrl_ops,
				 V4L2_CID_ANALOGUE_GAIN,
				 16,   /* min, 16 = 1.0x */
				 1023, /* max (10 bits) */
				 1,    /* step */
				 32);  /* default, 32 = 2.0x */
	ctrl->flags |= V4L2_CTRL_FLAG_EXECUTE_ON_WRITE;

	/* Set the default mode before we init the subdev */
	sensor->mode = OV5647_DEFAULT_MODE;

	/* By default, PIXEL_RATE is read only, but it does change per mode */
	sensor->pixel_rate = v4l2_ctrl_new_std(&sensor->ctrls, &ov5647_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       sensor->mode->pixel_rate,
					       sensor->mode->pixel_rate, 1,
					       sensor->mode->pixel_rate);

	if (sensor->ctrls.error) {
		ret = sensor->ctrls.error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}
	sensor->sd.ctrl_handler = &sensor->ctrls;

	/* Write out the register set over I2C on stream-on. */
	sensor->write_mode_regs = true;

	sd = &sensor->sd;
	v4l2_i2c_subdev_init(sd, client, &ov5647_subdev_ops);
	sensor->sd.internal_ops = &ov5647_subdev_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &sensor->pad);
	if (ret < 0)
		goto mutex_remove;

	if (sensor->pwdn) {
		gpiod_set_value_cansleep(sensor->pwdn, 0);
		msleep(PWDN_ACTIVE_DELAY_MS);
	}

	ret = ov5647_detect(sd);

	gpiod_set_value_cansleep(sensor->pwdn, 1);

	if (ret < 0)
		goto error;

	ret = v4l2_async_register_subdev(sd);
	if (ret < 0)
		goto error;

	dev_dbg(dev, "OmniVision OV5647 camera driver probed\n");
	return 0;
error:
	media_entity_cleanup(&sd->entity);
mutex_remove:
	v4l2_ctrl_handler_free(&sensor->ctrls);
	mutex_destroy(&sensor->lock);
	return ret;
}

static int ov5647_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5647 *ov5647 = to_state(sd);

	v4l2_async_unregister_subdev(&ov5647->sd);
	media_entity_cleanup(&ov5647->sd.entity);
	v4l2_ctrl_handler_free(&ov5647->ctrls);
	v4l2_device_unregister_subdev(sd);
	mutex_destroy(&ov5647->lock);

	return 0;
}

static const struct i2c_device_id ov5647_id[] = {
	{ "ov5647", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ov5647_id);

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id ov5647_of_match[] = {
	{ .compatible = "ovti,ov5647" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ov5647_of_match);
#endif

static struct i2c_driver ov5647_driver = {
	.driver = {
		.of_match_table = of_match_ptr(ov5647_of_match),
		.name	= SENSOR_NAME,
	},
	.probe_new	= ov5647_probe,
	.remove		= ov5647_remove,
	.id_table	= ov5647_id,
};

module_i2c_driver(ov5647_driver);

MODULE_AUTHOR("Ramiro Oliveira <roliveir@synopsys.com>");
MODULE_DESCRIPTION("A low-level driver for OmniVision ov5647 sensors");
MODULE_LICENSE("GPL v2");
