// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 Intel Corporation

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

#define IMX258_REG_VALUE_08BIT		1
#define IMX258_REG_VALUE_16BIT		2

#define IMX258_REG_MODE_SELECT		0x0100
#define IMX258_MODE_STANDBY		0x00
#define IMX258_MODE_STREAMING		0x01

#define IMX258_REG_RESET		0x0103

/* Chip ID */
#define IMX258_REG_CHIP_ID		0x0016
#define IMX258_CHIP_ID			0x0258

/* V_TIMING internal */
#define IMX258_VTS_30FPS		0x0c50
#define IMX258_VTS_30FPS_2K		0x0638
#define IMX258_VTS_30FPS_VGA		0x034c
#define IMX258_VTS_MAX			65525

#define IMX258_REG_VTS			0x0340

/* Exposure control */
#define IMX258_REG_EXPOSURE		0x0202
#define IMX258_EXPOSURE_OFFSET		10
#define IMX258_EXPOSURE_MIN		4
#define IMX258_EXPOSURE_STEP		1
#define IMX258_EXPOSURE_DEFAULT		0x640
#define IMX258_EXPOSURE_MAX		(IMX258_VTS_MAX - IMX258_EXPOSURE_OFFSET)

/* HBLANK control - read only */
#define IMX258_PPL_DEFAULT		5352

/* Analog gain control */
#define IMX258_REG_ANALOG_GAIN		0x0204
#define IMX258_ANA_GAIN_MIN		0
#define IMX258_ANA_GAIN_MAX		480
#define IMX258_ANA_GAIN_STEP		1
#define IMX258_ANA_GAIN_DEFAULT		0x0

/* Digital gain control */
#define IMX258_REG_GR_DIGITAL_GAIN	0x020e
#define IMX258_REG_R_DIGITAL_GAIN	0x0210
#define IMX258_REG_B_DIGITAL_GAIN	0x0212
#define IMX258_REG_GB_DIGITAL_GAIN	0x0214
#define IMX258_DGTL_GAIN_MIN		0
#define IMX258_DGTL_GAIN_MAX		4096	/* Max = 0xFFF */
#define IMX258_DGTL_GAIN_DEFAULT	1024
#define IMX258_DGTL_GAIN_STEP		1

/* HDR control */
#define IMX258_REG_HDR			0x0220
#define IMX258_HDR_ON			BIT(0)
#define IMX258_REG_HDR_RATIO		0x0222
#define IMX258_HDR_RATIO_MIN		0
#define IMX258_HDR_RATIO_MAX		5
#define IMX258_HDR_RATIO_STEP		1
#define IMX258_HDR_RATIO_DEFAULT	0x0

/* Long exposure multiplier */
#define IMX258_LONG_EXP_SHIFT_MAX	7
#define IMX258_LONG_EXP_SHIFT_REG	0x3002

/* Test Pattern Control */
#define IMX258_REG_TEST_PATTERN		0x0600

#define IMX258_CLK_BLANK_STOP		0x4040

/* Orientation */
#define REG_MIRROR_FLIP_CONTROL		0x0101
#define REG_CONFIG_MIRROR_HFLIP		0x01
#define REG_CONFIG_MIRROR_VFLIP		0x02
#define REG_CONFIG_FLIP_TEST_PATTERN	0x02

/* IMX258 native and active pixel array size. */
#define IMX258_NATIVE_WIDTH		4224U
#define IMX258_NATIVE_HEIGHT		3192U
#define IMX258_PIXEL_ARRAY_LEFT		8U
#define IMX258_PIXEL_ARRAY_TOP		16U
#define IMX258_PIXEL_ARRAY_WIDTH	4208U
#define IMX258_PIXEL_ARRAY_HEIGHT	3120U

struct imx258_reg {
	u16 address;
	u8 val;
};

struct imx258_reg_list {
	u32 num_of_regs;
	const struct imx258_reg *regs;
};

struct imx258_link_cfg {
	unsigned int lf_to_pix_rate_factor;
	struct imx258_reg_list reg_list;
};

#define IMX258_LANE_CONFIGS	2
#define IMX258_2_LANE_MODE	0
#define IMX258_4_LANE_MODE	1

/* Link frequency config */
struct imx258_link_freq_config {
	u64 link_frequency;
	u32 pixels_per_line;

	/* Configuration for this link frequency / num lanes selection */
	struct imx258_link_cfg link_cfg[IMX258_LANE_CONFIGS];
};

/* Mode : resolution and related config&values */
struct imx258_mode {
	/* Frame width */
	u32 width;
	/* Frame height */
	u32 height;

	/* V-timing */
	u32 vts_def;
	u32 vts_min;

	/* Index of Link frequency config to be used */
	u32 link_freq_index;
	/* Default register values */
	struct imx258_reg_list reg_list;

	/* Analog crop rectangle. */
	struct v4l2_rect crop;
};

/* 4208x3120 needs 1267Mbps/lane, 4 lanes. Use that rate on 2 lanes as well */
static const struct imx258_reg mipi_1267mbps_19_2mhz_2l[] = {
	{ 0x0136, 0x13 },
	{ 0x0137, 0x33 },
	{ 0x0301, 0x0A },
	{ 0x0303, 0x02 },
	{ 0x0305, 0x03 },
	{ 0x0306, 0x00 },
	{ 0x0307, 0xC6 },
	{ 0x0309, 0x0A },
	{ 0x030B, 0x01 },
	{ 0x030D, 0x02 },
	{ 0x030E, 0x00 },
	{ 0x030F, 0xD8 },
	{ 0x0310, 0x00 },

	{ 0x0114, 0x01 },
	{ 0x0820, 0x09 },
	{ 0x0821, 0xa6 },
	{ 0x0822, 0x66 },
	{ 0x0823, 0x66 },
};

static const struct imx258_reg mipi_1267mbps_19_2mhz_4l[] = {
	{ 0x0136, 0x13 },
	{ 0x0137, 0x33 },
	{ 0x0301, 0x05 },
	{ 0x0303, 0x02 },
	{ 0x0305, 0x03 },
	{ 0x0306, 0x00 },
	{ 0x0307, 0xC6 },
	{ 0x0309, 0x0A },
	{ 0x030B, 0x01 },
	{ 0x030D, 0x02 },
	{ 0x030E, 0x00 },
	{ 0x030F, 0xD8 },
	{ 0x0310, 0x00 },

	{ 0x0114, 0x03 },
	{ 0x0820, 0x13 },
	{ 0x0821, 0x4C },
	{ 0x0822, 0xCC },
	{ 0x0823, 0xCC },
};

static const struct imx258_reg mipi_1272mbps_24mhz_2l[] = {
	{ 0x0136, 0x18 },
	{ 0x0137, 0x00 },
	{ 0x0301, 0x0a },
	{ 0x0303, 0x02 },
	{ 0x0305, 0x04 },
	{ 0x0306, 0x00 },
	{ 0x0307, 0xD4 },
	{ 0x0309, 0x0A },
	{ 0x030B, 0x01 },
	{ 0x030D, 0x02 },
	{ 0x030E, 0x00 },
	{ 0x030F, 0xD8 },
	{ 0x0310, 0x00 },

	{ 0x0114, 0x01 },
	{ 0x0820, 0x13 },
	{ 0x0821, 0x4C },
	{ 0x0822, 0xCC },
	{ 0x0823, 0xCC },
};

