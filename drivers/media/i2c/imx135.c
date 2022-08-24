// SPDX-License-Identifier: GPL-2.0
/*
 * A V4L2 driver for Sony IMX135 cameras.
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd
 *
 * Framework based on Sony imx258 camera driver
 * Copyright (C) 2018 Intel Corporation
 * Using register sets from the Nvidia Tegra drivers for IMX135
 * https://android.googlesource.com/kernel/tegra/+/2268683075e741190919217a72fcf13eb174dc57/drivers/media/platform/tegra/imx135.c
 * https://nv-tegra.nvidia.com/gitweb/?p=linux-3.10.git;a=commitdiff;h=a4201ceac4a89a495d759885b1e2d5a1c1466073
 * Copyright (c) 2013, NVIDIA CORPORATION.
 */

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <asm/unaligned.h>

#define IMX135_REG_VALUE_08BIT		1
#define IMX135_REG_VALUE_16BIT		2

#define IMX135_REG_MODE_SELECT		0x0100
#define IMX135_MODE_STANDBY		0x00
#define IMX135_MODE_STREAMING		0x01

/* Chip ID */
#define IMX135_REG_CHIP_ID		0x0016
#define IMX135_CHIP_ID			0x0135

/* V_TIMING internal */
#define IMX135_VTS_MAX			0xffff

#define IMX135_REG_VTS			0x0340

/* Exposure control */
#define IMX135_REG_EXPOSURE		0x0202
#define IMX135_EXPOSURE_OFFSET		10
#define IMX135_EXPOSURE_MIN		4
#define IMX135_EXPOSURE_STEP		1
#define IMX135_EXPOSURE_DEFAULT		0x640
#define IMX135_EXPOSURE_MAX		(IMX135_VTS_MAX - IMX135_EXPOSURE_OFFSET)

#define IMX135_PIXEL_RATE		270320000 //Calculated

#define IMX135_LINK_FREQ		337900000 //Calculated

/* HBLANK control - read only */
#define IMX135_PPL_DEFAULT		4572

/* Analog gain control */
#define IMX135_REG_ANALOG_GAIN		0x0205
#define IMX135_ANA_GAIN_MIN		0
#define IMX135_ANA_GAIN_MAX		0xff
#define IMX135_ANA_GAIN_STEP		1
#define IMX135_ANA_GAIN_DEFAULT		0x0

/* Digital gain control */
#define IMX135_REG_GR_DIGITAL_GAIN	0x020e
#define IMX135_REG_R_DIGITAL_GAIN	0x0210
#define IMX135_REG_B_DIGITAL_GAIN	0x0212
#define IMX135_REG_GB_DIGITAL_GAIN	0x0214
#define IMX135_DGTL_GAIN_MIN		0
#define IMX135_DGTL_GAIN_MAX		4096	/* Max = 0xFFF */
#define IMX135_DGTL_GAIN_DEFAULT	1024
#define IMX135_DGTL_GAIN_STEP		1

/* HDR control */
#define IMX135_REG_HDR			0x0220
#define IMX135_HDR_ON			BIT(0)
#define IMX135_REG_HDR_RATIO		0x0222
#define IMX135_HDR_RATIO_MIN		0
#define IMX135_HDR_RATIO_MAX		5
#define IMX135_HDR_RATIO_STEP		1
#define IMX135_HDR_RATIO_DEFAULT	0x0

/* Test Pattern Control */
#define IMX135_REG_TEST_PATTERN		0x0600

/* Orientation */
#define REG_MIRROR_FLIP_CONTROL		0x0101
#define REG_CONFIG_MIRROR_HFLIP		0x01
#define REG_CONFIG_MIRROR_VFLIP		0x02
#define REG_CONFIG_FLIP_TEST_PATTERN	0x02

/* Input clock frequency in Hz */
#define IMX135_INPUT_CLOCK_FREQ		24000000

/* IMX135 native and active pixel array size. */
/* These need confirming */
#define IMX135_NATIVE_WIDTH		4224U
#define IMX135_NATIVE_HEIGHT		3192U
#define IMX135_PIXEL_ARRAY_LEFT		8U
#define IMX135_PIXEL_ARRAY_TOP		16U
#define IMX135_PIXEL_ARRAY_WIDTH	4208U
#define IMX135_PIXEL_ARRAY_HEIGHT	3120U

struct imx135_reg {
	u16 address;
	u8 val;
};

struct imx135_reg_list {
	u32 num_of_regs;
	const struct imx135_reg *regs;
};

/* Mode : resolution and related config&values */
struct imx135_mode {
	/* Frame width */
	u32 width;
	/* Frame height */
	u32 height;

	/* V-timing */
	u32 vts_def;
	u32 vts_min;
	/* Analog crop rectangle. */
	struct v4l2_rect crop;

	/* Default register values */
	struct imx135_reg_list reg_list;
};

