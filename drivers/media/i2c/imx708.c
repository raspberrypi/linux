// SPDX-License-Identifier: GPL-2.0
/*
 * A V4L2 driver for Sony IMX708 cameras.
 * Copyright (C) 2022, Raspberry Pi Ltd
 *
 * Based on Sony imx477 camera driver
 * Copyright (C) 2020 Raspberry Pi Ltd
 */
#include <asm/unaligned.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/mipi-csi2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

/*
 * Parameter to adjust Quad Bayer re-mosaic broken line correction
 * strength, used in full-resolution mode only. Set zero to disable.
 */
static int qbc_adjust = 2;
module_param(qbc_adjust, int, 0644);
MODULE_PARM_DESC(qbc_adjust, "Quad Bayer broken line correction strength [0,2-5]");

#define IMX708_REG_VALUE_08BIT		1
#define IMX708_REG_VALUE_16BIT		2

/* Chip ID */
#define IMX708_REG_CHIP_ID		0x0016
#define IMX708_CHIP_ID			0x0708

#define IMX708_REG_MODE_SELECT		0x0100
#define IMX708_MODE_STANDBY		0x00
#define IMX708_MODE_STREAMING		0x01

#define IMX708_REG_ORIENTATION		0x101

#define IMX708_INCLK_FREQ		24000000

/* Default initial pixel rate, will get updated for each mode. */
#define IMX708_INITIAL_PIXEL_RATE	590000000

/* V_TIMING internal */
#define IMX708_REG_FRAME_LENGTH		0x0340
#define IMX708_FRAME_LENGTH_MAX		0xffff

/* Long exposure multiplier */
#define IMX708_LONG_EXP_SHIFT_MAX	7
#define IMX708_LONG_EXP_SHIFT_REG	0x3100

/* Exposure control */
#define IMX708_REG_EXPOSURE		0x0202
#define IMX708_EXPOSURE_OFFSET		48
#define IMX708_EXPOSURE_DEFAULT		0x640
#define IMX708_EXPOSURE_STEP		1
#define IMX708_EXPOSURE_MIN		1
#define IMX708_EXPOSURE_MAX		(IMX708_FRAME_LENGTH_MAX - \
					 IMX708_EXPOSURE_OFFSET)

/* Analog gain control */
#define IMX708_REG_ANALOG_GAIN		0x0204
#define IMX708_ANA_GAIN_MIN		112
#define IMX708_ANA_GAIN_MAX		960
#define IMX708_ANA_GAIN_STEP		1
#define IMX708_ANA_GAIN_DEFAULT	   IMX708_ANA_GAIN_MIN

/* Digital gain control */
#define IMX708_REG_DIGITAL_GAIN		0x020e
#define IMX708_DGTL_GAIN_MIN		0x0100
#define IMX708_DGTL_GAIN_MAX		0xffff
#define IMX708_DGTL_GAIN_DEFAULT	0x0100
#define IMX708_DGTL_GAIN_STEP		1

/* Colour balance controls */
#define IMX708_REG_COLOUR_BALANCE_RED   0x0b90
#define IMX708_REG_COLOUR_BALANCE_BLUE	0x0b92
#define IMX708_COLOUR_BALANCE_MIN	0x01
#define IMX708_COLOUR_BALANCE_MAX	0xffff
#define IMX708_COLOUR_BALANCE_STEP	0x01
#define IMX708_COLOUR_BALANCE_DEFAULT	0x100

/* Test Pattern Control */
#define IMX708_REG_TEST_PATTERN		0x0600
#define IMX708_TEST_PATTERN_DISABLE	0
#define IMX708_TEST_PATTERN_SOLID_COLOR	1
#define IMX708_TEST_PATTERN_COLOR_BARS	2
#define IMX708_TEST_PATTERN_GREY_COLOR	3
#define IMX708_TEST_PATTERN_PN9		4

/* Test pattern colour components */
#define IMX708_REG_TEST_PATTERN_R	0x0602
#define IMX708_REG_TEST_PATTERN_GR	0x0604
#define IMX708_REG_TEST_PATTERN_B	0x0606
#define IMX708_REG_TEST_PATTERN_GB	0x0608
#define IMX708_TEST_PATTERN_COLOUR_MIN	0
#define IMX708_TEST_PATTERN_COLOUR_MAX	0x0fff
#define IMX708_TEST_PATTERN_COLOUR_STEP	1

#define IMX708_REG_BASE_SPC_GAINS_L	0x7b10
#define IMX708_REG_BASE_SPC_GAINS_R	0x7c00

/* HDR exposure ratio (long:med == med:short) */
#define IMX708_HDR_EXPOSURE_RATIO       4
#define IMX708_REG_MID_EXPOSURE		0x3116
#define IMX708_REG_SHT_EXPOSURE		0x0224
#define IMX708_REG_MID_ANALOG_GAIN	0x3118
#define IMX708_REG_SHT_ANALOG_GAIN	0x0216

/* QBC Re-mosaic broken line correction registers */
#define IMX708_LPF_INTENSITY_EN		0xC428
#define IMX708_LPF_INTENSITY_ENABLED	0x00
#define IMX708_LPF_INTENSITY_DISABLED	0x01
#define IMX708_LPF_INTENSITY		0xC429

/* IMX708 native and active pixel array size. */
#define IMX708_NATIVE_FORMAT		MEDIA_BUS_FMT_SRGGB10_1X10
#define IMX708_NATIVE_WIDTH		4640U
#define IMX708_NATIVE_HEIGHT		2658U
#define IMX708_PIXEL_ARRAY_LEFT		16U
#define IMX708_PIXEL_ARRAY_TOP		24U
#define IMX708_PIXEL_ARRAY_WIDTH	4608U
#define IMX708_PIXEL_ARRAY_HEIGHT	2592U

/*
 * Metadata buffer holds a variety of data, all sent with the same VC/DT (0x12).
 * It comprises two scanlines (of up to 5760 bytes each, for 4608 pixels)
 * of embedded data, one line of PDAF data, and two lines of AE-HIST data
 * (AE histograms are valid for HDR mode and empty in non-HDR modes).
 */
#define IMX708_EMBEDDED_DATA_WIDTH	IMX708_PIXEL_ARRAY_WIDTH
#define IMX708_EMBEDDED_DATA_HEIGHT	5U

struct imx708_reg {
	u16 address;
	u8 val;
};

struct imx708_reg_list {
	unsigned int num_of_regs;
	const struct imx708_reg *regs;
};

/* Mode : resolution and related config&values */
struct imx708_mode {
	/* Frame width */
	unsigned int width;

	/* Frame height */
	unsigned int height;

	/* H-timing in pixels */
	unsigned int line_length_pix;

	/* Analog crop rectangle. */
	struct v4l2_rect crop;

	/* Highest possible framerate. */
	unsigned int vblank_min;

	/* Default framerate. */
	unsigned int vblank_default;

	/* Default register values */
	struct imx708_reg_list reg_list;

	/* Not all modes have the same pixel rate. */
	u64 pixel_rate;

	/* Not all modes have the same minimum exposure. */
	u32 exposure_lines_min;

	/* Not all modes have the same exposure lines step. */
	u32 exposure_lines_step;

	/* HDR flag, used for checking if the current mode is HDR */
	bool hdr;

	/* Quad Bayer Re-mosaic flag */
	bool remosaic;
};

/* Default PDAF pixel correction gains */
static const u8 pdaf_gains[2][9] = {
	{ 0x4c, 0x4c, 0x4c, 0x46, 0x3e, 0x38, 0x35, 0x35, 0x35 },
	{ 0x35, 0x35, 0x35, 0x38, 0x3e, 0x46, 0x4c, 0x4c, 0x4c }
};

/* Link frequency setup */
enum {
	IMX708_LINK_FREQ_450MHZ,
	IMX708_LINK_FREQ_447MHZ,
	IMX708_LINK_FREQ_453MHZ,
};

