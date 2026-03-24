// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX662 CMOS Image Sensor Driver
 *
 * Copyright (C) 2022 Soho Enterprise Ltd.
 * Copyright (C) 2026 Raspberry Pi.
 *
 *
 */
#define DEBUG
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

static bool hcg_mode;
module_param(hcg_mode, bool, 0664);
MODULE_PARM_DESC(hcg_mode, "Enable HCG mode");

#define IMX662_STANDBY		CCI_REG8(0x3000)
#define IMX662_REGHOLD		CCI_REG8(0x3001)
#define IMX662_XMSTA		CCI_REG8(0x3002)
#define IMX662_INCK_SEL		CCI_REG8(0x3014)
	#define IMX662_INCK_SEL_74_25	0x00
	#define IMX662_INCK_SEL_37_125	0x01
	#define IMX662_INCK_SEL_72	0x02
	#define IMX662_INCK_SEL_27	0x03
	#define IMX662_INCK_SEL_24	0x04
#define IMX662_LANE_RATE	CCI_REG8(0x3015)
	#define IMX662_LANE_RATE_2376	0x00
	#define IMX662_LANE_RATE_2079	0x01
	#define IMX662_LANE_RATE_1782	0x02
	#define IMX662_LANE_RATE_1440	0x03
	#define IMX662_LANE_RATE_1188	0x04
	#define IMX662_LANE_RATE_891	0x05
	#define IMX662_LANE_RATE_720	0x06
	#define IMX662_LANE_RATE_594	0x07
#define IMX662_FLIP_WINMODEH	CCI_REG8(0x3020)
#define IMX662_FLIP_WINMODEV	CCI_REG8(0x3021)
#define IMX662_ADBIT		CCI_REG8(0x3022)
#define IMX662_MDBIT		CCI_REG8(0x3023)
#define IMX662_VMAX		CCI_REG24_LE(0x3028)
	#define IMX662_VMAX_MAX		0x03ffff
#define IMX662_HMAX		CCI_REG16_LE(0x302c)
	#define IMX662_HMAX_MAX		0xffff
#define IMX662_FR_FDG_SEL0	CCI_REG8(0x3030)
	#define IMX662_FDG_SEL0_LCG	0x00
	#define IMX662_FDG_SEL0_HCG	0x01
#define IMX662_FR_FDG_SEL1	CCI_REG8(0x3031)
#define IMX662_FR_FDG_SEL2	CCI_REG8(0x3032)
#define IMX662_CSI_LANE_MODE	CCI_REG8(0x3040)
#define IMX662_EXPOSURE		CCI_REG24_LE(0x3050)
#define IMX662_GAIN		CCI_REG8(0x3070)

#define IMX662_PIX_HSTART	CCI_REG16_LE(0x303c)
#define IMX662_PIX_HWIDTH	CCI_REG16_LE(0x303e)
	/* PIX_HWIDTH = n4 * 16, and n4 > 32 */
	#define IMX662_PIX_HWIDTH_MIN	528
#define IMX662_PIX_VSTART	CCI_REG16_LE(0x3044)
#define IMX662_PIX_VWIDTH	CCI_REG16_LE(0x3046)
	/* VMAX is required to be >=820, and VMAX >= PIX_VWIDTH + 70 */
	#define IMX662_PIX_VWIDTH_MIN	750

#define IMX662_EXPOSURE_MIN	1
#define IMX662_EXPOSURE_STEP	1
/* Exposure must be this many lines less than VMAX */
#define IMX662_EXPOSURE_OFFSET  4

#define IMX662_NATIVE_WIDTH		1936
#define IMX662_NATIVE_HEIGHT		1101
#define IMX662_PIXEL_ARRAY_LEFT		0
#define IMX662_PIXEL_ARRAY_TOP		0
#define IMX662_PIXEL_ARRAY_WIDTH	1936
#define IMX662_PIXEL_ARRAY_HEIGHT	1100

static const char * const imx662_supply_name[] = {
	"vdda",
	"vddd",
	"vdddo",
};

#define IMX662_NUM_SUPPLIES ARRAY_SIZE(imx662_supply_name)

struct imx662_mode {
	u32 width;
	u32 height;
	u32 hmax_min;
	u32 vmax_min;
	struct v4l2_rect crop;

	const struct cci_reg_sequence *mode_data;
	u32 mode_data_size;
};

struct imx662_sensor_cfg {
	bool mono;
};

struct imx662 {
	struct device *dev;
	struct clk *xclk;
	u8 inck_sel;
	struct regmap *regmap;
	bool hcg_mode;
	u8 nlanes;
	u8 bpp;

	const struct imx662_sensor_cfg *sensor_cfg;

	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_mbus_framefmt current_format;
	const struct imx662_mode *current_mode;

	struct regulator_bulk_data supplies[IMX662_NUM_SUPPLIES];
	struct gpio_desc *rst_gpio;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *exposure;
};

