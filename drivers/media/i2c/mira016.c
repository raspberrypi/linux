// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, ams OSRAM
 * Copyright (C) 2026, Ideas On Board Oy
 *
 * Based on AMS Mira220 driver
 * Copyright (C) 2026, ams OSRAM
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>

#include <media/mipi-csi2.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

/* Register select. */
#define MIRA016_BANK_SEL_REG		CCI_REG8(0xe000)
#define MIRA016_ACTIVE_CONTEXT_REG	CCI_REG8(0x4002)
#define MIRA016_NEXT_ACTIVE_CONTEXT_REG	CCI_REG8(0xe003)
#define MIRA016_RW_CONTEXT_REG		CCI_REG8(0xe004)
#define MIRA016_CMD_REQ_1_REG		CCI_REG8(0x000a)
#define MIRA016_CMD_HALT_BLOCK_REG	CCI_REG8(0x000c)

/* Chip id */
#define MIRA016_CHIP_ID_REG		CCI_REG8(0x011B)
#define MIRA016_CHIP_ID			33

/* PLL config */
#define MHZ(f)				((u32)(f) * HZ_PER_MHZ)
#define MIRA016_PLL_N_MIN		1
#define MIRA016_PLL_N_MAX		32
#define MIRA016_PLL_M_MIN		16
#define MIRA016_PLL_M_MAX		255
#define MIRA016_PLL_PLL1_MIN		MHZ(12)
#define MIRA016_PLL_PLL1_MAX		MHZ(22)
#define MIRA016_PLL_PLL2_MIN		MHZ(640)
#define MIRA016_PLL_PLL2_MAX		MHZ(1500)

#define MIRA016_PLL_DIV_M_REG		CCI_REG8(0x2076)
#define MIRA016_PLL_DIV_N_REG		CCI_REG8(0x2077)
#define MIRA016_CLKGEN_CP_DIV_REG	CCI_REG8(0x00ce)
#define MIRA016_OTP_GRANULARITY_REG	CCI_REG8(0x0070)
#define MIRA016_GRAN_TG_REG		CCI_REG8(0x016d)
#define MIRA016_LUT_DEL008_REG		CCI_REG8(0x0176)
#define MIRA016_CLKGEN_TX_ESC_H_REG	CCI_REG24(0x20c6)
#define MIRA016_CLKGEN_TX_ESC_L_REG	CCI_REG24(0x20c9)
#define MIRA016_PLL_PD_REG		CCI_REG8(0x2075)
#define MIRA016_PLL_LOCK_REG		CCI_REG8(0x207c)
#define MIRA016_PLL_LOCKED		BIT(0)

#define MIRA016_EXP_TIME_L_REG		CCI_REG32(0x000e)
#define MIRA016_EXP_TIME_S_REG		CCI_REG32(0x0012)
#define MIRA016_NUM_FRAMES_REG		CCI_REG8(0x0007)
#define MIRA016_TARGET_FRAME_TIME_REG	CCI_REG32(0x0008)

#define MIRA016_TIME_UNIT_REG		CCI_REG16(0x0012)
#define MIRA016_GLOB_TIME_REG		CCI_REG16(0x015a)
#define MIRA016_GLOB_TIME_A_REG		CCI_REG16(0x015c)
#define MIRA016_GLOB_TIME_B_REG		CCI_REG16(0x015e)
/*
 * TODO: LPS timings should be calculated to support new link frequency as
 * their value derives from the BYTE_PERIOD bus parameter.
 *
 * Use the hardcoded values for 1500Mbps.
 */
#define MIRA016_ENTER_LPS_TIME_REG	CCI_REG16(0x0162)
#define MIRA016_ENTER_LPS_TIME_1500MBPS	5
#define MIRA016_EXIT_LPS_TIME_REG	CCI_REG16(0x0164)
#define MIRA016_EXIT_LPS_TIME_1500MBPS	0x479
#define MIRA016_LPS_CYCLE_TIME_REG	CCI_REG16(0x0166)
#define MIRA016_LPS_CYCLE_TIME_1500MBPS	0x479

#define MIRA016_ROW_TIMINGS_REG		CCI_REG16(0x0032)

#define MIRA016_PIXEL_ARRAY_TOP		14
#define MIRA016_PIXEL_ARRAY_LEFT	14
#define MIRA016_PIXEL_ARRAY_WIDTH	400
#define MIRA016_PIXEL_ARRAY_HEIGHT	400
#define MIRA016_NATIVE_WIDTH		428
#define MIRA016_NATIVE_HEIGHT		428

/* X ROI */
#define MIRA016_XWIN_LEFT_REG		CCI_REG16(0xe02c)
#define MIRA016_XWIN_RIGHT_REG		CCI_REG16(0xe02e)
#define MIRA016_XMIRROR_REG		CCI_REG8(0xe030)
#define MIRA016_XSUBSAMPLING_REG	CCI_REG8(0xe025)
#define MIRA016_XBINNING_REG		CCI_REG8(0xe02a)
#define MIRA016_MIPI_DATA_FIFO_THR_REG	CCI_REG8(0x2029)
#define MIRA016_HSYNC_LENGTH		CCI_REG16(0x0034)

/* Y ROI Bank 1 */
#define MIRA016_YWIN_ENA_REG		CCI_REG16(0x001e)
#define MIRA016_YBINNING_REG		CCI_REG8(0x002b)
/* Y ROI Bank 0 */
#define MIRA016_YWIN_BLACK_REG		CCI_REG16(0X001f)
#define MIRA016_YWIN_DIR_REG		CCI_REG8(0x0023)
#define MIRA016_YWIN_BASE_SIZE		0x0024
#define MIRA016_YWIN_BASE_START		0x0026
#define MIRA016_YWIN_BASE_SUBS		0x0028
#define MIRA016_YWIN_SIZE_REG(x)	CCI_REG16(MIRA016_YWIN_BASE_SIZE \
						  + (x) * 5)
#define MIRA016_YWIN_START_REG(x)	CCI_REG16(MIRA016_YWIN_BASE_START \
						  + (x) * 5)
#define MIRA016_YWIN_SUBS_REG(x)	CCI_REG8(MIRA016_YWIN_BASE_SUBS \
						  + (x) * 5)
#define MIRA016_NUM_Y_WIND		10

/* Data format */
#define MIRA016_DPATH_BITMODE_REG	CCI_REG8(0x016a)
#define MIRA016_DPATH_BITWIDTH_A_REG	CCI_REG16(0x00c0)
#define MIRA016_DPATH_BITWIDTH_B_REG	CCI_REG16(0x00c2)
#define MIRA016_CSI2_DATA_TYPE_REG	CCI_REG8(0x0168)

/* Fine analogue gain */
#define MIRA016_GDIG_PREAMP		CCI_REG8(0x0195)
#define MIRA016_BIAS_RG_ADCGAIN		CCI_REG8(0x01f0)
#define MIRA016_BIAS_RG_ADCGAIN_MAX	63
#define MIRA016_BIAS_RG_MULT		CCI_REG8(0x01f3)

#define MIRA016_GAIN_UNIT		1000
#define MIRA016_GAIN_MAX_8B		16000
#define MIRA016_GAIN_MAX_10B		4000
#define MIRA016_ADC_GAIN_VAL(n)		(57000 << (n))
#define MIRA016_FINE_GAIN_REF_MULT	2

/* MIPI CSI-2 link frequency for 1.5Mbps per lane */
#define MIRA016_LINK_FREQ_750M		750000000

#define MIRA016_DEFAULT_DURATION_NSEC	8333000ULL
#define MIRA016_MAX_VBLANK		500000

/*
 * Copied from the libcamera IPA. The sensor manual doesn't really specify a
 * minimum difference between the exposure time and the frame duration.
 */
#define MIRA016_FRAME_INTEGRATION_DIFF	4

#define MIRA016_MAX_EXPOSURE		500000
#define MIRA016_EXPOSURE_DEF_NSEC	1000000ULL

static const s64 mira016_link_freqs[] = {
	[0] = MIRA016_LINK_FREQ_750M,
};

static const char *const mira016_supplies[] = {
	/* Supplies shall be enabled in the following order */
	"vdd11",
	"vdd28",
};

struct mira016 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct clk *xclk;
	u32 xclk_freq;

	struct gpio_desc *reset_gpio;

	struct regulator *supplies[ARRAY_SIZE(mira016_supplies)];

	unsigned long link_freq_bitmap;
	unsigned int bus_config;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;
	struct v4l2_ctrl *prate;

	struct {
		u8 n;
		u8 m;
		u8 cp_div;
		u8 otp_gran;
		u8 gran_tg;
		u8 lut_del;
		u32 esc_h;
		u32 esc_l;
		u32 esc_period;
		u32 byte_period;
		u32 data_rate_mbps;
	} pll;

	struct {
		/*
		 * Mira016 timings registers are expressed in useconds
		 * with a time base of `seq_time_base`:
		 *
		 * line_duration = seq_time_base * row_length;
		 *
		 * As a typical value for seq_time_base is 5.333 nanoseconds
		 * for a 1500Mbps@24MHz configuration, calculate the base time
		 * in picoseconds otherwise we would lose precision.
		 */
		u32 time_base;
		u32 seq_time_base;
		u32 min_exposure_time;
		u32 glob_time;
		u32 row_length;
	} timings;

	struct regmap *regmap;
};

static inline struct mira016 *to_mira016(struct v4l2_subdev *sd)
{
	return container_of_const(sd, struct mira016, sd);
}

/*
 * All Mira016 timing registers are expressed in useconds, while V4L2
 * specifies timings in line.
 *
 * Convert back and forth using these helpers which use the row time (in
 * picoseconds, see the comment on the 'timings' anonymous structure).
 */

static inline u32 mira016_trow_psec(struct mira016 *mira016)
{
	return mira016->timings.seq_time_base * mira016->timings.row_length;
}

static inline u32 mira016_lines_to_usec(struct mira016 *mira016, u32 lines)
{
	return div_u64((u64)lines * mira016_trow_psec(mira016), HZ_PER_MHZ);
}

static inline u32 mira016_nsec_to_lines(struct mira016 *mira016, u32 nsec)
{
	return nsec / (mira016_trow_psec(mira016) / HZ_PER_KHZ);
}

static inline u32 mira016_calc_prate(struct mira016 *mira016, u32 h_tot)
{
	/*
	 * Pixel rate is calculate as the row duration divided by the total line
	 * length.
	 *
	 * pixel time (psec) = t_row(psec) / h_tot
	 * pixel rate (mbps) = 10^12 / pixel_time
	 *		     = 10^12 * h_tot / t_row
	 *		     = h_tot * 10^6 / t_row * 10^6 to avoid overflows
	 */
	u32 trow_psec = mira016_trow_psec(mira016);

	return h_tot * HZ_PER_MHZ / trow_psec * HZ_PER_MHZ;
}

static inline u32 mira016_calc_min_vblank(struct mira016 *mira016, u32 y_tot)
{
	/*
	 * See 3.15.2 Frame Rate, equation 4.
	 *
	 * TODO: The minimum frame duration has to be expanded if embedded data
	 * are used.
	 */
	u32 trow_nsec = mira016_trow_psec(mira016) / HZ_PER_KHZ;
	u32 min_duration_nsec = trow_nsec * (y_tot + 35) + 50 * HZ_PER_KHZ;

	return mira016_nsec_to_lines(mira016, min_duration_nsec) - y_tot;
}

