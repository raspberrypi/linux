// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Sony IMX662 CMOS Image Sensor
 *
 * Copyright (C) 2026 Alexander Shiyan <eagle.alexander923@gmail.com>
 * Copyright (C) 2026 Raspberry Pi
 *
 * Some parts of code taken from imx662.c by:
 * Copyright (C) 2022 Soho Enterprise Ltd.
 * Author: Tetsuya Nomura <tetsuya.nomura@soho-enterprise.com>
 *
 * Some parts of code taken from imx290.c by:
 * Copyright (C) 2019 FRAMOS GmbH.
 * Copyright (C) 2019 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define IMX662_STANDBY				CCI_REG8(0x3000)
#define IMX662_REGHOLD				CCI_REG8(0x3001)
#define IMX662_XMSTA				CCI_REG8(0x3002)
#define IMX662_INCK_SEL				CCI_REG8(0x3014)
#	define IMX662_INCK_SEL_74_25		(0x00)
#	define IMX662_INCK_SEL_37_125		(0x01)
#	define IMX662_INCK_SEL_72		(0x02)
#	define IMX662_INCK_SEL_27		(0x03)
#	define IMX662_INCK_SEL_24		(0x04)
#define IMX662_DATARATE_SEL			CCI_REG8(0x3015)

enum {
	IMX662_DATARATE_2376	= 0,
	IMX662_DATARATE_2079	= 1,
	IMX662_DATARATE_1782	= 2,
	IMX662_DATARATE_1440	= 3,
	IMX662_DATARATE_1188	= 4,
	IMX662_DATARATE_891	= 5,
	IMX662_DATARATE_720	= 6,
	IMX662_DATARATE_594	= 7,
	IMX662_DATARATE_MAX
};

#define IMX662_WINMODE				CCI_REG8(0x3018)
#define IMX662_WDMODE				CCI_REG8(0x301a)
#define IMX662_THIN_V_EN			CCI_REG8(0x301c)
#define IMX662_VCMODE				CCI_REG8(0x301e)
#define IMX662_HREVERSE				CCI_REG8(0x3020)
#define IMX662_VREVERSE				CCI_REG8(0x3021)
#define IMX662_ADBIT				CCI_REG8(0x3022)
#define IMX662_MDBIT				CCI_REG8(0x3023)
#define IMX662_VMAX				CCI_REG24_LE(0x3028)
#	define IMX662_VMAX_MIN			40
#	define IMX662_VMAX_MAX			(0x0ffffc)
#define IMX662_HMAX				CCI_REG16_LE(0x302c)
#	define IMX662_HMAX_MAX			(0xfff0)
#define IMX662_FDG_SEL0				CCI_REG8(0x3030)
#	define IMX662_FDG_SEL0_LCG		(0x00)
#	define IMX662_FDG_SEL0_HCG		(0x01)
#define IMX662_FDG_SEL1				CCI_REG8(0x3031)
#define IMX662_PIX_HST				CCI_REG16_LE(0x303c)
#define IMX662_LANEMODE				CCI_REG8(0x3040)
#define IMX662_PIX_HWIDTH			CCI_REG16_LE(0x303e)
#define IMX662_PIX_VST				CCI_REG16_LE(0x3044)
#define IMX662_PIX_VWIDTH			CCI_REG16_LE(0x3046)
#define IMX662_SHR0				CCI_REG24_LE(0x3050)
#define IMX662_SHR1				CCI_REG24_LE(0x3054)
#	define IMX662_SHR1_DEF			(0x000093)
#define IMX662_RHS1				CCI_REG24_LE(0x3060)
#	define IMX662_RHS1_DEF			(0x000095)
#define IMX662_CHDR_GAIN_EN			CCI_REG8(0x3069)
#define IMX662_GAIN				CCI_REG16_LE(0x3070)
#	define IMX662_GAIN_HCG_MIN		(0x22)
#	define IMX662_GAIN_MAX			240
#define IMX662_GAIN1				CCI_REG16_LE(0x3072)
#define IMX662_EXP_GAIN				CCI_REG8(0x3081)
#	define IMX662_EXP_GAIN_STEP		(20)
#	define IMX662_EXP_GAIN_MAX		(5)
#define IMX662_CHDR_DGAIN0_HG			CCI_REG16_LE(0x308c)
#define IMX662_CHDR_AGAIN0_LG			CCI_REG16_LE(0x3094)
#define IMX662_CHDR_AGAIN1			CCI_REG16_LE(0x3096)
#define IMX662_CHDR_AGAIN0_HG			CCI_REG16_LE(0x309c)
#define IMX662_BLKLEVEL				CCI_REG16_LE(0x30dc)
#define IMX662_GAIN_PGC_FIDMD			CCI_REG8(0x3400)