static const struct cci_reg_sequence imx662_global_settings[] = {
	{ CCI_REG8(0x3002), 0x00}, //#Master mode operation start
	{ CCI_REG8(0x301A), 0x00}, // HDR mode select (Normal)
	{ CCI_REG8(0x301B), 0x00}, // Normal/binning
	{ CCI_REG8(0x301C), 0x00}, // XVS sub sample
	{ CCI_REG8(0x301E), 0x01}, // virtual channel
	{ CCI_REG8(0x3060), 0x16}, // DOL output timing
	{ CCI_REG8(0x3061), 0x01}, // DOL output timing
	{ CCI_REG8(0x3062), 0x00}, // DOL output timing
	{ CCI_REG8(0x3064), 0xC4}, // DOL output timing
	{ CCI_REG8(0x3065), 0x0C}, // DOL output timing
	{ CCI_REG8(0x3066), 0x00}, // DOL output timing
	{ CCI_REG8(0x3069), 0x00}, // Direct Gain Enable
	{ CCI_REG8(0x3072), 0x00}, // GAIN SEF1
	{ CCI_REG8(0x3073), 0x00}, // GAIN SEF1
	{ CCI_REG8(0x3074), 0x00}, // GAIN SEF2
	{ CCI_REG8(0x3075), 0x00}, // GAIN SEF2
	{ CCI_REG8(0x3081), 0x00}, // EXP_GAIN
	{ CCI_REG8(0x308C), 0x00}, // Clear HDR DGAIN
	{ CCI_REG8(0x308D), 0x01}, // Clear HDR DGAIN
	{ CCI_REG8(0x3094), 0x00}, // CHDR AGAIN LG
	{ CCI_REG8(0x3095), 0x00}, // CHDR AGAIN LG
	{ CCI_REG8(0x3096), 0x00}, // CHDR AGAIN1
	{ CCI_REG8(0x3097), 0x00}, // CHDR AGAIN1
	{ CCI_REG8(0x309C), 0x00}, // CHDR AGAIN HG
	{ CCI_REG8(0x309D), 0x00}, // CHDR AGAIN HG
	{ CCI_REG8(0x30A4), 0xAA}, // XVS/XHS OUT
	{ CCI_REG8(0x30A6), 0x0F}, // XVS/XHS DRIVE HiZ
	{ CCI_REG8(0x30CC), 0x00}, // XVS width
	{ CCI_REG8(0x30CD), 0x00}, // XHS width
	{ CCI_REG8(0x3400), 0x01}, // GAIN Adjust
	{ CCI_REG8(0x3444), 0xAC}, // RESERVED
	{ CCI_REG8(0x3460), 0x21}, // Normal Mode 22H=C HDR mode
	{ CCI_REG8(0x3492), 0x08}, // RESERVED
	{ CCI_REG8(0x3A50), 0xFF}, // Normal 12bit
	{ CCI_REG8(0x3A51), 0x03}, // Normal 12bit
	{ CCI_REG8(0x3A52), 0x00}, // AD 12bit
	{ CCI_REG8(0x3B00), 0x39}, // RESERVED
	{ CCI_REG8(0x3B23), 0x2D}, // RESERVED
	{ CCI_REG8(0x3B45), 0x04}, // RESERVED
	{ CCI_REG8(0x3C0A), 0x1F}, // RESERVED
	{ CCI_REG8(0x3C0B), 0x1E}, // RESERVED
	{ CCI_REG8(0x3C38), 0x21}, // RESERVED
	{ CCI_REG8(0x3C40), 0x06}, // Normal mode. CHDR=05h
	{ CCI_REG8(0x3C44), 0x00}, // RESERVED
	{ CCI_REG8(0x3CB6), 0xD8}, // RESERVED
	{ CCI_REG8(0x3CC4), 0xDA}, // RESERVED
	{ CCI_REG8(0x3E24), 0x79}, // RESERVED
	{ CCI_REG8(0x3E2C), 0x15}, // RESERVED
	{ CCI_REG8(0x3EDC), 0x2D}, // RESERVED
	{ CCI_REG8(0x4498), 0x05}, // RESERVED
	{ CCI_REG8(0x449C), 0x19}, // RESERVED
	{ CCI_REG8(0x449D), 0x00}, // RESERVED
	{ CCI_REG8(0x449E), 0x32}, // RESERVED
	{ CCI_REG8(0x449F), 0x01}, // RESERVED
	{ CCI_REG8(0x44A0), 0x92}, // RESERVED
	{ CCI_REG8(0x44A2), 0x91}, // RESERVED
	{ CCI_REG8(0x44A4), 0x8C}, // RESERVED
	{ CCI_REG8(0x44A6), 0x87}, // RESERVED
	{ CCI_REG8(0x44A8), 0x82}, // RESERVED
	{ CCI_REG8(0x44AA), 0x78}, // RESERVED
	{ CCI_REG8(0x44AC), 0x6E}, // RESERVED
	{ CCI_REG8(0x44AE), 0x69}, // RESERVED
	{ CCI_REG8(0x44B0), 0x92}, // RESERVED
	{ CCI_REG8(0x44B2), 0x91}, // RESERVED
	{ CCI_REG8(0x44B4), 0x8C}, // RESERVED
	{ CCI_REG8(0x44B6), 0x87}, // RESERVED
	{ CCI_REG8(0x44B8), 0x82}, // RESERVED
	{ CCI_REG8(0x44BA), 0x78}, // RESERVED
	{ CCI_REG8(0x44BC), 0x6E}, // RESERVED
	{ CCI_REG8(0x44BE), 0x69}, // RESERVED
	{ CCI_REG8(0x44C1), 0x01}, // RESERVED
	{ CCI_REG8(0x44C2), 0x7F}, // RESERVED
	{ CCI_REG8(0x44C3), 0x01}, // RESERVED
	{ CCI_REG8(0x44C4), 0x7A}, // RESERVED
	{ CCI_REG8(0x44C5), 0x01}, // RESERVED
	{ CCI_REG8(0x44C6), 0x7A}, // RESERVED
	{ CCI_REG8(0x44C7), 0x01}, // RESERVED
	{ CCI_REG8(0x44C8), 0x70}, // RESERVED
	{ CCI_REG8(0x44C9), 0x01}, // RESERVED
	{ CCI_REG8(0x44CA), 0x6B}, // RESERVED
	{ CCI_REG8(0x44CB), 0x01}, // RESERVED
	{ CCI_REG8(0x44CC), 0x6B}, // RESERVED
	{ CCI_REG8(0x44CD), 0x01}, // RESERVED
	{ CCI_REG8(0x44CE), 0x5C}, // RESERVED
	{ CCI_REG8(0x44CF), 0x01}, // RESERVED
	{ CCI_REG8(0x44D0), 0x7F}, // RESERVED
	{ CCI_REG8(0x44D1), 0x01}, // RESERVED
	{ CCI_REG8(0x44D2), 0x7F}, // RESERVED
	{ CCI_REG8(0x44D3), 0x01}, // RESERVED
	{ CCI_REG8(0x44D4), 0x7A}, // RESERVED
	{ CCI_REG8(0x44D5), 0x01}, // RESERVED
	{ CCI_REG8(0x44D6), 0x7A}, // RESERVED
	{ CCI_REG8(0x44D7), 0x01}, // RESERVED
	{ CCI_REG8(0x44D8), 0x70}, // RESERVED
	{ CCI_REG8(0x44D9), 0x01}, // RESERVED
	{ CCI_REG8(0x44DA), 0x6B}, // RESERVED
	{ CCI_REG8(0x44DB), 0x01}, // RESERVED
	{ CCI_REG8(0x44DC), 0x6B}, // RESERVED
	{ CCI_REG8(0x44DD), 0x01}, // RESERVED
	{ CCI_REG8(0x44DE), 0x5C}, // RESERVED
	{ CCI_REG8(0x44DF), 0x01}, // RESERVED
	{ CCI_REG8(0x4534), 0x1C}, // RESERVED
	{ CCI_REG8(0x4535), 0x03}, // RESERVED
	{ CCI_REG8(0x4538), 0x1C}, // RESERVED
	{ CCI_REG8(0x4539), 0x1C}, // RESERVED
	{ CCI_REG8(0x453A), 0x1C}, // RESERVED
	{ CCI_REG8(0x453B), 0x1C}, // RESERVED
	{ CCI_REG8(0x453C), 0x1C}, // RESERVED
	{ CCI_REG8(0x453D), 0x1C}, // RESERVED
	{ CCI_REG8(0x453E), 0x1C}, // RESERVED
	{ CCI_REG8(0x453F), 0x1C}, // RESERVED
	{ CCI_REG8(0x4540), 0x1C}, // RESERVED
	{ CCI_REG8(0x4541), 0x03}, // RESERVED
	{ CCI_REG8(0x4542), 0x03}, // RESERVED
	{ CCI_REG8(0x4543), 0x03}, // RESERVED
	{ CCI_REG8(0x4544), 0x03}, // RESERVED
	{ CCI_REG8(0x4545), 0x03}, // RESERVED
	{ CCI_REG8(0x4546), 0x03}, // RESERVED
	{ CCI_REG8(0x4547), 0x03}, // RESERVED
	{ CCI_REG8(0x4548), 0x03}, // RESERVED
	{ CCI_REG8(0x4549), 0x03}, // RESERVED
};