static const struct cci_reg_sequence mira016_8b_fine_gain_init[] = {
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x01bb), 0xb4 },
	{ CCI_REG8(0x01bc), 0xac },
	{ CCI_REG8(0x00d0), 0x0 },
	{ CCI_REG8(0x016e), 0xfd },
	{ CCI_REG8(0x0172), 0x0 },
	{ CCI_REG8(0x0173), 0x0 },
	{ CCI_REG8(0x016f), 0x7e },
	{ CCI_REG8(0x0170), 0x0 },
	{ CCI_REG8(0x0171), 0xfd },
	{ CCI_REG8(0x0174), 0x0 },
	{ CCI_REG8(0x0175), 0x0 },
	{ CCI_REG8(0x0177), 0x78 },
	{ CCI_REG8(0x018b), 0x4 },
	{ CCI_REG8(0x018c), 0xe },
	{ CCI_REG8(0x018d), 0x2 },
	{ CCI_REG8(0x018e), 0x56 },
	{ CCI_REG8(0x018f), 0x6 },
	{ CCI_REG8(0x0190), 0xe },
	{ CCI_REG8(0x00e8), 0x8 },
	{ CCI_REG8(0x01ee), 0x1b },
	{ CCI_REG8(0x01ef), 0x46 },
	{ CCI_REG8(0x01a2), 0x0 },
	{ CCI_REG8(0x01a3), 0x1 },
	{ CCI_REG8(0x031f), 0x0 },
	{ CCI_REG8(0x0320), 0xa },
	{ CCI_REG8(0x01a6), 0x0 },
	{ CCI_REG8(0x01a7), 0x98 },
	{ CCI_REG8(0x01a4), 0x4 },
	{ CCI_REG8(0x01a5), 0x27 },
	{ CCI_REG8(0x0321), 0x4 },
	{ CCI_REG8(0x0322), 0x30 },
	{ CCI_REG8(0x01a8), 0x4 },
	{ CCI_REG8(0x01a9), 0xbe },
	{ CCI_REG8(0x01a0), 0x1 },
	{ CCI_REG8(0x01a1), 0x3a },
	{ CCI_REG8(0x01b2), 0x1 },
	{ CCI_REG8(0x01b3), 0x54 },
	{ CCI_REG8(0x01b0), 0x1 },
	{ CCI_REG8(0x01b1), 0x4d },
	{ CCI_REG8(0x01ac), 0x1 },
	{ CCI_REG8(0x01ad), 0x58 },
	{ CCI_REG8(0x01f0), 0x24 },
	{ CCI_REG8(0x01f3), 0x2 },
	{ CCI_REG8(0xe000), 0x1 },
	{ CCI_REG8(0xe024), 0x3 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x005c), 0x0 },
	{ CCI_REG8(0x005d), 0x18 },
};

static const struct cci_reg_sequence mira016_10b_fine_gain_init[] = {
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x01bb), 0xb4 },
	{ CCI_REG8(0x01bc), 0xac },
	{ CCI_REG8(0x00d0), 0x0 },
	{ CCI_REG8(0x016e), 0xfd },
	{ CCI_REG8(0x0172), 0x0 },
	{ CCI_REG8(0x0173), 0x0 },
	{ CCI_REG8(0x016f), 0x7e },
	{ CCI_REG8(0x0170), 0x0 },
	{ CCI_REG8(0x0171), 0xfd },
	{ CCI_REG8(0x0174), 0x0 },
	{ CCI_REG8(0x0175), 0x0 },
	{ CCI_REG8(0x0177), 0x78 },
	{ CCI_REG8(0x018b), 0x4 },
	{ CCI_REG8(0x018c), 0x4 },
	{ CCI_REG8(0x018d), 0x2 },
	{ CCI_REG8(0x018e), 0x60 },
	{ CCI_REG8(0x018f), 0x6 },
	{ CCI_REG8(0x0190), 0x4 },
	{ CCI_REG8(0x00e8), 0x8 },
	{ CCI_REG8(0xe000), 0x1 },
	{ CCI_REG8(0xe024), 0xf },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x005c), 0x0 },
	{ CCI_REG8(0x005d), 0x18 },
	{ CCI_REG8(0x01ee), 0x1b },
	{ CCI_REG8(0x01ef), 0x46 },
	{ CCI_REG8(0x01a2), 0x0 },
	{ CCI_REG8(0x01a3), 0x1 },
	{ CCI_REG8(0x031f), 0x0 },
	{ CCI_REG8(0x0320), 0xa },
	{ CCI_REG8(0x01a6), 0x0 },
	{ CCI_REG8(0x01a7), 0x98 },
	{ CCI_REG8(0x01a4), 0x4 },
	{ CCI_REG8(0x01a5), 0x27 },
	{ CCI_REG8(0x0321), 0x4 },
	{ CCI_REG8(0x0322), 0x30 },
	{ CCI_REG8(0x01a8), 0x4 },
	{ CCI_REG8(0x01a9), 0xbe },
	{ CCI_REG8(0x01a0), 0x0 },
	{ CCI_REG8(0x01a1), 0xfb },
	{ CCI_REG8(0x01b2), 0x1 },
	{ CCI_REG8(0x01b3), 0x15 },
	{ CCI_REG8(0x01b0), 0x1 },
	{ CCI_REG8(0x01b1), 0xe },
	{ CCI_REG8(0x01ac), 0x1 },
	{ CCI_REG8(0x01ad), 0x19 },
	{ CCI_REG8(0x01f0), 0x24 },
	{ CCI_REG8(0x01f3), 0x2 },
};

static const struct cci_reg_sequence mira016_12b_fixed_gain_init[] = {
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x01bb), 0xb4 },
	{ CCI_REG8(0x01bc), 0xac },
	{ CCI_REG8(0x00d0), 0x0 },
	{ CCI_REG8(0x01f0), 0x7 },
	{ CCI_REG8(0x01f3), 0x1 },
	{ CCI_REG8(0x016e), 0xfd },
	{ CCI_REG8(0x0172), 0x0 },
	{ CCI_REG8(0x0173), 0x0 },
	{ CCI_REG8(0x016f), 0xff },
	{ CCI_REG8(0x0170), 0xff },
	{ CCI_REG8(0x0171), 0xfd },
	{ CCI_REG8(0x0174), 0x0 },
	{ CCI_REG8(0x0175), 0x0 },
	{ CCI_REG8(0x0177), 0xdc },
	{ CCI_REG8(0x018b), 0x4 },
	{ CCI_REG8(0x018c), 0xe },
	{ CCI_REG8(0x018d), 0x2 },
	{ CCI_REG8(0x018e), 0x56 },
	{ CCI_REG8(0x018f), 0xc },
	{ CCI_REG8(0x0190), 0xe },
	{ CCI_REG8(0x00e8), 0x3 },
	{ CCI_REG8(0xe000), 0x1 },
	{ CCI_REG8(0xe024), 0xf },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x005c), 0x0 },
	{ CCI_REG8(0x005d), 0x60 },
	{ CCI_REG8(0x01ee), 0x1b },
	{ CCI_REG8(0x01ef), 0x46 },
	{ CCI_REG8(0x01a2), 0x0 },
	{ CCI_REG8(0x01a3), 0x1 },
	{ CCI_REG8(0x031f), 0x0 },
	{ CCI_REG8(0x0320), 0xa },
	{ CCI_REG8(0x01a6), 0x0 },
	{ CCI_REG8(0x01a7), 0x98 },
	{ CCI_REG8(0x01a4), 0x5 },
	{ CCI_REG8(0x01a5), 0xa7 },
	{ CCI_REG8(0x0321), 0x5 },
	{ CCI_REG8(0x0322), 0xb0 },
	{ CCI_REG8(0x01a8), 0x6 },
	{ CCI_REG8(0x01a9), 0x3e },
	{ CCI_REG8(0x01a0), 0x0 },
	{ CCI_REG8(0x01a1), 0xf3 },
	{ CCI_REG8(0x01b2), 0x1 },
	{ CCI_REG8(0x01b3), 0xd },
	{ CCI_REG8(0x01b0), 0x1 },
	{ CCI_REG8(0x01b1), 0x6 },
	{ CCI_REG8(0x01ac), 0x1 },
	{ CCI_REG8(0x01ad), 0x11 },
};