static const struct imx258_reg mipi_1272mbps_24mhz_4l[] = {
	{ 0x0136, 0x18 },
	{ 0x0137, 0x00 },
	{ 0x0301, 0x05 },
	{ 0x0303, 0x02 },
	{ 0x0305, 0x04 },
	{ 0x0306, 0x00 },
	{ 0x0307, 0xD4 },
	{ 0x0309, 0x0A },
	{ 0x030B, 0x01 },
	{ 0x030D, 0x02 },
	{ 0x030E, 0x00 },
	{ 0x030F, 0xD8 },
	{ 0x0310, 0x00 },

	{ 0x0114, 0x03 },
	{ 0x0820, 0x13 },
	{ 0x0821, 0xE0 },
	{ 0x0822, 0x00 },
	{ 0x0823, 0x00 },
};

static const struct imx258_reg mipi_640mbps_19_2mhz_2l[] = {
	{ 0x0136, 0x13 },
	{ 0x0137, 0x33 },
	{ 0x0301, 0x05 },
	{ 0x0303, 0x02 },
	{ 0x0305, 0x03 },
	{ 0x0306, 0x00 },
	{ 0x0307, 0x64 },
	{ 0x0309, 0x0A },
	{ 0x030B, 0x01 },
	{ 0x030D, 0x02 },
	{ 0x030E, 0x00 },
	{ 0x030F, 0xD8 },
	{ 0x0310, 0x00 },

	{ 0x0114, 0x01 },
	{ 0x0820, 0x05 },
	{ 0x0821, 0x00 },
	{ 0x0822, 0x00 },
	{ 0x0823, 0x00 },
};

static const struct imx258_reg mipi_640mbps_19_2mhz_4l[] = {
	{ 0x0136, 0x13 },
	{ 0x0137, 0x33 },
	{ 0x0301, 0x05 },
	{ 0x0303, 0x02 },
	{ 0x0305, 0x03 },
	{ 0x0306, 0x00 },
	{ 0x0307, 0x64 },
	{ 0x0309, 0x0A },
	{ 0x030B, 0x01 },
	{ 0x030D, 0x02 },
	{ 0x030E, 0x00 },
	{ 0x030F, 0xD8 },
	{ 0x0310, 0x00 },

	{ 0x0114, 0x03 },
	{ 0x0820, 0x0A },
	{ 0x0821, 0x00 },
	{ 0x0822, 0x00 },
	{ 0x0823, 0x00 },
};

static const struct imx258_reg mipi_642mbps_24mhz_2l[] = {
	{ 0x0136, 0x18 },
	{ 0x0137, 0x00 },
	{ 0x0301, 0x05 },
	{ 0x0303, 0x02 },
	{ 0x0305, 0x04 },
	{ 0x0306, 0x00 },
	{ 0x0307, 0x6B },
	{ 0x0309, 0x0A },
	{ 0x030B, 0x01 },
	{ 0x030D, 0x02 },
	{ 0x030E, 0x00 },
	{ 0x030F, 0xD8 },
	{ 0x0310, 0x00 },

	{ 0x0114, 0x01 },
	{ 0x0820, 0x0A },
	{ 0x0821, 0x00 },
	{ 0x0822, 0x00 },
	{ 0x0823, 0x00 },
};

static const struct imx258_reg mipi_642mbps_24mhz_4l[] = {
	{ 0x0136, 0x18 },
	{ 0x0137, 0x00 },
	{ 0x0301, 0x05 },
	{ 0x0303, 0x02 },
	{ 0x0305, 0x04 },
	{ 0x0306, 0x00 },
	{ 0x0307, 0x6B },
	{ 0x0309, 0x0A },
	{ 0x030B, 0x01 },
	{ 0x030D, 0x02 },
	{ 0x030E, 0x00 },
	{ 0x030F, 0xD8 },
	{ 0x0310, 0x00 },

	{ 0x0114, 0x03 },
	{ 0x0820, 0x0A },
	{ 0x0821, 0x00 },
	{ 0x0822, 0x00 },
	{ 0x0823, 0x00 },
};

static const struct imx258_reg mode_4208x3120_regs[] = {
	{ 0x3051, 0x00 },
	{ 0x6B11, 0xCF },
	{ 0x7FF0, 0x08 },
	{ 0x7FF1, 0x0F },
	{ 0x7FF2, 0x08 },
	{ 0x7FF3, 0x1B },
	{ 0x7FF4, 0x23 },
	{ 0x7FF5, 0x60 },
	{ 0x7FF6, 0x00 },
	{ 0x7FF7, 0x01 },
	{ 0x7FF8, 0x00 },
	{ 0x7FF9, 0x78 },
	{ 0x7FFA, 0x00 },
	{ 0x7FFB, 0x00 },
	{ 0x7FFC, 0x00 },
	{ 0x7FFD, 0x00 },
	{ 0x7FFE, 0x00 },
	{ 0x7FFF, 0x03 },
	{ 0x7F76, 0x03 },
	{ 0x7F77, 0xFE },
	{ 0x7FA8, 0x03 },
	{ 0x7FA9, 0xFE },
	{ 0x7B24, 0x81 },
	{ 0x6564, 0x07 },
	{ 0x6B0D, 0x41 },
	{ 0x653D, 0x04 },
	{ 0x6B05, 0x8C },
	{ 0x6B06, 0xF9 },
	{ 0x6B08, 0x65 },
	{ 0x6B09, 0xFC },
	{ 0x6B0A, 0xCF },
	{ 0x6B0B, 0xD2 },
	{ 0x6700, 0x0E },
	{ 0x6707, 0x0E },
	{ 0x9104, 0x00 },
	{ 0x4648, 0x7F },
	{ 0x7420, 0x00 },
	{ 0x7421, 0x1C },
	{ 0x7422, 0x00 },
	{ 0x7423, 0xD7 },
	{ 0x5F04, 0x00 },
	{ 0x5F05, 0xED },
	{ 0x0112, 0x0A },
	{ 0x0113, 0x0A },
	{ 0x0342, 0x14 },
	{ 0x0343, 0xE8 },
	{ 0x0344, 0x00 },
	{ 0x0345, 0x00 },
	{ 0x0346, 0x00 },
	{ 0x0347, 0x00 },
	{ 0x0348, 0x10 },
	{ 0x0349, 0x6F },
	{ 0x034A, 0x0C },
	{ 0x034B, 0x2F },
	{ 0x0381, 0x01 },
	{ 0x0383, 0x01 },
	{ 0x0385, 0x01 },
	{ 0x0387, 0x01 },
	{ 0x0900, 0x00 },
	{ 0x0901, 0x11 },
	{ 0x0401, 0x00 },
	{ 0x0404, 0x00 },
	{ 0x0405, 0x10 },
	{ 0x0408, 0x00 },
	{ 0x0409, 0x00 },
	{ 0x040A, 0x00 },
	{ 0x040B, 0x00 },
	{ 0x040C, 0x10 },
	{ 0x040D, 0x70 },
	{ 0x040E, 0x0C },
	{ 0x040F, 0x30 },
	{ 0x3038, 0x00 },
	{ 0x303A, 0x00 },
	{ 0x303B, 0x10 },
	{ 0x300D, 0x00 },
	{ 0x034C, 0x10 },
	{ 0x034D, 0x70 },
	{ 0x034E, 0x0C },
	{ 0x034F, 0x30 },
	{ 0x0350, 0x00 },
	{ 0x0204, 0x00 },
	{ 0x0205, 0x00 },
	{ 0x020E, 0x01 },
	{ 0x020F, 0x00 },
	{ 0x0210, 0x01 },
	{ 0x0211, 0x00 },
	{ 0x0212, 0x01 },
	{ 0x0213, 0x00 },
	{ 0x0214, 0x01 },
	{ 0x0215, 0x00 },
	{ 0x7BCD, 0x00 },
	{ 0x94DC, 0x20 },
	{ 0x94DD, 0x20 },
	{ 0x94DE, 0x20 },
	{ 0x95DC, 0x20 },
	{ 0x95DD, 0x20 },
	{ 0x95DE, 0x20 },
	{ 0x7FB0, 0x00 },
	{ 0x9010, 0x3E },
	{ 0x9419, 0x50 },
	{ 0x941B, 0x50 },
	{ 0x9519, 0x50 },
	{ 0x951B, 0x50 },
	{ 0x3030, 0x00 },
	{ 0x3032, 0x00 },
	{ 0x0220, 0x00 },
};