static const s64 link_freqs[] = {
	[IMX708_LINK_FREQ_450MHZ] = 450000000,
	[IMX708_LINK_FREQ_447MHZ] = 447000000,
	[IMX708_LINK_FREQ_453MHZ] = 453000000,
};

/* 450MHz is the nominal "default" link frequency */
static const struct imx708_reg link_450Mhz_regs[] = {
	{0x030E, 0x01},
	{0x030F, 0x2c},
};

static const struct imx708_reg link_447Mhz_regs[] = {
	{0x030E, 0x01},
	{0x030F, 0x2a},
};

static const struct imx708_reg link_453Mhz_regs[] = {
	{0x030E, 0x01},
	{0x030F, 0x2e},
};

static const struct imx708_reg_list link_freq_regs[] = {
	[IMX708_LINK_FREQ_450MHZ] = {
		.regs = link_450Mhz_regs,
		.num_of_regs = ARRAY_SIZE(link_450Mhz_regs)
	},
	[IMX708_LINK_FREQ_447MHZ] = {
		.regs = link_447Mhz_regs,
		.num_of_regs = ARRAY_SIZE(link_447Mhz_regs)
	},
	[IMX708_LINK_FREQ_453MHZ] = {
		.regs = link_453Mhz_regs,
		.num_of_regs = ARRAY_SIZE(link_453Mhz_regs)
	},
};

static const struct imx708_reg mode_common_regs[] = {
	{0x0100, 0x00},
	{0x0136, 0x18},
	{0x0137, 0x00},
	{0x33F0, 0x02},
	{0x33F1, 0x05},
	{0x3062, 0x00},
	{0x3063, 0x12},
	{0x3068, 0x00},
	{0x3069, 0x12},
	{0x306A, 0x00},
	{0x306B, 0x30},
	{0x3076, 0x00},
	{0x3077, 0x30},
	{0x3078, 0x00},
	{0x3079, 0x30},
	{0x5E54, 0x0C},
	{0x6E44, 0x00},
	{0xB0B6, 0x01},
	{0xE829, 0x00},
	{0xF001, 0x08},
	{0xF003, 0x08},
	{0xF00D, 0x10},
	{0xF00F, 0x10},
	{0xF031, 0x08},
	{0xF033, 0x08},
	{0xF03D, 0x10},
	{0xF03F, 0x10},
	{0x0112, 0x0A},
	{0x0113, 0x0A},
	{0x0114, 0x01},
	{0x0B8E, 0x01},
	{0x0B8F, 0x00},
	{0x0B94, 0x01},
	{0x0B95, 0x00},
	{0x3400, 0x01},
	{0x3478, 0x01},
	{0x3479, 0x1c},
	{0x3091, 0x01},
	{0x3092, 0x00},
	{0x3419, 0x00},
	{0xBCF1, 0x02},
	{0x3094, 0x01},
	{0x3095, 0x01},
	{0x3362, 0x00},
	{0x3363, 0x00},
	{0x3364, 0x00},
	{0x3365, 0x00},
	{0x0138, 0x01},
};

/* 10-bit. */
static const struct imx708_reg mode_4608x2592_regs[] = {
	{0x0342, 0x3D},
	{0x0343, 0x20},
	{0x0340, 0x0A},
	{0x0341, 0x59},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x11},
	{0x0349, 0xFF},
	{0x034A, 0X0A},
	{0x034B, 0x1F},
	{0x0220, 0x62},
	{0x0222, 0x01},
	{0x0900, 0x00},
	{0x0901, 0x11},
	{0x0902, 0x0A},
	{0x3200, 0x01},
	{0x3201, 0x01},
	{0x32D5, 0x01},
	{0x32D6, 0x00},
	{0x32DB, 0x01},
	{0x32DF, 0x00},
	{0x350C, 0x00},
	{0x350D, 0x00},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x12},
	{0x040D, 0x00},
	{0x040E, 0x0A},
	{0x040F, 0x20},
	{0x034C, 0x12},
	{0x034D, 0x00},
	{0x034E, 0x0A},
	{0x034F, 0x20},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x02},
	{0x0306, 0x00},
	{0x0307, 0x7C},
	{0x030B, 0x02},
	{0x030D, 0x04},
	{0x0310, 0x01},
	{0x3CA0, 0x00},
	{0x3CA1, 0x64},
	{0x3CA4, 0x00},
	{0x3CA5, 0x00},
	{0x3CA6, 0x00},
	{0x3CA7, 0x00},
	{0x3CAA, 0x00},
	{0x3CAB, 0x00},
	{0x3CB8, 0x00},
	{0x3CB9, 0x08},
	{0x3CBA, 0x00},
	{0x3CBB, 0x00},
	{0x3CBC, 0x00},
	{0x3CBD, 0x3C},
	{0x3CBE, 0x00},
	{0x3CBF, 0x00},
	{0x0202, 0x0A},
	{0x0203, 0x29},
	{0x0224, 0x01},
	{0x0225, 0xF4},
	{0x3116, 0x01},
	{0x3117, 0xF4},
	{0x0204, 0x00},
	{0x0205, 0x00},
	{0x0216, 0x00},
	{0x0217, 0x00},
	{0x0218, 0x01},
	{0x0219, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x3118, 0x00},
	{0x3119, 0x00},
	{0x311A, 0x01},
	{0x311B, 0x00},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x01},
	{0x341f, 0x20},
	{0x3420, 0x00},
	{0x3421, 0xd8},
	{0x3366, 0x00},
	{0x3367, 0x00},
	{0x3368, 0x00},
	{0x3369, 0x00},
};

static const struct imx708_reg mode_2x2binned_regs[] = {
	{0x0342, 0x1E},
	{0x0343, 0x90},
	{0x0340, 0x05},
	{0x0341, 0x38},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x11},
	{0x0349, 0xFF},
	{0x034A, 0X0A},
	{0x034B, 0x1F},
	{0x0220, 0x62},
	{0x0222, 0x01},
	{0x0900, 0x01},
	{0x0901, 0x22},
	{0x0902, 0x08},
	{0x3200, 0x41},
	{0x3201, 0x41},
	{0x32D5, 0x00},
	{0x32D6, 0x00},
	{0x32DB, 0x01},
	{0x32DF, 0x00},
	{0x350C, 0x00},
	{0x350D, 0x00},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x09},
	{0x040D, 0x00},
	{0x040E, 0x05},
	{0x040F, 0x10},
	{0x034C, 0x09},
	{0x034D, 0x00},
	{0x034E, 0x05},
	{0x034F, 0x10},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x02},
	{0x0306, 0x00},
	{0x0307, 0x7A},
	{0x030B, 0x02},
	{0x030D, 0x04},
	{0x0310, 0x01},
	{0x3CA0, 0x00},
	{0x3CA1, 0x3C},
	{0x3CA4, 0x00},
	{0x3CA5, 0x3C},
	{0x3CA6, 0x00},
	{0x3CA7, 0x00},
	{0x3CAA, 0x00},
	{0x3CAB, 0x00},
	{0x3CB8, 0x00},
	{0x3CB9, 0x1C},
	{0x3CBA, 0x00},
	{0x3CBB, 0x08},
	{0x3CBC, 0x00},
	{0x3CBD, 0x1E},
	{0x3CBE, 0x00},
	{0x3CBF, 0x0A},
	{0x0202, 0x05},
	{0x0203, 0x08},
	{0x0224, 0x01},
	{0x0225, 0xF4},
	{0x3116, 0x01},
	{0x3117, 0xF4},
	{0x0204, 0x00},
	{0x0205, 0x70},
	{0x0216, 0x00},
	{0x0217, 0x70},
	{0x0218, 0x01},
	{0x0219, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x3118, 0x00},
	{0x3119, 0x70},
	{0x311A, 0x01},
	{0x311B, 0x00},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x00},
	{0x341f, 0x90},
	{0x3420, 0x00},
	{0x3421, 0x6c},
	{0x3366, 0x00},
	{0x3367, 0x00},
	{0x3368, 0x00},
	{0x3369, 0x00},
};