static const struct imx135_reg mode_4208x3120_regs[] = {
	//18.026fps

	/* software reset */
	{0x0103, 0x01},
	/* global settings */
	{0x0105, 0x01},
	{0x0110, 0x00},
	{0x0220, 0x01},
	{0x3302, 0x11},
	{0x3833, 0x20},
	{0x3893, 0x00},
	{0x3906, 0x08},
	{0x3907, 0x01},
	{0x391B, 0x01},
	{0x3C09, 0x01},
	{0x600A, 0x00},
	{0x3008, 0xB0},
	{0x320A, 0x01},
	{0x320D, 0x10},
	{0x3216, 0x2E},
	{0x322C, 0x02},
	{0x3409, 0x0C},
	{0x340C, 0x2D},
	{0x3411, 0x39},
	{0x3414, 0x1E},
	{0x3427, 0x04},
	{0x3480, 0x1E},
	{0x3484, 0x1E},
	{0x3488, 0x1E},
	{0x348C, 0x1E},
	{0x3490, 0x1E},
	{0x3494, 0x1E},
	{0x3511, 0x8F},
	{0x364F, 0x2D},
	/* Clock Setting */
	{0x011E, 0x18},
	{0x011F, 0x00},
	{0x0301, 0x05},
	{0x0303, 0x01},
	{0x0305, 0x0C},
	{0x0309, 0x05},
	{0x030B, 0x01},
	{0x030C, 0x01},
	{0x030D, 0xC2},
	{0x030E, 0x01},
	{0x3A06, 0x11},
	/* Mode Settings */
	{0x0108, 0x03},
	{0x0112, 0x0A},
	{0x0113, 0x0A},
	{0x0381, 0x01},
	{0x0383, 0x01},
	{0x0385, 0x01},
	{0x0387, 0x01},
	{0x0390, 0x00},
	{0x0391, 0x11},
	{0x0392, 0x00},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x4082, 0x01},
	{0x4083, 0x01},
	{0x7006, 0x04},
	/* Optinal/Function settings */
	{0x0700, 0x00},
	{0x3A63, 0x00},
	{0x4100, 0xF8},
	{0x4203, 0xFF},
	{0x4344, 0x00},
	{0x441C, 0x01},
	/* Size Setting */
	{0x0342, 0x11},
	{0x0343, 0xDC},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x10},
	{0x0349, 0x6F},
	{0x034A, 0x0C},
	{0x034B, 0x2F},
	{0x034C, 0x10},
	{0x034D, 0x70},
	{0x034E, 0x0C},
	{0x034F, 0x30},
	{0x0350, 0x00},
	{0x0351, 0x00},
	{0x0352, 0x00},
	{0x0353, 0x00},
	{0x0354, 0x10},
	{0x0355, 0x70},
	{0x0356, 0x0C},
	{0x0357, 0x30},
	{0x301D, 0x30},
	{0x3310, 0x10},
	{0x3311, 0x70},
	{0x3312, 0x0C},
	{0x3313, 0x30},
	{0x331C, 0x01},
	{0x331D, 0x68},
	{0x4084, 0x00},
	{0x4085, 0x00},
	{0x4086, 0x00},
	{0x4087, 0x00},
	{0x4400, 0x00},
	/* Global Timing Setting */
	{0x0830, 0x87},
	{0x0831, 0x3F},
	{0x0832, 0x67},
	{0x0833, 0x3F},
	{0x0834, 0x3F},
	{0x0835, 0x4F},
	{0x0836, 0xDF},
	{0x0837, 0x47},
	{0x0839, 0x1F},
	{0x083A, 0x17},
	{0x083B, 0x02},
	/* HDR Setting */
	{0x0230, 0x00},
	{0x0231, 0x00},
	{0x0233, 0x00},
	{0x0234, 0x00},
	{0x0235, 0x40},
	{0x0238, 0x01},
	{0x0239, 0x04},
	{0x023B, 0x00},
	{0x023C, 0x01},
	{0x33B0, 0x04},
	{0x33B1, 0x00},
	{0x33B3, 0x00},
	{0x33B4, 0x01},
	{0x3800, 0x00},
	{0x3A43, 0x01},
};

static const struct imx135_reg mode_2104x1560[] = {
	/* software reset */
	{0x0103, 0x01},
	/* global settings */
	{0x0105, 0x01},
	{0x0110, 0x00},
	{0x0220, 0x01},
	{0x3302, 0x11},
	{0x3833, 0x20},
	{0x3873, 0x03},
	{0x3893, 0x00},
	{0x3906, 0x08},
	{0x3907, 0x01},
	{0x391B, 0x00},
	{0x3C09, 0x01},
	{0x600A, 0x00},
	{0x3008, 0xB0},
	{0x320A, 0x01},
	{0x320D, 0x10},
	{0x3216, 0x2E},
	{0x322C, 0x02},
	{0x3409, 0x0C},
	{0x340C, 0x2D},
	{0x3411, 0x39},
	{0x3414, 0x1E},
	{0x3427, 0x04},
	{0x3480, 0x1E},
	{0x3484, 0x1E},
	{0x3488, 0x1E},
	{0x348C, 0x1E},
	{0x3490, 0x1E},
	{0x3494, 0x1E},
	{0x3511, 0x8F},
	{0x364F, 0x2D},
	/* Clock Setting */
	{0x011E, 0x18},
	{0x011F, 0x00},
	{0x0301, 0x05},
	{0x0303, 0x01},
	{0x0305, 0x0C},
	{0x0309, 0x05},
	{0x030B, 0x02},
	{0x030C, 0x01},
	{0x030D, 0x10},
	{0x030E, 0x01},
	{0x3A06, 0x12},
	/* Mode Settings */
	{0x0108, 0x03},
	{0x0112, 0x0A},
	{0x0113, 0x0A},
	{0x0381, 0x01},
	{0x0383, 0x01},
	{0x0385, 0x01},
	{0x0387, 0x01},
	{0x0390, 0x01},
	{0x0391, 0x21},
	{0x0392, 0x00},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x4082, 0x01},
	{0x4083, 0x01},
	{0x7006, 0x04},
	/* Optinal/Function settings */
	{0x0700, 0x00},
	{0x3A63, 0x00},
	{0x4100, 0xF8},
	{0x4203, 0xFF},
	{0x4344, 0x00},
	{0x441C, 0x01},
	/* Size Setting */
	{0x0342, 0x11},
	{0x0343, 0xDC},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x10},
	{0x0349, 0x6F},
	{0x034A, 0x0C},
	{0x034B, 0x2F},
	{0x034C, 0x08},
	{0x034D, 0x38},
	{0x034E, 0x06},
	{0x034F, 0x18},
	{0x0350, 0x00},
	{0x0351, 0x00},
	{0x0352, 0x00},
	{0x0353, 0x00},
	{0x0354, 0x08},
	{0x0355, 0x38},
	{0x0356, 0x06},
	{0x0357, 0x18},
	{0x301D, 0x30},
	{0x3310, 0x08},
	{0x3311, 0x38},
	{0x3312, 0x06},
	{0x3313, 0x18},
	{0x331C, 0x00},
	{0x331D, 0x52},
	{0x4084, 0x00},
	{0x4085, 0x00},
	{0x4086, 0x00},
	{0x4087, 0x00},
	{0x4400, 0x00},
	/* Global Timing Setting */
	{0x0830, 0x5F},
	{0x0831, 0x17},
	{0x0832, 0x37},
	{0x0833, 0x17},
	{0x0834, 0x17},
	{0x0835, 0x17},
	{0x0836, 0x57},
	{0x0837, 0x27},
	{0x0839, 0x1F},
	{0x083A, 0x17},
	{0x083B, 0x02},
	/* HDR Setting */
	{0x0230, 0x00},
	{0x0231, 0x00},
	{0x0233, 0x00},
	{0x0234, 0x00},
	{0x0235, 0x40},
	{0x0238, 0x01},
	{0x0239, 0x04},
	{0x023B, 0x00},
	{0x023C, 0x01},
	{0x33B0, 0x08},
	{0x33B1, 0x38},
	{0x33B3, 0x01},
	{0x33B4, 0x01},
	{0x3800, 0x00},

	{0x3024, 0xE0},
	{0x302B, 0x01},
	{0x302A, 0x01},
	{0x3029, 0x01},
	{0x3028, 0x05},
	{0x3025, 0x00},
	{0x300C, 0x9C},
};