static const struct imx258_reg mode_2104_1560_regs[] = {
	{ 0x3051, 0x00 },
	{ 0x6B11, 0xCF },
	{ 0x7FF0, 0x08 },
	{ 0x7FF1, 0x0F },
	{ 0x7FF2, 0x08 },
	{ 0x7FF3, 0x1B },
	{ 0x7FF4, 0x23 },
	{ 0x7FF5, 0x60 },
	{ 0x7FF6, 0x00 },
	{ 0x7FF7, 0x01 },
	{ 0x7FF8, 0x00 },
	{ 0x7FF9, 0x78 },
	{ 0x7FFA, 0x00 },
	{ 0x7FFB, 0x00 },
	{ 0x7FFC, 0x00 },
	{ 0x7FFD, 0x00 },
	{ 0x7FFE, 0x00 },
	{ 0x7FFF, 0x03 },
	{ 0x7F76, 0x03 },
	{ 0x7F77, 0xFE },
	{ 0x7FA8, 0x03 },
	{ 0x7FA9, 0xFE },
	{ 0x7B24, 0x81 },
	{ 0x6564, 0x07 },
	{ 0x6B0D, 0x41 },
	{ 0x653D, 0x04 },
	{ 0x6B05, 0x8C },
	{ 0x6B06, 0xF9 },
	{ 0x6B08, 0x65 },
	{ 0x6B09, 0xFC },
	{ 0x6B0A, 0xCF },
	{ 0x6B0B, 0xD2 },
	{ 0x6700, 0x0E },
	{ 0x6707, 0x0E },
	{ 0x9104, 0x00 },
	{ 0x4648, 0x7F },
	{ 0x7420, 0x00 },
	{ 0x7421, 0x1C },
	{ 0x7422, 0x00 },
	{ 0x7423, 0xD7 },
	{ 0x5F04, 0x00 },
	{ 0x5F05, 0xED },
	{ 0x0112, 0x0A },
	{ 0x0113, 0x0A },
	{ 0x0342, 0x14 },
	{ 0x0343, 0xE8 },
	{ 0x0344, 0x00 },
	{ 0x0345, 0x00 },
	{ 0x0346, 0x00 },
	{ 0x0347, 0x00 },
	{ 0x0348, 0x10 },
	{ 0x0349, 0x6F },
	{ 0x034A, 0x0C },
	{ 0x034B, 0x2F },
	{ 0x0381, 0x01 },
	{ 0x0383, 0x01 },
	{ 0x0385, 0x01 },
	{ 0x0387, 0x01 },
	{ 0x0900, 0x01 },
	{ 0x0901, 0x12 },
	{ 0x0401, 0x01 },
	{ 0x0404, 0x00 },
	{ 0x0405, 0x20 },
	{ 0x0408, 0x00 },
	{ 0x0409, 0x00 },
	{ 0x040A, 0x00 },
	{ 0x040B, 0x00 },
	{ 0x040C, 0x10 },
	{ 0x040D, 0x70 },
	{ 0x040E, 0x06 },
	{ 0x040F, 0x18 },
	{ 0x3038, 0x00 },
	{ 0x303A, 0x00 },
	{ 0x303B, 0x10 },
	{ 0x300D, 0x00 },
	{ 0x034C, 0x08 },
	{ 0x034D, 0x38 },
	{ 0x034E, 0x06 },
	{ 0x034F, 0x18 },
	{ 0x0350, 0x00 },
	{ 0x0204, 0x00 },
	{ 0x0205, 0x00 },
	{ 0x020E, 0x01 },
	{ 0x020F, 0x00 },
	{ 0x0210, 0x01 },
	{ 0x0211, 0x00 },
	{ 0x0212, 0x01 },
	{ 0x0213, 0x00 },
	{ 0x0214, 0x01 },
	{ 0x0215, 0x00 },
	{ 0x7BCD, 0x01 },
	{ 0x94DC, 0x20 },
	{ 0x94DD, 0x20 },
	{ 0x94DE, 0x20 },
	{ 0x95DC, 0x20 },
	{ 0x95DD, 0x20 },
	{ 0x95DE, 0x20 },
	{ 0x7FB0, 0x00 },
	{ 0x9010, 0x3E },
	{ 0x9419, 0x50 },
	{ 0x941B, 0x50 },
	{ 0x9519, 0x50 },
	{ 0x951B, 0x50 },
	{ 0x3030, 0x00 },
	{ 0x3032, 0x00 },
	{ 0x0220, 0x00 },
};