static const struct cci_reg_sequence imx662_1080p_common_settings[] = {
	/* mode settings */
	{ CCI_REG8(0x3018), 0x04}, // WINMODE - window cropping mode
	{ IMX662_FR_FDG_SEL1, 0x00 },
	{ IMX662_FR_FDG_SEL2, 0x00 },
};

/* supported link frequencies */
static const s64 imx662_link_freq_2lanes[] = {
	594000000,
};

static const s64 imx662_link_freq_4lanes[] = {
	297000000,
};

/*
 * In this function and in the similar ones below we rely on imx662_probe()
 * to ensure that nlanes is either 2 or 4.
 */
static inline const s64 *imx662_link_freqs_ptr(const struct imx662 *imx662)
{
	if (imx662->nlanes == 2)
		return imx662_link_freq_2lanes;
	else
		return imx662_link_freq_4lanes;
}

/* Mode configs */
static const struct imx662_mode imx662_modes[] = {
	{
		/*
		 * Note that this mode reads out the areas documented as
		 * "effective margin for color processing" and "effective pixel
		 * ignored area" in the datasheet.
		 */
		.width = 1936,
		.height = 1100,
		.hmax_min = 990 * 3,
		.vmax_min = 1250,
		.crop = {
			.left = IMX662_PIXEL_ARRAY_LEFT,
			.top = IMX662_PIXEL_ARRAY_TOP,
			.width = IMX662_NATIVE_WIDTH,
			.height = IMX662_NATIVE_HEIGHT,
		},
		.mode_data = imx662_1080p_common_settings,
		.mode_data_size = ARRAY_SIZE(imx662_1080p_common_settings),
	},
};