#define IMX662_PIXEL_RATE			(74250000LL)

#define IMX662_NATIVE_WIDTH			(1936U)
#define IMX662_NATIVE_HEIGHT			(1100U)
#define IMX662_PIXEL_ARRAY_LEFT			(0U)
#define IMX662_PIXEL_ARRAY_TOP			(0U)
#define IMX662_PIXEL_ARRAY_WIDTH		(1936U)
#define IMX662_PIXEL_ARRAY_HEIGHT		(1096U)
#define IMX662_MIN_CROP_WIDTH			(80U)
#define IMX662_MIN_CROP_HEIGHT			(180U)
#define IMX662_CROP_WIDTH_STEP			(16U)
#define IMX662_CROP_HEIGHT_STEP			(4U)

/* Number of lines by which exposure must be less than VMAX */
#define IMX662_EXPOSURE_OFFSET			4

enum imx662_colour_variant {
	IMX662_VARIANT_COLOUR,
	IMX662_VARIANT_MONO,
	IMX662_VARIANT_MAX
};

static const char * const imx662_supply_names[] = {
	"avdd",
	"dvdd",
	"ovdd",
};

struct imx662_format {
	u8 ad_md_bit;
	u32 hmax_min_lane2[IMX662_DATARATE_MAX];
	u32 hmax_min_lane4[IMX662_DATARATE_MAX];
	u32 code[IMX662_VARIANT_MAX];
};

struct imx662 {
	struct clk *clk;
	struct regmap *regmap;

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct regulator_bulk_data supplies[ARRAY_SIZE(imx662_supply_names)];
	struct gpio_desc *reset;

	u8 link_freq_index;

	u8 inck;

	unsigned int num_data_lanes;

	enum imx662_colour_variant variant;

	const struct imx662_format *format;

	struct v4l2_ctrl_handler ctrls;

	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;
};

static inline struct imx662 *to_imx662(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx662, sd);
}

static const struct cci_reg_sequence imx662_regs_common[] = {
	{ IMX662_WINMODE, 0x04 },
	{ IMX662_VCMODE, 0x01 },
	{ IMX662_SHR1, IMX662_SHR1_DEF },
	{ IMX662_RHS1, IMX662_RHS1_DEF },
	{ IMX662_CHDR_GAIN_EN, 0x00 },
	{ IMX662_CHDR_DGAIN0_HG, 0x0100 },
	{ IMX662_CHDR_AGAIN0_LG, 0x0000 },
	{ IMX662_CHDR_AGAIN1, 0x0000 },
	{ IMX662_CHDR_AGAIN0_HG, 0x0000 },
	{ IMX662_WDMODE, 0x00 },
	{ IMX662_THIN_V_EN, 0x00 },
	{ IMX662_FDG_SEL1, 0x00 },
	{ IMX662_GAIN1, 0x0000 },
	{ IMX662_EXP_GAIN, 0x00 },
	{ IMX662_GAIN_PGC_FIDMD, 0x01 },
};

static const s64 imx662_link_freqs[] = {
	[IMX662_DATARATE_2376]	= 2376000000LL / 2,
	[IMX662_DATARATE_2079]	= 2079000000LL / 2,
	[IMX662_DATARATE_1782]	= 1782000000LL / 2,
	[IMX662_DATARATE_1440]	= 1440000000LL / 2,
	[IMX662_DATARATE_1188]	= 1188000000LL / 2,
	[IMX662_DATARATE_891]	= 891000000LL / 2,
	[IMX662_DATARATE_720]	= 720000000LL / 2,
	[IMX662_DATARATE_594]	= 594000000LL / 2
};