static const struct imx258_reg mode_1048_780_regs[] = {
	{ 0x3051, 0x00 },
	{ 0x6B11, 0xCF },
	{ 0x7FF0, 0x08 },
	{ 0x7FF1, 0x0F },
	{ 0x7FF2, 0x08 },
	{ 0x7FF3, 0x1B },
	{ 0x7FF4, 0x23 },
	{ 0x7FF5, 0x60 },
	{ 0x7FF6, 0x00 },
	{ 0x7FF7, 0x01 },
	{ 0x7FF8, 0x00 },
	{ 0x7FF9, 0x78 },
	{ 0x7FFA, 0x00 },
	{ 0x7FFB, 0x00 },
	{ 0x7FFC, 0x00 },
	{ 0x7FFD, 0x00 },
	{ 0x7FFE, 0x00 },
	{ 0x7FFF, 0x03 },
	{ 0x7F76, 0x03 },
	{ 0x7F77, 0xFE },
	{ 0x7FA8, 0x03 },
	{ 0x7FA9, 0xFE },
	{ 0x7B24, 0x81 },
	{ 0x6564, 0x07 },
	{ 0x6B0D, 0x41 },
	{ 0x653D, 0x04 },
	{ 0x6B05, 0x8C },
	{ 0x6B06, 0xF9 },
	{ 0x6B08, 0x65 },
	{ 0x6B09, 0xFC },
	{ 0x6B0A, 0xCF },
	{ 0x6B0B, 0xD2 },
	{ 0x6700, 0x0E },
	{ 0x6707, 0x0E },
	{ 0x9104, 0x00 },
	{ 0x4648, 0x7F },
	{ 0x7420, 0x00 },
	{ 0x7421, 0x1C },
	{ 0x7422, 0x00 },
	{ 0x7423, 0xD7 },
	{ 0x5F04, 0x00 },
	{ 0x5F05, 0xED },
	{ 0x0112, 0x0A },
	{ 0x0113, 0x0A },
	{ 0x0342, 0x14 },
	{ 0x0343, 0xE8 },
	{ 0x0344, 0x00 },
	{ 0x0345, 0x00 },
	{ 0x0346, 0x00 },
	{ 0x0347, 0x00 },
	{ 0x0348, 0x10 },
	{ 0x0349, 0x6F },
	{ 0x034A, 0x0C },
	{ 0x034B, 0x2F },
	{ 0x0381, 0x01 },
	{ 0x0383, 0x01 },
	{ 0x0385, 0x01 },
	{ 0x0387, 0x01 },
	{ 0x0900, 0x01 },
	{ 0x0901, 0x14 },
	{ 0x0401, 0x01 },
	{ 0x0404, 0x00 },
	{ 0x0405, 0x40 },
	{ 0x0408, 0x00 },
	{ 0x0409, 0x00 },
	{ 0x040A, 0x00 },
	{ 0x040B, 0x00 },
	{ 0x040C, 0x10 },
	{ 0x040D, 0x70 },
	{ 0x040E, 0x03 },
	{ 0x040F, 0x0C },
	{ 0x3038, 0x00 },
	{ 0x303A, 0x00 },
	{ 0x303B, 0x10 },
	{ 0x300D, 0x00 },
	{ 0x034C, 0x04 },
	{ 0x034D, 0x18 },
	{ 0x034E, 0x03 },
	{ 0x034F, 0x0C },
	{ 0x0350, 0x00 },
	{ 0x0204, 0x00 },
	{ 0x0205, 0x00 },
	{ 0x020E, 0x01 },
	{ 0x020F, 0x00 },
	{ 0x0210, 0x01 },
	{ 0x0211, 0x00 },
	{ 0x0212, 0x01 },
	{ 0x0213, 0x00 },
	{ 0x0214, 0x01 },
	{ 0x0215, 0x00 },
	{ 0x7BCD, 0x00 },
	{ 0x94DC, 0x20 },
	{ 0x94DD, 0x20 },
	{ 0x94DE, 0x20 },
	{ 0x95DC, 0x20 },
	{ 0x95DD, 0x20 },
	{ 0x95DE, 0x20 },
	{ 0x7FB0, 0x00 },
	{ 0x9010, 0x3E },
	{ 0x9419, 0x50 },
	{ 0x941B, 0x50 },
	{ 0x9519, 0x50 },
	{ 0x951B, 0x50 },
	{ 0x3030, 0x00 },
	{ 0x3032, 0x00 },
	{ 0x0220, 0x00 },
};

struct imx258_variant_cfg {
	const struct imx258_reg *regs;
	unsigned int num_regs;
};

static const struct imx258_reg imx258_cfg_regs[] = {
	{ 0x3052, 0x00 },
	{ 0x4E21, 0x14 },
	{ 0x7B25, 0x00 },
};

static const struct imx258_variant_cfg imx258_cfg = {
	.regs = imx258_cfg_regs,
	.num_regs = ARRAY_SIZE(imx258_cfg_regs),
};

static const struct imx258_reg imx258_pdaf_cfg_regs[] = {
	{ 0x3052, 0x01 },
	{ 0x4E21, 0x10 },
	{ 0x7B25, 0x01 },
};

static const struct imx258_variant_cfg imx258_pdaf_cfg = {
	.regs = imx258_pdaf_cfg_regs,
	.num_regs = ARRAY_SIZE(imx258_pdaf_cfg_regs),
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
	MEDIA_BUS_FMT_SBGGR10_1X10
};

static const char * const imx258_test_pattern_menu[] = {
	"Disabled",
	"Solid Colour",
	"Eight Vertical Colour Bars",
	"Colour Bars With Fade to Grey",
	"Pseudorandom Sequence (PN9)",
};

/* regulator supplies */
static const char * const imx258_supply_name[] = {
	/* Supplies can be enabled in any order */
	"vana",  /* Analog (2.8V) supply */
	"vdig",  /* Digital Core (1.05V) supply */
	"vif",  /* IF (1.8V) supply */
};

#define IMX258_NUM_SUPPLIES ARRAY_SIZE(imx258_supply_name)

enum {
	IMX258_LINK_FREQ_1267MBPS,
	IMX258_LINK_FREQ_640MBPS,
};

/*
 * Pixel rate does not necessarily relate to link frequency on this sensor as
 * there is a FIFO between the pixel array pipeline and the MIPI serializer.
 * The recommendation from Sony is that the pixel array is always run with a
 * line length of 5352 pixels, which means that there is a large amount of
 * blanking time for the 1048x780 mode. There is no need to replicate this
 * blanking on the CSI2 bus, and the configuration of register 0x0301 allows the
 * divider to be altered.
 *
 * The actual factor between link frequency and pixel rate is in the
 * imx258_link_cfg, so use this to convert between the two.
 * bits per pixel being 10, and D-PHY being DDR is assumed by this function, so
 * the value is only the combination of number of lanes and pixel clock divider.
 */
static u64 link_freq_to_pixel_rate(u64 f, const struct imx258_link_cfg *link_cfg)
{
	f *= 2 * link_cfg->lf_to_pix_rate_factor;
	do_div(f, 10);

	return f;
}

/* Menu items for LINK_FREQ V4L2 control */
/* Configurations for supported link frequencies */
#define IMX258_LINK_FREQ_634MHZ	633600000ULL
#define IMX258_LINK_FREQ_320MHZ	320000000ULL

static const s64 link_freq_menu_items_19_2[] = {
	IMX258_LINK_FREQ_634MHZ,
	IMX258_LINK_FREQ_320MHZ,
};

/* Configurations for supported link frequencies */
#define IMX258_LINK_FREQ_636MHZ	636000000ULL
#define IMX258_LINK_FREQ_321MHZ	321000000ULL

static const s64 link_freq_menu_items_24[] = {
	IMX258_LINK_FREQ_636MHZ,
	IMX258_LINK_FREQ_321MHZ,
};

#define REGS(_list) { .num_of_regs = ARRAY_SIZE(_list), .regs = _list, }

/* Link frequency configs */
static const struct imx258_link_freq_config link_freq_configs_19_2[] = {
	[IMX258_LINK_FREQ_1267MBPS] = {
		.pixels_per_line = IMX258_PPL_DEFAULT,
		.link_cfg = {
			[IMX258_2_LANE_MODE] = {
				.lf_to_pix_rate_factor = 2 * 2,
				.reg_list = REGS(mipi_1267mbps_19_2mhz_2l),
			},
			[IMX258_4_LANE_MODE] = {
				.lf_to_pix_rate_factor = 4,
				.reg_list = REGS(mipi_1267mbps_19_2mhz_4l),
			},
		}
	},
	[IMX258_LINK_FREQ_640MBPS] = {
		.pixels_per_line = IMX258_PPL_DEFAULT,
		.link_cfg = {
			[IMX258_2_LANE_MODE] = {
				.lf_to_pix_rate_factor = 2,
				.reg_list = REGS(mipi_640mbps_19_2mhz_2l),
			},
			[IMX258_4_LANE_MODE] = {
				.lf_to_pix_rate_factor = 4,
				.reg_list = REGS(mipi_640mbps_19_2mhz_4l),
			},
		}
	},
};