static const struct imx708_reg mode_2x2binned_720p_regs[] = {
	{0x0342, 0x14},
	{0x0343, 0x60},
	{0x0340, 0x04},
	{0x0341, 0xB6},
	{0x0344, 0x03},
	{0x0345, 0x00},
	{0x0346, 0x01},
	{0x0347, 0xB0},
	{0x0348, 0x0E},
	{0x0349, 0xFF},
	{0x034A, 0x08},
	{0x034B, 0x6F},
	{0x0220, 0x62},
	{0x0222, 0x01},
	{0x0900, 0x01},
	{0x0901, 0x22},
	{0x0902, 0x08},
	{0x3200, 0x41},
	{0x3201, 0x41},
	{0x32D5, 0x00},
	{0x32D6, 0x00},
	{0x32DB, 0x01},
	{0x32DF, 0x01},
	{0x350C, 0x00},
	{0x350D, 0x00},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x06},
	{0x040D, 0x00},
	{0x040E, 0x03},
	{0x040F, 0x60},
	{0x034C, 0x06},
	{0x034D, 0x00},
	{0x034E, 0x03},
	{0x034F, 0x60},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x02},
	{0x0306, 0x00},
	{0x0307, 0x76},
	{0x030B, 0x02},
	{0x030D, 0x04},
	{0x0310, 0x01},
	{0x3CA0, 0x00},
	{0x3CA1, 0x3C},
	{0x3CA4, 0x01},
	{0x3CA5, 0x5E},
	{0x3CA6, 0x00},
	{0x3CA7, 0x00},
	{0x3CAA, 0x00},
	{0x3CAB, 0x00},
	{0x3CB8, 0x00},
	{0x3CB9, 0x0C},
	{0x3CBA, 0x00},
	{0x3CBB, 0x04},
	{0x3CBC, 0x00},
	{0x3CBD, 0x1E},
	{0x3CBE, 0x00},
	{0x3CBF, 0x05},
	{0x0202, 0x04},
	{0x0203, 0x86},
	{0x0224, 0x01},
	{0x0225, 0xF4},
	{0x3116, 0x01},
	{0x3117, 0xF4},
	{0x0204, 0x00},
	{0x0205, 0x70},
	{0x0216, 0x00},
	{0x0217, 0x70},
	{0x0218, 0x01},
	{0x0219, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x3118, 0x00},
	{0x3119, 0x70},
	{0x311A, 0x01},
	{0x311B, 0x00},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x00},
	{0x341f, 0x60},
	{0x3420, 0x00},
	{0x3421, 0x48},
	{0x3366, 0x00},
	{0x3367, 0x00},
	{0x3368, 0x00},
	{0x3369, 0x00},
};

static const struct imx708_reg mode_hdr_regs[] = {
	{0x0342, 0x14},
	{0x0343, 0x60},
	{0x0340, 0x0A},
	{0x0341, 0x5B},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x11},
	{0x0349, 0xFF},
	{0x034A, 0X0A},
	{0x034B, 0x1F},
	{0x0220, 0x01},
	{0x0222, IMX708_HDR_EXPOSURE_RATIO},
	{0x0900, 0x00},
	{0x0901, 0x11},
	{0x0902, 0x0A},
	{0x3200, 0x01},
	{0x3201, 0x01},
	{0x32D5, 0x00},
	{0x32D6, 0x00},
	{0x32DB, 0x01},
	{0x32DF, 0x00},
	{0x350C, 0x00},
	{0x350D, 0x00},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040A, 0x00},
	{0x040B, 0x00},
	{0x040C, 0x09},
	{0x040D, 0x00},
	{0x040E, 0x05},
	{0x040F, 0x10},
	{0x034C, 0x09},
	{0x034D, 0x00},
	{0x034E, 0x05},
	{0x034F, 0x10},
	{0x0301, 0x05},
	{0x0303, 0x02},
	{0x0305, 0x02},
	{0x0306, 0x00},
	{0x0307, 0xA2},
	{0x030B, 0x02},
	{0x030D, 0x04},
	{0x0310, 0x01},
	{0x3CA0, 0x00},
	{0x3CA1, 0x00},
	{0x3CA4, 0x00},
	{0x3CA5, 0x00},
	{0x3CA6, 0x00},
	{0x3CA7, 0x28},
	{0x3CAA, 0x00},
	{0x3CAB, 0x00},
	{0x3CB8, 0x00},
	{0x3CB9, 0x30},
	{0x3CBA, 0x00},
	{0x3CBB, 0x00},
	{0x3CBC, 0x00},
	{0x3CBD, 0x32},
	{0x3CBE, 0x00},
	{0x3CBF, 0x00},
	{0x0202, 0x0A},
	{0x0203, 0x2B},
	{0x0224, 0x0A},
	{0x0225, 0x2B},
	{0x3116, 0x0A},
	{0x3117, 0x2B},
	{0x0204, 0x00},
	{0x0205, 0x00},
	{0x0216, 0x00},
	{0x0217, 0x00},
	{0x0218, 0x01},
	{0x0219, 0x00},
	{0x020E, 0x01},
	{0x020F, 0x00},
	{0x3118, 0x00},
	{0x3119, 0x00},
	{0x311A, 0x01},
	{0x311B, 0x00},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x00},
	{0x341f, 0x90},
	{0x3420, 0x00},
	{0x3421, 0x6c},
	{0x3360, 0x01},
	{0x3361, 0x01},
	{0x3366, 0x09},
	{0x3367, 0x00},
	{0x3368, 0x05},
	{0x3369, 0x10},
};

/* Mode configs. Keep separate lists for when HDR is enabled or not. */
static const struct imx708_mode supported_modes_10bit_no_hdr[] = {
	{
		/* Full resolution. */
		.width = 4608,
		.height = 2592,
		.line_length_pix = 0x3d20,
		.crop = {
			.left = IMX708_PIXEL_ARRAY_LEFT,
			.top = IMX708_PIXEL_ARRAY_TOP,
			.width = 4608,
			.height = 2592,
		},
		.vblank_min = 58,
		.vblank_default = 58,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_4608x2592_regs),
			.regs = mode_4608x2592_regs,
		},
		.pixel_rate = 595200000,
		.exposure_lines_min = 8,
		.exposure_lines_step = 1,
		.hdr = false,
		.remosaic = true
	},
	{
		/* regular 2x2 binned. */
		.width = 2304,
		.height = 1296,
		.line_length_pix = 0x1e90,
		.crop = {
			.left = IMX708_PIXEL_ARRAY_LEFT,
			.top = IMX708_PIXEL_ARRAY_TOP,
			.width = 4608,
			.height = 2592,
		},
		.vblank_min = 40,
		.vblank_default = 1198,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2x2binned_regs),
			.regs = mode_2x2binned_regs,
		},
		.pixel_rate = 585600000,
		.exposure_lines_min = 4,
		.exposure_lines_step = 2,
		.hdr = false,
		.remosaic = false
	},
	{
		/* 2x2 binned and cropped for 720p. */
		.width = 1536,
		.height = 864,
		.line_length_pix = 0x1460,
		.crop = {
			.left = IMX708_PIXEL_ARRAY_LEFT + 768,
			.top = IMX708_PIXEL_ARRAY_TOP + 432,
			.width = 3072,
			.height = 1728,
		},
		.vblank_min = 40,
		.vblank_default = 2755,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2x2binned_720p_regs),
			.regs = mode_2x2binned_720p_regs,
		},
		.pixel_rate = 566400000,
		.exposure_lines_min = 4,
		.exposure_lines_step = 2,
		.hdr = false,
		.remosaic = false
	},
};