static const struct imx662_format imx662_formats[] = {
	{
		.ad_md_bit = 0,
		.hmax_min_lane2 = {
			[IMX662_DATARATE_594] = 2376,
			[IMX662_DATARATE_720] = 1980,
			[IMX662_DATARATE_891] = 990,
			[IMX662_DATARATE_1188] = 990,	/* Undocumented */
			[IMX662_DATARATE_1440] = 660,
			[IMX662_DATARATE_1782] = 660,	/* Undocumented */
			[IMX662_DATARATE_2079] = 660,	/* Undocumented */
			[IMX662_DATARATE_2376] = 660,	/* Undocumented */
		},
		.hmax_min_lane4 = {
			[IMX662_DATARATE_594] = 990,
			[IMX662_DATARATE_720] = 660,
			[IMX662_DATARATE_891] = 660,	/* Undocumented */
			[IMX662_DATARATE_1188] = 660,	/* Undocumented */
			[IMX662_DATARATE_1440] = 660,	/* Undocumented */
			[IMX662_DATARATE_1782] = 660,	/* Undocumented */
			[IMX662_DATARATE_2079] = 660,	/* Undocumented */
			[IMX662_DATARATE_2376] = 660,	/* Undocumented */
		},
		.code = {
			[IMX662_VARIANT_COLOUR] = MEDIA_BUS_FMT_SRGGB10_1X10,
			[IMX662_VARIANT_MONO] = MEDIA_BUS_FMT_Y10_1X10,
		},
	}, {
		.ad_md_bit = BIT(0),
		.hmax_min_lane2 = {
			[IMX662_DATARATE_594] = 2376,	/* Undocumented */
			[IMX662_DATARATE_720] = 1980,
			[IMX662_DATARATE_891] = 1188,
			[IMX662_DATARATE_1188] = 990,
			[IMX662_DATARATE_1440] = 990,	/* Undocumented */
			[IMX662_DATARATE_1782] = 990,	/* Undocumented */
			[IMX662_DATARATE_2079] = 990,	/* Undocumented */
			[IMX662_DATARATE_2376] = 990,	/* Undocumented */
		},
		.hmax_min_lane4 = {
			[IMX662_DATARATE_594] = 990,
			[IMX662_DATARATE_720] = 990,	/* Undocumented */
			[IMX662_DATARATE_891] = 990,	/* Undocumented */
			[IMX662_DATARATE_1188] = 990,	/* Undocumented */
			[IMX662_DATARATE_1440] = 990,	/* Undocumented */
			[IMX662_DATARATE_1782] = 990,	/* Undocumented */
			[IMX662_DATARATE_2079] = 990,	/* Undocumented */
			[IMX662_DATARATE_2376] = 990,	/* Undocumented */
		},
		.code = {
			[IMX662_VARIANT_COLOUR] = MEDIA_BUS_FMT_SRGGB12_1X12,
			[IMX662_VARIANT_MONO] = MEDIA_BUS_FMT_Y12_1X12,
		},
	},
};

static void imx662_exposure_update(struct imx662 *imx662, u32 height)
{
	unsigned int exposure_max;

	if (!imx662->exposure)
		return;

	exposure_max = imx662->vblank->val + height - IMX662_EXPOSURE_OFFSET;

	__v4l2_ctrl_modify_range(imx662->exposure, 1, exposure_max, 1,
				 exposure_max);
}

static s64 imx662_get_hmax_min(struct imx662 *imx662)
{
	s64 hblank_min;

	if (imx662->num_data_lanes == 2)
		hblank_min =
			imx662->format->hmax_min_lane2[imx662->link_freq_index];
	else
		hblank_min =
			imx662->format->hmax_min_lane4[imx662->link_freq_index];

	return hblank_min * 3;
}

static void imx662_blank_update(struct imx662 *imx662, u32 width, u32 height)
{
	s64 hblank_min, hblank_max;

	hblank_min = imx662_get_hmax_min(imx662) - width;
	hblank_max = IMX662_HMAX_MAX - width;

	if (imx662->hblank)
		__v4l2_ctrl_modify_range(imx662->hblank, hblank_min, hblank_max,
					 1, hblank_min);
}