static const struct cci_reg_sequence mira016_init_sequence[] = {
	{ CCI_REG8(0x01e4), 0x0 },
	{ CCI_REG8(0x01e5), 0x13 },
	{ CCI_REG8(0x01e2), 0x17 },
	{ CCI_REG8(0x01e3), 0xa8 },
	{ CCI_REG8(0x01e6), 0x0 },
	{ CCI_REG8(0x01e7), 0xca },
	{ CCI_REG8(0x016c), 0x1 },
	{ CCI_REG8(0x016b), 0x1 },
	{ CCI_REG8(0x0208), 0x1 },
	{ CCI_REG8(0x0209), 0xf0 },
	{ CCI_REG8(0x020a), 0x3 },
	{ CCI_REG8(0x020b), 0x4d },
	{ CCI_REG8(0x020c), 0x2 },
	{ CCI_REG8(0x020d), 0x10 },
	{ CCI_REG8(0x020e), 0x3 },
	{ CCI_REG8(0x020f), 0x1 },
	{ CCI_REG8(0x0210), 0x0 },
	{ CCI_REG8(0x0211), 0x13 },
	{ CCI_REG8(0x0212), 0x0 },
	{ CCI_REG8(0x0213), 0x3 },
	{ CCI_REG8(0x0214), 0x3 },
	{ CCI_REG8(0x0215), 0xef },
	{ CCI_REG8(0x0216), 0x3 },
	{ CCI_REG8(0x0217), 0xf3 },
	{ CCI_REG8(0x0218), 0x3 },
	{ CCI_REG8(0x0219), 0xf4 },
	{ CCI_REG8(0x021a), 0x0 },
	{ CCI_REG8(0x021b), 0x1 },
	{ CCI_REG8(0x021c), 0x3 },
	{ CCI_REG8(0x021d), 0xf8 },
	{ CCI_REG8(0x021e), 0x0 },
	{ CCI_REG8(0x021f), 0x2 },
	{ CCI_REG8(0x0220), 0x1 },
	{ CCI_REG8(0x0221), 0xf2 },
	{ CCI_REG8(0x0222), 0x3 },
	{ CCI_REG8(0x0223), 0x1b },
	{ CCI_REG8(0x0224), 0x0 },
	{ CCI_REG8(0x0225), 0x21 },
	{ CCI_REG8(0x0226), 0x3 },
	{ CCI_REG8(0x0227), 0xf0 },
	{ CCI_REG8(0x0228), 0x3 },
	{ CCI_REG8(0x0229), 0xf1 },
	{ CCI_REG8(0x022a), 0x3 },
	{ CCI_REG8(0x022b), 0xf2 },
	{ CCI_REG8(0x022c), 0x3 },
	{ CCI_REG8(0x022d), 0xf5 },
	{ CCI_REG8(0x022e), 0x3 },
	{ CCI_REG8(0x022f), 0xf6 },
	{ CCI_REG8(0x0230), 0x0 },
	{ CCI_REG8(0x0231), 0xc1 },
	{ CCI_REG8(0x0232), 0x0 },
	{ CCI_REG8(0x0233), 0x2 },
	{ CCI_REG8(0x0234), 0x1 },
	{ CCI_REG8(0x0235), 0xf2 },
	{ CCI_REG8(0x0236), 0x3 },
	{ CCI_REG8(0x0237), 0x6b },
	{ CCI_REG8(0x0238), 0x3 },
	{ CCI_REG8(0x0239), 0xff },
	{ CCI_REG8(0x023a), 0x3 },
	{ CCI_REG8(0x023b), 0x31 },
	{ CCI_REG8(0x023c), 0x1 },
	{ CCI_REG8(0x023d), 0xf0 },
	{ CCI_REG8(0x023e), 0x3 },
	{ CCI_REG8(0x023f), 0x87 },
	{ CCI_REG8(0x0240), 0x0 },
	{ CCI_REG8(0x0241), 0xa },
	{ CCI_REG8(0x0242), 0x0 },
	{ CCI_REG8(0x0243), 0xb },
	{ CCI_REG8(0x0244), 0x1 },
	{ CCI_REG8(0x0245), 0xf9 },
	{ CCI_REG8(0x0246), 0x3 },
	{ CCI_REG8(0x0247), 0xd },
	{ CCI_REG8(0x0248), 0x0 },
	{ CCI_REG8(0x0249), 0x7 },
	{ CCI_REG8(0x024a), 0x3 },
	{ CCI_REG8(0x024b), 0xef },
	{ CCI_REG8(0x024c), 0x3 },
	{ CCI_REG8(0x024d), 0xf3 },
	{ CCI_REG8(0x024e), 0x3 },
	{ CCI_REG8(0x024f), 0xf4 },
	{ CCI_REG8(0x0250), 0x3 },
	{ CCI_REG8(0x0251), 0x0 },
	{ CCI_REG8(0x0252), 0x0 },
	{ CCI_REG8(0x0253), 0x7 },
	{ CCI_REG8(0x0254), 0x0 },
	{ CCI_REG8(0x0255), 0xc },
	{ CCI_REG8(0x0256), 0x1 },
	{ CCI_REG8(0x0257), 0xf1 },
	{ CCI_REG8(0x0258), 0x3 },
	{ CCI_REG8(0x0259), 0x43 },
	{ CCI_REG8(0x025a), 0x1 },
	{ CCI_REG8(0x025b), 0xf8 },
	{ CCI_REG8(0x025c), 0x3 },
	{ CCI_REG8(0x025d), 0x10 },
	{ CCI_REG8(0x025e), 0x0 },
	{ CCI_REG8(0x025f), 0x7 },
	{ CCI_REG8(0x0260), 0x3 },
	{ CCI_REG8(0x0261), 0xf0 },
	{ CCI_REG8(0x0262), 0x3 },
	{ CCI_REG8(0x0263), 0xf1 },
	{ CCI_REG8(0x0264), 0x3 },
	{ CCI_REG8(0x0265), 0xf2 },
	{ CCI_REG8(0x0266), 0x3 },
	{ CCI_REG8(0x0267), 0xf5 },
	{ CCI_REG8(0x0268), 0x3 },
	{ CCI_REG8(0x0269), 0xf6 },
	{ CCI_REG8(0x026a), 0x3 },
	{ CCI_REG8(0x026b), 0x0 },
	{ CCI_REG8(0x026c), 0x2 },
	{ CCI_REG8(0x026d), 0x87 },
	{ CCI_REG8(0x026e), 0x0 },
	{ CCI_REG8(0x026f), 0x1 },
	{ CCI_REG8(0x0270), 0x3 },
	{ CCI_REG8(0x0271), 0xff },
	{ CCI_REG8(0x0272), 0x3 },
	{ CCI_REG8(0x0273), 0x0 },
	{ CCI_REG8(0x0274), 0x3 },
	{ CCI_REG8(0x0275), 0xff },
	{ CCI_REG8(0x0276), 0x2 },
	{ CCI_REG8(0x0277), 0x87 },
	{ CCI_REG8(0x0278), 0x3 },
	{ CCI_REG8(0x0279), 0x2 },
	{ CCI_REG8(0x027a), 0x3 },
	{ CCI_REG8(0x027b), 0xf },
	{ CCI_REG8(0x027c), 0x3 },
	{ CCI_REG8(0x027d), 0xf7 },
	{ CCI_REG8(0x027e), 0x0 },
	{ CCI_REG8(0x027f), 0x16 },
	{ CCI_REG8(0x0280), 0x0 },
	{ CCI_REG8(0x0281), 0x33 },
	{ CCI_REG8(0x0282), 0x0 },
	{ CCI_REG8(0x0283), 0x4 },
	{ CCI_REG8(0x0284), 0x0 },
	{ CCI_REG8(0x0285), 0x11 },
	{ CCI_REG8(0x0286), 0x3 },
	{ CCI_REG8(0x0287), 0x9 },
	{ CCI_REG8(0x0288), 0x0 },
	{ CCI_REG8(0x0289), 0x2 },
	{ CCI_REG8(0x028a), 0x0 },
	{ CCI_REG8(0x028b), 0x20 },
	{ CCI_REG8(0x028c), 0x0 },
	{ CCI_REG8(0x028d), 0xb5 },
	{ CCI_REG8(0x028e), 0x0 },
	{ CCI_REG8(0x028f), 0xe5 },
	{ CCI_REG8(0x0290), 0x0 },
	{ CCI_REG8(0x0291), 0x12 },
	{ CCI_REG8(0x0292), 0x0 },
	{ CCI_REG8(0x0293), 0xb5 },
	{ CCI_REG8(0x0294), 0x0 },
	{ CCI_REG8(0x0295), 0xe5 },
	{ CCI_REG8(0x0296), 0x0 },
	{ CCI_REG8(0x0297), 0x10 },
	{ CCI_REG8(0x0298), 0x0 },
	{ CCI_REG8(0x0299), 0x2 },
	{ CCI_REG8(0x029a), 0x0 },
	{ CCI_REG8(0x029b), 0x20 },
	{ CCI_REG8(0x029c), 0x0 },
	{ CCI_REG8(0x029d), 0xb5 },
	{ CCI_REG8(0x029e), 0x0 },
	{ CCI_REG8(0x029f), 0xe5 },
	{ CCI_REG8(0x02a0), 0x0 },
	{ CCI_REG8(0x02a1), 0x12 },
	{ CCI_REG8(0x02a2), 0x0 },
	{ CCI_REG8(0x02a3), 0xb5 },
	{ CCI_REG8(0x02a4), 0x0 },
	{ CCI_REG8(0x02a5), 0xe5 },
	{ CCI_REG8(0x02a6), 0x0 },
	{ CCI_REG8(0x02a7), 0x0 },
	{ CCI_REG8(0x02a8), 0x0 },
	{ CCI_REG8(0x02a9), 0x12 },
	{ CCI_REG8(0x02aa), 0x0 },
	{ CCI_REG8(0x02ab), 0x12 },
	{ CCI_REG8(0x02ac), 0x0 },
	{ CCI_REG8(0x02ad), 0x20 },
	{ CCI_REG8(0x02ae), 0x0 },
	{ CCI_REG8(0x02af), 0xb5 },
	{ CCI_REG8(0x02b0), 0x0 },
	{ CCI_REG8(0x02b1), 0xe5 },
	{ CCI_REG8(0x02b2), 0x0 },
	{ CCI_REG8(0x02b3), 0x0 },
	{ CCI_REG8(0x02b4), 0x0 },
	{ CCI_REG8(0x02b5), 0x12 },
	{ CCI_REG8(0x02b6), 0x0 },
	{ CCI_REG8(0x02b7), 0x12 },
	{ CCI_REG8(0x02b8), 0x0 },
	{ CCI_REG8(0x02b9), 0x20 },
	{ CCI_REG8(0x02ba), 0x0 },
	{ CCI_REG8(0x02bb), 0x47 },
	{ CCI_REG8(0x02bc), 0x0 },
	{ CCI_REG8(0x02bd), 0x27 },
	{ CCI_REG8(0x02be), 0x0 },
	{ CCI_REG8(0x02bf), 0xb5 },
	{ CCI_REG8(0x02c0), 0x0 },
	{ CCI_REG8(0x02c1), 0xe5 },
	{ CCI_REG8(0x02c2), 0x0 },
	{ CCI_REG8(0x02c3), 0x0 },
	{ CCI_REG8(0x02c4), 0x0 },
	{ CCI_REG8(0x02c5), 0x4 },
	{ CCI_REG8(0x02c6), 0x0 },
	{ CCI_REG8(0x02c7), 0x43 },
	{ CCI_REG8(0x02c8), 0x0 },
	{ CCI_REG8(0x02c9), 0x1 },
	{ CCI_REG8(0x02ca), 0x3 },
	{ CCI_REG8(0x02cb), 0x2 },
	{ CCI_REG8(0x02cc), 0x0 },
	{ CCI_REG8(0x02cd), 0x8 },
	{ CCI_REG8(0x02ce), 0x3 },
	{ CCI_REG8(0x02cf), 0xff },
	{ CCI_REG8(0x02d0), 0x2 },
	{ CCI_REG8(0x02d1), 0x87 },
	{ CCI_REG8(0x02d2), 0x3 },
	{ CCI_REG8(0x02d3), 0xc7 },
	{ CCI_REG8(0x02d4), 0x3 },
	{ CCI_REG8(0x02d5), 0xf7 },
	{ CCI_REG8(0x02d6), 0x0 },
	{ CCI_REG8(0x02d7), 0x77 },
	{ CCI_REG8(0x02d8), 0x0 },
	{ CCI_REG8(0x02d9), 0x17 },
	{ CCI_REG8(0x02da), 0x0 },
	{ CCI_REG8(0x02db), 0x8 },
	{ CCI_REG8(0x02dc), 0x3 },
	{ CCI_REG8(0x02dd), 0xff },
	{ CCI_REG8(0x02de), 0x0 },
	{ CCI_REG8(0x02df), 0x38 },
	{ CCI_REG8(0x02e0), 0x0 },
	{ CCI_REG8(0x02e1), 0x17 },
	{ CCI_REG8(0x02e2), 0x0 },
	{ CCI_REG8(0x02e3), 0x8 },
	{ CCI_REG8(0x02e4), 0x3 },
	{ CCI_REG8(0x02e5), 0xff },
	{ CCI_REG8(0x02e6), 0x3 },
	{ CCI_REG8(0x02e7), 0xff },
	{ CCI_REG8(0x02e8), 0x3 },
	{ CCI_REG8(0x02e9), 0xff },
	{ CCI_REG8(0x02ea), 0x3 },
	{ CCI_REG8(0x02eb), 0xff },
	{ CCI_REG8(0x02ec), 0x3 },
	{ CCI_REG8(0x02ed), 0xff },
	{ CCI_REG8(0x02ee), 0x3 },
	{ CCI_REG8(0x02ef), 0xff },
	{ CCI_REG8(0x02f0), 0x3 },
	{ CCI_REG8(0x02f1), 0xff },
	{ CCI_REG8(0x02f2), 0x3 },
	{ CCI_REG8(0x02f3), 0xff },
	{ CCI_REG8(0x02f4), 0x3 },
	{ CCI_REG8(0x02f5), 0xff },
	{ CCI_REG8(0x02f6), 0x3 },
	{ CCI_REG8(0x02f7), 0xff },
	{ CCI_REG8(0x02f8), 0x3 },
	{ CCI_REG8(0x02f9), 0xff },
	{ CCI_REG8(0x02fa), 0x3 },
	{ CCI_REG8(0x02fb), 0xff },
	{ CCI_REG8(0x02fc), 0x3 },
	{ CCI_REG8(0x02fd), 0xff },
	{ CCI_REG8(0x02fe), 0x3 },
	{ CCI_REG8(0x02ff), 0xff },
	{ CCI_REG8(0x0300), 0x3 },
	{ CCI_REG8(0x0301), 0xff },
	{ CCI_REG8(0x0302), 0x3 },
	{ CCI_REG8(0x0303), 0xff },
	{ CCI_REG8(0x01e9), 0x0 },
	{ CCI_REG8(0x01e8), 0x19 },
	{ CCI_REG8(0x01ea), 0x35 },
	{ CCI_REG8(0x01eb), 0x37 },
	{ CCI_REG8(0x01ec), 0x64 },
	{ CCI_REG8(0x01ed), 0x6b },
	{ CCI_REG8(0x01f8), 0xf },
	{ CCI_REG8(0x01d8), 0x1 },
	{ CCI_REG8(0x01dc), 0x1 },
	{ CCI_REG8(0x01de), 0x1 },
	{ CCI_REG8(0x0189), 0x1 },
	{ CCI_REG8(0x01b7), 0x1 },
	{ CCI_REG8(0x01c1), 0x7 },
	{ CCI_REG8(0x01c2), 0xf6 },
	{ CCI_REG8(0x01c3), 0xff },
	{ CCI_REG8(0x01c9), 0x7 },
	{ CCI_REG8(0x0325), 0x0 },
	{ CCI_REG8(0xe159), 0x0 },
	{ CCI_REG8(0x033a), 0x0 },
	{ CCI_REG8(0x01b8), 0x1 },
	{ CCI_REG8(0x01ba), 0x33 },
	{ CCI_REG8(0x01be), 0x74 },
	{ CCI_REG8(0x01bf), 0x36 },
	{ CCI_REG8(0x01c0), 0x53 },
	{ CCI_REG8(0x00ef), 0x0 },
	{ CCI_REG8(0x0326), 0x0 },
	{ CCI_REG8(0x00f0), 0x0 },
	{ CCI_REG8(0x00f1), 0x0 },
	{ CCI_REG8(0x0327), 0x0 },
	{ CCI_REG8(0x00f2), 0x0 },
	{ CCI_REG8(0x0071), 0x1 },
	{ CCI_REG8(0x01b4), 0x1 },
	{ CCI_REG8(0x01b5), 0x1 },
	{ CCI_REG8(0x01f1), 0x1 },
	{ CCI_REG8(0x01f4), 0x1 },
	{ CCI_REG8(0x01f5), 0x1 },
	{ CCI_REG8(0x0314), 0x1 },
	{ CCI_REG8(0x0315), 0x1 },
	{ CCI_REG8(0x0316), 0x1 },
	{ CCI_REG8(0x0207), 0x0 },
	{ CCI_REG8(0x4207), 0x2 },
	{ CCI_REG8(0x2207), 0x2 },
	{ CCI_REG8(0xe088), 0x1 },
	{ CCI_REG8(0xe08d), 0x0 },
	{ CCI_REG8(0xe08e), 0x30 },
	{ CCI_REG8(0xe08f), 0xd4 },
	{ CCI_REG8(0xe089), 0x56 },
	{ CCI_REG8(0xe08a), 0x10 },
	{ CCI_REG8(0xe08b), 0x1f },
	{ CCI_REG8(0xe08c), 0xf },
	{ CCI_REG8(0xe0a6), 0x0 },
	{ CCI_REG8(0xe0a9), 0x10 },
	{ CCI_REG8(0xe0aa), 0x0 },
	{ CCI_REG8(0xe0ad), 0xa },
	{ CCI_REG8(0xe0a8), 0x30 },
	{ CCI_REG8(0xe0a7), 0xf },
	{ CCI_REG8(0xe0ac), 0x10 },
	{ CCI_REG8(0xe0ab), 0xf },
	{ CCI_REG8(0x209d), 0x0 },
	{ CCI_REG8(0x0328), 0x0 },
	{ CCI_REG8(0x0063), 0x1 },
	{ CCI_REG8(0x01f7), 0xf },
	{ CCI_REG8(0xe0e3), 0x1 },
	{ CCI_REG8(0xe0e7), 0x3 },
	{ CCI_REG8(0xe33b), 0x0 },
	{ CCI_REG8(0xe336), 0x0 },
	{ CCI_REG8(0xe337), 0x0 },
	{ CCI_REG8(0xe338), 0x0 },
	{ CCI_REG8(0xe339), 0x0 },
	{ CCI_REG8(0x00e9), 0x1 },
	{ CCI_REG8(0x00ea), 0xfe },
	{ CCI_REG8(0x0309), 0x3 },
	{ CCI_REG8(0x030a), 0x2 },
	{ CCI_REG8(0x030b), 0x2 },
	{ CCI_REG8(0x030c), 0x5 },
	{ CCI_REG8(0x030e), 0x15 },
	{ CCI_REG8(0x030d), 0x14 },
	{ CCI_REG8(0x030f), 0x1 },
	{ CCI_REG8(0x0310), 0xd },
	{ CCI_REG8(0x01d0), 0x1f },
	{ CCI_REG8(0x01d1), 0x1f },
	{ CCI_REG8(0x01cd), 0x11 },
	{ CCI_REG8(0x0016), 0x0 },
	{ CCI_REG8(0x0017), 0x5 },
	{ CCI_REG8(0x01f2), 0x0 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0xe0a2), 0x0 },
	{ CCI_REG8(0xe07b), 0x0 },
	{ CCI_REG8(0xe078), 0x0 },
	{ CCI_REG8(0xe079), 0x1 },
	{ CCI_REG8(0xe136), 0x0 },
	{ CCI_REG8(0xe0c6), 0x0 },
	{ CCI_REG8(0xe0c7), 0x0 },
	{ CCI_REG8(0xe0c8), 0x1 },
	{ CCI_REG8(0xe0c9), 0x0 },
	{ CCI_REG8(0xe0ca), 0x0 },
	{ CCI_REG8(0xe0cb), 0x1 },
	{ CCI_REG8(0xe0cc), 0x80 },
	{ CCI_REG8(0xe0cd), 0x80 },
	{ CCI_REG8(0xe0be), 0x2 },
	{ CCI_REG8(0xe0bf), 0x2 },
	{ CCI_REG8(0xe0c4), 0x8 },
	{ CCI_REG8(0xe0c5), 0x8 },
	{ CCI_REG8(0x2075), 0x0 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x001e), 0x1 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x207e), 0x0 },
	{ CCI_REG8(0xe084), 0x0 },
	{ CCI_REG8(0xe085), 0x0 },
	{ CCI_REG8(0xe086), 0x1 },
	{ CCI_REG8(0xe087), 0x0 },
	{ CCI_REG8(0x207f), 0x0 },
	{ CCI_REG8(0x2080), 0x0 },
	{ CCI_REG8(0x2081), 0x3 },
	{ CCI_REG8(0x2082), 0x0 },
	{ CCI_REG8(0x2083), 0x2 },
	{ CCI_REG8(0x0090), 0x0 },
	{ CCI_REG8(0x2097), 0x0 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x0011), 0x3 },
	{ CCI_REG8(0x011d), 0x0 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x0193), 0x8 },
	{ CCI_REG8(0x0194), 0x4 },
	{ CCI_REG8(0xe32c), 0x1 },
	{ CCI_REG8(0xe32d), 0x0 },
	{ CCI_REG8(0xe14e), 0x0 },
	{ CCI_REG8(0xe312), 0x7f },
	{ CCI_REG8(0xe329), 0x1 },
	{ CCI_REG8(0xe32b), 0x0 },
	{ CCI_REG8(0xe331), 0xf },
	{ CCI_REG8(0xe332), 0x3 },
	{ CCI_REG8(0xe32a), 0x0 },
	{ CCI_REG8(0xe11e), 0x1 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0xe009), 0x1 },
	{ CCI_REG8(0x212f), 0x1 },
	{ CCI_REG8(0x2130), 0x1 },
	{ CCI_REG8(0x2131), 0x1 },
	{ CCI_REG8(0x2132), 0x1 },
	{ CCI_REG8(0x2133), 0x1 },
	{ CCI_REG8(0x2134), 0x1 },
	{ CCI_REG8(0x2135), 0x1 },
	{ CCI_REG8(0xe0e1), 0x1 },
	{ CCI_REG8(0x018a), 0x1 },
	{ CCI_REG8(0x00e0), 0x1 },
	{ CCI_REG8(0xe32e), 0x1 },
	{ CCI_REG8(0xe340), 0x1 },
	{ CCI_REG8(0xe004), 0x0 },
	{ CCI_REG8(0xe000), 0x1 },
	{ CCI_REG8(0x000e), 0x0 },
	{ CCI_REG8(0x000f), 0x0 },
	{ CCI_REG8(0x0010), 0x3 },
	{ CCI_REG8(0x0011), 0xe8 },
	{ CCI_REG8(0x0012), 0x0 },
	{ CCI_REG8(0x0013), 0x0 },
	{ CCI_REG8(0x0014), 0x0 },
	{ CCI_REG8(0x0015), 0x0 },
	{ CCI_REG8(0xe004), 0x1 },
	{ CCI_REG8(0x000e), 0x0 },
	{ CCI_REG8(0x000f), 0x0 },
	{ CCI_REG8(0x0010), 0x3 },
	{ CCI_REG8(0x0011), 0xe8 },
	{ CCI_REG8(0x0012), 0x0 },
	{ CCI_REG8(0x0013), 0x0 },
	{ CCI_REG8(0x0014), 0x0 },
	{ CCI_REG8(0x0015), 0x0 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x0057), 0x0 },
	{ CCI_REG8(0x0058), 0x0 },
	{ CCI_REG8(0x0059), 0x2 },
	{ CCI_REG8(0x005a), 0x2 },
	{ CCI_REG8(0x005b), 0x0 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0xe008), 0x0 },
	{ CCI_REG8(0x0006), 0x1 },
	{ CCI_REG8(0xe003), 0x0 },
	{ CCI_REG8(0x0006), 0x0 },
	{ CCI_REG8(0xe008), 0x0 },
	{ CCI_REG8(0xe004), 0x0 },
	{ CCI_REG8(0xe000), 0x1 },
	{ CCI_REG8(0x0031), 0x0 },
	{ CCI_REG8(0xe004), 0x1 },
	{ CCI_REG8(0x0031), 0x0 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x0138), 0x0 },
	{ CCI_REG8(0xe005), 0x0 },
	{ CCI_REG8(0x0139), 0x0 },
	{ CCI_REG8(0x013a), 0x0 },
	{ CCI_REG8(0x013b), 0x64 },
	{ CCI_REG8(0x013c), 0x0 },
	{ CCI_REG8(0x013d), 0x0 },
	{ CCI_REG8(0x013e), 0x64 },
	{ CCI_REG8(0x013f), 0x6 },
	{ CCI_REG8(0x0140), 0x1 },
	{ CCI_REG8(0x0141), 0x10 },
	{ CCI_REG8(0x0142), 0x1 },
	{ CCI_REG8(0x0143), 0x0 },
	{ CCI_REG8(0x0144), 0x0 },
	{ CCI_REG8(0x0146), 0x0 },
	{ CCI_REG8(0x0147), 0x0 },
	{ CCI_REG8(0x0148), 0x0 },
	{ CCI_REG8(0xe004), 0x0 },
	{ CCI_REG8(0xe000), 0x1 },
	{ CCI_REG8(0x0026), 0x0 },
	{ CCI_REG8(0xe004), 0x1 },
	{ CCI_REG8(0x0026), 0x0 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x0169), 0x12 },
	{ CCI_REG8(0xe004), 0x0 },
	{ CCI_REG8(0xe000), 0x1 },
	{ CCI_REG8(0x001c), 0x0 },
	{ CCI_REG8(0x0019), 0x0 },
	{ CCI_REG8(0x001a), 0x7 },
	{ CCI_REG8(0x001b), 0x53 },
	{ CCI_REG8(0x0016), 0x8 },
	{ CCI_REG8(0x0017), 0x0 },
	{ CCI_REG8(0x0018), 0x0 },
	{ CCI_REG8(0xe004), 0x1 },
	{ CCI_REG8(0x001c), 0x0 },
	{ CCI_REG8(0x0019), 0x0 },
	{ CCI_REG8(0x001a), 0x7 },
	{ CCI_REG8(0x001b), 0x53 },
	{ CCI_REG8(0x0016), 0x8 },
	{ CCI_REG8(0x0017), 0x0 },
	{ CCI_REG8(0x0018), 0x0 },
	{ CCI_REG8(0xe004), 0x0 },
	{ CCI_REG8(0xe000), 0x1 },
	{ CCI_REG8(0x001d), 0x0 },
	{ CCI_REG8(0xe004), 0x1 },
	{ CCI_REG8(0x001d), 0x0 },
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0x001a), 0x0 },
	{ CCI_REG8(0x001b), 0x0 },
	{ CCI_REG8(0x001c), 0x0 },
};