static const struct imx708_mode supported_modes_10bit_hdr[] = {
	{
		/* There's only one HDR mode, which is 2x2 downscaled */
		.width = 2304,
		.height = 1296,
		.line_length_pix = 0x1460,
		.crop = {
			.left = IMX708_PIXEL_ARRAY_LEFT,
			.top = IMX708_PIXEL_ARRAY_TOP,
			.width = 4608,
			.height = 2592,
		},
		.vblank_min = 3673,
		.vblank_default = 3673,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_hdr_regs),
			.regs = mode_hdr_regs,
		},
		.pixel_rate = 777600000,
		.exposure_lines_min = 8 * IMX708_HDR_EXPOSURE_RATIO * IMX708_HDR_EXPOSURE_RATIO,
		.exposure_lines_step = 2 * IMX708_HDR_EXPOSURE_RATIO * IMX708_HDR_EXPOSURE_RATIO,
		.hdr = true,
		.remosaic = false
	}
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

static const char * const imx708_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
	"Solid Color",
	"Grey Color Bars",
	"PN9"
};

static const int imx708_test_pattern_val[] = {
	IMX708_TEST_PATTERN_DISABLE,
	IMX708_TEST_PATTERN_COLOR_BARS,
	IMX708_TEST_PATTERN_SOLID_COLOR,
	IMX708_TEST_PATTERN_GREY_COLOR,
	IMX708_TEST_PATTERN_PN9,
};

/* regulator supplies */
static const char * const imx708_supply_name[] = {
	/* Supplies can be enabled in any order */
	"vana1",  /* Analog1 (2.8V) supply */
	"vana2",  /* Analog2 (1.8V) supply */
	"vdig",  /* Digital Core (1.1V) supply */
	"vddl",  /* IF (1.8V) supply */
};

/*
 * Initialisation delay between XCLR low->high and the moment when the sensor
 * can start capture (i.e. can leave software standby), given by T7 in the
 * datasheet is 8ms.  This does include I2C setup time as well.
 *
 * Note, that delay between XCLR low->high and reading the CCI ID register (T6
 * in the datasheet) is much smaller - 600us.
 */
#define IMX708_XCLR_MIN_DELAY_US	8000
#define IMX708_XCLR_DELAY_RANGE_US	1000

enum imx708_pad_ids {
	IMX708_PAD_SOURCE,
	IMX708_PAD_IMAGE,
	IMX708_PAD_EDATA,
	IMX708_NUM_PADS,
};

enum imx708_stream_ids {
	IMX708_STREAM_IMAGE,
	IMX708_STREAM_EDATA,
};

struct imx708 {
	struct v4l2_subdev sd;
	struct media_pad pads[IMX708_NUM_PADS];

	struct clk *inclk;
	u32 inclk_freq;

	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx708_supply_name)];

	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *hdr_mode;
	struct v4l2_ctrl *link_freq;
	struct {
		struct v4l2_ctrl *hflip;
		struct v4l2_ctrl *vflip;
	};

	/* Current mode */
	const struct imx708_mode *mode;

	/* Rewrite common registers on stream on? */
	bool common_regs_written;

	/* Current long exposure factor in use. Set through V4L2_CID_VBLANK */
	unsigned int long_exp_shift;

	unsigned int link_freq_idx;
};

static inline struct imx708 *to_imx708(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx708, sd);
}

static inline void get_mode_table(unsigned int code,
				  const struct imx708_mode **mode_list,
				  unsigned int *num_modes,
				  bool hdr_enable)
{
	switch (code) {
	/* 10-bit */
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SBGGR10_1X10:
		if (hdr_enable) {
			*mode_list = supported_modes_10bit_hdr;
			*num_modes = ARRAY_SIZE(supported_modes_10bit_hdr);
		} else {
			*mode_list = supported_modes_10bit_no_hdr;
			*num_modes = ARRAY_SIZE(supported_modes_10bit_no_hdr);
		}
		break;
	default:
		*mode_list = NULL;
		*num_modes = 0;
	}
}