static int imx662_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx662 *imx662 = container_of(ctrl->handler,
					     struct imx662, ctrls);
	struct v4l2_subdev *sd = &imx662->sd;
	struct v4l2_subdev_state *state;
	struct v4l2_rect *crop;
	int ret = 0;

	state = v4l2_subdev_get_locked_active_state(sd);
	crop = v4l2_subdev_state_get_crop(state, 0);

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		imx662_exposure_update(imx662, crop->height);
		break;
	default:
		break;
	}

	if (!pm_runtime_get_if_in_use(imx662->sd.dev))
		goto ctrl_unlock;

	switch (ctrl->id) {
	case V4L2_CID_HBLANK:
		cci_write(imx662->regmap, IMX662_HMAX,
			  (ctrl->val + crop->width) / 3, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(imx662->regmap, IMX662_VMAX,
			  ctrl->val + crop->height, &ret);
		if (ret)
			break;
		ctrl = imx662->exposure;
		fallthrough;
	case V4L2_CID_EXPOSURE:
		cci_write(imx662->regmap, IMX662_SHR0,
			  imx662->vblank->val + crop->height - ctrl->val - 1,
			  &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(imx662->regmap, IMX662_GAIN, imx662->gain->val, &ret);
		break;
	case V4L2_CID_HFLIP:
		cci_write(imx662->regmap, IMX662_HREVERSE,
			  ctrl->val ? BIT(0) : 0x00, &ret);
		break;
	case V4L2_CID_VFLIP:
		cci_write(imx662->regmap, IMX662_VREVERSE,
			  ctrl->val ? BIT(0) : 0x00, &ret);
		break;
	default:
		dev_err(imx662->sd.dev, "Invalid control %d\n", ctrl->id);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(imx662->sd.dev);

ctrl_unlock:
	v4l2_subdev_unlock_state(state);

	return ret;
}

static const struct v4l2_ctrl_ops imx662_ctrl_ops = {
	.s_ctrl = imx662_set_ctrl,
};

static int imx662_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx662 *imx662 = to_imx662(sd);

	if (code->index >= ARRAY_SIZE(imx662_formats))
		return -EINVAL;

	code->code = imx662_formats[code->index].code[imx662->variant];

	return 0;
}

static int imx662_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index)
		return -EINVAL;

	fse->min_width = IMX662_MIN_CROP_WIDTH;
	fse->max_width = IMX662_PIXEL_ARRAY_WIDTH;
	fse->min_height = IMX662_MIN_CROP_HEIGHT;
	fse->max_height = IMX662_PIXEL_ARRAY_HEIGHT;

	return 0;
}

static int imx662_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx662 *imx662 = to_imx662(sd);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;
	const struct imx662_format *sensor_fmt = &imx662_formats[0];
	int i;

	for (i = 0; i < ARRAY_SIZE(imx662_formats); i++) {
		if (imx662_formats[i].code[imx662->variant] ==
							fmt->format.code) {
			sensor_fmt = &imx662_formats[i];
			break;
		}
	}

	crop = v4l2_subdev_state_get_crop(sd_state, fmt->pad);
	format = v4l2_subdev_state_get_format(sd_state, fmt->pad);

	format->width = crop->width;
	format->height = crop->height;
	format->code = sensor_fmt->code[imx662->variant];
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;
	format->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_NONE;

	fmt->format = *format;
	imx662->format = sensor_fmt;

	imx662_blank_update(imx662, crop->width, crop->height);

	imx662_exposure_update(imx662, crop->height);

	return 0;
}

static int imx662_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(sd_state, sel->pad);

		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.top = IMX662_PIXEL_ARRAY_TOP;
		sel->r.left = IMX662_PIXEL_ARRAY_LEFT;
		sel->r.width = IMX662_PIXEL_ARRAY_WIDTH;
		sel->r.height = IMX662_PIXEL_ARRAY_HEIGHT;

		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = IMX662_NATIVE_WIDTH;
		sel->r.height = IMX662_NATIVE_HEIGHT;

		return 0;
	}

	return -EINVAL;
}