static const struct cci_reg_sequence mira016_soft_reset_release[] = {
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0xe009), 0x1 },
	{ CCI_REG8(0x212f), 0x1 },
	{ CCI_REG8(0x2130), 0x1 },
	{ CCI_REG8(0x2131), 0x1 },
	{ CCI_REG8(0x2132), 0x1 },
	{ CCI_REG8(0x2133), 0x1 },
	{ CCI_REG8(0x2134), 0x1 },
	{ CCI_REG8(0x2135), 0x1 },
	{ CCI_REG8(0xe0e1), 0x1 },
	{ CCI_REG8(0x018a), 0x1 },
	{ CCI_REG8(0x00e0), 0x1 },
};

static const struct cci_reg_sequence mira016_soft_reset_assert[] = {
	{ CCI_REG8(0xe000), 0x0 },
	{ CCI_REG8(0xe009), 0x0 },
	{ CCI_REG8(0x018a), 0x0 },
	{ CCI_REG8(0x00e0), 0x0 },
	{ CCI_REG8(0xe0e1), 0x0 },
	{ CCI_REG8(0x212f), 0x0 },
	{ CCI_REG8(0x2130), 0x0 },
	{ CCI_REG8(0x2131), 0x0 },
	{ CCI_REG8(0x2132), 0x0 },
	{ CCI_REG8(0x2133), 0x0 },
	{ CCI_REG8(0x2134), 0x0 },
	{ CCI_REG8(0x2135), 0x0 },
	{ CCI_REG8(0xe009), 0x0 },
};