static const struct imx258_link_freq_config link_freq_configs_24[] = {
	[IMX258_LINK_FREQ_1267MBPS] = {
		.pixels_per_line = IMX258_PPL_DEFAULT,
		.link_cfg = {
			[IMX258_2_LANE_MODE] = {
				.lf_to_pix_rate_factor = 2,
				.reg_list = REGS(mipi_1272mbps_24mhz_2l),
			},
			[IMX258_4_LANE_MODE] = {
				.lf_to_pix_rate_factor = 4,
				.reg_list = REGS(mipi_1272mbps_24mhz_4l),
			},
		}
	},
	[IMX258_LINK_FREQ_640MBPS] = {
		.pixels_per_line = IMX258_PPL_DEFAULT,
		.link_cfg = {
			[IMX258_2_LANE_MODE] = {
				.lf_to_pix_rate_factor = 2 * 2,
				.reg_list = REGS(mipi_642mbps_24mhz_2l),
			},
			[IMX258_4_LANE_MODE] = {
				.lf_to_pix_rate_factor = 4,
				.reg_list = REGS(mipi_642mbps_24mhz_4l),
			},
		}
	},
};

/* Mode configs */
static const struct imx258_mode supported_modes[] = {
	{
		.width = 4208,
		.height = 3120,
		.vts_def = IMX258_VTS_30FPS,
		.vts_min = IMX258_VTS_30FPS,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_4208x3120_regs),
			.regs = mode_4208x3120_regs,
		},
		.link_freq_index = IMX258_LINK_FREQ_1267MBPS,
		.crop = {
			.left = IMX258_PIXEL_ARRAY_LEFT,
			.top = IMX258_PIXEL_ARRAY_TOP,
			.width = 4208,
			.height = 3120,
		},
	},
	{
		.width = 2104,
		.height = 1560,
		.vts_def = IMX258_VTS_30FPS_2K,
		.vts_min = IMX258_VTS_30FPS_2K,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2104_1560_regs),
			.regs = mode_2104_1560_regs,
		},
		.link_freq_index = IMX258_LINK_FREQ_640MBPS,
		.crop = {
			.left = IMX258_PIXEL_ARRAY_LEFT,
			.top = IMX258_PIXEL_ARRAY_TOP,
			.width = 4208,
			.height = 3120,
		},
	},
	{
		.width = 1048,
		.height = 780,
		.vts_def = IMX258_VTS_30FPS_VGA,
		.vts_min = IMX258_VTS_30FPS_VGA,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1048_780_regs),
			.regs = mode_1048_780_regs,
		},
		.link_freq_index = IMX258_LINK_FREQ_640MBPS,
		.crop = {
			.left = IMX258_PIXEL_ARRAY_LEFT,
			.top = IMX258_PIXEL_ARRAY_TOP,
			.width = 4208,
			.height = 3120,
		},
	},
};

struct imx258 {
	struct v4l2_subdev sd;
	struct media_pad pad;

	const struct imx258_variant_cfg *variant_cfg;

	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;
	/* Current long exposure factor in use. Set through V4L2_CID_VBLANK */
	unsigned int long_exp_shift;

	/* Current mode */
	const struct imx258_mode *cur_mode;

	const struct imx258_link_freq_config *link_freq_configs;
	const s64 *link_freq_menu_items;
	unsigned int lane_mode_idx;
	unsigned int csi2_flags;

	/*
	 * Mutex for serialized access:
	 * Protect sensor module set pad format and start/stop streaming safely.
	 */
	struct mutex mutex;

	/* Streaming on/off */
	bool streaming;

	struct clk *clk;
	struct regulator_bulk_data supplies[IMX258_NUM_SUPPLIES];
};

static inline struct imx258 *to_imx258(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx258, sd);
}

/* Read registers up to 2 at a time */
static int imx258_read_reg(struct imx258 *imx258, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx258->sd);
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
static int imx258_write_reg(struct imx258 *imx258, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx258->sd);
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
static int imx258_write_regs(struct imx258 *imx258,
			     const struct imx258_reg *regs, u32 len)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx258->sd);
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = imx258_write_reg(imx258, regs[i].address, 1,
					regs[i].val);
		if (ret) {
			dev_err_ratelimited(
				&client->dev,
				"Failed to write reg 0x%4.4x. error = %d\n",
				regs[i].address, ret);

			return ret;
		}
	}

	return 0;
}

/* Get bayer order based on flip setting. */
static u32 imx258_get_format_code(struct imx258 *imx258)
{
	unsigned int i;

	lockdep_assert_held(&imx258->mutex);

	i = (imx258->vflip->val ? 2 : 0) |
	    (imx258->hflip->val ? 1 : 0);

	return codes[i];
}
/* Open sub-device */
static int imx258_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx258 *imx258 = to_imx258(sd);
	struct v4l2_mbus_framefmt *try_fmt =
		v4l2_subdev_state_get_format(fh->state, 0);
	struct v4l2_rect *try_crop;

	/* Initialize try_fmt */
	try_fmt->width = supported_modes[0].width;
	try_fmt->height = supported_modes[0].height;
	try_fmt->code = imx258_get_format_code(imx258);
	try_fmt->field = V4L2_FIELD_NONE;

	/* Initialize try_crop */
	try_crop = v4l2_subdev_state_get_crop(fh->state, 0);
	try_crop->left = IMX258_PIXEL_ARRAY_LEFT;
	try_crop->top = IMX258_PIXEL_ARRAY_TOP;
	try_crop->width = IMX258_PIXEL_ARRAY_WIDTH;
	try_crop->height = IMX258_PIXEL_ARRAY_HEIGHT;

	return 0;
}

static int imx258_update_digital_gain(struct imx258 *imx258, u32 len, u32 val)
{
	int ret;

	ret = imx258_write_reg(imx258, IMX258_REG_GR_DIGITAL_GAIN,
				IMX258_REG_VALUE_16BIT,
				val);
	if (ret)
		return ret;
	ret = imx258_write_reg(imx258, IMX258_REG_GB_DIGITAL_GAIN,
				IMX258_REG_VALUE_16BIT,
				val);
	if (ret)
		return ret;
	ret = imx258_write_reg(imx258, IMX258_REG_R_DIGITAL_GAIN,
				IMX258_REG_VALUE_16BIT,
				val);
	if (ret)
		return ret;
	ret = imx258_write_reg(imx258, IMX258_REG_B_DIGITAL_GAIN,
				IMX258_REG_VALUE_16BIT,
				val);
	if (ret)
		return ret;
	return 0;
}

static void imx258_adjust_exposure_range(struct imx258 *imx258)
{
	int exposure_max, exposure_def;

	/* Honour the VBLANK limits when setting exposure. */
	exposure_max = imx258->cur_mode->height + imx258->vblank->val -
		       IMX258_EXPOSURE_OFFSET;
	exposure_def = min(exposure_max, imx258->exposure->val);
	__v4l2_ctrl_modify_range(imx258->exposure, imx258->exposure->minimum,
				 exposure_max, imx258->exposure->step,
				 exposure_def);
}