static int imx662_set_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct imx662 *imx662 = to_imx662(sd);
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_rect rect = sel->r;
	struct v4l2_rect *crop;
	u32 max_left, max_top;

	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	max_left = IMX662_PIXEL_ARRAY_LEFT + IMX662_PIXEL_ARRAY_WIDTH -
		   IMX662_MIN_CROP_WIDTH;
	max_top = IMX662_PIXEL_ARRAY_TOP + IMX662_PIXEL_ARRAY_HEIGHT -
		  IMX662_MIN_CROP_HEIGHT;

	rect.left = clamp_t(u32, rect.left, IMX662_PIXEL_ARRAY_LEFT, max_left);
	rect.top = clamp_t(u32, rect.top, IMX662_PIXEL_ARRAY_TOP, max_top);

	rect.width =
		clamp_t(u32,
			round_down(rect.width, IMX662_CROP_WIDTH_STEP),
			IMX662_MIN_CROP_WIDTH, IMX662_PIXEL_ARRAY_WIDTH);
	rect.height =
		clamp_t(u32,
			round_down(rect.height, IMX662_CROP_HEIGHT_STEP),
			IMX662_MIN_CROP_HEIGHT, IMX662_PIXEL_ARRAY_HEIGHT);

	if (rect.left + rect.width - 1 >
	    IMX662_PIXEL_ARRAY_LEFT + IMX662_PIXEL_ARRAY_WIDTH - 1)
		rect.left =
			IMX662_PIXEL_ARRAY_LEFT + IMX662_PIXEL_ARRAY_WIDTH -
			rect.width;
	if (rect.top + rect.height - 1 >
	    IMX662_PIXEL_ARRAY_TOP + IMX662_PIXEL_ARRAY_HEIGHT - 1)
		rect.top =
			IMX662_PIXEL_ARRAY_TOP + IMX662_PIXEL_ARRAY_HEIGHT -
			rect.height;

	if (sel->flags & V4L2_SEL_FLAG_GE) {
		if (rect.width < sel->r.width) {
			u32 new_width = rect.width + IMX662_CROP_WIDTH_STEP;

			if (new_width <= IMX662_PIXEL_ARRAY_WIDTH)
				rect.width = new_width;
		}
		if (rect.height < sel->r.height) {
			u32 new_height = rect.height + IMX662_CROP_HEIGHT_STEP;

			if (new_height <= IMX662_PIXEL_ARRAY_HEIGHT)
				rect.height = new_height;
		}
	}

	if (sel->flags & V4L2_SEL_FLAG_LE) {
		if (rect.width > sel->r.width &&
		    rect.width >= IMX662_MIN_CROP_WIDTH +
				  IMX662_CROP_WIDTH_STEP)
			rect.width -= IMX662_CROP_WIDTH_STEP;
		if (rect.height > sel->r.height &&
		    rect.height >= IMX662_MIN_CROP_HEIGHT +
				   IMX662_CROP_HEIGHT_STEP)
			rect.height -= IMX662_CROP_HEIGHT_STEP;
	}

	if (rect.width < IMX662_MIN_CROP_WIDTH ||
	    rect.height < IMX662_MIN_CROP_HEIGHT)
		return -EINVAL;

	crop = v4l2_subdev_state_get_crop(sd_state, sel->pad);

	if (rect.width != crop->width || rect.height != crop->height) {
		/*
		 * Reset the output image size if the crop rectangle size has
		 * been modified.
		 */
		fmt = v4l2_subdev_state_get_format(sd_state, sel->pad);
		fmt->width = rect.width;
		fmt->height = rect.height;
	}

	*crop = rect;

	if (v4l2_subdev_is_streaming(sd))
		return -EBUSY;

	imx662_blank_update(imx662, crop->width, crop->height);
	imx662_exposure_update(imx662, crop->height);

	sel->r = rect;

	return 0;
}

static int imx662_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct imx662 *imx662 = to_imx662(sd);
	struct v4l2_subdev_format fmt = {
		.format = {
			.width = IMX662_PIXEL_ARRAY_WIDTH,
			.height = IMX662_PIXEL_ARRAY_HEIGHT,
			.code = imx662->format->code[imx662->variant],
		},
	};
	struct v4l2_subdev_selection sel = {
		.target = V4L2_SEL_TGT_CROP,
		.r.width = IMX662_PIXEL_ARRAY_WIDTH,
		.r.height = IMX662_PIXEL_ARRAY_HEIGHT,
		.r.top = IMX662_PIXEL_ARRAY_TOP,
		.r.left = IMX662_PIXEL_ARRAY_LEFT,
	};

	imx662_set_selection(sd, state, &sel);
	imx662_set_pad_format(sd, state, &fmt);

	return 0;
}