struct mira016_fine_gain {
	u8 gdig_preamp;
	u8 rg_adcgain;
	u8 rg_mult;
};

/*
 * Fine analogue gain of the Mira016, from the formula:
 *
 * analogue_gain = 57 * 2^(MIRA016_RG_REF - RG_MULT) / (ADC_GAIN + 21)
 *
 * with ADC_GAIN in the [21, 63] range and MIRA016_RG_REF fixed at 2 for Mira016.
 *
 * The analogue gain is realized by controlling the ADC ramp current which gives
 * a gain multiplier up to 4x. In 8-bit mode the digital gain GDIG_PREAMP takes
 * the 10-bit ADC result to 8 bit and gives an additional 4x.
 *
 * @max_shift: 2 in 8-bit mode, 0 in 10-bit mode
 * @bit_factor: 4 in 8-bit mode, 1 in 10-bit mode
 * @fg: the calculated register values
 */
static void mira016_fine_gain(u32 code, u32 gain, struct mira016_fine_gain *fg)
{
	unsigned int bit_shift;
	unsigned int n, k, j;
	int adc;

	/*
	 * In 8-bit mode the digital gain takes the 10-bit ADC result to 8 bit
	 * and gives another 4x.
	 */
	switch (code) {
	case MEDIA_BUS_FMT_Y8_1X8:
		bit_shift = 2;
		break;
	case MEDIA_BUS_FMT_Y10_1X10:
		bit_shift = 0;
		break;
	default:
		/* 12 bit gain is fixed, we shouldn't be getting here. */
		return;
	};

	gain = clamp_t(u32, gain, MIRA016_GAIN_UNIT,
		       (4U * MIRA016_GAIN_UNIT) << bit_shift);

	/*
	 * Analogue octave: largest n with 57 * 2^n <= 84 * gain (84 = 63 + 21).
	 *
	 * Limited to 2 for 10 bits, goes up to 4 for 8 bits. The remainder of
	 * the gain that cannot be obtained through the ADC ramp current goes to
	 * the digital part for the 8-bits mode.
	 */
	for (n = 0; n < 2 + bit_shift; n++) {
		if (MIRA016_ADC_GAIN_VAL(n + 1) > 84U * gain)
			break;
	}

	k = min(n, 2U);
	j = n - k;
	adc = (int)DIV_ROUND_CLOSEST(MIRA016_ADC_GAIN_VAL(n), gain) - 21;

	fg->rg_adcgain = clamp(adc, 0, MIRA016_BIAS_RG_ADCGAIN_MAX);
	fg->rg_mult = MIRA016_FINE_GAIN_REF_MULT - k;
	fg->gdig_preamp = (16U << j) / BIT(bit_shift) - 1;
}

static const u32 mira016_mbus_formats[] = {
	MEDIA_BUS_FMT_Y12_1X12,
	MEDIA_BUS_FMT_Y10_1X10,
	MEDIA_BUS_FMT_Y8_1X8,
};

static void mira016_update_pad_format(struct mira016 *mira016,
				      struct v4l2_mbus_framefmt *fmt)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mira016_mbus_formats); ++i) {
		if (mira016_mbus_formats[i] == fmt->code)
			break;
	}
	if (i == ARRAY_SIZE(mira016_mbus_formats))
		fmt->code = mira016_mbus_formats[0];

	fmt->width = MIRA016_PIXEL_ARRAY_WIDTH;
	fmt->height = MIRA016_PIXEL_ARRAY_HEIGHT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static u32 mira016_calc_phy_timings(struct mira016 *mira016,
				    struct v4l2_mbus_framefmt *fmt)
{
	u32 phy_time = 580;

	/*
	 * PHY timings depend on format, clock mode and integration mode.
	 * Assume 'global timing mode' unconditionally.
	 */

	switch (fmt->code) {
	case MEDIA_BUS_FMT_Y8_1X8:
		break;
	case MEDIA_BUS_FMT_Y10_1X10:
		phy_time += 100;
		break;
	case MEDIA_BUS_FMT_Y12_1X12:
		phy_time += 200;
		break;
	}

	if (mira016->bus_config & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK)
		phy_time += 130;

	return phy_time;
}

static u32 mira016_calc_adc_timings(struct mira016 *mira016,
				    struct v4l2_mbus_framefmt *fmt)
{
	u32 adc_time = 1062;

	/*
	 * ADC timings depend on format, gain mode, clock mode and binning.
	 *
	 * TODO: assume 'fine' gain model for 8 and 10 bit modes and no binning
	 * until proper support is implemented using the Common Raw Sensor
	 * model.
	 */
	switch (fmt->code) {
	case MEDIA_BUS_FMT_Y8_1X8:
		adc_time = 1062;
		break;
	case MEDIA_BUS_FMT_Y10_1X10:
		adc_time = 1062;
		break;
	case MEDIA_BUS_FMT_Y12_1X12:
		adc_time = 1504;
		break;
	}

	return adc_time;
}

static int mira016_calc_row_length(struct mira016 *mira016,
				   struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt = v4l2_subdev_state_get_format(state, 0);
	u32 phy_timings = mira016_calc_phy_timings(mira016, fmt);
	u32 adc_timings = mira016_calc_adc_timings(mira016, fmt);

	mira016->timings.row_length = max(phy_timings, adc_timings);

	return 0;
}

static int mira016_set_pad_format(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_format *fmt)
{
	struct mira016 *mira016 = to_mira016(sd);
	struct v4l2_rect *crop;
	u32 pixel_rate;
	u32 min_vblank;
	u32 gain_max;
	int ret;

	/*
	 * Changing the active image format implies updating the row_length
	 * parameter, which changes the line duration and the pixel rate
	 * consequentially. Also, changing the image format changes the analogue
	 * gain limits.
	 *
	 * Do not allow to change image format while the subdevice is streaming.
	 *
	 * TODO: row length depends on binning, update it also in the
	 * implementation of set_selection.
	 */
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    v4l2_subdev_is_streaming(sd))
		return -EBUSY;

	mira016_update_pad_format(mira016, &fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;

	crop = v4l2_subdev_state_get_crop(state, 0);
	crop->width = fmt->format.width;
	crop->height = fmt->format.height;
	crop->left = MIRA016_PIXEL_ARRAY_LEFT;
	crop->top = MIRA016_PIXEL_ARRAY_TOP;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	switch (fmt->format.code) {
	case MEDIA_BUS_FMT_Y8_1X8:
		gain_max = MIRA016_GAIN_MAX_8B;
		break;
	case MEDIA_BUS_FMT_Y10_1X10:
		gain_max = MIRA016_GAIN_MAX_10B;
		break;
	case MEDIA_BUS_FMT_Y12_1X12:
	default:
		/*
		 * TODO: Clarify how to handle 12 bit 2x fixed gain which
		 * changes the line timings while streaming. Only allow 1x
		 * for the time being.
		 */
		gain_max = MIRA016_GAIN_UNIT;
		break;
	}

	ret = __v4l2_ctrl_modify_range(mira016->gain, MIRA016_GAIN_UNIT,
				       gain_max, 1, MIRA016_GAIN_UNIT);
	if (ret)
		return ret;

	mira016_calc_row_length(mira016, state);

	min_vblank = mira016_calc_min_vblank(mira016, crop->height);
	ret = __v4l2_ctrl_modify_range(mira016->vblank, min_vblank,
				       MIRA016_MAX_VBLANK, 1, min_vblank);
	if (ret)
		return ret;

	pixel_rate = mira016_calc_prate(mira016, crop->width);

	return __v4l2_ctrl_modify_range(mira016->prate, pixel_rate, pixel_rate,
					1, pixel_rate);
}

static int mira016_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(mira016_mbus_formats))
		return -EINVAL;

	code->code = mira016_mbus_formats[code->index];

	return 0;
}

static int mira016_get_selection(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		return 0;

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = MIRA016_NATIVE_WIDTH;
		sel->r.height = MIRA016_NATIVE_HEIGHT;
		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.top = MIRA016_PIXEL_ARRAY_TOP;
		sel->r.left = MIRA016_PIXEL_ARRAY_LEFT;
		sel->r.width = MIRA016_PIXEL_ARRAY_WIDTH;
		sel->r.height = MIRA016_PIXEL_ARRAY_HEIGHT;
		return 0;
	}

	return -EINVAL;
}

static int mira016_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	unsigned int i;

	if (fse->index)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(mira016_mbus_formats); ++i) {
		if (mira016_mbus_formats[i] == fse->code)
			break;
	}
	if (i == ARRAY_SIZE(mira016_mbus_formats))
		return -EINVAL;

	fse->min_width = MIRA016_PIXEL_ARRAY_WIDTH;
	fse->max_width = fse->min_width;
	fse->min_height = MIRA016_PIXEL_ARRAY_HEIGHT;
	fse->max_height = fse->min_height;

	return 0;
}

static int mira016_start_streaming(struct mira016 *mira016)
{
	int ret = 0;

	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 0, &ret);
	cci_write(mira016->regmap, MIRA016_RW_CONTEXT_REG, 0, &ret);
	cci_write(mira016->regmap, MIRA016_CMD_REQ_1_REG, 1, &ret);
	fsleep(10);
	cci_write(mira016->regmap, MIRA016_CMD_REQ_1_REG, 0, &ret);
	fsleep(10);

	return ret;
}

static int mira016_stop_streaming(struct mira016 *mira016)
{
	int ret = 0;

	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 0, &ret);
	cci_write(mira016->regmap, MIRA016_CMD_HALT_BLOCK_REG, 1, &ret);
	fsleep(10);
	cci_write(mira016->regmap, MIRA016_CMD_HALT_BLOCK_REG, 0, &ret);
	fsleep(10);

	return ret;
}

static int mira016_configure_pll(struct mira016 *mira016)
{
	int ret = 0;
	u64 val;

	/* Configure the PLL clock tree. */

	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 0, &ret);

	cci_write(mira016->regmap, MIRA016_PLL_DIV_N_REG, mira016->pll.n, &ret);
	cci_write(mira016->regmap, MIRA016_PLL_DIV_M_REG, mira016->pll.m, &ret);
	cci_write(mira016->regmap, MIRA016_CLKGEN_CP_DIV_REG,
		  mira016->pll.cp_div, &ret);
	cci_write(mira016->regmap, MIRA016_OTP_GRANULARITY_REG,
		  mira016->pll.otp_gran, &ret);
	cci_write(mira016->regmap, MIRA016_GRAN_TG_REG, mira016->pll.gran_tg, &ret);
	cci_write(mira016->regmap, MIRA016_LUT_DEL008_REG,
		  mira016->pll.lut_del, &ret);
	cci_write(mira016->regmap, MIRA016_CLKGEN_TX_ESC_H_REG,
		  mira016->pll.esc_h, &ret);
	cci_write(mira016->regmap, MIRA016_CLKGEN_TX_ESC_L_REG,
		  mira016->pll.esc_l, &ret);
	cci_write(mira016->regmap, MIRA016_PLL_PD_REG, 0, &ret);
	if (ret)
		return ret;

	return read_poll_timeout(cci_read, ret,
				 (ret < 0 || (val & MIRA016_PLL_LOCKED)),
				 500, 10000, false, mira016->regmap,
				 MIRA016_PLL_LOCK_REG, &val, NULL);
}