static int imx258_set_frame_length(struct imx258 *imx258, unsigned int val)
{
	int ret;

	imx258->long_exp_shift = 0;

	while (val > IMX258_VTS_MAX) {
		imx258->long_exp_shift++;
		val >>= 1;
	}

	ret = imx258_write_reg(imx258, IMX258_REG_VTS,
			       IMX258_REG_VALUE_16BIT, val);
	if (ret)
		return ret;

	return imx258_write_reg(imx258, IMX258_LONG_EXP_SHIFT_REG,
				IMX258_REG_VALUE_08BIT, imx258->long_exp_shift);
}

static int imx258_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx258 *imx258 =
		container_of(ctrl->handler, struct imx258, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&imx258->sd);
	int ret = 0;

	/*
	 * The VBLANK control may change the limits of usable exposure, so check
	 * and adjust if necessary.
	 */
	if (ctrl->id == V4L2_CID_VBLANK)
		imx258_adjust_exposure_range(imx258);

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx258_write_reg(imx258, IMX258_REG_ANALOG_GAIN,
				IMX258_REG_VALUE_16BIT,
				ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		ret = imx258_write_reg(imx258, IMX258_REG_EXPOSURE,
				IMX258_REG_VALUE_16BIT,
				ctrl->val >> imx258->long_exp_shift);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = imx258_update_digital_gain(imx258, IMX258_REG_VALUE_16BIT,
				ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = imx258_write_reg(imx258, IMX258_REG_TEST_PATTERN,
				IMX258_REG_VALUE_16BIT,
				ctrl->val);
		break;
	case V4L2_CID_WIDE_DYNAMIC_RANGE:
		if (!ctrl->val) {
			ret = imx258_write_reg(imx258, IMX258_REG_HDR,
					       IMX258_REG_VALUE_08BIT,
					       IMX258_HDR_RATIO_MIN);
		} else {
			ret = imx258_write_reg(imx258, IMX258_REG_HDR,
					       IMX258_REG_VALUE_08BIT,
					       IMX258_HDR_ON);
			if (ret)
				break;
			ret = imx258_write_reg(imx258, IMX258_REG_HDR_RATIO,
					       IMX258_REG_VALUE_08BIT,
					       BIT(IMX258_HDR_RATIO_MAX));
		}
		break;
	case V4L2_CID_VBLANK:
		ret = imx258_set_frame_length(imx258,
					      imx258->cur_mode->height + ctrl->val);
		break;
	case V4L2_CID_VFLIP:
	case V4L2_CID_HFLIP:
		ret = imx258_write_reg(imx258, REG_MIRROR_FLIP_CONTROL,
				       IMX258_REG_VALUE_08BIT,
				       (imx258->hflip->val ?
					REG_CONFIG_MIRROR_HFLIP : 0) |
				       (imx258->vflip->val ?
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

static const struct v4l2_ctrl_ops imx258_ctrl_ops = {
	.s_ctrl = imx258_set_ctrl,
};

static int imx258_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx258 *imx258 = to_imx258(sd);

	/* Only one bayer format (10 bit) is supported */
	if (code->index > 0)
		return -EINVAL;

	code->code = imx258_get_format_code(imx258);

	return 0;
}

static int imx258_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx258 *imx258 = to_imx258(sd);
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != imx258_get_format_code(imx258))
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static void imx258_update_pad_format(struct imx258 *imx258,
				     const struct imx258_mode *mode,
				     struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = imx258_get_format_code(imx258);
	fmt->format.field = V4L2_FIELD_NONE;
}

static int __imx258_get_pad_format(struct imx258 *imx258,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_format *fmt)
{
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt->format = *v4l2_subdev_state_get_format(sd_state,
							    fmt->pad);
	else
		imx258_update_pad_format(imx258, imx258->cur_mode, fmt);

	return 0;
}

static int imx258_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx258 *imx258 = to_imx258(sd);
	int ret;

	mutex_lock(&imx258->mutex);
	ret = __imx258_get_pad_format(imx258, sd_state, fmt);
	mutex_unlock(&imx258->mutex);

	return ret;
}

static int imx258_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx258 *imx258 = to_imx258(sd);
	const struct imx258_link_freq_config *link_freq_cfgs;
	const struct imx258_link_cfg *link_cfg;
	struct v4l2_mbus_framefmt *framefmt;
	const struct imx258_mode *mode;
	s32 vblank_def;
	s32 vblank_min;
	s64 h_blank;
	s64 pixel_rate;
	s64 link_freq;

	mutex_lock(&imx258->mutex);

	fmt->format.code = imx258_get_format_code(imx258);

	mode = v4l2_find_nearest_size(supported_modes,
		ARRAY_SIZE(supported_modes), width, height,
		fmt->format.width, fmt->format.height);
	imx258_update_pad_format(imx258, mode, fmt);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_state_get_format(sd_state, fmt->pad);
		*framefmt = fmt->format;
	} else {
		imx258->cur_mode = mode;
		__v4l2_ctrl_s_ctrl(imx258->link_freq, mode->link_freq_index);

		link_freq = imx258->link_freq_menu_items[mode->link_freq_index];
		link_freq_cfgs =
			&imx258->link_freq_configs[mode->link_freq_index];

		link_cfg = &link_freq_cfgs->link_cfg[imx258->lane_mode_idx];
		pixel_rate = link_freq_to_pixel_rate(link_freq, link_cfg);
		__v4l2_ctrl_modify_range(imx258->pixel_rate, pixel_rate,
					 pixel_rate, 1, pixel_rate);
		/* Update limits and set FPS to default */
		vblank_def = imx258->cur_mode->vts_def -
			     imx258->cur_mode->height;
		vblank_min = imx258->cur_mode->vts_min -
			     imx258->cur_mode->height;
		__v4l2_ctrl_modify_range(
			imx258->vblank, vblank_min,
			((1 << IMX258_LONG_EXP_SHIFT_MAX) * IMX258_VTS_MAX) -
						imx258->cur_mode->height,
			1, vblank_def);
		__v4l2_ctrl_s_ctrl(imx258->vblank, vblank_def);
		h_blank =
			imx258->link_freq_configs[mode->link_freq_index].pixels_per_line
			 - imx258->cur_mode->width;
		__v4l2_ctrl_modify_range(imx258->hblank, h_blank,
					 h_blank, 1, h_blank);
	}

	mutex_unlock(&imx258->mutex);

	return 0;
}

static const struct v4l2_rect *
__imx258_get_pad_crop(struct imx258 *imx258,
		      struct v4l2_subdev_state *sd_state,
		      unsigned int pad, enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_state_get_crop(sd_state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &imx258->cur_mode->crop;
	}

	return NULL;
}

static int imx258_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP: {
		struct imx258 *imx258 = to_imx258(sd);

		mutex_lock(&imx258->mutex);
		sel->r = *__imx258_get_pad_crop(imx258, sd_state, sel->pad,
						sel->which);
		mutex_unlock(&imx258->mutex);

		return 0;
	}

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX258_NATIVE_WIDTH;
		sel->r.height = IMX258_NATIVE_HEIGHT;

		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = IMX258_PIXEL_ARRAY_LEFT;
		sel->r.top = IMX258_PIXEL_ARRAY_TOP;
		sel->r.width = IMX258_PIXEL_ARRAY_WIDTH;
		sel->r.height = IMX258_PIXEL_ARRAY_HEIGHT;

		return 0;
	}

	return -EINVAL;
}