static int imx662_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct imx662 *imx662 = to_imx662(sd);
	struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, pad);
	int ret;

	if (streams_mask != 1)
		return -EINVAL;

	ret = pm_runtime_resume_and_get(imx662->sd.dev);
	if (ret)
		return ret;

	cci_multi_reg_write(imx662->regmap, imx662_regs_common,
			    ARRAY_SIZE(imx662_regs_common), &ret);

	cci_write(imx662->regmap, IMX662_INCK_SEL, imx662->inck, &ret);

	cci_write(imx662->regmap, IMX662_PIX_HST, crop->left, &ret);
	cci_write(imx662->regmap, IMX662_PIX_VST, crop->top, &ret);
	cci_write(imx662->regmap, IMX662_PIX_HWIDTH, crop->width, &ret);
	cci_write(imx662->regmap, IMX662_PIX_VWIDTH, crop->height, &ret);

	cci_write(imx662->regmap, IMX662_LANEMODE, imx662->num_data_lanes - 1,
		  &ret);

	cci_write(imx662->regmap, IMX662_DATARATE_SEL,
		  imx662->link_freq_index, &ret);

	cci_write(imx662->regmap, IMX662_ADBIT, imx662->format->ad_md_bit,
		  &ret);
	cci_write(imx662->regmap, IMX662_MDBIT, imx662->format->ad_md_bit,
		  &ret);

	if (ret)
		goto start_err;

	ret = __v4l2_ctrl_handler_setup(imx662->sd.ctrl_handler);
	if (ret) {
		dev_err(imx662->sd.dev, "Could not sync v4l2 controls\n");
		return ret;
	}

	cci_write(imx662->regmap, IMX662_STANDBY, 0x00, &ret);

	fsleep(24000);

	cci_write(imx662->regmap, IMX662_XMSTA, 0x00, &ret);
	if (!ret)
		return 0;

start_err:
	pm_runtime_put_autosuspend(imx662->sd.dev);

	dev_err(imx662->sd.dev, "Failed to setup sensor\n");

	return ret;
}

static int imx662_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct imx662 *imx662 = to_imx662(sd);
	int ret;

	if (streams_mask != 1)
		return -EINVAL;

	ret = cci_write(imx662->regmap, IMX662_STANDBY, 0x01, NULL);

	cci_write(imx662->regmap, IMX662_XMSTA, 0x01, &ret);

	pm_runtime_put_autosuspend(imx662->sd.dev);

	return ret;
}

static int imx662_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
				struct v4l2_mbus_config *config)
{
	struct imx662 *imx662 = to_imx662(sd);

	config->type = V4L2_MBUS_CSI2_DPHY;
	config->bus.mipi_csi2.flags = 0;
	config->bus.mipi_csi2.num_data_lanes = imx662->num_data_lanes;

	return 0;
}

static const struct v4l2_subdev_video_ops imx662_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx662_pad_ops = {
	.enum_mbus_code = imx662_enum_mbus_code,
	.enum_frame_size = imx662_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx662_set_pad_format,
	.get_selection = imx662_get_selection,
	.set_selection = imx662_set_selection,
	.enable_streams = imx662_enable_streams,
	.disable_streams = imx662_disable_streams,
	.get_mbus_config = imx662_g_mbus_config,
};