/* Read registers up to 2 at a time */
static int imx708_read_reg(struct imx708 *imx708, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->sd);
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
static int imx708_write_reg(struct imx708 *imx708, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->sd);
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
static int imx708_write_regs(struct imx708 *imx708,
			     const struct imx708_reg *regs, u32 len)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->sd);
	unsigned int i;

	for (i = 0; i < len; i++) {
		int ret;

		ret = imx708_write_reg(imx708, regs[i].address, 1, regs[i].val);
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
static u32 imx708_get_format_code(struct imx708 *imx708)
{
	unsigned int i;

	i = (imx708->vflip->val ? 2 : 0) |
	    (imx708->hflip->val ? 1 : 0);

	return codes[i];
}

static int imx708_set_exposure(struct imx708 *imx708, unsigned int val)
{
	val = max(val, imx708->mode->exposure_lines_min);
	val -= val % imx708->mode->exposure_lines_step;

	/*
	 * In HDR mode this will set the longest exposure. The sensor
	 * will automatically divide the medium and short ones by 4,16.
	 */
	return imx708_write_reg(imx708, IMX708_REG_EXPOSURE,
				IMX708_REG_VALUE_16BIT,
				val >> imx708->long_exp_shift);
}

static void imx708_adjust_exposure_range(struct imx708 *imx708,
					 struct v4l2_ctrl *ctrl)
{
	int exposure_max, exposure_def;

	/* Honour the VBLANK limits when setting exposure. */
	exposure_max = imx708->mode->height + imx708->vblank->val -
		IMX708_EXPOSURE_OFFSET;
	exposure_def = min(exposure_max, imx708->exposure->val);
	__v4l2_ctrl_modify_range(imx708->exposure, imx708->exposure->minimum,
				 exposure_max, imx708->exposure->step,
				 exposure_def);
}

static int imx708_set_analogue_gain(struct imx708 *imx708, unsigned int val)
{
	int ret;

	/*
	 * In HDR mode this will set the gain for the longest exposure,
	 * and by default the sensor uses the same gain for all of them.
	 */
	ret = imx708_write_reg(imx708, IMX708_REG_ANALOG_GAIN,
			       IMX708_REG_VALUE_16BIT, val);

	return ret;
}

static int imx708_set_frame_length(struct imx708 *imx708, unsigned int val)
{
	int ret;

	imx708->long_exp_shift = 0;

	while (val > IMX708_FRAME_LENGTH_MAX) {
		imx708->long_exp_shift++;
		val >>= 1;
	}

	ret = imx708_write_reg(imx708, IMX708_REG_FRAME_LENGTH,
			       IMX708_REG_VALUE_16BIT, val);
	if (ret)
		return ret;

	return imx708_write_reg(imx708, IMX708_LONG_EXP_SHIFT_REG,
				IMX708_REG_VALUE_08BIT, imx708->long_exp_shift);
}

static void imx708_set_framing_limits(struct imx708 *imx708)
{
	const struct imx708_mode *mode = imx708->mode;
	unsigned int hblank;

	__v4l2_ctrl_modify_range(imx708->pixel_rate,
				 mode->pixel_rate, mode->pixel_rate,
				 1, mode->pixel_rate);

	/* Update limits and set FPS to default */
	__v4l2_ctrl_modify_range(imx708->vblank, mode->vblank_min,
				 ((1 << IMX708_LONG_EXP_SHIFT_MAX) *
					IMX708_FRAME_LENGTH_MAX) - mode->height,
				 1, mode->vblank_default);

	/*
	 * Currently PPL is fixed to the mode specified value, so hblank
	 * depends on mode->width only, and is not changeable in any
	 * way other than changing the mode.
	 */
	hblank = mode->line_length_pix - mode->width;
	__v4l2_ctrl_modify_range(imx708->hblank, hblank, hblank, 1, hblank);
}

static int imx708_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx708 *imx708 =
		container_of(ctrl->handler, struct imx708, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->sd);
	const struct imx708_mode *mode_list;
	struct v4l2_subdev_state *state;
	unsigned int code, num_modes;
	int ret = 0;

	state = v4l2_subdev_get_locked_active_state(&imx708->sd);

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/*
		 * The VBLANK control may change the limits of usable exposure,
		 * so check and adjust if necessary.
		 */
		imx708_adjust_exposure_range(imx708, ctrl);
		break;

	case V4L2_CID_WIDE_DYNAMIC_RANGE:
		/*
		 * The WIDE_DYNAMIC_RANGE control can also be applied immediately
		 * as it doesn't set any registers. Don't do anything if the mode
		 * already matches.
		 */
		if (imx708->mode && imx708->mode->hdr != ctrl->val) {
			code = imx708_get_format_code(imx708);
			get_mode_table(code, &mode_list, &num_modes, ctrl->val);
			imx708->mode = v4l2_find_nearest_size(mode_list,
							      num_modes,
							      width, height,
							      imx708->mode->width,
							      imx708->mode->height);
			imx708_set_framing_limits(imx708);
		}
		break;
	}

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		imx708_set_analogue_gain(imx708, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		ret = imx708_set_exposure(imx708, ctrl->val);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = imx708_write_reg(imx708, IMX708_REG_DIGITAL_GAIN,
				       IMX708_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = imx708_write_reg(imx708, IMX708_REG_TEST_PATTERN,
				       IMX708_REG_VALUE_16BIT,
				       imx708_test_pattern_val[ctrl->val]);
		break;
	case V4L2_CID_TEST_PATTERN_RED:
		ret = imx708_write_reg(imx708, IMX708_REG_TEST_PATTERN_R,
				       IMX708_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN_GREENR:
		ret = imx708_write_reg(imx708, IMX708_REG_TEST_PATTERN_GR,
				       IMX708_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN_BLUE:
		ret = imx708_write_reg(imx708, IMX708_REG_TEST_PATTERN_B,
				       IMX708_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN_GREENB:
		ret = imx708_write_reg(imx708, IMX708_REG_TEST_PATTERN_GB,
				       IMX708_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = imx708_write_reg(imx708, IMX708_REG_ORIENTATION, 1,
				       imx708->hflip->val |
				       imx708->vflip->val << 1);
		break;
	case V4L2_CID_VBLANK:
		ret = imx708_set_frame_length(imx708,
					      imx708->mode->height + ctrl->val);
		break;
	case V4L2_CID_NOTIFY_GAINS:
		ret = imx708_write_reg(imx708, IMX708_REG_COLOUR_BALANCE_BLUE,
				       IMX708_REG_VALUE_16BIT,
				       ctrl->p_new.p_u32[0]);
		if (ret)
			break;
		ret = imx708_write_reg(imx708, IMX708_REG_COLOUR_BALANCE_RED,
				       IMX708_REG_VALUE_16BIT,
				       ctrl->p_new.p_u32[3]);
		break;
	case V4L2_CID_WIDE_DYNAMIC_RANGE:
		/* Already handled above. */
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

static const struct v4l2_ctrl_ops imx708_ctrl_ops = {
	.s_ctrl = imx708_set_ctrl,
};

static int imx708_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx708 *imx708 = to_imx708(sd);

	if (code->pad >= IMX708_NUM_PADS)
		return -EINVAL;

	switch (code->pad) {
	case IMX708_PAD_IMAGE:
		/* The internal image pad is hardwired to the native format. */
		if (code->index > 0)
			return -EINVAL;

		code->code = IMX708_NATIVE_FORMAT;
		return 0;

	case IMX708_PAD_EDATA:
		if (code->index > 0)
			return -EINVAL;

		code->code = MEDIA_BUS_FMT_CCS_EMBEDDED;
		return 0;

	case IMX708_PAD_SOURCE:
	default:
		break;
	}

	/*
	 * On the source pad, the sensor supports multiple image raw formats
	 * with different bit depths. The embedded data format bit depth
	 * follows the image stream.
	 */
	if (code->stream == IMX708_STREAM_IMAGE) {
		if (code->index >= (ARRAY_SIZE(codes) / 4))
			return -EINVAL;

		code->code = imx708_get_format_code(imx708);
	} else {
		struct v4l2_mbus_framefmt *fmt;

		if (code->index > 0)
			return -EINVAL;

		fmt = v4l2_subdev_state_get_format(sd_state, IMX708_PAD_SOURCE,
						   IMX708_STREAM_EDATA);
		code->code = fmt->code;
	}

	return 0;
}

static int imx708_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx708 *imx708 = to_imx708(sd);

	switch (fse->pad) {
	case IMX708_PAD_IMAGE:
		if (fse->code != IMX708_NATIVE_FORMAT || fse->index > 0)
			return -EINVAL;

		fse->min_width = IMX708_NATIVE_WIDTH;
		fse->max_width = IMX708_NATIVE_WIDTH;
		fse->min_height = IMX708_NATIVE_HEIGHT;
		fse->max_height = IMX708_NATIVE_HEIGHT;
		return 0;

	case IMX708_PAD_EDATA:
		if (fse->code != MEDIA_BUS_FMT_CCS_EMBEDDED || fse->index > 0)
			return -EINVAL;

		fse->min_width = IMX708_EMBEDDED_DATA_WIDTH;
		fse->max_width = IMX708_EMBEDDED_DATA_WIDTH;
		fse->min_height = IMX708_EMBEDDED_DATA_HEIGHT;
		fse->max_height = IMX708_EMBEDDED_DATA_HEIGHT;
		return 0;

	case IMX708_PAD_SOURCE:
	default:
		break;
	}

	if (fse->stream == IMX708_STREAM_IMAGE) {
		const struct imx708_mode *mode_list;
		unsigned int num_modes;

		get_mode_table(fse->code, &mode_list, &num_modes,
			       imx708->hdr_mode->val);

		if (fse->code != imx708_get_format_code(imx708) ||
		    fse->index >= num_modes)
			return -EINVAL;

		fse->min_width = mode_list[fse->index].width;
		fse->max_width = fse->min_width;
		fse->min_height = mode_list[fse->index].height;
		fse->max_height = fse->min_height;
	} else {
		struct v4l2_mbus_framefmt *fmt;

		fmt = v4l2_subdev_state_get_format(sd_state, IMX708_PAD_SOURCE,
						   IMX708_STREAM_EDATA);
		if (fse->code != fmt->code)
			return -EINVAL;

		if (fse->index)
			return -EINVAL;

		fse->min_width = IMX708_EMBEDDED_DATA_WIDTH;
		fse->max_width = IMX708_EMBEDDED_DATA_WIDTH;
		fse->min_height = IMX708_EMBEDDED_DATA_HEIGHT;
		fse->max_height = IMX708_EMBEDDED_DATA_HEIGHT;
	}

	return 0;
}

static int imx708_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				 struct v4l2_mbus_frame_desc *fd)
{
	const struct v4l2_mbus_framefmt *fmt;
	struct v4l2_subdev_state *state;
	u32 code;

	if (pad != IMX708_PAD_SOURCE)
		return -EINVAL;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	fmt = v4l2_subdev_state_get_format(state, IMX708_PAD_SOURCE,
					   IMX708_STREAM_IMAGE);
	code = fmt->code;
	v4l2_subdev_unlock_state(state);

	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 2;

	memset(fd->entry, 0, sizeof(fd->entry));

	fd->entry[0].pixelcode = code;
	fd->entry[0].stream = IMX708_STREAM_IMAGE;
	fd->entry[0].bus.csi2.vc = 0;
	fd->entry[0].bus.csi2.dt = MIPI_CSI2_DT_RAW10;

	fd->entry[1].pixelcode = code;
	fd->entry[1].stream = IMX708_STREAM_EDATA;
	fd->entry[1].bus.csi2.vc = 0;
	fd->entry[1].bus.csi2.dt = MIPI_CSI2_DT_EMBEDDED_8B;

	return 0;
}

static int imx708_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx708 *imx708 = to_imx708(sd);
	struct v4l2_mbus_framefmt *format, *ed_format;
	const struct imx708_mode *mode_list;
	const struct imx708_mode *mode;
	struct v4l2_rect *compose;
	struct v4l2_rect *crop;
	unsigned int num_modes;

	/*
	 * The driver is mode-based, the format can be set on the source pad
	 * only, and only for the image stream.
	 */
	if (fmt->pad != IMX708_PAD_SOURCE || fmt->stream != IMX708_STREAM_IMAGE)
		return v4l2_subdev_get_fmt(sd, sd_state, fmt);

	get_mode_table(fmt->format.code, &mode_list, &num_modes,
			imx708->hdr_mode->val);

	/*
	 * Adjust the requested format to match the closest mode. The Bayer
	 * order varies with flips.
	 */
	mode = v4l2_find_nearest_size(mode_list, num_modes, width, height,
				      fmt->format.width, fmt->format.height);

	fmt->format.code = imx708_get_format_code(imx708);
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc =
		V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->format.colorspace);
	fmt->format.xfer_func =
		V4L2_MAP_XFER_FUNC_DEFAULT(fmt->format.colorspace);
	fmt->format.quantization =
		V4L2_MAP_QUANTIZATION_DEFAULT(true, fmt->format.colorspace,
					      fmt->format.ycbcr_enc);

	/* Propagate the format through the sensor. */

	/* The image pad models the pixel array, and thus has a fixed size. */
	format = v4l2_subdev_state_get_format(sd_state, IMX708_PAD_IMAGE);
	*format = fmt->format;
	format->code = IMX708_NATIVE_FORMAT;
	format->width = IMX708_NATIVE_WIDTH;
	format->height = IMX708_NATIVE_HEIGHT;

	/* Get the crop rectangle from the mode list */
	crop = v4l2_subdev_state_get_crop(sd_state, IMX708_PAD_IMAGE);
	*crop = mode->crop;

	/* The compose rectangle size is the sensor output size. */
	compose = v4l2_subdev_state_get_compose(sd_state, IMX708_PAD_IMAGE);
	compose->left = 0;
	compose->top = 0;
	compose->width = fmt->format.width;
	compose->height = fmt->format.height;

	/*
	 * No mode use digital crop, the source pad crop rectangle size and
	 * format are thus identical to the image pad compose rectangle.
	 */
	crop = v4l2_subdev_state_get_crop(sd_state, IMX708_PAD_SOURCE,
					  IMX708_STREAM_IMAGE);
	crop->left = 0;
	crop->top = 0;
	crop->width = fmt->format.width;
	crop->height = fmt->format.height;

	format = v4l2_subdev_state_get_format(sd_state, IMX708_PAD_SOURCE,
					      IMX708_STREAM_IMAGE);
	*format = fmt->format;

	/*
	 * Finally, update the formats on the sink and source sides of the
	 * embedded data stream.
	 */
	ed_format = v4l2_subdev_state_get_format(sd_state, IMX708_PAD_EDATA);
	ed_format->code = MEDIA_BUS_FMT_META_10;
	ed_format->width = IMX708_EMBEDDED_DATA_WIDTH;
	ed_format->height = IMX708_EMBEDDED_DATA_HEIGHT;
	ed_format->field = V4L2_FIELD_NONE;

	format = v4l2_subdev_state_get_format(sd_state, IMX708_PAD_SOURCE,
					      IMX708_STREAM_EDATA);
	*format = *ed_format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		imx708->mode = mode;
		imx708_set_framing_limits(imx708);
	}

	return 0;
}