static const struct imx135_reg mode_1920x1080_regs[] = {
	//32.375fps.

	/* software reset */
	{0x0103, 0x01},
	/* global settings */
	{0x0105, 0x01},
	{0x0110, 0x00},
	{0x0220, 0x01},
	{0x3302, 0x11},
	{0x3833, 0x20},
	{0x3893, 0x00},
	{0x3906, 0x08},
	{0x3907, 0x01},
	{0x391B, 0x01},
	{0x3C09, 0x01},
	{0x600A, 0x00},
	{0x3008, 0xB0},
	{0x320A, 0x01},
	{0x320D, 0x10},
	{0x3216, 0x2E},
	{0x322C, 0x02},
	{0x3409, 0x0C},
	{0x340C, 0x2D},
	{0x3411, 0x39},
	{0x3414, 0x1E},
	{0x3427, 0x04},
	{0x3480, 0x1E},
	{0x3484, 0x1E},
	{0x3488, 0x1E},
	{0x348C, 0x1E},
	{0x3490, 0x1E},
	{0x3494, 0x1E},
	{0x3511, 0x8F},
	{0x364F, 0x2D},
	/* Clock Setting */
	{0x011E, 0x18},
	{0x011F, 0x00},
	{0x0301, 0x05},
	{0x0303, 0x01},
	{0x0305, 0x0C},
	{0x0309, 0x05},
	{0x030B, 0x02},
	{0x030C, 0x01},
	{0x030D, 0xC2},
	{0x030E, 0x01},
	{0x3A06, 0x12},
	/* Mode Settings */
	{0x0108, 0x03},
	{0x0112, 0x0A},
	{0x0113, 0x0A},
	{0x0381, 0x01},
	{0x0383, 0x01},
	{0x0385, 0x01},
	{0x0387, 0x01},
	{0x0390, 0x01},
	{0x0391, 0x22},
	{0x0392, 0x00},
	{0x0401, 0x02},
	{0x0404, 0x00},
	{0x0405, 0x11},
	{0x4082, 0x00},
	{0x4083, 0x00},
	{0x7006, 0x04},
	/* Optinal/Function settings */
	{0x0700, 0x00},
	{0x3A63, 0x00},
	{0x4100, 0xF8},
	{0x4203, 0xFF},
	{0x4344, 0x00},
	{0x441C, 0x01},
	/* Size Setting */
	{0x0342, 0x11},
	{0x0343, 0xDC},
	{0x0344, 0x00},
	{0x0345, 0x40},
	{0x0346, 0x01},
	{0x0347, 0x9C},
	{0x0348, 0x10},
	{0x0349, 0x2F},
	{0x034A, 0x0A},
	{0x034B, 0x93},
	{0x034C, 0x07},
	{0x034D, 0x80},
	{0x034E, 0x04},
	{0x034F, 0x38},
	{0x0350, 0x00},
	{0x0351, 0x00},
	{0x0352, 0x00},
	{0x0353, 0x00},
	{0x0354, 0x07},
	{0x0355, 0xF8},
	{0x0356, 0x04},
	{0x0357, 0x7C},
	{0x301D, 0x30},
	{0x3310, 0x07},
	{0x3311, 0x80},
	{0x3312, 0x04},
	{0x3313, 0x38},
	{0x331C, 0x00},
	{0x331D, 0xD2},
	{0x4084, 0x07},
	{0x4085, 0x80},
	{0x4086, 0x04},
	{0x4087, 0x38},
	{0x4400, 0x00},
	/* Global Timing Setting */
	{0x0830, 0x67},
	{0x0831, 0x27},
	{0x0832, 0x47},
	{0x0833, 0x27},
	{0x0834, 0x27},
	{0x0835, 0x1F},
	{0x0836, 0x87},
	{0x0837, 0x2F},
	{0x0839, 0x1F},
	{0x083A, 0x17},
	{0x083B, 0x02},
	/* HDR Setting */
	{0x0230, 0x00},
	{0x0231, 0x00},
	{0x0233, 0x00},
	{0x0234, 0x00},
	{0x0235, 0x40},
	{0x0238, 0x01},
	{0x0239, 0x04},
	{0x023B, 0x00},
	{0x023C, 0x01},
	{0x33B0, 0x04},
	{0x33B1, 0x00},
	{0x33B3, 0x00},
	{0x33B4, 0x01},
	{0x3800, 0x00},
	{0x3A43, 0x01},
};