static const struct v4l2_subdev_ops imx662_subdev_ops = {
	.video = &imx662_video_ops,
	.pad = &imx662_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx662_internal_ops = {
	.init_state = imx662_init_state,
};

static const struct media_entity_operations imx662_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int imx662_ctrls_init(struct imx662 *imx662)
{
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	s64 pixel_rate;
	int ret;

	/* The exact value of pixel_rate is not described in the datasheet. */
	/* The multiplier 3 was chosen experimentally. */
	pixel_rate = IMX662_PIXEL_RATE * 3;
	v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  pixel_rate, pixel_rate, 1, pixel_rate);

	ctrl = v4l2_ctrl_new_int_menu(&imx662->ctrls, NULL,
				      V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(imx662_link_freqs) - 1,
				      imx662->link_freq_index,
				      imx662_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx662->hblank = v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops,
					   V4L2_CID_HBLANK, 0, 1, 1, 0);

	imx662->vblank = v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops,
					   V4L2_CID_VBLANK, IMX662_VMAX_MIN,
					   IMX662_VMAX_MAX - IMX662_PIXEL_ARRAY_HEIGHT,
					   1, IMX662_VMAX_MIN);

	imx662->exposure = v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops,
					     V4L2_CID_EXPOSURE, 1, 0xffff,
					     1, 0xffff);

	imx662->gain = v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops,
					 V4L2_CID_ANALOGUE_GAIN, 0, 1, 1, 0);

	v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops, V4L2_CID_HFLIP, 0,
			  1, 1, 0);

	v4l2_ctrl_new_std(&imx662->ctrls, &imx662_ctrl_ops, V4L2_CID_VFLIP, 0,
			  1, 1, 0);

	ret = v4l2_fwnode_device_parse(imx662->sd.dev, &props);
	if (ret)
		return ret;

	ret = v4l2_ctrl_new_fwnode_properties(&imx662->ctrls, &imx662_ctrl_ops,
					      &props);
	if (ret)
		return ret;

	imx662->sd.ctrl_handler = &imx662->ctrls;

	//imx662_blank_update(imx662, IMX662_PIXEL_ARRAY_WIDTH,
	//		    IMX662_PIXEL_ARRAY_HEIGHT);
	//imx662_exposure_update(imx662, IMX662_PIXEL_ARRAY_HEIGHT);

	return 0;
}

static int imx662_init_clk(struct imx662 *imx662)
{
	u32 xclk_freq;

	imx662->clk = devm_v4l2_sensor_clk_get(imx662->sd.dev, NULL);
	if (IS_ERR(imx662->clk))
		return dev_err_probe(imx662->sd.dev, PTR_ERR(imx662->clk),
				     "Failed to get clock\n");

	xclk_freq = clk_get_rate(imx662->clk);

	switch (xclk_freq) {
	case 24000000:
		imx662->inck = IMX662_INCK_SEL_24;
		break;
	case 27000000:
		imx662->inck = IMX662_INCK_SEL_27;
		break;
	case 37125000:
		imx662->inck = IMX662_INCK_SEL_37_125;
		break;
	case 72000000:
		imx662->inck = IMX662_INCK_SEL_72;
		break;
	case 74250000:
		imx662->inck = IMX662_INCK_SEL_74_25;
		break;
	default:
		return dev_err_probe(imx662->sd.dev, -EINVAL,
				     "EXT_CLK frequency %u is not supported\n",
				     xclk_freq);
		return -EINVAL;
	}

	return 0;
}

static int imx662_parse_hw_config(struct imx662 *imx662)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct fwnode_handle *ep;
	unsigned long link_freq;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(imx662_supply_names); i++)
		imx662->supplies[i].supply = imx662_supply_names[i];

	ret = devm_regulator_bulk_get(imx662->sd.dev,
				      ARRAY_SIZE(imx662_supply_names),
				      imx662->supplies);
	if (ret)
		return dev_err_probe(imx662->sd.dev, ret,
				     "Failed to get supplies\n");

	imx662->reset = devm_gpiod_get_optional(imx662->sd.dev, "reset",
						GPIOD_OUT_HIGH);
	if (IS_ERR(imx662->reset))
		return dev_err_probe(imx662->sd.dev, PTR_ERR(imx662->reset),
				     "Failed to get reset GPIO\n");

	ret = imx662_init_clk(imx662);
	if (ret)
		return ret;

	imx662->variant = (uintptr_t)of_device_get_match_data(imx662->sd.dev);

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(imx662->sd.dev), NULL);
	if (!ep)
		return -ENXIO;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	switch (bus_cfg.bus.mipi_csi2.num_data_lanes) {
	case 2:
	case 4:
		imx662->num_data_lanes = bus_cfg.bus.mipi_csi2.num_data_lanes;
		break;
	default:
		ret = dev_err_probe(imx662->sd.dev, -EINVAL,
				    "Invalid number of CSI2 data lanes %d\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes);
		goto done_endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(imx662->sd.dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       imx662_link_freqs,
				       ARRAY_SIZE(imx662_link_freqs),
				       &link_freq);
	if (ret)
		goto done_endpoint_free;

	imx662->link_freq_index = __fls(link_freq);

done_endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int imx662_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx662 *imx662 = to_imx662(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx662->supplies),
				    imx662->supplies);
	if (ret) {
		dev_err(dev, "Failed to enable regulators\n");
		return ret;
	}

	fsleep(1);

	gpiod_set_value_cansleep(imx662->reset, 0);

	fsleep(1);

	ret = clk_prepare_enable(imx662->clk);
	if (ret) {
		dev_err(dev, "Failed to enable clock\n");
		regulator_bulk_disable(ARRAY_SIZE(imx662->supplies),
				       imx662->supplies);
		return ret;
	}

	fsleep(20);

	return 0;
}