#define IMX662_NUM_MODES ARRAY_SIZE(imx662_modes)

static inline struct imx662 *to_imx662(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx662, sd);
}

#define IMX662_NUM_FORMATS 2

static __u32 imx662_get_code(const struct imx662 *imx662, unsigned int index)
{
	if (index >= IMX662_NUM_FORMATS)
		return 0;

	if (imx662->sensor_cfg->mono)
		return index ? MEDIA_BUS_FMT_Y12_1X12 : MEDIA_BUS_FMT_Y10_1X10;

	return index ? MEDIA_BUS_FMT_SRGGB12_1X12 : MEDIA_BUS_FMT_SRGGB10_1X10;
}

static int imx662_set_exposure(struct imx662 *imx662,
			       const struct v4l2_mbus_framefmt *format,
			       u32 value)
{
	u32 exposure = (format->height + imx662->vblank->val) - value - 1;
	int ret;

	ret = cci_write(imx662->regmap, IMX662_EXPOSURE, exposure, NULL);
	if (ret)
		dev_err(imx662->dev, "Unable to write exposure\n");

	return ret;
}

static int imx662_set_hmax(struct imx662 *imx662,
			   const struct v4l2_mbus_framefmt *format,
			   u32 value)
{
	u32 hmax = (value + format->width) / 3;
	int ret;

	ret = cci_write(imx662->regmap, IMX662_HMAX, hmax, NULL);
	if (ret)
		dev_err(imx662->dev, "Error setting HMAX register\n");

	return ret;
}

static int imx662_set_vmax(struct imx662 *imx662,
			   const struct v4l2_mbus_framefmt *format,
			   u32 value)
{
	u32 vmax = value + format->height;

	int ret;

	ret = cci_write(imx662->regmap, IMX662_VMAX, vmax, NULL);
	if (ret)
		dev_err(imx662->dev, "Unable to write vmax\n");

	/*
	 * Changing vblank changes the allowed range for exposure.
	 * We don't supply the current exposure as default here as it
	 * may lie outside the new range. We will reset it just below.
	 */
	__v4l2_ctrl_modify_range(imx662->exposure,
				 IMX662_EXPOSURE_MIN,
				 vmax - IMX662_EXPOSURE_OFFSET,
				 IMX662_EXPOSURE_STEP,
				 vmax - IMX662_EXPOSURE_OFFSET);

	/*
	 * Becuse of the way exposure works for this sensor, updating
	 * vblank causes the effective exposure to change, so we must
	 * set it back to the "new" correct value.
	 */
	imx662_set_exposure(imx662, format, imx662->exposure->val);

	return ret;
}

/* Stop streaming */
static int imx662_stop_streaming(struct imx662 *imx662)
{
	int ret;

	ret = cci_write(imx662->regmap, IMX662_STANDBY, 0x01, NULL);

	msleep(30);

	return cci_write(imx662->regmap, IMX662_XMSTA, 0x01, &ret);
}