static const struct imx135_reg mode_1280x720_regs[] = {
	// 56.08fps

	/* software reset */
	{0x0103, 0x01},
	/* global settings */
	{0x0105, 0x01},
	{0x0110, 0x00},
	{0x0220, 0x01},
	{0x3302, 0x11},
	{0x3833, 0x20},
	{0x3893, 0x00},
	{0x3906, 0x08},
	{0x3907, 0x01},
	{0x391B, 0x01},
	{0x3C09, 0x01},
	{0x600A, 0x00},
	{0x3008, 0xB0},
	{0x320A, 0x01},
	{0x320D, 0x10},
	{0x3216, 0x2E},
	{0x322C, 0x02},
	{0x3409, 0x0C},
	{0x340C, 0x2D},
	{0x3411, 0x39},
	{0x3414, 0x1E},
	{0x3427, 0x04},
	{0x3480, 0x1E},
	{0x3484, 0x1E},
	{0x3488, 0x1E},
	{0x348C, 0x1E},
	{0x3490, 0x1E},
	{0x3494, 0x1E},
	{0x3511, 0x8F},
	{0x364F, 0x2D},
	/* Clock Setting */
	{0x011E, 0x18},
	{0x011F, 0x00},
	{0x0301, 0x05},
	{0x0303, 0x01},
	{0x0305, 0x0C},
	{0x0309, 0x05},
	{0x030B, 0x02},
	{0x030C, 0x01},
	{0x030D, 0xC2},
	{0x030E, 0x01},
	{0x3A06, 0x12},
	/* Mode Settings */
	{0x0108, 0x03},
	{0x0112, 0x0A},
	{0x0113, 0x0A},
	{0x0381, 0x01},
	{0x0383, 0x01},
	{0x0385, 0x01},
	{0x0387, 0x01},
	{0x0390, 0x01},
	{0x0391, 0x22},
	{0x0392, 0x00},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x4082, 0x01},
	{0x4083, 0x01},
	{0x7006, 0x04},
	/* Optinal/Function settings */
	{0x0700, 0x00},
	{0x3A63, 0x00},
	{0x4100, 0xF8},
	{0x4203, 0xFF},
	{0x4344, 0x00},
	{0x441C, 0x01},
	/* Size Setting */
	{0x0342, 0x11},
	{0x0343, 0xDC},
	{0x0344, 0x03},
	{0x0345, 0x38},
	{0x0346, 0x03},
	{0x0347, 0x48},
	{0x0348, 0x0D},
	{0x0349, 0x37},
	{0x034A, 0x08},
	{0x034B, 0xE7},
	{0x034C, 0x05},
	{0x034D, 0x00},
	{0x034E, 0x02},
	{0x034F, 0xD0},
	{0x0350, 0x00},
	{0x0351, 0x00},
	{0x0352, 0x00},
	{0x0353, 0x00},
	{0x0354, 0x05},
	{0x0355, 0x00},
	{0x0356, 0x02},
	{0x0357, 0xD0},
	{0x301D, 0x30},
	{0x3310, 0x05},
	{0x3311, 0x00},
	{0x3312, 0x02},
	{0x3313, 0xD0},
	{0x331C, 0x00},
	{0x331D, 0x10},
	{0x4084, 0x00},
	{0x4085, 0x00},
	{0x4086, 0x00},
	{0x4087, 0x00},
	{0x4400, 0x00},
	/* Global Timing Setting */
	{0x0830, 0x67},
	{0x0831, 0x27},
	{0x0832, 0x47},
	{0x0833, 0x27},
	{0x0834, 0x27},
	{0x0835, 0x1F},
	{0x0836, 0x87},
	{0x0837, 0x2F},
	{0x0839, 0x1F},
	{0x083A, 0x17},
	{0x083B, 0x02},
	/* HDR Setting */
	{0x0230, 0x00},
	{0x0231, 0x00},
	{0x0233, 0x00},
	{0x0234, 0x00},
	{0x0235, 0x40},
	{0x0238, 0x01},
	{0x0239, 0x04},
	{0x023B, 0x00},
	{0x023C, 0x01},
	{0x33B0, 0x04},
	{0x33B1, 0x00},
	{0x33B3, 0x00},
	{0x33B4, 0x01},
	{0x3800, 0x00},
	{0x3A43, 0x01},
};

/*
 * The supported formats.
 * This table MUST contain 4 entries per format, to cover the various flip
 * combinations in the order
 * - no flip
 * - h flip
 * - v flip
 * - h&v flips
 */
static const u32 codes[] = {
	/* 10-bit modes. */
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,
};

static const char * const imx135_test_pattern_menu[] = {
	"Disabled",
	"Solid Colour",
	"Eight Vertical Colour Bars",
	"Colour Bars With Fade to Grey",
	"Pseudorandom Sequence (PN9)",
};

static const s64 link_freq_menu_items[] = {
	IMX135_LINK_FREQ
};

/* regulator supplies */
static const char * const imx135_supply_name[] = {
	/* Supplies can be enabled in any order */
	"vana",  /* Analog (2.8V) supply */
	"vdig",  /* Digital Core (1.05V) supply */
	"vif",  /* IF (1.8V) supply */
};