static int mira016_configure_timings(struct mira016 *mira016)
{
	int ret = 0;
	/* Configure the sensor timings. */

	cci_write(mira016->regmap, MIRA016_TIME_UNIT_REG,
		  mira016->timings.time_base, &ret);
	cci_write(mira016->regmap, MIRA016_GLOB_TIME_REG,
		  mira016->timings.glob_time, &ret);
	cci_write(mira016->regmap, MIRA016_GLOB_TIME_A_REG,
		  mira016->timings.glob_time, &ret);
	cci_write(mira016->regmap, MIRA016_GLOB_TIME_B_REG,
		  mira016->timings.glob_time, &ret);
	cci_write(mira016->regmap, MIRA016_ENTER_LPS_TIME_REG,
		  MIRA016_ENTER_LPS_TIME_1500MBPS, &ret);
	cci_write(mira016->regmap, MIRA016_EXIT_LPS_TIME_REG,
		  MIRA016_EXIT_LPS_TIME_1500MBPS, &ret);
	cci_write(mira016->regmap, MIRA016_LPS_CYCLE_TIME_REG,
		  MIRA016_LPS_CYCLE_TIME_1500MBPS, &ret);

	return ret;
}

static int mira016_configure_horizontal_roi(struct mira016 *mira016,
					    struct v4l2_subdev_state *state)
{
	/*
	 * TODO: Support for binning and subsampling using the Common Raw
	 * Sensor model. This will require to re-calculate the FIFO threshold
	 * and the hsync length with an updated:
	 *
	 *	reduction_factor = 2 ^ xbinning * 2 ^ xsubsampling
	 */
	u16 reduction_factor = 4;
	struct v4l2_mbus_framefmt *fmt = v4l2_subdev_state_get_format(state, 0);
	struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, 0);
	u16 nr_of_pixels = crop->width / reduction_factor;
	u16 xwin_right = crop->left + crop->width - 1;
	u16 fifo_threshold;
	u16 hsync_length;
	int ret = 0;

	cci_write(mira016->regmap, MIRA016_XWIN_LEFT_REG, crop->left, &ret);
	cci_write(mira016->regmap, MIRA016_XWIN_RIGHT_REG, xwin_right, &ret);
	cci_write(mira016->regmap, MIRA016_XMIRROR_REG,
		  mira016->hflip->val, &ret);

	cci_write(mira016->regmap, MIRA016_XSUBSAMPLING_REG, 0, &ret);
	cci_write(mira016->regmap, MIRA016_XBINNING_REG, 0, &ret);

	fifo_threshold = nr_of_pixels - (nr_of_pixels / reduction_factor);
	fifo_threshold = max(70, fifo_threshold);

	/*
	 * FIXME: override for 8 bit mode with magic number from register
	 * sequences. Using the value calculated according to the datasheet
	 * introduces artifacts in 8-bit mode.
	 */
	if (fmt->code == MEDIA_BUS_FMT_Y8_1X8)
		fifo_threshold = 0x28;

	cci_write(mira016->regmap, MIRA016_MIPI_DATA_FIFO_THR_REG,
		  fifo_threshold, &ret);

	/* Assume Global Timing mode when calculating HSYNC length. */
	hsync_length = max(56, crop->width / 2);
	cci_write(mira016->regmap, MIRA016_HSYNC_LENGTH, hsync_length, &ret);

	return ret;
}

static int mira016_configure_vertical_roi(struct mira016 *mira016,
					  struct v4l2_subdev_state *state)
{
	struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, 0);
	int ret = 0;
	u32 top;

	/*
	 * Program the first two windows and zero the other 8 to disable them.
	 *
	 * FIXME: Mira016 workaround - Window 1 is not functional. Program
	 * 2 identical windows in Window 1 and Window 2 but only use the second
	 * one (YWIN_ENA = 2).
	 *
	 * TODO: Support for binning and subsampling using the Common Raw
	 * Sensor model.
	 */

	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 1, &ret);
	cci_write(mira016->regmap, MIRA016_RW_CONTEXT_REG, 0, &ret);
	cci_write(mira016->regmap, MIRA016_YWIN_ENA_REG, 0x02, &ret);
	cci_write(mira016->regmap, MIRA016_YBINNING_REG, 0x00, &ret);
	cci_write(mira016->regmap, MIRA016_RW_CONTEXT_REG, 1, &ret);
	cci_write(mira016->regmap, MIRA016_YWIN_ENA_REG, 0x02, &ret);
	cci_write(mira016->regmap, MIRA016_YBINNING_REG, 0x00, &ret);

	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 0, &ret);
	cci_write(mira016->regmap, MIRA016_YWIN_BLACK_REG, 0x00, &ret);
	cci_write(mira016->regmap, MIRA016_YWIN_DIR_REG,
		  mira016->vflip->val, &ret);

	/* The vertical start position depends on the reading direction. */
	top = mira016->vflip->val ? crop->top + crop->height - 1 : crop->top;
	cci_write(mira016->regmap, MIRA016_YWIN_SIZE_REG(0), crop->height, &ret);
	cci_write(mira016->regmap, MIRA016_YWIN_START_REG(0), top, &ret);
	cci_write(mira016->regmap, MIRA016_YWIN_SUBS_REG(0), 0x00, &ret);
	cci_write(mira016->regmap, MIRA016_YWIN_SIZE_REG(1), crop->height, &ret);
	cci_write(mira016->regmap, MIRA016_YWIN_START_REG(1), top, &ret);
	cci_write(mira016->regmap, MIRA016_YWIN_SUBS_REG(1), 0x00, &ret);

	for (unsigned int i = 2; i < MIRA016_NUM_Y_WIND; ++i) {
		cci_write(mira016->regmap, MIRA016_YWIN_SIZE_REG(i), 0x00, &ret);
		cci_write(mira016->regmap, MIRA016_YWIN_START_REG(i), 0x00, &ret);
		cci_write(mira016->regmap, MIRA016_YWIN_SUBS_REG(i), 0x00, &ret);
	}

	return ret;
}

static int mira016_configure_roi(struct mira016 *mira016,
				 struct v4l2_subdev_state *state)
{
	int ret = 0;

	/*
	 * Write horizontal configuration to context 0 first, then repeat for
	 * context 1.
	 */
	cci_write(mira016->regmap, MIRA016_RW_CONTEXT_REG, 0, &ret);
	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 1, &ret);
	if (ret)
		return ret;

	ret = mira016_configure_horizontal_roi(mira016, state);
	if (ret)
		return ret;

	cci_write(mira016->regmap, MIRA016_RW_CONTEXT_REG, 1, &ret);
	if (ret)
		return ret;

	ret = mira016_configure_horizontal_roi(mira016, state);
	if (ret)
		return ret;

	ret = mira016_configure_vertical_roi(mira016, state);
	if (ret)
		return ret;

	return 0;
}

static int mira016_configure_row_length(struct mira016 *mira016)
{
	int ret = 0;

	/* Configure ROW_LENGTH in both contexts. */
	cci_write(mira016->regmap, MIRA016_RW_CONTEXT_REG, 0, &ret);
	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 1, &ret);
	cci_write(mira016->regmap, MIRA016_ROW_TIMINGS_REG,
		  mira016->timings.row_length, &ret);
	cci_write(mira016->regmap, MIRA016_RW_CONTEXT_REG, 1, &ret);
	cci_write(mira016->regmap, MIRA016_ROW_TIMINGS_REG,
		  mira016->timings.row_length, &ret);

	return ret;
}

static int mira016_configure_format(struct mira016 *mira016,
				    struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt = v4l2_subdev_state_get_format(state, 0);
	u16 bitwidth;
	u8 bitmode;
	u8 csi2_dt;
	int ret = 0;

	switch (fmt->code) {
	case MEDIA_BUS_FMT_Y8_1X8:
		csi2_dt = MIPI_CSI2_DT_RAW8;
		bitwidth = 8;
		bitmode = 0;
		break;
	case MEDIA_BUS_FMT_Y10_1X10:
		csi2_dt = MIPI_CSI2_DT_RAW10;
		bitwidth = 16;
		bitmode = 1;
		break;
	case MEDIA_BUS_FMT_Y12_1X12:
	default:
		csi2_dt = MIPI_CSI2_DT_RAW12;
		bitwidth = 32;
		bitmode = 2;
		break;
	}

	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 0, &ret);
	cci_write(mira016->regmap, MIRA016_DPATH_BITMODE_REG, bitmode, &ret);
	cci_write(mira016->regmap, MIRA016_DPATH_BITWIDTH_A_REG, bitwidth, &ret);
	cci_write(mira016->regmap, MIRA016_DPATH_BITWIDTH_B_REG, bitwidth, &ret);
	cci_write(mira016->regmap, MIRA016_CSI2_DATA_TYPE_REG, csi2_dt, &ret);

	return ret;
}

static int mira016_init_analogue_gain(struct mira016 *mira016,
				      struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt = v4l2_subdev_state_get_format(state, 0);
	const struct cci_reg_sequence *gain_init_seq;
	size_t gain_init_num_regs;

	switch (fmt->code) {
	case MEDIA_BUS_FMT_Y8_1X8:
		gain_init_seq = mira016_8b_fine_gain_init;
		gain_init_num_regs = ARRAY_SIZE(mira016_8b_fine_gain_init);
		break;
	case MEDIA_BUS_FMT_Y10_1X10:
		gain_init_seq = mira016_10b_fine_gain_init;
		gain_init_num_regs = ARRAY_SIZE(mira016_10b_fine_gain_init);
		break;
	case MEDIA_BUS_FMT_Y12_1X12:
	default:
		gain_init_seq = mira016_12b_fixed_gain_init;
		gain_init_num_regs = ARRAY_SIZE(mira016_12b_fixed_gain_init);
		break;
	}

	return cci_multi_reg_write(mira016->regmap, gain_init_seq,
				   gain_init_num_regs, NULL);
}

static int mira016_enable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct mira016 *mira016 = to_mira016(sd);
	u32 xclk_mhz = mira016->xclk_freq / HZ_PER_MHZ;
	/* t_rst is 5000 * clk_in (plus 100usec margin for safety) */
	u32 t_rst_usec = HZ_PER_KHZ / xclk_mhz * 5 + 100;
	int ret;

	/*
	 * vflip and hflip cannot change during streaming as they're programmed
	 * with the ROI windows.
	 */
	__v4l2_ctrl_grab(mira016->hflip, true);
	__v4l2_ctrl_grab(mira016->vflip, true);

	ret = pm_runtime_resume_and_get(mira016->dev);
	if (ret < 0)
		goto err_flip_grab;

	ret = cci_multi_reg_write(mira016->regmap, mira016_soft_reset_release,
				  ARRAY_SIZE(mira016_soft_reset_release),
				  NULL);
	fsleep(t_rst_usec);

	if (!ret)
		ret = mira016_configure_format(mira016, state);
	if (!ret)
		ret = mira016_configure_timings(mira016);
	if (!ret)
		ret = mira016_init_analogue_gain(mira016, state);
	if (!ret)
		ret = mira016_configure_roi(mira016, state);
	if (!ret)
		ret = mira016_configure_row_length(mira016);
	if (!ret)
		ret = __v4l2_ctrl_handler_setup(mira016->sd.ctrl_handler);
	if (!ret)
		ret = mira016_start_streaming(mira016);
	if (ret)
		goto err_rpm_put;

	return 0;

err_rpm_put:
	pm_runtime_put_autosuspend(mira016->dev);
err_flip_grab:
	__v4l2_ctrl_grab(mira016->hflip, false);
	__v4l2_ctrl_grab(mira016->vflip, false);

	return ret;
}