static int imx662_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx662 *imx662 = to_imx662(sd);

	clk_disable_unprepare(imx662->clk);
	gpiod_set_value_cansleep(imx662->reset, 1);
	regulator_bulk_disable(ARRAY_SIZE(imx662->supplies), imx662->supplies);

	return 0;
}

static void imx662_subdev_cleanup(struct imx662 *imx662)
{
	media_entity_cleanup(&imx662->sd.entity);
	v4l2_ctrl_handler_free(&imx662->ctrls);
}

static int imx662_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx662 *imx662;
	int ret;

	imx662 = devm_kzalloc(dev, sizeof(*imx662), GFP_KERNEL);
	if (!imx662)
		return -ENOMEM;

	imx662->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx662->regmap))
		return PTR_ERR(imx662->regmap);

	v4l2_i2c_subdev_init(&imx662->sd, client, &imx662_subdev_ops);

	ret = imx662_parse_hw_config(imx662);
	if (ret)
		return ret;

	ret = v4l2_ctrl_handler_init(&imx662->ctrls, 8 + 2);
	if (ret)
		return ret;

	ret = imx662_power_on(dev);
	if (ret)
		return ret;

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	imx662->format = &imx662_formats[0];

	cci_write(imx662->regmap, IMX662_STANDBY, 0x01, &ret);
	cci_write(imx662->regmap, IMX662_XMSTA, 0x01, &ret);
	if (ret)
		goto error_pm;

	imx662->sd.internal_ops = &imx662_internal_ops;
	imx662->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx662->sd.entity.ops = &imx662_subdev_entity_ops;
	imx662->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	imx662->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx662->sd.entity, 1, &imx662->pad);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to init entity pads\n");
		goto error_pm;
	}

	imx662->sd.state_lock = imx662->ctrls.lock;

	ret = v4l2_subdev_init_finalize(&imx662->sd);
	if (ret) {
		dev_err_probe(dev, ret, "Subdev init error\n");
		goto error_media;
	}

	ret = imx662_ctrls_init(imx662);
	if (ret)
		goto error_subdev;

	ret = v4l2_async_register_subdev_sensor(&imx662->sd);
	if (ret) {
		dev_err_probe(dev, ret,
			      "Failed to register sensor sub-device\n");
		goto error_subdev;
	}

	pm_runtime_put_autosuspend(dev);

	return 0;

error_subdev:
	imx662_subdev_cleanup(imx662);

error_media:
	media_entity_cleanup(&imx662->sd.entity);

error_pm:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	imx662_power_off(dev);

	return ret;
}

static void imx662_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx662 *imx662 = to_imx662(sd);

	v4l2_async_unregister_subdev(sd);
	imx662_subdev_cleanup(imx662);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx662_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct of_device_id imx662_of_match[] __maybe_unused = {
	{
		.compatible = "sony,imx662aaqr",
		.data = (void *)IMX662_VARIANT_COLOUR,
	},
	{
		.compatible = "sony,imx662aamr",
		.data = (void *)IMX662_VARIANT_MONO,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, imx662_of_match);

static DEFINE_RUNTIME_DEV_PM_OPS(imx662_pm_ops, imx662_power_off,
				 imx662_power_on, NULL);

static struct i2c_driver imx662_i2c_driver = {
	.probe = imx662_probe,
	.remove = imx662_remove,
	.driver = {
		.name = "imx662",
		.pm = pm_ptr(&imx662_pm_ops),
		.of_match_table = imx662_of_match,
	},
};
module_i2c_driver(imx662_i2c_driver);

MODULE_DESCRIPTION("Sony IMX662 CMOS Image Sensor Driver");
MODULE_AUTHOR("Dave Stevenson <dave.stevenson@raspberrypi.com>");
MODULE_LICENSE("GPL");