static int imx708_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_route routes[2] = {
		{
			.sink_pad = IMX708_PAD_IMAGE,
			.sink_stream = 0,
			.source_pad = IMX708_PAD_SOURCE,
			.source_stream = IMX708_STREAM_IMAGE,
			.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		}, {
			.sink_pad = IMX708_PAD_EDATA,
			.sink_stream = 0,
			.source_pad = IMX708_PAD_SOURCE,
			.source_stream = IMX708_STREAM_EDATA,
			.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		},
	};
	struct v4l2_subdev_krouting routing = {
		.len_routes = ARRAY_SIZE(routes),
		.num_routes = ARRAY_SIZE(routes),
		.routes = routes,
	};
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = IMX708_PAD_SOURCE,
		.stream = IMX708_STREAM_IMAGE,
		.format = {
			.code = IMX708_NATIVE_FORMAT,
			.width = supported_modes_10bit_no_hdr[0].width,
			.height = supported_modes_10bit_no_hdr[0].height,
			.colorspace = V4L2_COLORSPACE_RAW,
			.ycbcr_enc =
				V4L2_MAP_YCBCR_ENC_DEFAULT(V4L2_COLORSPACE_RAW),
			.xfer_func =
				V4L2_MAP_XFER_FUNC_DEFAULT(V4L2_COLORSPACE_RAW),
			.quantization =
			  V4L2_MAP_QUANTIZATION_DEFAULT
			    (true, V4L2_COLORSPACE_RAW,
			     V4L2_MAP_YCBCR_ENC_DEFAULT(V4L2_COLORSPACE_RAW))
		},
	};
	int ret;

	ret = v4l2_subdev_set_routing(sd, state, &routing);
	if (ret)
		return ret;

	/*
	 * Set the image stream format on the source pad. This will be
	 * propagated to all formats and selection rectangles internally.
	 */
	imx708_set_pad_format(sd, state, &fmt);

	return 0;
}

static int imx708_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct v4l2_rect *compose;

	/*
	 * The embedded data stream doesn't support selection rectangles,
	 * neither on the embedded data pad nor on the source pad.
	 */
	if (sel->pad == IMX708_PAD_EDATA || sel->stream != 0)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(sd_state, sel->pad);
		return 0;

	case V4L2_SEL_TGT_NATIVE_SIZE:
		if (sel->pad != IMX708_PAD_IMAGE)
			return -EINVAL;

		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX708_NATIVE_WIDTH;
		sel->r.height = IMX708_NATIVE_HEIGHT;
		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		switch (sel->pad) {
		case IMX708_PAD_IMAGE:
			sel->r.top = IMX708_PIXEL_ARRAY_TOP;
			sel->r.left = IMX708_PIXEL_ARRAY_LEFT;
			sel->r.width = IMX708_PIXEL_ARRAY_WIDTH;
			sel->r.height = IMX708_PIXEL_ARRAY_HEIGHT;
			return 0;

		case IMX708_PAD_SOURCE:
			compose = v4l2_subdev_state_get_compose(sd_state,
								IMX708_PAD_IMAGE);
			sel->r.top = 0;
			sel->r.left = 0;
			sel->r.width = compose->width;
			sel->r.height = compose->height;
			return 0;
		}

		break;

	case V4L2_SEL_TGT_COMPOSE:
		if (sel->pad != IMX708_PAD_IMAGE)
			return -EINVAL;

		sel->r = *v4l2_subdev_state_get_compose(sd_state, sel->pad);
		return 0;
	}

	return -EINVAL;
}