static int mira016_disable_streams(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state, u32 pad,
				   u64 streams_mask)
{
	struct mira016 *mira016 = to_mira016(sd);

	mira016_stop_streaming(mira016);

	cci_multi_reg_write(mira016->regmap, mira016_soft_reset_assert,
			    ARRAY_SIZE(mira016_soft_reset_assert), NULL);

	__v4l2_ctrl_grab(mira016->hflip, false);
	__v4l2_ctrl_grab(mira016->vflip, false);

	pm_runtime_put_autosuspend(mira016->dev);

	return 0;
}

static const struct v4l2_subdev_pad_ops mira016_pad_ops = {
	.enum_mbus_code = mira016_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = mira016_set_pad_format,
	.get_selection = mira016_get_selection,
	.enum_frame_size = mira016_enum_frame_size,
	.enable_streams = mira016_enable_streams,
	.disable_streams = mira016_disable_streams,
};

static const struct v4l2_subdev_video_ops mira016_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_ops mira016_subdev_ops = {
	.video = &mira016_video_ops,
	.pad = &mira016_pad_ops,
};

static int mira016_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.code = MEDIA_BUS_FMT_Y12_1X12,
			.width = MIRA016_PIXEL_ARRAY_WIDTH,
			.height = MIRA016_PIXEL_ARRAY_HEIGHT
		},
	};

	return mira016_set_pad_format(sd, state, &fmt);
}

static const struct v4l2_subdev_internal_ops mira016_internal_ops = {
	.init_state = mira016_init_state,
};

static int mira016_write_exposure_reg(struct mira016 *mira016,
				      u32 exposure_lines)
{
	u32 exposure_us;
	int ret = 0;

	exposure_us = mira016_lines_to_usec(mira016, exposure_lines);

	/* Write Bank 1 context 0 and context 1 */
	cci_write(mira016->regmap, MIRA016_RW_CONTEXT_REG, 0, &ret);
	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 1, &ret);
	cci_write(mira016->regmap, MIRA016_EXP_TIME_L_REG, exposure_us, &ret);

	cci_write(mira016->regmap, MIRA016_RW_CONTEXT_REG, 1, &ret);
	cci_write(mira016->regmap, MIRA016_EXP_TIME_L_REG, exposure_us, &ret);

	return ret;
}

static int mira016_write_frame_duration_reg(struct mira016 *mira016,
					    struct v4l2_subdev_state *state,
					    u32 vblank_lines)
{
	struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, 0);
	u32 frame_duration;
	int ret = 0;

	frame_duration = mira016_lines_to_usec(mira016,
					       crop->height + vblank_lines);

	/* Write Bank 1 context 0 and context 1 */
	cci_write(mira016->regmap, MIRA016_RW_CONTEXT_REG, 0, &ret);
	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 1, &ret);
	cci_write(mira016->regmap, MIRA016_NUM_FRAMES_REG, 1, &ret);
	cci_write(mira016->regmap, MIRA016_TARGET_FRAME_TIME_REG,
		  frame_duration, &ret);

	cci_write(mira016->regmap, MIRA016_RW_CONTEXT_REG, 1, &ret);
	cci_write(mira016->regmap, MIRA016_NUM_FRAMES_REG, 1, &ret);
	cci_write(mira016->regmap, MIRA016_TARGET_FRAME_TIME_REG,
		  frame_duration, &ret);

	return ret;
}

static int mira016_write_analogue_gain(struct mira016 *mira016,
				       struct v4l2_subdev_state *state,
				       u32 gain)
{
	struct v4l2_mbus_framefmt *format;
	struct mira016_fine_gain fg = {};
	int ret = 0;

	format = v4l2_subdev_state_get_format(state, 0);
	switch (format->code) {
	case MEDIA_BUS_FMT_Y8_1X8:
		fallthrough;
	case MEDIA_BUS_FMT_Y10_1X10:
		mira016_fine_gain(format->code, gain, &fg);
		break;
	default:
		/* 12 bit gain is fixed, nothing to do here. */
		return 0;
	}

	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 0, &ret);

	cci_write(mira016->regmap, MIRA016_BIAS_RG_ADCGAIN, fg.rg_adcgain, &ret);
	cci_write(mira016->regmap, MIRA016_BIAS_RG_MULT, fg.rg_mult, &ret);
	cci_write(mira016->regmap, MIRA016_GDIG_PREAMP, fg.gdig_preamp, &ret);

	return ret;
}

static int mira016_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mira016 *mira016 =
		container_of(ctrl->handler, struct mira016, ctrl_handler);
	struct v4l2_subdev_state *state;
	struct v4l2_rect *crop;
	int ret = 0;

	state = v4l2_subdev_get_locked_active_state(&mira016->sd);
	crop = v4l2_subdev_state_get_crop(state, 0);

	if (ctrl->id == V4L2_CID_VBLANK) {
		s32 exposure_min =
			mira016_nsec_to_lines(mira016,
					      mira016->timings.min_exposure_time);
		s32 exposure_max = crop->height + ctrl->val
				 - MIRA016_FRAME_INTEGRATION_DIFF;
		s32 exposure_def = min(exposure_max,
				       mira016->exposure->val);

		ret = __v4l2_ctrl_modify_range(mira016->exposure,
					       exposure_min,
					       exposure_max,
					       mira016->exposure->step,
					       exposure_def);
		if (ret)
			return ret;
	}

	if (!pm_runtime_get_if_in_use(mira016->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = mira016_write_exposure_reg(mira016, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		ret = mira016_write_frame_duration_reg(mira016, state, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = mira016_write_analogue_gain(mira016, state, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(mira016->dev);

	return ret;
}

static const struct v4l2_ctrl_ops mira016_ctrl_ops = {
	.s_ctrl = mira016_set_ctrl,
};

static int mira016_identify_module(struct mira016 *mira016)
{
	int ret = 0;
	u64 val;

	cci_write(mira016->regmap, MIRA016_BANK_SEL_REG, 0, &ret);
	cci_read(mira016->regmap, MIRA016_CHIP_ID_REG, &val, &ret);
	if (ret || val != MIRA016_CHIP_ID)
		return -EINVAL;

	return 0;
}

/* Power/clock management functions */
static int mira016_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mira016 *mira016 = to_mira016(sd);

	gpiod_set_value_cansleep(mira016->reset_gpio, 1);
	clk_disable_unprepare(mira016->xclk);
	for (int i = ARRAY_SIZE(mira016_supplies) - 1; i >= 0; i--)
		regulator_disable(mira016->supplies[i]);

	return 0;
}

static int mira016_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mira016 *mira016 = to_mira016(sd);
	int reg_counter;
	int ret;

	for (reg_counter = 0;
	     reg_counter < ARRAY_SIZE(mira016_supplies); ++reg_counter) {
		ret = regulator_enable(mira016->supplies[reg_counter]);
		if (ret) {
			dev_err(mira016->dev, "failed to enable regulator %s\n",
				mira016_supplies[reg_counter]);
			goto err_reg_disable;
		}

		fsleep(500);
	}

	ret = clk_prepare_enable(mira016->xclk);
	if (ret) {
		dev_err(mira016->dev, "%s: failed to enable clock\n", __func__);
		goto err_reg_disable;
	}

	gpiod_set_value_cansleep(mira016->reset_gpio, 0);

	fsleep(500);

	ret = cci_multi_reg_write(mira016->regmap, mira016_init_sequence,
				  ARRAY_SIZE(mira016_init_sequence), NULL);
	if (!ret)
		ret = mira016_configure_pll(mira016);
	if (ret)
		goto err_reset_assert;

	return 0;

err_reset_assert:
	gpiod_set_value_cansleep(mira016->reset_gpio, 1);
	clk_disable_unprepare(mira016->xclk);
err_reg_disable:
	for (reg_counter--; reg_counter >= 0; reg_counter--)
		regulator_disable(mira016->supplies[reg_counter]);
	return ret;
}

static const struct dev_pm_ops mira016_pm_ops = {
	SET_RUNTIME_PM_OPS(mira016_power_off, mira016_power_on, NULL)
};

static int mira016_init_controls(struct mira016 *mira016)
{
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *hblank;
	u32 min_exposure_lines;
	u32 def_exposure;
	u32 min_vblank;
	u32 def_vblank;
	u32 pixel_rate;
	int ret;

	ret = v4l2_fwnode_device_parse(mira016->dev, &props);
	if (ret)
		return ret;

	ctrl_hdlr = &mira016->ctrl_handler;
	v4l2_ctrl_handler_init(ctrl_hdlr, 12);

	/* By default, PIXEL_RATE is read only */
	pixel_rate = mira016_calc_prate(mira016, MIRA016_PIXEL_ARRAY_WIDTH);
	mira016->prate = v4l2_ctrl_new_std(ctrl_hdlr, NULL, V4L2_CID_PIXEL_RATE,
					   pixel_rate, pixel_rate, 1,
					   pixel_rate);

	def_vblank = mira016_nsec_to_lines(mira016,
					   MIRA016_DEFAULT_DURATION_NSEC);
	def_vblank -= MIRA016_PIXEL_ARRAY_HEIGHT;

	min_vblank = mira016_calc_min_vblank(mira016,
					     MIRA016_PIXEL_ARRAY_HEIGHT);
	mira016->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &mira016_ctrl_ops,
					    V4L2_CID_VBLANK, min_vblank,
					    MIRA016_MAX_VBLANK, 1,
					    def_vblank);

	/* Fixed 0 horizontal blanking. */
	hblank = v4l2_ctrl_new_std(ctrl_hdlr, NULL, V4L2_CID_HBLANK, 0,
				   0, 1, 0);

	link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, NULL, V4L2_CID_LINK_FREQ,
					   0, 0, &mira016_link_freqs[0]);

	min_exposure_lines = mira016_nsec_to_lines(mira016,
						   mira016->timings.min_exposure_time);
	def_exposure = mira016_nsec_to_lines(mira016,
					     MIRA016_EXPOSURE_DEF_NSEC);
	mira016->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &mira016_ctrl_ops,
					      V4L2_CID_EXPOSURE,
					      min_exposure_lines,
					      MIRA016_MAX_EXPOSURE, 1,
					      def_exposure);

	mira016->gain = v4l2_ctrl_new_std(ctrl_hdlr, &mira016_ctrl_ops,
					  V4L2_CID_ANALOGUE_GAIN,
					  MIRA016_GAIN_UNIT, MIRA016_GAIN_UNIT,
					  1, MIRA016_GAIN_UNIT);

	/*
	 * Changing VFLIP requires re-programming the top point, hence we
	 * program flips along with the ROI windows at enable_streams time. As
	 * we grab the flip controls there, there's no need to handle the two
	 * controls while streaming.
	 */
	mira016->hflip = v4l2_ctrl_new_std(ctrl_hdlr, NULL,
					   V4L2_CID_HFLIP, 0, 1, 1, 0);

	mira016->vflip = v4l2_ctrl_new_std(ctrl_hdlr, NULL,
					   V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &mira016_ctrl_ops,
					&props);

	if (ctrl_hdlr->error)
		goto error;

	link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	mira016->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	return ctrl_hdlr->error;
}

static void mira016_timings_calc(struct mira016 *mira016)
{
	/* Time base is the number of input clock cycles per microsecond. */
	mira016->timings.time_base = mira016->xclk_freq / HZ_PER_MHZ;

	/* Seq time base is in pico-seconds, to not lose precision. */
	mira016->timings.seq_time_base = 8 * HZ_PER_MHZ
				       / mira016->pll.data_rate_mbps;

	/* Min exposure time is in nanoseconds. */
	mira016->timings.min_exposure_time = (151 + mira016->pll.lut_del)
					   * mira016->pll.gran_tg
					   * mira016->timings.seq_time_base
					   / HZ_PER_KHZ;

	/* Glob time is in microseconds. */
	mira016->timings.glob_time = (252 + mira016->pll.lut_del)
				   * mira016->pll.gran_tg
				   * mira016->timings.seq_time_base
				   / HZ_PER_MHZ;
}