static int imx662_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx662 *imx662 = container_of(ctrl->handler,
					     struct imx662, ctrls);
	const struct v4l2_mbus_framefmt *format;
	struct v4l2_subdev_state *state;
	int ret = 0;

	if (ctrl->flags & V4L2_CTRL_FLAG_READ_ONLY)
		return 0;

	/* V4L2 controls values will be applied only when power is already up */
	if (!pm_runtime_get_if_in_use(imx662->dev))
		return 0;

	state = v4l2_subdev_get_locked_active_state(&imx662->sd);
	format = v4l2_subdev_state_get_format(state, 0);

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(imx662->regmap, IMX662_GAIN, ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = imx662_set_exposure(imx662, format, ctrl->val);
		break;
	case V4L2_CID_HBLANK:
		ret = imx662_set_hmax(imx662, format, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		ret = imx662_set_vmax(imx662, format, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
		ret = cci_write(imx662->regmap, IMX662_FLIP_WINMODEH, ctrl->val,
				NULL);
		break;
	case V4L2_CID_VFLIP:
		ret = cci_write(imx662->regmap, IMX662_FLIP_WINMODEV, ctrl->val,
				NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(imx662->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx662_ctrl_ops = {
	.s_ctrl = imx662_set_ctrl,
};

static int imx662_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	const struct imx662 *imx662 = to_imx662(sd);

	if (code->index >= IMX662_NUM_FORMATS)
		return -EINVAL;

	code->code = imx662_get_code(imx662, code->index);

	return 0;
}

static int imx662_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	const struct imx662 *imx662 = to_imx662(sd);

	if (fse->code != imx662_get_code(imx662, 0) &&
	    fse->code != imx662_get_code(imx662, 1))
		return -EINVAL;

	if (fse->index >= IMX662_NUM_MODES)
		return -EINVAL;

	fse->min_width = imx662_modes[fse->index].width;
	fse->max_width = imx662_modes[fse->index].width;
	fse->min_height = imx662_modes[fse->index].height;
	fse->max_height = imx662_modes[fse->index].height;

	return 0;
}

static u64 imx662_calc_pixel_rate(struct imx662 *imx662)
{
	/*
	 * Horizontal blanking timing is programmed based on units of clock,
	 * not directly of pixel rate.
	 * Analysing the register settings provided by Sony shows that the HMAX
	 * register takes units of 74.25MHz. However that only gives a register
	 * value of 660 for 90fps, which doesn't fit within V4L2's idea of how
	 * line timings would be calculated.
	 *
	 * Multiplying the 74.25MHz by 3 gives us a line length of 1980, which
	 * is long enough, and still provides a simple conversion back to the
	 * required register setting.
	 */
	return 222750000;
}

static int imx662_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx662 *imx662 = to_imx662(sd);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;
	unsigned int i;

	crop = v4l2_subdev_state_get_crop(state, fmt->pad);
	format = v4l2_subdev_state_get_format(state, fmt->pad);

	format->width = crop->width;
	format->height = crop->height;

	for (i = 0; i < IMX662_NUM_FORMATS; i++)
		if (imx662_get_code(imx662, i) == fmt->format.code)
			break;

	if (i >= IMX662_NUM_FORMATS)
		i = 0;

	format->code = imx662_get_code(imx662, i);
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;
	format->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_NONE;

	fmt->format = *format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
/*
		FIXME: Update blanking and exposure ranges on new mode.
		Slightly irrelevant as there is only one mode at present.

		imx662->current_mode = mode;

		__v4l2_ctrl_modify_range(imx662->hblank,
					 mode->hmax_min - mode->width,
					 IMX662_HMAX_MAX - mode->width,
					 1, mode->hmax_min - mode->width);
		__v4l2_ctrl_s_ctrl(imx662->hblank,
				   mode->hmax_min - mode->width);

		__v4l2_ctrl_modify_range(imx662->vblank,
					 mode->vmax_min - mode->height,
					 IMX662_VMAX_MAX - mode->height,
					 2,
					 mode->vmax_min - mode->height);
		__v4l2_ctrl_s_ctrl(imx662->vblank,
				   mode->vmax_min - mode->height);

		__v4l2_ctrl_modify_range(imx662->exposure,
					 IMX662_EXPOSURE_MIN,
					 mode->vmax_min - 2,
					 IMX662_EXPOSURE_STEP,
					 mode->vmax_min - 2);
*/
	}

	return 0;
}

static int imx662_set_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;
	struct v4l2_rect rect;

	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	/*
	 * Clamp the crop rectangle boundaries and align them to a multiple of 4
	 * pixels to satisfy hardware requirements.
	 */
	rect.left = clamp(ALIGN(sel->r.left, 2), 0,
			  IMX662_PIXEL_ARRAY_WIDTH - IMX662_PIX_HWIDTH_MIN);
	rect.top = clamp(ALIGN(sel->r.top, 4), 0,
			 IMX662_PIXEL_ARRAY_HEIGHT - IMX662_PIX_HWIDTH_MIN);
	rect.width = clamp_t(unsigned int, ALIGN(sel->r.width, 16),
			     IMX662_PIX_VWIDTH_MIN, IMX662_PIXEL_ARRAY_WIDTH);
	rect.height = clamp_t(unsigned int, ALIGN(sel->r.height, 4),
			      IMX662_PIX_VWIDTH_MIN, IMX662_PIXEL_ARRAY_HEIGHT);

	rect.width = min_t(unsigned int, rect.width,
			   IMX662_PIXEL_ARRAY_WIDTH - rect.left);
	rect.height = min_t(unsigned int, rect.height,
			    IMX662_PIXEL_ARRAY_HEIGHT - rect.top);

	crop = v4l2_subdev_state_get_crop(state, sel->pad);

	if (rect.width != crop->width || rect.height != crop->height) {
		/*
		 * Reset the output image size if the crop rectangle size has
		 * been modified.
		 */
		format = v4l2_subdev_state_get_format(state, sel->pad);
		format->width = rect.width;
		format->height = rect.height;
	}

	*crop = rect;
	sel->r = rect;

	return 0;
}

static int imx662_entity_init_state(struct v4l2_subdev *subdev,
				    struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_selection sel = {
		.target = V4L2_SEL_TGT_CROP,
		.r.width = IMX662_PIXEL_ARRAY_WIDTH & ~1,
		.r.height = IMX662_PIXEL_ARRAY_HEIGHT,
	};
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.format = {
			.width = IMX662_PIXEL_ARRAY_WIDTH & ~1,
			.height = IMX662_PIXEL_ARRAY_HEIGHT,
		},
	};

	imx662_set_selection(subdev, sd_state, &sel);
	imx662_set_fmt(subdev, sd_state, &fmt);

	return 0;
}

static int imx662_write_current_format(struct imx662 *imx662,
				       const struct v4l2_mbus_framefmt *format)
{
	int ret;

	switch (format->code) {
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_Y10_1X10:
		ret = cci_write(imx662->regmap, IMX662_ADBIT, 0x00, NULL);
		ret = cci_write(imx662->regmap, IMX662_MDBIT, 0x00, &ret);
		ret = cci_write(imx662->regmap, CCI_REG8(0x3a50), 0x62, &ret);
		ret = cci_write(imx662->regmap, CCI_REG8(0x3a51), 0x01, &ret);
		ret = cci_write(imx662->regmap, CCI_REG8(0x3a52), 0x19, &ret);
		break;
	case MEDIA_BUS_FMT_SRGGB12_1X12:
	case MEDIA_BUS_FMT_Y12_1X12:
		ret = cci_write(imx662->regmap, IMX662_ADBIT, 0x01, NULL);
		ret = cci_write(imx662->regmap, IMX662_MDBIT, 0x01, &ret);
		ret = cci_write(imx662->regmap, CCI_REG8(0x3a50), 0xff, &ret);
		ret = cci_write(imx662->regmap, CCI_REG8(0x3a51), 0x03, &ret);
		ret = cci_write(imx662->regmap, CCI_REG8(0x3a52), 0x00, &ret);
		break;
	default:
		dev_err(imx662->dev, "Unknown bpp %u\n", imx662->bpp);
		return -EINVAL;
	}

	return ret;
}