/* Start streaming */
static int imx258_start_streaming(struct imx258 *imx258)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx258->sd);
	const struct imx258_reg_list *reg_list;
	const struct imx258_link_freq_config *link_freq_cfg;
	int ret, link_freq_index;

	ret = imx258_write_reg(imx258, IMX258_REG_RESET, IMX258_REG_VALUE_08BIT,
			       0x01);
	if (ret) {
		dev_err(&client->dev, "%s failed to reset sensor\n", __func__);
		return ret;
	}
	usleep_range(10000, 15000);

	/* Setup PLL */
	link_freq_index = imx258->cur_mode->link_freq_index;
	link_freq_cfg = &imx258->link_freq_configs[link_freq_index];

	reg_list = &link_freq_cfg->link_cfg[imx258->lane_mode_idx].reg_list;
	ret = imx258_write_regs(imx258, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(&client->dev, "%s failed to set plls\n", __func__);
		return ret;
	}

	ret = imx258_write_regs(imx258, imx258->variant_cfg->regs,
				imx258->variant_cfg->num_regs);
	if (ret) {
		dev_err(&client->dev, "%s failed to set variant config\n",
			__func__);
		return ret;
	}

	ret = imx258_write_reg(imx258, IMX258_CLK_BLANK_STOP,
			       IMX258_REG_VALUE_08BIT,
			       imx258->csi2_flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK ?
			       1 : 0);
	if (ret) {
		dev_err(&client->dev, "%s failed to set clock lane mode\n", __func__);
		return ret;
	}

	/* Apply default values of current mode */
	reg_list = &imx258->cur_mode->reg_list;
	ret = imx258_write_regs(imx258, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(&client->dev, "%s failed to set mode\n", __func__);
		return ret;
	}

	/* Apply customized values from user */
	ret =  __v4l2_ctrl_handler_setup(imx258->sd.ctrl_handler);
	if (ret)
		return ret;

	/* set stream on register */
	return imx258_write_reg(imx258, IMX258_REG_MODE_SELECT,
				IMX258_REG_VALUE_08BIT,
				IMX258_MODE_STREAMING);
}

/* Stop streaming */
static int imx258_stop_streaming(struct imx258 *imx258)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx258->sd);
	int ret;

	/* set stream off register */
	ret = imx258_write_reg(imx258, IMX258_REG_MODE_SELECT,
		IMX258_REG_VALUE_08BIT, IMX258_MODE_STANDBY);
	if (ret)
		dev_err(&client->dev, "%s failed to set stream\n", __func__);

	/*
	 * Return success even if it was an error, as there is nothing the
	 * caller can do about it.
	 */
	return 0;
}

static int imx258_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx258 *imx258 = to_imx258(sd);
	int ret;

	ret = regulator_bulk_enable(IMX258_NUM_SUPPLIES,
				    imx258->supplies);
	if (ret) {
		dev_err(dev, "%s: failed to enable regulators\n",
			__func__);
		return ret;
	}

	ret = clk_prepare_enable(imx258->clk);
	if (ret) {
		dev_err(dev, "failed to enable clock\n");
		regulator_bulk_disable(IMX258_NUM_SUPPLIES, imx258->supplies);
	}

	return ret;
}

static int imx258_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx258 *imx258 = to_imx258(sd);

	clk_disable_unprepare(imx258->clk);
	regulator_bulk_disable(IMX258_NUM_SUPPLIES, imx258->supplies);

	return 0;
}

static int imx258_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx258 *imx258 = to_imx258(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&imx258->mutex);
	if (imx258->streaming == enable) {
		mutex_unlock(&imx258->mutex);
		return 0;
	}

	if (enable) {
		ret = pm_runtime_resume_and_get(&client->dev);
		if (ret < 0)
			goto err_unlock;

		/*
		 * Apply default & customized values
		 * and then start streaming.
		 */
		ret = imx258_start_streaming(imx258);
		if (ret)
			goto err_rpm_put;
	} else {
		imx258_stop_streaming(imx258);
		pm_runtime_put(&client->dev);
	}

	imx258->streaming = enable;
	mutex_unlock(&imx258->mutex);

	return ret;

err_rpm_put:
	pm_runtime_put(&client->dev);
err_unlock:
	mutex_unlock(&imx258->mutex);

	return ret;
}

static int __maybe_unused imx258_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx258 *imx258 = to_imx258(sd);

	if (imx258->streaming)
		imx258_stop_streaming(imx258);

	return 0;
}

static int __maybe_unused imx258_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx258 *imx258 = to_imx258(sd);
	int ret;

	if (imx258->streaming) {
		ret = imx258_start_streaming(imx258);
		if (ret)
			goto error;
	}

	return 0;

error:
	imx258_stop_streaming(imx258);
	imx258->streaming = 0;
	return ret;
}

/* Verify chip ID */
static int imx258_identify_module(struct imx258 *imx258)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx258->sd);
	int ret;
	u32 val;

	ret = imx258_read_reg(imx258, IMX258_REG_CHIP_ID,
			      IMX258_REG_VALUE_16BIT, &val);
	if (ret) {
		dev_err(&client->dev, "failed to read chip id %x\n",
			IMX258_CHIP_ID);
		return ret;
	}

	if (val != IMX258_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%x\n",
			IMX258_CHIP_ID, val);
		return -EIO;
	}

	return 0;
}

static const struct v4l2_subdev_video_ops imx258_video_ops = {
	.s_stream = imx258_set_stream,
};

static const struct v4l2_subdev_pad_ops imx258_pad_ops = {
	.enum_mbus_code = imx258_enum_mbus_code,
	.get_fmt = imx258_get_pad_format,
	.set_fmt = imx258_set_pad_format,
	.enum_frame_size = imx258_enum_frame_size,
	.get_selection = imx258_get_selection,
};