#define IMX135_NUM_SUPPLIES ARRAY_SIZE(imx135_supply_name)

/* Mode configs */
static const struct imx135_mode supported_modes[] = {
	{
		.width = 4208,
		.height = 3120,
		.vts_def = 0xCD0,
		.vts_min = 0xCD0,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_4208x3120_regs),
			.regs = mode_4208x3120_regs,
		},
		.crop = {
			.left = IMX135_PIXEL_ARRAY_LEFT,
			.top = IMX135_PIXEL_ARRAY_TOP,
			.width = 4208,
			.height = 3120,
		},
	},
	{
		.width = 2104,
		.height = 1560,
		.vts_def = 0x630,
		.vts_min = 0x630,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2104x1560),
			.regs = mode_2104x1560,
		},
		.crop = {
			.left = IMX135_PIXEL_ARRAY_LEFT,
			.top = IMX135_PIXEL_ARRAY_TOP,
			.width = 4208,
			.height = 3120,
		},
	},
	{
		.width = 1920,
		.height = 1080,
		.vts_def = 0xA40,
		.vts_min = 0xA40,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1920x1080_regs),
			.regs = mode_1920x1080_regs,
		},
		.crop = {
			/* X - 64 to 4143 */
			/* Y - 412 to 2707 */
			.left = IMX135_PIXEL_ARRAY_LEFT + 64,
			.top = IMX135_PIXEL_ARRAY_TOP + 412,
			.width = 4080,
			.height = 2296,
		},
	},
	{
		.width = 1280,
		.height = 720,
		.vts_def = 0x36A,
		.vts_min = 0x36A,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1280x720_regs),
			.regs = mode_1280x720_regs,
		},
		.crop = {
			/* X - 824 to 3383 */
			/* Y - 840 to 2279 */
			.left = IMX135_PIXEL_ARRAY_LEFT + 824,
			.top = IMX135_PIXEL_ARRAY_TOP + 840,
			.width = 2260,
			.height = 1440,
		},
	},
};

struct imx135 {
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	/* Current mode */
	const struct imx135_mode *cur_mode;

	unsigned int nlanes;

	/*
	 * Mutex for serialized access:
	 * Protect sensor module set pad format and start/stop streaming safely.
	 */
	struct mutex mutex;

	/* Streaming on/off */
	bool streaming;

	struct clk *clk;
	struct regulator_bulk_data supplies[IMX135_NUM_SUPPLIES];
};

static inline struct imx135 *to_imx135(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx135, sd);
}

/* Read registers up to 2 at a time */
static int imx135_read_reg(struct imx135 *imx135, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx135->sd);
	struct i2c_msg msgs[2];
	u8 addr_buf[2] = { reg >> 8, reg & 0xff };
	u8 data_buf[4] = { 0, };
	int ret;

	if (len > 4)
		return -EINVAL;

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_buf[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = get_unaligned_be32(data_buf);

	return 0;
}

/* Write registers up to 2 at a time */
static int imx135_write_reg(struct imx135 *imx135, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx135->sd);
	u8 buf[6];

	if (len > 4)
		return -EINVAL;

	put_unaligned_be16(reg, buf);
	put_unaligned_be32(val << (8 * (4 - len)), buf + 2);
	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

/* Write a list of registers */
static int imx135_write_regs(struct imx135 *imx135,
			     const struct imx135_reg *regs, u32 len)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx135->sd);
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = imx135_write_reg(imx135, regs[i].address, 1,
				       regs[i].val);
		if (ret) {
			dev_err_ratelimited(&client->dev,
					    "Failed to write reg 0x%4.4x. error = %d\n",
					    regs[i].address, ret);

			return ret;
		}
	}

	return 0;
}

/* Get bayer order based on flip setting. */
static u32 imx135_get_format_code(struct imx135 *imx135)
{
	unsigned int i;

	lockdep_assert_held(&imx135->mutex);

	i = (imx135->vflip->val ? 2 : 0) |
	    (imx135->hflip->val ? 1 : 0);

	return codes[i];
}

/* Open sub-device */
static int imx135_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx135 *imx135 = to_imx135(sd);
	struct v4l2_mbus_framefmt *try_fmt =
		v4l2_subdev_get_try_format(sd, fh->pad, 0);

	/* Initialize try_fmt */
	try_fmt->width = supported_modes[0].width;
	try_fmt->height = supported_modes[0].height;
	try_fmt->code = imx135_get_format_code(imx135);
	try_fmt->field = V4L2_FIELD_NONE;

	return 0;
}

static void imx135_adjust_exposure_range(struct imx135 *imx135)
{
	int exposure_max, exposure_def;

	/* Honour the VBLANK limits when setting exposure. */
	exposure_max = imx135->cur_mode->height + imx135->vblank->val -
		       IMX135_EXPOSURE_OFFSET;
	exposure_def = min(exposure_max, imx135->exposure->val);
	__v4l2_ctrl_modify_range(imx135->exposure, imx135->exposure->minimum,
				 exposure_max, imx135->exposure->step,
				 exposure_def);
}

static int imx135_update_digital_gain(struct imx135 *imx135, u32 val)
{
	int ret;

	ret = imx135_write_reg(imx135, IMX135_REG_GR_DIGITAL_GAIN,
			       IMX135_REG_VALUE_16BIT, val);
	if (ret)
		return ret;
	ret = imx135_write_reg(imx135, IMX135_REG_GB_DIGITAL_GAIN,
			       IMX135_REG_VALUE_16BIT, val);
	if (ret)
		return ret;
	ret = imx135_write_reg(imx135, IMX135_REG_R_DIGITAL_GAIN,
			       IMX135_REG_VALUE_16BIT, val);
	if (ret)
		return ret;
	ret = imx135_write_reg(imx135, IMX135_REG_B_DIGITAL_GAIN,
			       IMX135_REG_VALUE_16BIT, val);
	if (ret)
		return ret;
	return 0;
}