static int imx662_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP: {
		sel->r = *v4l2_subdev_state_get_crop(state, sel->pad);

		return 0;
	}

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = IMX662_NATIVE_WIDTH;
		sel->r.height = IMX662_NATIVE_HEIGHT;

		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.top = IMX662_PIXEL_ARRAY_TOP;
		sel->r.left = IMX662_PIXEL_ARRAY_LEFT;
		sel->r.width = IMX662_PIXEL_ARRAY_WIDTH;
		sel->r.height = IMX662_PIXEL_ARRAY_HEIGHT;

		return 0;
	}

	return -EINVAL;
}

/* Start streaming */
static int imx662_start_streaming(struct imx662 *imx662,
				  struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *format;
	const struct v4l2_rect *crop;
	int ret;

	format = v4l2_subdev_state_get_format(state, 0);
	crop = v4l2_subdev_state_get_crop(state, 0);

	/* Set init register settings */
	ret = cci_multi_reg_write(imx662->regmap, imx662_global_settings,
				  ARRAY_SIZE(imx662_global_settings), NULL);

	cci_write(imx662->regmap, IMX662_INCK_SEL, imx662->inck_sel, &ret);

	/* Apply the register values related to current frame format */
	ret = imx662_write_current_format(imx662, format);

	/* Apply default values of current mode */
	cci_multi_reg_write(imx662->regmap, imx662->current_mode->mode_data,
			    imx662->current_mode->mode_data_size, &ret);

	cci_write(imx662->regmap, IMX662_PIX_HSTART, crop->left & ~1, &ret);
	cci_write(imx662->regmap, IMX662_PIX_VSTART, crop->top & ~1, &ret);
	cci_write(imx662->regmap, IMX662_PIX_HWIDTH, crop->width & ~1, &ret);
	cci_write(imx662->regmap, IMX662_PIX_VWIDTH, crop->height & ~1, &ret);

	cci_write(imx662->regmap, IMX662_FR_FDG_SEL0,
		  (imx662->hcg_mode ? IMX662_FDG_SEL0_HCG : IMX662_FDG_SEL0_LCG),
		  &ret);

	/* Apply lane config registers of current mode */
	cci_write(imx662->regmap, IMX662_CSI_LANE_MODE,
		  imx662->nlanes == 2 ? 0x01 : 0x03, &ret);

	cci_write(imx662->regmap, IMX662_LANE_RATE,
		  imx662->nlanes == 2 ? IMX662_LANE_RATE_1188 :
					IMX662_LANE_RATE_594,
		  &ret);

	if (ret < 0) {
		dev_err(imx662->dev, "Failed sending start streaming setup\n");
		return ret;
	}

	/* Apply customized values from user */
	ret = __v4l2_ctrl_handler_setup(imx662->sd.ctrl_handler);
	if (ret) {
		dev_err(imx662->dev, "Could not sync v4l2 controls\n");
		return ret;
	}

	cci_write(imx662->regmap, IMX662_STANDBY, 0x00, &ret);
	cci_write(imx662->regmap, IMX662_XMSTA, 0x00, &ret);
	if (ret < 0)
		dev_err(imx662->dev, "Failed sending start streaming setup pt2\n");

	return ret;
}

static int imx662_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx662 *imx662 = to_imx662(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	if (enable) {
		ret = pm_runtime_resume_and_get(imx662->dev);
		if (ret < 0)
			goto unlock_and_return;

		ret = imx662_start_streaming(imx662, state);
		if (ret) {
			dev_err(imx662->dev, "Start stream failed\n");
			pm_runtime_put(imx662->dev);
			goto unlock_and_return;
		}
	} else {
		imx662_stop_streaming(imx662);
		pm_runtime_put(imx662->dev);
	}

	/*
	 * vflip should not be changed during streaming as the sensor will
	 * produce an invalid frame.
	 * hflip can be changed whilst streaming without generating an invalid
	 * frame.
	 */
	__v4l2_ctrl_grab(imx662->vflip, enable);

unlock_and_return:
	v4l2_subdev_unlock_state(state);

	return ret;
}

static int imx662_get_regulators(struct device *dev, struct imx662 *imx662)
{
	unsigned int i;

	for (i = 0; i < IMX662_NUM_SUPPLIES; i++)
		imx662->supplies[i].supply = imx662_supply_name[i];

	return devm_regulator_bulk_get(dev, IMX662_NUM_SUPPLIES,
				       imx662->supplies);
}