/* Start streaming */
static int imx708_start_streaming(struct imx708 *imx708)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->sd);
	const struct imx708_reg_list *reg_list, *freq_regs;
	int i, ret;
	u32 val;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret < 0)
		return ret;

	if (!imx708->common_regs_written) {
		ret = imx708_write_regs(imx708, mode_common_regs,
					ARRAY_SIZE(mode_common_regs));
		if (ret) {
			dev_err(&client->dev, "%s failed to set common settings\n",
				__func__);
			goto error_pm_put;
		}

		ret = imx708_read_reg(imx708, IMX708_REG_BASE_SPC_GAINS_L,
				      IMX708_REG_VALUE_08BIT, &val);
		if (ret == 0 && val == 0x40) {
			for (i = 0; i < 54 && ret == 0; i++) {
				ret = imx708_write_reg(imx708,
						       IMX708_REG_BASE_SPC_GAINS_L + i,
						       IMX708_REG_VALUE_08BIT,
						       pdaf_gains[0][i % 9]);
			}
			for (i = 0; i < 54 && ret == 0; i++) {
				ret = imx708_write_reg(imx708,
						       IMX708_REG_BASE_SPC_GAINS_R + i,
						       IMX708_REG_VALUE_08BIT,
						       pdaf_gains[1][i % 9]);
			}
		}
		if (ret) {
			dev_err(&client->dev, "%s failed to set PDAF gains\n",
				__func__);
			goto error_pm_put;
		}
		imx708->common_regs_written = true;
	}

	/* Apply default values of current mode */
	reg_list = &imx708->mode->reg_list;
	ret = imx708_write_regs(imx708, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(&client->dev, "%s failed to set mode\n", __func__);
		goto error_pm_put;
	}
	/* Update the link frequency registers */
	freq_regs = &link_freq_regs[imx708->link_freq_idx];
	ret = imx708_write_regs(imx708, freq_regs->regs,
				freq_regs->num_of_regs);
	if (ret) {
		dev_err(&client->dev, "%s failed to set link frequency registers\n",
			__func__);
		goto error_pm_put;
	}

	/* Quad Bayer re-mosaic adjustments (for full-resolution mode only) */
	if (imx708->mode->remosaic && qbc_adjust > 0) {
		imx708_write_reg(imx708, IMX708_LPF_INTENSITY,
				 IMX708_REG_VALUE_08BIT, qbc_adjust);
		imx708_write_reg(imx708,
				 IMX708_LPF_INTENSITY_EN,
				 IMX708_REG_VALUE_08BIT,
				 IMX708_LPF_INTENSITY_ENABLED);
	} else {
		imx708_write_reg(imx708,
				 IMX708_LPF_INTENSITY_EN,
				 IMX708_REG_VALUE_08BIT,
				 IMX708_LPF_INTENSITY_DISABLED);
	}
	/* Apply customized values from user */
	ret =  __v4l2_ctrl_handler_setup(imx708->sd.ctrl_handler);
	if (ret)
		goto error_pm_put;
	/* set stream on register */
	ret = imx708_write_reg(imx708, IMX708_REG_MODE_SELECT,
			       IMX708_REG_VALUE_08BIT, IMX708_MODE_STREAMING);
	if (ret)
		goto error_pm_put;

	return 0;

error_pm_put:
	pm_runtime_put(&client->dev);
	return ret;
}

/* Stop streaming */
static void imx708_stop_streaming(struct imx708 *imx708)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->sd);
	int ret;

	/* set stream off register */
	ret = imx708_write_reg(imx708, IMX708_REG_MODE_SELECT,
			       IMX708_REG_VALUE_08BIT, IMX708_MODE_STANDBY);
	if (ret)
		dev_err(&client->dev, "%s failed to set stream\n", __func__);

	pm_runtime_put(&client->dev);
}

static int imx708_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx708 *imx708 = to_imx708(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	if (enable) {
		/*
		 * Apply default & customized values
		 * and then start streaming.
		 */
		ret = imx708_start_streaming(imx708);
	} else
		imx708_stop_streaming(imx708);

	/* vflip/hflip and hdr mode cannot change during streaming */
	__v4l2_ctrl_grab(imx708->vflip, enable);
	__v4l2_ctrl_grab(imx708->hflip, enable);
	__v4l2_ctrl_grab(imx708->hdr_mode, enable);

	v4l2_subdev_unlock_state(state);

	return ret;
}

/* Power/clock management functions */
static int imx708_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx708 *imx708 = to_imx708(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx708_supply_name),
				    imx708->supplies);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable regulators\n",
			__func__);
		return ret;
	}

	ret = clk_prepare_enable(imx708->inclk);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable clock\n",
			__func__);
		goto reg_off;
	}

	gpiod_set_value_cansleep(imx708->reset_gpio, 1);
	usleep_range(IMX708_XCLR_MIN_DELAY_US,
		     IMX708_XCLR_MIN_DELAY_US + IMX708_XCLR_DELAY_RANGE_US);

	return 0;

reg_off:
	regulator_bulk_disable(ARRAY_SIZE(imx708_supply_name),
			       imx708->supplies);
	return ret;
}

static int imx708_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx708 *imx708 = to_imx708(sd);

	gpiod_set_value_cansleep(imx708->reset_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(imx708_supply_name),
			       imx708->supplies);
	clk_disable_unprepare(imx708->inclk);

	/* Force reprogramming of the common registers when powered up again. */
	imx708->common_regs_written = false;

	return 0;
}

static int imx708_get_regulators(struct imx708 *imx708)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->sd);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(imx708_supply_name); i++)
		imx708->supplies[i].supply = imx708_supply_name[i];

	return devm_regulator_bulk_get(&client->dev,
				       ARRAY_SIZE(imx708_supply_name),
				       imx708->supplies);
}

/* Verify chip ID */
static int imx708_identify_module(struct imx708 *imx708)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->sd);
	int ret;
	u32 val;

	ret = imx708_read_reg(imx708, IMX708_REG_CHIP_ID,
			      IMX708_REG_VALUE_16BIT, &val);
	if (ret) {
		dev_err(&client->dev, "failed to read chip id %x, with error %d\n",
			IMX708_CHIP_ID, ret);
		return ret;
	}

	if (val != IMX708_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%x\n",
			IMX708_CHIP_ID, val);
		return -EIO;
	}

	ret = imx708_read_reg(imx708, 0x0000, IMX708_REG_VALUE_16BIT, &val);
	if (!ret) {
		dev_info(&client->dev, "camera module ID 0x%04x\n", val);
		snprintf(imx708->sd.name, sizeof(imx708->sd.name), "imx708%s%s",
			 val & 0x02 ? "_wide" : "",
			 val & 0x80 ? "_noir" : "");
	}

	return 0;
}

static const struct v4l2_subdev_core_ops imx708_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops imx708_video_ops = {
	.s_stream = imx708_set_stream,
};

static const struct v4l2_subdev_pad_ops imx708_pad_ops = {
	.enum_mbus_code = imx708_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx708_set_pad_format,
	.get_selection = imx708_get_selection,
	.enum_frame_size = imx708_enum_frame_size,
	.get_frame_desc = imx708_get_frame_desc,
};