static const struct v4l2_subdev_ops imx258_subdev_ops = {
	.video = &imx258_video_ops,
	.pad = &imx258_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx258_internal_ops = {
	.open = imx258_open,
};

/* Initialize control handlers */
static int imx258_init_controls(struct imx258 *imx258)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx258->sd);
	const struct imx258_link_freq_config *link_freq_cfgs;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	const struct imx258_link_cfg *link_cfg;
	s64 vblank_def;
	s64 vblank_min;
	s64 pixel_rate;
	int ret;

	ctrl_hdlr = &imx258->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 13);
	if (ret)
		return ret;

	mutex_init(&imx258->mutex);
	ctrl_hdlr->lock = &imx258->mutex;
	imx258->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr,
				&imx258_ctrl_ops,
				V4L2_CID_LINK_FREQ,
				ARRAY_SIZE(link_freq_menu_items_19_2) - 1,
				0,
				imx258->link_freq_menu_items);

	if (imx258->link_freq)
		imx258->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx258->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx258_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 1);
	if (imx258->hflip)
		imx258->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx258->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx258_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 1);
	if (imx258->vflip)
		imx258->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	link_freq_cfgs = &imx258->link_freq_configs[0];
	link_cfg = link_freq_cfgs[imx258->lane_mode_idx].link_cfg;
	pixel_rate = link_freq_to_pixel_rate(imx258->link_freq_menu_items[0],
					     link_cfg);

	/* By default, PIXEL_RATE is read only */
	imx258->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &imx258_ctrl_ops,
				V4L2_CID_PIXEL_RATE,
				pixel_rate, pixel_rate,
				1, pixel_rate);


	vblank_def = imx258->cur_mode->vts_def - imx258->cur_mode->height;
	vblank_min = imx258->cur_mode->vts_min - imx258->cur_mode->height;
	imx258->vblank = v4l2_ctrl_new_std(
				ctrl_hdlr, &imx258_ctrl_ops, V4L2_CID_VBLANK,
				vblank_min,
				IMX258_VTS_MAX - imx258->cur_mode->height, 1,
				vblank_def);

	imx258->hblank = v4l2_ctrl_new_std(
				ctrl_hdlr, &imx258_ctrl_ops, V4L2_CID_HBLANK,
				IMX258_PPL_DEFAULT - imx258->cur_mode->width,
				IMX258_PPL_DEFAULT - imx258->cur_mode->width,
				1,
				IMX258_PPL_DEFAULT - imx258->cur_mode->width);

	if (imx258->hblank)
		imx258->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx258->exposure = v4l2_ctrl_new_std(
				ctrl_hdlr, &imx258_ctrl_ops,
				V4L2_CID_EXPOSURE, IMX258_EXPOSURE_MIN,
				IMX258_EXPOSURE_MAX, IMX258_EXPOSURE_STEP,
				IMX258_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx258_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
				IMX258_ANA_GAIN_MIN, IMX258_ANA_GAIN_MAX,
				IMX258_ANA_GAIN_STEP, IMX258_ANA_GAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx258_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
				IMX258_DGTL_GAIN_MIN, IMX258_DGTL_GAIN_MAX,
				IMX258_DGTL_GAIN_STEP,
				IMX258_DGTL_GAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx258_ctrl_ops, V4L2_CID_WIDE_DYNAMIC_RANGE,
				0, 1, 1, IMX258_HDR_RATIO_DEFAULT);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx258_ctrl_ops,
				V4L2_CID_TEST_PATTERN,
				ARRAY_SIZE(imx258_test_pattern_menu) - 1,
				0, 0, imx258_test_pattern_menu);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
				__func__, ret);
		goto error;
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx258_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	imx258->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	mutex_destroy(&imx258->mutex);

	return ret;
}

static void imx258_free_controls(struct imx258 *imx258)
{
	v4l2_ctrl_handler_free(imx258->sd.ctrl_handler);
	mutex_destroy(&imx258->mutex);
}

static int imx258_get_regulators(struct imx258 *imx258,
				 struct i2c_client *client)
{
	unsigned int i;

	for (i = 0; i < IMX258_NUM_SUPPLIES; i++)
		imx258->supplies[i].supply = imx258_supply_name[i];

	return devm_regulator_bulk_get(&client->dev,
				       IMX258_NUM_SUPPLIES,
				       imx258->supplies);
}

static const struct of_device_id imx258_dt_ids[] = {
	{ .compatible = "sony,imx258", .data = &imx258_cfg },
	{ .compatible = "sony,imx258-pdaf", .data = &imx258_pdaf_cfg },
	{ /* sentinel */ }
};

static int imx258_probe(struct i2c_client *client)
{
	struct imx258 *imx258;
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	const struct of_device_id *match;
	int ret;
	u32 val = 0;

	imx258 = devm_kzalloc(&client->dev, sizeof(*imx258), GFP_KERNEL);
	if (!imx258)
		return -ENOMEM;

	ret = imx258_get_regulators(imx258, client);
	if (ret)
		return ret;

	imx258->clk = devm_clk_get_optional(&client->dev, NULL);
	if (IS_ERR(imx258->clk))
		return dev_err_probe(&client->dev, PTR_ERR(imx258->clk),
				     "error getting clock\n");
	if (!imx258->clk) {
		dev_dbg(&client->dev,
			"no clock provided, using clock-frequency property\n");

		device_property_read_u32(&client->dev, "clock-frequency", &val);
	} else if (IS_ERR(imx258->clk)) {
		return dev_err_probe(&client->dev, PTR_ERR(imx258->clk),
				     "error getting clock\n");
	} else {
		val = clk_get_rate(imx258->clk);
	}

	switch (val) {
	case 19200000:
		imx258->link_freq_configs = link_freq_configs_19_2;
		imx258->link_freq_menu_items = link_freq_menu_items_19_2;
		break;
	case 24000000:
		imx258->link_freq_configs = link_freq_configs_24;
		imx258->link_freq_menu_items = link_freq_menu_items_24;
		break;
	default:
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
	switch (ep.bus.mipi_csi2.num_data_lanes) {
	case 2:
		imx258->lane_mode_idx = IMX258_2_LANE_MODE;
		break;
	case 4:
		imx258->lane_mode_idx = IMX258_4_LANE_MODE;
		break;
	default:
		dev_err(&client->dev, "Invalid data lanes: %u\n",
			ep.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto error_endpoint_poweron;
	}

	imx258->csi2_flags = ep.bus.mipi_csi2.flags;

	match = i2c_of_match_device(imx258_dt_ids, client);
	if (!match || !match->data)
		imx258->variant_cfg = &imx258_cfg;
	else
		imx258->variant_cfg =
			(const struct imx258_variant_cfg *)match->data;

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&imx258->sd, client, &imx258_subdev_ops);

	/* Will be powered off via pm_runtime_idle */
	ret = imx258_power_on(&client->dev);
	if (ret)
		goto error_endpoint_poweron;

	/* Check module identity */
	ret = imx258_identify_module(imx258);
	if (ret)
		goto error_identify;

	/* Set default mode to max resolution */
	imx258->cur_mode = &supported_modes[0];

	ret = imx258_init_controls(imx258);
	if (ret)
		goto error_identify;

	/* Initialize subdev */
	imx258->sd.internal_ops = &imx258_internal_ops;
	imx258->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx258->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	imx258->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx258->sd.entity, 1, &imx258->pad);
	if (ret)
		goto error_handler_free;

	ret = v4l2_async_register_subdev_sensor(&imx258->sd);
	if (ret < 0)
		goto error_media_entity;

	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_idle(&client->dev);

	return 0;

error_media_entity:
	media_entity_cleanup(&imx258->sd.entity);

error_handler_free:
	imx258_free_controls(imx258);

error_identify:
	imx258_power_off(&client->dev);

error_endpoint_poweron:
	v4l2_fwnode_endpoint_free(&ep);

	return ret;
}

static void imx258_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx258 *imx258 = to_imx258(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	imx258_free_controls(imx258);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx258_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct dev_pm_ops imx258_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(imx258_suspend, imx258_resume)
	SET_RUNTIME_PM_OPS(imx258_power_off, imx258_power_on, NULL)
};

#ifdef CONFIG_ACPI
static const struct acpi_device_id imx258_acpi_ids[] = {
	{ "SONY258A" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(acpi, imx258_acpi_ids);
#endif

MODULE_DEVICE_TABLE(of, imx258_dt_ids);

static struct i2c_driver imx258_i2c_driver = {
	.driver = {
		.name = "imx258",
		.pm = &imx258_pm_ops,
		.acpi_match_table = ACPI_PTR(imx258_acpi_ids),
		.of_match_table	= imx258_dt_ids,
	},
	.probe = imx258_probe,
	.remove = imx258_remove,
};

module_i2c_driver(imx258_i2c_driver);

MODULE_AUTHOR("Yeh, Andy <andy.yeh@intel.com>");
MODULE_AUTHOR("Chiang, Alan");
MODULE_AUTHOR("Chen, Jason");
MODULE_DESCRIPTION("Sony IMX258 sensor driver");
MODULE_LICENSE("GPL v2");