static int imx135_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx135 *imx135 =
		container_of(ctrl->handler, struct imx135, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&imx135->sd);
	int ret = 0;

	/*
	 * The VBLANK control may change the limits of usable exposure, so check
	 * and adjust if necessary.
	 */
	if (ctrl->id == V4L2_CID_VBLANK)
		imx135_adjust_exposure_range(imx135);

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx135_write_reg(imx135, IMX135_REG_ANALOG_GAIN,
				       IMX135_REG_VALUE_08BIT,
				       ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		ret = imx135_write_reg(imx135, IMX135_REG_EXPOSURE,
				       IMX135_REG_VALUE_16BIT,
				       ctrl->val);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = imx135_update_digital_gain(imx135, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = imx135_write_reg(imx135, IMX135_REG_TEST_PATTERN,
				       IMX135_REG_VALUE_16BIT,
				       ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		ret = imx135_write_reg(imx135, IMX135_REG_VTS,
				       IMX135_REG_VALUE_16BIT,
				       imx135->cur_mode->width + ctrl->val);
		break;
	case V4L2_CID_VFLIP:
	case V4L2_CID_HFLIP:
		ret = imx135_write_reg(imx135, REG_MIRROR_FLIP_CONTROL,
				       IMX135_REG_VALUE_08BIT,
				       (imx135->hflip->val ?
					REG_CONFIG_MIRROR_HFLIP : 0) |
				       (imx135->vflip->val ?
					REG_CONFIG_MIRROR_VFLIP : 0));
		break;
	default:
		dev_info(&client->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx135_ctrl_ops = {
	.s_ctrl = imx135_set_ctrl,
};

static int imx135_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx135 *imx135 = to_imx135(sd);

	/* Only one bayer format (10 bit) is supported */
	if (code->index > 0)
		return -EINVAL;

	code->code = imx135_get_format_code(imx135);

	return 0;
}

static int imx135_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx135 *imx135 = to_imx135(sd);

	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != imx135_get_format_code(imx135))
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static void imx135_update_pad_format(struct imx135 *imx135,
				     const struct imx135_mode *mode,
				     struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = imx135_get_format_code(imx135);
	fmt->format.field = V4L2_FIELD_NONE;
}

static int __imx135_get_pad_format(struct imx135 *imx135,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_format *fmt)
{
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt->format = *v4l2_subdev_get_try_format(&imx135->sd, cfg,
							  fmt->pad);
	else
		imx135_update_pad_format(imx135, imx135->cur_mode, fmt);

	return 0;
}

static int imx135_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_format *fmt)
{
	struct imx135 *imx135 = to_imx135(sd);
	int ret;

	mutex_lock(&imx135->mutex);
	ret = __imx135_get_pad_format(imx135, cfg, fmt);
	mutex_unlock(&imx135->mutex);

	return ret;
}

static int imx135_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_format *fmt)
{
	struct imx135 *imx135 = to_imx135(sd);
	const struct imx135_mode *mode;
	struct v4l2_mbus_framefmt *framefmt;
	s32 vblank_def;
	s32 vblank_min;
	s64 h_blank;

	mutex_lock(&imx135->mutex);

	fmt->format.code = imx135_get_format_code(imx135);

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);
	imx135_update_pad_format(imx135, mode, fmt);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
		*framefmt = fmt->format;
	} else {
		imx135->cur_mode = mode;
		/* Update limits and set FPS to default */
		vblank_def = imx135->cur_mode->vts_def -
			     imx135->cur_mode->height;
		vblank_min = imx135->cur_mode->vts_min -
			     imx135->cur_mode->height;
		__v4l2_ctrl_modify_range(imx135->vblank, vblank_min,
					 IMX135_VTS_MAX -
						imx135->cur_mode->height,
					 1, vblank_def);
		__v4l2_ctrl_s_ctrl(imx135->vblank, vblank_def);
		h_blank = IMX135_PPL_DEFAULT - imx135->cur_mode->width;
		__v4l2_ctrl_modify_range(imx135->hblank, h_blank,
					 h_blank, 1, h_blank);
	}

	mutex_unlock(&imx135->mutex);

	return 0;
}

static const struct v4l2_rect *
__imx135_get_pad_crop(struct imx135 *imx135, struct v4l2_subdev_pad_config *cfg,
		      unsigned int pad, enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_crop(&imx135->sd, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &imx135->cur_mode->crop;
	}

	return NULL;
}

static int imx135_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP: {
		struct imx135 *imx135 = to_imx135(sd);

		mutex_lock(&imx135->mutex);
		sel->r = *__imx135_get_pad_crop(imx135, cfg, sel->pad,
						sel->which);
		mutex_unlock(&imx135->mutex);

		return 0;
	}

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX135_NATIVE_WIDTH;
		sel->r.height = IMX135_NATIVE_HEIGHT;
		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = IMX135_PIXEL_ARRAY_LEFT;
		sel->r.top = IMX135_PIXEL_ARRAY_TOP;
		sel->r.width = IMX135_PIXEL_ARRAY_WIDTH;
		sel->r.height = IMX135_PIXEL_ARRAY_HEIGHT;
		return 0;
	}

	return -EINVAL;
}