static const struct v4l2_subdev_ops imx708_subdev_ops = {
	.core = &imx708_core_ops,
	.video = &imx708_video_ops,
	.pad = &imx708_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx708_internal_ops = {
	.init_state = imx708_init_state,
};

static const struct v4l2_ctrl_config imx708_notify_gains_ctrl = {
	.ops = &imx708_ctrl_ops,
	.id = V4L2_CID_NOTIFY_GAINS,
	.type = V4L2_CTRL_TYPE_U32,
	.min = IMX708_COLOUR_BALANCE_MIN,
	.max = IMX708_COLOUR_BALANCE_MAX,
	.step = IMX708_COLOUR_BALANCE_STEP,
	.def = IMX708_COLOUR_BALANCE_DEFAULT,
	.dims = { 4 },
	.elem_size = sizeof(u32),
};

/* Initialize control handlers */
static int imx708_init_controls(struct imx708 *imx708)
{
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct i2c_client *client = v4l2_get_subdevdata(&imx708->sd);
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	unsigned int i;
	int ret;

	ctrl_hdlr = &imx708->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 16);
	if (ret)
		return ret;

	/* By default, PIXEL_RATE is read only */
	imx708->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       IMX708_INITIAL_PIXEL_RATE,
					       IMX708_INITIAL_PIXEL_RATE, 1,
					       IMX708_INITIAL_PIXEL_RATE);

	ctrl = v4l2_ctrl_new_int_menu(ctrl_hdlr, &imx708_ctrl_ops,
				      V4L2_CID_LINK_FREQ, 0, 0,
				      &link_freqs[imx708->link_freq_idx]);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/*
	 * Create the controls here, but mode specific limits are setup
	 * in the imx708_set_framing_limits() call.
	 */
	imx708->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					   V4L2_CID_VBLANK, 0, 0xffff, 1, 0);
	imx708->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					   V4L2_CID_HBLANK, 0, 0xffff, 1, 0);

	imx708->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX708_EXPOSURE_MIN,
					     IMX708_EXPOSURE_MAX,
					     IMX708_EXPOSURE_STEP,
					     IMX708_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX708_ANA_GAIN_MIN, IMX708_ANA_GAIN_MAX,
			  IMX708_ANA_GAIN_STEP, IMX708_ANA_GAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  IMX708_DGTL_GAIN_MIN, IMX708_DGTL_GAIN_MAX,
			  IMX708_DGTL_GAIN_STEP, IMX708_DGTL_GAIN_DEFAULT);

	imx708->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);

	imx708->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_cluster(2, &imx708->hflip);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx708_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx708_test_pattern_menu) - 1,
				     0, 0, imx708_test_pattern_menu);
	for (i = 0; i < 4; i++) {
		/*
		 * The assumption is that
		 * V4L2_CID_TEST_PATTERN_GREENR == V4L2_CID_TEST_PATTERN_RED + 1
		 * V4L2_CID_TEST_PATTERN_BLUE   == V4L2_CID_TEST_PATTERN_RED + 2
		 * V4L2_CID_TEST_PATTERN_GREENB == V4L2_CID_TEST_PATTERN_RED + 3
		 */
		v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
				  V4L2_CID_TEST_PATTERN_RED + i,
				  IMX708_TEST_PATTERN_COLOUR_MIN,
				  IMX708_TEST_PATTERN_COLOUR_MAX,
				  IMX708_TEST_PATTERN_COLOUR_STEP,
				  IMX708_TEST_PATTERN_COLOUR_MAX);
		/* The "Solid color" pattern is white by default */
	}

	v4l2_ctrl_new_custom(ctrl_hdlr, &imx708_notify_gains_ctrl, NULL);

	imx708->hdr_mode = v4l2_ctrl_new_std(ctrl_hdlr, &imx708_ctrl_ops,
					     V4L2_CID_WIDE_DYNAMIC_RANGE,
					     0, 1, 1, 0);

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx708_ctrl_ops, &props);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	imx708->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	imx708->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	imx708->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	imx708->hdr_mode->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx708->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static void imx708_free_controls(struct imx708 *imx708)
{
	v4l2_ctrl_handler_free(imx708->sd.ctrl_handler);
}

static int imx708_check_hwcfg(struct device *dev, struct imx708 *imx708)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret = -EINVAL;
	int i;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	if (v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg)) {
		dev_err(dev, "could not parse endpoint\n");
		goto error_out;
	}

	/* Check the number of MIPI CSI2 data lanes */
	if (ep_cfg.bus.mipi_csi2.num_data_lanes != 2) {
		dev_err(dev, "only 2 data lanes are currently supported\n");
		goto error_out;
	}

	/* Check the link frequency set in device tree */
	if (!ep_cfg.nr_of_link_frequencies) {
		dev_err(dev, "link-frequency property not found in DT\n");
		goto error_out;
	}

	for (i = 0; i < ARRAY_SIZE(link_freqs); i++) {
		if (link_freqs[i] == ep_cfg.link_frequencies[0]) {
			imx708->link_freq_idx = i;
			break;
		}
	}

	if (i == ARRAY_SIZE(link_freqs)) {
		dev_err(dev, "Link frequency not supported: %lld\n",
			ep_cfg.link_frequencies[0]);
			ret = -EINVAL;
			goto error_out;
	}

	ret = 0;

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static int imx708_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx708 *imx708;
	int ret;

	imx708 = devm_kzalloc(&client->dev, sizeof(*imx708), GFP_KERNEL);
	if (!imx708)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&imx708->sd, client, &imx708_subdev_ops);
	imx708->sd.internal_ops = &imx708_internal_ops;

	/* Check the hardware configuration in device tree */
	if (imx708_check_hwcfg(dev, imx708))
		return -EINVAL;

	/* Get system clock (inclk) */
	imx708->inclk = devm_clk_get(dev, "inclk");
	if (IS_ERR(imx708->inclk))
		return dev_err_probe(dev, PTR_ERR(imx708->inclk),
				     "failed to get inclk\n");

	imx708->inclk_freq = clk_get_rate(imx708->inclk);
	if (imx708->inclk_freq != IMX708_INCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "inclk frequency not supported: %d Hz\n",
				     imx708->inclk_freq);

	ret = imx708_get_regulators(imx708);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	/* Request optional enable pin */
	imx708->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);

	/*
	 * The sensor must be powered for imx708_identify_module()
	 * to be able to read the CHIP_ID register
	 */
	ret = imx708_power_on(dev);
	if (ret)
		return ret;

	ret = imx708_identify_module(imx708);
	if (ret)
		goto error_power_off;

	/* This needs the pm runtime to be registered. */
	ret = imx708_init_controls(imx708);
	if (ret)
		goto error_power_off;

	/* Initialize subdev */
	imx708->sd.internal_ops = &imx708_internal_ops;
	imx708->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS |
			    V4L2_SUBDEV_FL_STREAMS;
	imx708->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pads */
	imx708->pads[IMX708_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	imx708->pads[IMX708_PAD_IMAGE].flags = MEDIA_PAD_FL_SINK |
					       MEDIA_PAD_FL_INTERNAL;
	imx708->pads[IMX708_PAD_EDATA].flags = MEDIA_PAD_FL_SINK |
					       MEDIA_PAD_FL_INTERNAL;

	ret = media_entity_pads_init(&imx708->sd.entity,
				     ARRAY_SIZE(imx708->pads), imx708->pads);
	if (ret) {
		dev_err(dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	imx708->sd.state_lock = imx708->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&imx708->sd);
	if (ret < 0) {
		dev_err(dev, "subdev init error: %d\n", ret);
		goto error_media_entity;
	}

	ret = v4l2_async_register_subdev_sensor(&imx708->sd);
	if (ret < 0) {
		dev_err(dev, "failed to register sensor sub-device: %d\n", ret);
		goto error_subdev_cleanup;
	}

	/* Enable runtime PM and turn off the device */
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(&imx708->sd);

error_media_entity:
	media_entity_cleanup(&imx708->sd.entity);

error_handler_free:
	imx708_free_controls(imx708);

error_power_off:
	imx708_power_off(&client->dev);

	return ret;
}

static void imx708_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx708 *imx708 = to_imx708(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	imx708_free_controls(imx708);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx708_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct of_device_id imx708_dt_ids[] = {
	{ .compatible = "sony,imx708" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx708_dt_ids);

static const struct dev_pm_ops imx708_pm_ops = {
	SET_RUNTIME_PM_OPS(imx708_power_off, imx708_power_on, NULL)
};

static struct i2c_driver imx708_i2c_driver = {
	.driver = {
		.name = "imx708",
		.of_match_table	= imx708_dt_ids,
		.pm = &imx708_pm_ops,
	},
	.probe = imx708_probe,
	.remove = imx708_remove,
};

module_i2c_driver(imx708_i2c_driver);

MODULE_AUTHOR("David Plowman <david.plowman@raspberrypi.com>");
MODULE_DESCRIPTION("Sony IMX708 sensor driver");
MODULE_LICENSE("GPL v2");