static int imx662_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx662 *imx662 = to_imx662(sd);
	int ret;

	ret = clk_prepare_enable(imx662->xclk);
	if (ret) {
		dev_err(dev, "Failed to enable clock\n");
		return ret;
	}

	ret = regulator_bulk_enable(IMX662_NUM_SUPPLIES, imx662->supplies);
	if (ret) {
		dev_err(dev, "Failed to enable regulators\n");
		clk_disable_unprepare(imx662->xclk);
		return ret;
	}

	usleep_range(1, 2);
	gpiod_set_value_cansleep(imx662->rst_gpio, 0);
	usleep_range(30000, 31000);

	return 0;
}

static int imx662_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx662 *imx662 = to_imx662(sd);

	clk_disable_unprepare(imx662->xclk);
	gpiod_set_value_cansleep(imx662->rst_gpio, 1);
	regulator_bulk_disable(IMX662_NUM_SUPPLIES, imx662->supplies);

	return 0;
}

static const struct dev_pm_ops imx662_pm_ops = {
	SET_RUNTIME_PM_OPS(imx662_power_off, imx662_power_on, NULL)
};

static const struct v4l2_subdev_video_ops imx662_video_ops = {
	.s_stream = imx662_set_stream,
};

static const struct v4l2_subdev_pad_ops imx662_pad_ops = {
	//.init_cfg = imx662_entity_init_cfg,
	.enum_mbus_code = imx662_enum_mbus_code,
	.enum_frame_size = imx662_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx662_set_fmt,
	.get_selection = imx662_get_selection,
	.set_selection = imx662_set_selection,
};

static const struct v4l2_subdev_ops imx662_subdev_ops = {
	.video = &imx662_video_ops,
	.pad = &imx662_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx662_internal_ops = {
	.init_state = imx662_entity_init_state,
};

static const struct media_entity_operations imx662_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/*
 * Returns 0 if all link frequencies used by the driver for the given number
 * of MIPI data lanes are mentioned in the device tree, or the value of the
 * first missing frequency otherwise.
 */
static int imx662_check_link_freqs(const struct imx662 *imx662,
				   const struct v4l2_fwnode_endpoint *ep)
{
	const s64 *freqs = imx662_link_freqs_ptr(imx662);

	if (ep->nr_of_link_frequencies != 1)
		return -EINVAL;
	if (freqs[0] != ep->link_frequencies[0])
		return -EINVAL;

	return 0;
}

static int imx662_probe(struct i2c_client *client)
{
	struct v4l2_fwnode_device_properties props;
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	/* Only CSI2 is supported for now: */
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	const struct imx662_mode *mode;
	struct v4l2_subdev_state *state;
	struct v4l2_ctrl *ctrl;
	struct imx662 *imx662;
	u32 xclk_freq;
	int ret;

	imx662 = devm_kzalloc(dev, sizeof(*imx662), GFP_KERNEL);
	if (!imx662)
		return -ENOMEM;

	imx662->dev = dev;
	imx662->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx662->regmap)) {
		dev_err(dev, "Unable to initialize I2C\n");
		return PTR_ERR(imx662->regmap);
	}

	imx662->hcg_mode = hcg_mode;

	imx662->sensor_cfg =
		(const struct imx662_sensor_cfg *)of_device_get_match_data(imx662->dev);

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "Endpoint node not found\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret == -ENXIO) {
		dev_err(dev, "Unsupported bus type, should be CSI2\n");
		goto free_err;
	} else if (ret) {
		dev_err(dev, "Parsing endpoint node failed\n");
		goto free_err;
	}

	/* Get number of data lanes */
	imx662->nlanes = ep.bus.mipi_csi2.num_data_lanes;
	if (imx662->nlanes != 2 && imx662->nlanes != 4) {
		dev_err(dev, "Invalid data lanes: %d\n", imx662->nlanes);
		ret = -EINVAL;
		goto free_err;
	}

	dev_dbg(dev, "Using %u data lanes\n", imx662->nlanes);

	/* Check that link frequences for all the modes are in device tree */
	ret = imx662_check_link_freqs(imx662, &ep);
	if (ret) {
		dev_err(dev, "Link frequency configuration not supported\n");
		goto free_err;
	}

	/* get system clock (xclk) */
	imx662->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(imx662->xclk)) {
		dev_err(dev, "Could not get xclk");
		ret = PTR_ERR(imx662->xclk);
		goto free_err;
	}

	ret = fwnode_property_read_u32(dev_fwnode(dev), "clock-frequency",
				       &xclk_freq);
	if (ret) {
		dev_err(dev, "Could not get xclk frequency\n");
		goto free_err;
	}

	/* external clock can be one of a range of values - validate it */
	switch (xclk_freq) {
	case 74250000:
		imx662->inck_sel = IMX662_INCK_SEL_74_25;
		break;
	case 37125000:
		imx662->inck_sel = IMX662_INCK_SEL_37_125;
		break;
	case 72000000:
		imx662->inck_sel = IMX662_INCK_SEL_72;
		break;
	case 27000000:
		imx662->inck_sel = IMX662_INCK_SEL_27;
		break;
	case 24000000:
		imx662->inck_sel = IMX662_INCK_SEL_24;
		break;
	default:
		dev_err(dev, "External clock frequency %u is not supported\n",
			xclk_freq);
		ret = -EINVAL;
		goto free_err;
	}

	ret = clk_set_rate(imx662->xclk, xclk_freq);
	if (ret) {
		dev_err(dev, "Could not set xclk frequency\n");
		goto free_err;
	}

	ret = imx662_get_regulators(dev, imx662);
	if (ret < 0) {
		dev_err(dev, "Cannot get regulators\n");
		goto free_err;
	}

	imx662->rst_gpio = devm_gpiod_get_optional(dev, "reset",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(imx662->rst_gpio)) {
		dev_err(dev, "Cannot get reset gpio\n");
		ret = PTR_ERR(imx662->rst_gpio);
		goto free_err;
	}

	imx662->current_mode = &imx662_modes[0];

	v4l2_ctrl_handler_init(&imx662->ctrls, 11);

	v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops,
			  V4L2_CID_ANALOGUE_GAIN, imx662->hcg_mode ? 34 : 0,
			  100, 1, imx662->hcg_mode ? 34 : 0);

	mode = imx662->current_mode;
	imx662->hblank = v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops,
					   V4L2_CID_HBLANK,
					   mode->hmax_min - mode->width,
					   IMX662_HMAX_MAX - mode->width, 1,
					   mode->hmax_min - mode->width);

	imx662->vblank = v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops,
					   V4L2_CID_VBLANK,
					   mode->vmax_min - mode->height,
					   IMX662_VMAX_MAX - mode->height, 2,
					   mode->vmax_min - mode->height);

	imx662->exposure = v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX662_EXPOSURE_MIN,
					     mode->vmax_min - 2,
					     IMX662_EXPOSURE_STEP,
					     mode->vmax_min - 2);

	imx662->hflip = v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	imx662->vflip = v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);

	ctrl = v4l2_ctrl_new_int_menu(&imx662->ctrls, NULL,
				      V4L2_CID_LINK_FREQ, 0, 0,
				      imx662_link_freqs_ptr(imx662));
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx662->pixel_rate = v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       imx662_calc_pixel_rate(imx662),
					       imx662_calc_pixel_rate(imx662),
					       1,
					       imx662_calc_pixel_rate(imx662));

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto free_ctrl;

	ret = v4l2_ctrl_new_fwnode_properties(&imx662->ctrls, &imx662_ctrl_ops,
					      &props);
	if (ret)
		goto free_ctrl;

	imx662->sd.ctrl_handler = &imx662->ctrls;

	if (imx662->ctrls.error) {
		dev_err(dev, "Control initialization error %d\n",
			imx662->ctrls.error);
		ret = imx662->ctrls.error;
		goto free_ctrl;
	}

	v4l2_i2c_subdev_init(&imx662->sd, client, &imx662_subdev_ops);
	imx662->sd.internal_ops = &imx662_internal_ops;
	imx662->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx662->sd.dev = &client->dev;
	imx662->sd.entity.ops = &imx662_subdev_entity_ops;
	imx662->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	imx662->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx662->sd.entity, 1, &imx662->pad);
	if (ret < 0) {
		dev_err(dev, "Could not register media entity\n");
		goto free_ctrl;
	}

	imx662->sd.state_lock = imx662->sd.ctrl_handler->lock;

	ret = v4l2_subdev_init_finalize(&imx662->sd);
	if (ret < 0) {
		dev_err(imx662->dev, "subdev initialization error %d\n", ret);
		goto free_ctrl;
	}

	state = v4l2_subdev_lock_and_get_active_state(&imx662->sd);
	//imx662_ctrl_update(imx662, imx662->current_mode);
	v4l2_subdev_unlock_state(state);

	ret = v4l2_async_register_subdev(&imx662->sd);
	if (ret < 0) {
		dev_err(dev, "Could not register v4l2 device\n");
		goto free_entity;
	}

	/* Power on the device to match runtime PM state below */
	ret = imx662_power_on(dev);
	if (ret < 0) {
		dev_err(dev, "Could not power on the device\n");
		goto free_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	v4l2_fwnode_endpoint_free(&ep);

	return 0;

free_entity:
	media_entity_cleanup(&imx662->sd.entity);
free_ctrl:
	v4l2_ctrl_handler_free(&imx662->ctrls);
free_err:
	v4l2_fwnode_endpoint_free(&ep);

	return ret;
}