/* Start streaming */
static int imx135_start_streaming(struct imx135 *imx135)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx135->sd);
	const struct imx135_reg_list *reg_list;
	int ret;

	/* Apply default values of current mode */
	reg_list = &imx135->cur_mode->reg_list;
	ret = imx135_write_regs(imx135, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(&client->dev, "%s failed to set mode\n", __func__);
		return ret;
	}

	/* Apply customized values from user */
	ret =  __v4l2_ctrl_handler_setup(imx135->sd.ctrl_handler);
	if (ret)
		return ret;

	/* set stream on register */
	return imx135_write_reg(imx135, IMX135_REG_MODE_SELECT,
				IMX135_REG_VALUE_08BIT,
				IMX135_MODE_STREAMING);
}

/* Stop streaming */
static int imx135_stop_streaming(struct imx135 *imx135)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx135->sd);
	int ret;

	/* set stream off register */
	ret = imx135_write_reg(imx135, IMX135_REG_MODE_SELECT,
			       IMX135_REG_VALUE_08BIT, IMX135_MODE_STANDBY);
	if (ret)
		dev_err(&client->dev, "%s failed to set stream\n", __func__);

	/*
	 * Return success even if it was an error, as there is nothing the
	 * caller can do about it.
	 */
	return 0;
}

static int imx135_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx135 *imx135 = to_imx135(sd);
	int ret;

	ret = regulator_bulk_enable(IMX135_NUM_SUPPLIES,
				    imx135->supplies);
	if (ret) {
		dev_err(dev, "%s: failed to enable regulators\n",
			__func__);
		return ret;
	}

	ret = clk_prepare_enable(imx135->clk);
	if (ret) {
		dev_err(dev, "failed to enable clock\n");
		regulator_bulk_disable(IMX135_NUM_SUPPLIES, imx135->supplies);
	}

	return ret;
}

static int imx135_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx135 *imx135 = to_imx135(sd);

	clk_disable_unprepare(imx135->clk);
	regulator_bulk_disable(IMX135_NUM_SUPPLIES, imx135->supplies);

	return 0;
}

static int imx135_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx135 *imx135 = to_imx135(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&imx135->mutex);
	if (imx135->streaming == enable) {
		mutex_unlock(&imx135->mutex);
		return 0;
	}

	if (enable) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto err_unlock;
		}

		/*
		 * Apply default & customized values
		 * and then start streaming.
		 */
		ret = imx135_start_streaming(imx135);
		if (ret)
			goto err_rpm_put;
	} else {
		imx135_stop_streaming(imx135);
		pm_runtime_put(&client->dev);
	}

	imx135->streaming = enable;
	mutex_unlock(&imx135->mutex);

	return ret;

err_rpm_put:
	pm_runtime_put(&client->dev);
err_unlock:
	mutex_unlock(&imx135->mutex);

	return ret;
}

static int __maybe_unused imx135_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx135 *imx135 = to_imx135(sd);

	if (imx135->streaming)
		imx135_stop_streaming(imx135);

	return 0;
}

static int __maybe_unused imx135_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx135 *imx135 = to_imx135(sd);
	int ret;

	if (imx135->streaming) {
		ret = imx135_start_streaming(imx135);
		if (ret)
			goto error;
	}

	return 0;

error:
	imx135_stop_streaming(imx135);
	imx135->streaming = 0;
	return ret;
}

/* Verify chip ID */
static int imx135_identify_module(struct imx135 *imx135)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx135->sd);
	int ret;
	u32 val;

	ret = imx135_read_reg(imx135, IMX135_REG_CHIP_ID,
			      IMX135_REG_VALUE_16BIT, &val);
	if (ret) {
		dev_err(&client->dev, "failed to read chip id %x\n",
			IMX135_CHIP_ID);
		return ret;
	}

	if (val != IMX135_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%x\n",
			IMX135_CHIP_ID, val);
		return -EIO;
	}

	return 0;
}

static const struct v4l2_subdev_video_ops imx135_video_ops = {
	.s_stream = imx135_set_stream,
};

static const struct v4l2_subdev_pad_ops imx135_pad_ops = {
	.enum_mbus_code = imx135_enum_mbus_code,
	.get_fmt = imx135_get_pad_format,
	.set_fmt = imx135_set_pad_format,
	.get_selection = imx135_get_selection,
	.enum_frame_size = imx135_enum_frame_size,
};

static const struct v4l2_subdev_ops imx135_subdev_ops = {
	.video = &imx135_video_ops,
	.pad = &imx135_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx135_internal_ops = {
	.open = imx135_open,
};

/* Initialize control handlers */
static int imx135_init_controls(struct imx135 *imx135)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx135->sd);
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct v4l2_ctrl *link_freq;
	s64 vblank_def;
	s64 vblank_min;
	int ret;

	ctrl_hdlr = &imx135->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 12);
	if (ret)
		return ret;

	mutex_init(&imx135->mutex);
	ctrl_hdlr->lock = &imx135->mutex;

	link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &imx135_ctrl_ops,
					   V4L2_CID_LINK_FREQ, 1, 0,
					   link_freq_menu_items);

	if (link_freq)
		link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* PIXEL_RATE is read only by default */
	v4l2_ctrl_new_std(ctrl_hdlr, &imx135_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  IMX135_PIXEL_RATE, IMX135_PIXEL_RATE, 1,
			  IMX135_PIXEL_RATE);

	vblank_def = imx135->cur_mode->vts_def - imx135->cur_mode->height;
	vblank_min = imx135->cur_mode->vts_min - imx135->cur_mode->height;
	imx135->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx135_ctrl_ops,
					   V4L2_CID_VBLANK, vblank_min,
					   IMX135_VTS_MAX -
						imx135->cur_mode->height,
					   1, vblank_def);

	imx135->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx135_ctrl_ops,
					   V4L2_CID_HBLANK,
					   IMX135_PPL_DEFAULT -
						imx135->cur_mode->width,
					   IMX135_PPL_DEFAULT -
						imx135->cur_mode->width,
					   1,
					   IMX135_PPL_DEFAULT -
						imx135->cur_mode->width);

	if (imx135->hblank)
		imx135->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx135->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx135_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX135_EXPOSURE_MIN,
					     IMX135_EXPOSURE_MAX,
					     IMX135_EXPOSURE_STEP,
					     IMX135_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx135_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX135_ANA_GAIN_MIN, IMX135_ANA_GAIN_MAX,
			  IMX135_ANA_GAIN_STEP, IMX135_ANA_GAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx135_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  IMX135_DGTL_GAIN_MIN, IMX135_DGTL_GAIN_MAX,
			  IMX135_DGTL_GAIN_STEP, IMX135_DGTL_GAIN_DEFAULT);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx135_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx135_test_pattern_menu) - 1,
				     0, 0, imx135_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;
	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx135_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	imx135->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx135_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1,
					  props.rotation == 180 ? 1 : 0);
	if (imx135->hflip)
		imx135->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx135->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx135_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1,
					  props.rotation == 180 ? 1 : 0);
	if (imx135->vflip)
		imx135->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	imx135->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	mutex_destroy(&imx135->mutex);

	return ret;
}