static u8 mira016_n_to_pll_n(u32 pll_n)
{
	/* Table 11: Lookup table for “N to PLL_DIV_N” mapping */
	static const struct pll_n_div {
		u8 n;
		u8 pll_n;
	} pll_n_lut[] = {
		{ 1, 31 }, { 2, 0  }, { 3, 16 }, { 4, 24 }, { 5, 28 },
		{ 6, 14 }, { 7, 7 }, { 8, 19 }, { 9, 9 }, { 10, 4},
		{ 11, 2 }, { 12, 17 }, { 13, 8 }, { 14, 20 }, { 15, 10 },
		{ 16, 21 }, { 17, 26 }, { 18, 29 }, { 19, 30 }, { 20, 15 },
		{ 21, 23 }, { 22, 27 }, { 23, 13 }, { 24, 22 }, { 25, 11 },
		{ 26, 5 }, { 27, 18 }, { 28, 25 }, { 29, 12 },
		{ 30, 6 }, { 31, 3 }, { 32, 1 }
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(pll_n_lut); ++i) {
		if (pll_n_lut[i].n != pll_n)
			continue;

		return pll_n_lut[i].pll_n;
	}

	return 0;
}

static u8 mira016_m_to_pll_m(u32 pll_m)
{
	/* Table 12: Lookup table for “M to PLL_DIV_M” mapping */
	static const struct pll_m_div {
		u8 m_min;
		u8 m_max;
		u8 pll_m_min;
		u8 pll_m_max;
	} pll_m_lut[] = {
		{ 16, 31, 224, 239 }, { 32, 63, 192, 223 },
		{ 64, 127, 128, 191 }, { 128, 255, 0, 127 },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(pll_m_lut); ++i) {
		const struct pll_m_div *p = &pll_m_lut[i];

		if (pll_m > p->m_max)
			continue;

		return p->pll_m_min + pll_m - p->m_min;
	}

	return 0;
}

static void mira016_pll_calc(struct mira016 *mira016)
{
	/* Multiply link_freq by 2 to account for D-PHY DDR. */
	u64 target_mbps = mira016_link_freqs[__ffs(mira016->link_freq_bitmap)] * 2;
	u32 clk_in = mira016->xclk_freq;
	u32 best = UINT_MAX;
	bool found = false;
	u64 data_rate_mbps;
	u32 n_best = 0;
	u32 m_best = 0;
	u32 gran_tg;
	u32 n;
	u32 m;

	/*
	 * The PLL clock tree is quite simple:
	 *
	 *	clk_in / n * m / 2 = csi_link_freq
	 *
	 * Test all the m/n values in the accepted ranges and if we can't get
	 * the exact output frequency use the best approximation we can find.
	 */
	for (n = MIRA016_PLL_N_MIN; n < MIRA016_PLL_N_MAX; ++n) {
		u32 pll1 = clk_in / n;

		if (pll1 < MIRA016_PLL_PLL1_MIN || pll1 > MIRA016_PLL_PLL1_MAX)
			continue;

		for (m = MIRA016_PLL_M_MIN; m < MIRA016_PLL_M_MAX; ++m) {
			u32 pll2 = pll1 * m;

			if (pll2 < MIRA016_PLL_PLL2_MIN ||
			    pll2 > MIRA016_PLL_PLL2_MAX)
				continue;

			if (pll2 == target_mbps) {
				found = true;
				break;
			}

			if (abs(pll2 - target_mbps) < best) {
				n_best = n;
				m_best = m;
				best = abs(pll2 - target_mbps);
			}
		}
		if (found)
			break;
	}

	if (!found) {
		dev_info(mira016->dev,
			 "Unable to achieve the desired link frequency; frame rate might not be accurate\n");
		n = n_best;
		m = m_best;
	}

	/*
	 * Re-calculate the actual output bandwidth in Mbps. The effective data
	 * rate is required to calculate the sensor base timings.
	 */
	data_rate_mbps = DIV_ROUND_DOWN_ULL((u64)(clk_in / n) * m, HZ_PER_MHZ);
	if (data_rate_mbps < 640 || data_rate_mbps > 1500) {
		dev_info(mira016->dev, "Invalid data rate %llu\n",
			 data_rate_mbps);
		data_rate_mbps = 1500;
	}
	mira016->pll.data_rate_mbps = (u32)data_rate_mbps;

	/*
	 * Translate the m and n values to their register codes using two
	 * dedicated look-up tables and complete the PLL configuration.
	 */
	mira016->pll.n = mira016_n_to_pll_n(n);
	mira016->pll.m = mira016_m_to_pll_m(m);

	mira016->pll.otp_gran = DIV_ROUND_UP_ULL(mira016->pll.data_rate_mbps, 160) - 1;

	/*
	 * As the division might introduce rounding errors, compute gran_tg
	 * in the Kbps domain and then divide by 10^3.
	 */
	gran_tg = DIV_ROUND_DOWN_ULL(data_rate_mbps * KHZ_PER_MHZ, 1500);
	mira016->pll.gran_tg = DIV_ROUND_UP_ULL(gran_tg * 50, KHZ_PER_MHZ);

	mira016->pll.lut_del = DIV_ROUND_UP_ULL(7000, mira016->pll.gran_tg) - 140;
	mira016->pll.byte_period = DIV_ROUND_DOWN_ULL(8000,
						      mira016->pll.data_rate_mbps);
}

static int mira016_parse_endpoint(struct device *dev, struct mira016 *mira016)
{
	struct fwnode_handle *endpoint __free(fwnode_handle) = NULL;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret;

	endpoint = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0, 0);
	if (!endpoint)
		return -ENODEV;

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg);
	if (ret)
		return ret;

	/*
	 * Link frequencies: the driver supports a single link frequency,
	 * no need to check bitmap after this call.
	 */
	ret = v4l2_link_freq_to_bitmap(dev, ep_cfg.link_frequencies,
				       ep_cfg.nr_of_link_frequencies,
				       mira016_link_freqs,
				       ARRAY_SIZE(mira016_link_freqs),
				       &mira016->link_freq_bitmap);
	if (ret) {
		v4l2_fwnode_endpoint_free(&ep_cfg);
		return ret;
	}

	/* TODO: Implement D-PHY configuration to support continuous clock. */
	if (!(ep_cfg.bus.mipi_csi2.flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK)) {
		dev_err(dev, "Continuous clock is not supported\n");
		v4l2_fwnode_endpoint_free(&ep_cfg);
		return -EINVAL;
	}

	mira016->bus_config = ep_cfg.bus.mipi_csi2.flags;

	v4l2_fwnode_endpoint_free(&ep_cfg);

	return 0;
}

static int mira016_get_regulators(struct mira016 *mira016)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(mira016_supplies); i++) {
		struct regulator *supply = devm_regulator_get(mira016->dev,
							      mira016_supplies[i]);
		if (IS_ERR(supply)) {
			dev_err(mira016->dev, "Failed to get supply %s\n",
				mira016_supplies[i]);
			return PTR_ERR(supply);
		}
		mira016->supplies[i] = supply;
	}

	return 0;
}

static int mira016_validate_xclk_freq(struct mira016 *mira016)
{
	u32 xclk_mhz = mira016->xclk_freq / HZ_PER_MHZ;

	/*
	 * Valid clock frequencies input range:
	 * 12MHz <= xclk < 20MHz - 24MHz >= xclk <= 64MHz
	 */
	if (xclk_mhz < 12 || xclk_mhz > 64 || (xclk_mhz >= 20 && xclk_mhz < 24))
		return -EINVAL;

	/* Set the PLL parameters that depend on the input clock frequency */
	mira016->pll.cp_div = xclk_mhz <= 30 ? 1 : 2;
	mira016->pll.esc_h = xclk_mhz < 20 ? 0 : xclk_mhz < 40 ? 1 : 2;
	mira016->pll.esc_l = xclk_mhz < 20 ? 0 : xclk_mhz < 60 ? 1 : 2;
	mira016->pll.esc_period = xclk_mhz < 20 ?
				  DIV_ROUND_CLOSEST(1000, xclk_mhz)
				: xclk_mhz < 40 ?
				  DIV_ROUND_CLOSEST(2000, xclk_mhz)
				: xclk_mhz < 60 ?
				  DIV_ROUND_CLOSEST(3000, xclk_mhz)
				: DIV_ROUND_CLOSEST(4000, xclk_mhz);

	return 0;
}

static int mira016_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mira016 *mira016;
	int ret;

	mira016 = devm_kzalloc(&client->dev, sizeof(*mira016), GFP_KERNEL);
	if (!mira016)
		return -ENOMEM;

	mira016->dev = &client->dev;

	ret = mira016_parse_endpoint(dev, mira016);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&mira016->sd, client, &mira016_subdev_ops);

	mira016->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(mira016->regmap))
		return dev_err_probe(dev, PTR_ERR(mira016->regmap),
				     "failed to initialize CCI\n");

	mira016->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(mira016->xclk))
		return dev_err_probe(dev, PTR_ERR(mira016->xclk),
				     "failed to get xclk\n");

	mira016->xclk_freq = clk_get_rate(mira016->xclk);
	if (mira016_validate_xclk_freq(mira016)) {
		dev_err(dev, "xclk frequency not supported: %d Hz\n",
			mira016->xclk_freq);
		return -EINVAL;
	}

	ret = mira016_get_regulators(mira016);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	mira016->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(mira016->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(mira016->reset_gpio),
				     "failed to get reset gpio\n");

	/*
	 * Calculate the PLL configuration based on the link frequency
	 * selected by .dts and compute the sensor timing bases.
	 *
	 * Initialize row_length to a value matching the default format for
	 * exposure and frame time limits calculations.
	 */
	mira016_pll_calc(mira016);
	mira016_timings_calc(mira016);
	mira016->timings.row_length = 1504;

	ret = mira016_power_on(dev);
	if (ret)
		return ret;

	/* Enable runtime PM and power on the device */
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = mira016_identify_module(mira016);
	if (ret)
		goto error_power_off;

	ret = mira016_init_controls(mira016);
	if (ret)
		goto error_power_off;

	/* Initialize subdev */
	mira016->sd.internal_ops = &mira016_internal_ops;
	mira016->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	mira016->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pads */
	mira016->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&mira016->sd.entity, 1, &mira016->pad);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init entity pads\n");
		goto error_handler_free;
	}

	mira016->sd.state_lock = mira016->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&mira016->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret, "subdev init error\n");
		goto error_media_entity;
	}

	ret = v4l2_async_register_subdev_sensor(&mira016->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret,
			      "failed to register sensor sub-device\n");
		goto error_subdev_cleanup;
	}

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(&mira016->sd);
error_media_entity:
	media_entity_cleanup(&mira016->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(mira016->sd.ctrl_handler);
error_power_off:
	pm_runtime_disable(dev);
	mira016_power_off(dev);
	pm_runtime_set_suspended(dev);
	return ret;
}

static void mira016_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mira016 *mira016 = to_mira016(sd);

	v4l2_ctrl_handler_free(mira016->sd.ctrl_handler);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(&mira016->sd);
	media_entity_cleanup(&sd->entity);

	pm_runtime_disable(mira016->dev);
	if (!pm_runtime_status_suspended(mira016->dev)) {
		mira016_power_off(mira016->dev);
		pm_runtime_set_suspended(mira016->dev);
	}
	pm_runtime_dont_use_autosuspend(mira016->dev);
}

static const struct of_device_id mira016_dt_ids[] = {
	{ .compatible = "ams,mira016" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mira016_dt_ids);

static struct i2c_driver mira016_i2c_driver = {
	.driver = {
		.name = "mira016",
		.of_match_table	= mira016_dt_ids,
		.pm = pm_ptr(&mira016_pm_ops),
	},
	.probe = mira016_probe,
	.remove = mira016_remove,
};

module_i2c_driver(mira016_i2c_driver);

MODULE_AUTHOR("Jacopo Mondi <jacopo.mondi@ideasonboard.com>");
MODULE_DESCRIPTION("ams OSRAM MIRA016 sensor driver");
MODULE_LICENSE("GPL");