static void imx662_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx662 *imx662 = to_imx662(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(imx662->dev);
	if (!pm_runtime_status_suspended(imx662->dev))
		imx662_power_off(imx662->dev);
	pm_runtime_set_suspended(imx662->dev);
}

static const struct imx662_sensor_cfg imx662_colour_cfg = {
};

static const struct imx662_sensor_cfg imx662_mono_cfg = {
	.mono = true,
};

static const struct of_device_id imx662_of_match[] = {
	{ .compatible = "sony,imx662aaqr", .data = &imx662_colour_cfg },
	{ .compatible = "sony,imx662aamr", .data = &imx662_mono_cfg },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx662_of_match);

static struct i2c_driver imx662_i2c_driver = {
	.probe  = imx662_probe,
	.remove = imx662_remove,
	.driver = {
		.name  = "imx662",
		.pm = &imx662_pm_ops,
		.of_match_table = of_match_ptr(imx662_of_match),
	},
};

module_i2c_driver(imx662_i2c_driver);

MODULE_DESCRIPTION("Sony IMX662 CMOS Image Sensor Driver");
MODULE_AUTHOR("Tetsuya Nomura <tetsuya.nomura@soho-enterprise.com>");
MODULE_AUTHOR("Dave Stevenson <dave.stevenson@raspberrypi.com>");
MODULE_LICENSE("GPL");