static void imx135_free_controls(struct imx135 *imx135)
{
	v4l2_ctrl_handler_free(imx135->sd.ctrl_handler);
	mutex_destroy(&imx135->mutex);
}

static int imx135_get_regulators(struct imx135 *imx135,
				 struct i2c_client *client)
{
	unsigned int i;

	for (i = 0; i < IMX135_NUM_SUPPLIES; i++)
		imx135->supplies[i].supply = imx135_supply_name[i];

	return devm_regulator_bulk_get(&client->dev,
				       IMX135_NUM_SUPPLIES,
				       imx135->supplies);
}

static int imx135_probe(struct i2c_client *client)
{
	struct imx135 *imx135;
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret;
	u32 val = 0;

	imx135 = devm_kzalloc(&client->dev, sizeof(*imx135), GFP_KERNEL);
	if (!imx135)
		return -ENOMEM;

	ret = imx135_get_regulators(imx135, client);
	if (ret)
		return ret;

	imx135->clk = devm_clk_get_optional(&client->dev, NULL);
	if (!imx135->clk) {
		dev_dbg(&client->dev,
			"no clock provided, using clock-frequency property\n");

		device_property_read_u32(&client->dev, "clock-frequency", &val);
	} else if (IS_ERR(imx135->clk)) {
		return dev_err_probe(&client->dev, PTR_ERR(imx135->clk),
				     "error getting clock\n");
	} else {
		val = clk_get_rate(imx135->clk);
	}

	if (val != IMX135_INPUT_CLOCK_FREQ) {
		dev_err(&client->dev, "input clock frequency of %u not supported\n",
			val);
		return -EINVAL;
	}

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(&client->dev), NULL);
	if (!endpoint) {
		dev_err(&client->dev, "Endpoint node not found\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret == -ENXIO) {
		dev_err(&client->dev, "Unsupported bus type, should be CSI2\n");
		goto error_endpoint_poweron;
	} else if (ret) {
		dev_err(&client->dev, "Parsing endpoint node failed\n");
		goto error_endpoint_poweron;
	}

	/* Get number of data lanes */
	imx135->nlanes = ep.bus.mipi_csi2.num_data_lanes;
	if (imx135->nlanes != 4) {
		dev_err(&client->dev, "Invalid data lanes: %u\n",
			imx135->nlanes);
		ret = -EINVAL;
		goto error_endpoint_poweron;
	}

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&imx135->sd, client, &imx135_subdev_ops);

	/* Will be powered off via pm_runtime_idle */
	ret = imx135_power_on(&client->dev);
	if (ret)
		goto error_endpoint_poweron;

	/* Check module identity */
	ret = imx135_identify_module(imx135);
	if (ret)
		goto error_identify;

	/* Set default mode to max resolution */
	imx135->cur_mode = &supported_modes[0];

	ret = imx135_init_controls(imx135);
	if (ret)
		goto error_identify;

	/* Initialize subdev */
	imx135->sd.internal_ops = &imx135_internal_ops;
	imx135->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx135->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	imx135->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx135->sd.entity, 1, &imx135->pad);
	if (ret)
		goto error_handler_free;

	ret = v4l2_async_register_subdev_sensor_common(&imx135->sd);
	if (ret < 0)
		goto error_media_entity;

	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_idle(&client->dev);

	return 0;

error_media_entity:
	media_entity_cleanup(&imx135->sd.entity);

error_handler_free:
	imx135_free_controls(imx135);

error_identify:
	imx135_power_off(&client->dev);

error_endpoint_poweron:
	v4l2_fwnode_endpoint_free(&ep);

	return ret;
}

static int imx135_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx135 *imx135 = to_imx135(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	imx135_free_controls(imx135);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx135_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);

	return 0;
}

static const struct dev_pm_ops imx135_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(imx135_suspend, imx135_resume)
	SET_RUNTIME_PM_OPS(imx135_power_off, imx135_power_on, NULL)
};

static const struct of_device_id imx135_dt_ids[] = {
	{ .compatible = "sony,imx135" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx135_dt_ids);

static struct i2c_driver imx135_i2c_driver = {
	.driver = {
		.name = "imx135",
		.pm = &imx135_pm_ops,
		.of_match_table	= imx135_dt_ids,
	},
	.probe_new = imx135_probe,
	.remove = imx135_remove,
};

module_i2c_driver(imx135_i2c_driver);

MODULE_AUTHOR("Dave Stevenson <dave.stevenson@raspberrypi.com>");
MODULE_DESCRIPTION("Sony IMX135 sensor driver");
MODULE_LICENSE("GPL v2");
