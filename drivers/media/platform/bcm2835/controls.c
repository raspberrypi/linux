/*
 * Broadcom BM2835 V4L2 driver
 *
 * Copyright © 2013 Raspberry Pi (Trading) Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Authors: Vincent Sanders <vincent.sanders@collabora.co.uk>
 *          Dave Stevenson <dsteve@broadcom.com>
 *          Simon Mellor <simellor@broadcom.com>
 *          Luke Diamand <luked@broadcom.com>
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <media/videobuf2-vmalloc.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>
#include <media/v4l2-common.h>

#include "mmal-common.h"
#include "mmal-vchiq.h"
#include "mmal-parameters.h"
#include "bcm2835-camera.h"

/* The supported V4L2_CID_AUTO_EXPOSURE_BIAS values are from -24 to +24.
 * These are in 1/6th increments so the effective range is -4.0EV to +4.0EV.
 */
static const s64 ev_bias_qmenu[] = {
	-24, -21, -18, -15, -12, -9, -6, -3, 0, 3, 6, 9, 12, 15, 18, 21, 24
};

/* Supported ISO values
 * ISOO = auto ISO
 */
static const s64 iso_qmenu[] = {
	0, 100, 200, 400, 800,
};

/* Supported video encode modes */
static const s64 bitrate_mode_qmenu[] = {
	(s64)V4L2_MPEG_VIDEO_BITRATE_MODE_VBR,
	(s64)V4L2_MPEG_VIDEO_BITRATE_MODE_CBR,
};


enum bm2835_mmal_ctrl_type {
	MMAL_CONTROL_TYPE_STD,
	MMAL_CONTROL_TYPE_STD_MENU,
	MMAL_CONTROL_TYPE_INT_MENU,
	MMAL_CONTROL_TYPE_CLUSTER, /* special cluster entry */
};

struct bm2835_mmal_v4l2_ctrl;

typedef	int(bm2835_mmal_v4l2_ctrl_cb)(
				struct bm2835_mmal_dev *dev,
				struct v4l2_ctrl *ctrl,
				const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl);

struct bm2835_mmal_v4l2_ctrl {
	u32 id; /* v4l2 control identifier */
	enum bm2835_mmal_ctrl_type type;
	/* control minimum value or
	 * mask for MMAL_CONTROL_TYPE_STD_MENU */
	s32 min;
	s32 max; /* maximum value of control */
	s32 def;  /* default value of control */
	s32 step; /* step size of the control */
	const s64 *imenu; /* integer menu array */
	u32 mmal_id; /* mmal parameter id */
	bm2835_mmal_v4l2_ctrl_cb *setter;
};

struct v4l2_to_mmal_effects_setting {
	u32 v4l2_effect;
	u32 mmal_effect;
	s32 col_fx_enable;
	s32 col_fx_fixed_cbcr;
	u32 u;
	u32 v;
	u32 num_effect_params;
	u32 effect_params[MMAL_MAX_IMAGEFX_PARAMETERS];
};

static const struct v4l2_to_mmal_effects_setting
	v4l2_to_mmal_effects_values[] = {
	{  V4L2_COLORFX_NONE,         MMAL_PARAM_IMAGEFX_NONE,
		0,   0,    0,    0,   0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_BW,           MMAL_PARAM_IMAGEFX_NONE,
		1,   0,    128,  128, 0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_SEPIA,        MMAL_PARAM_IMAGEFX_NONE,
		1,   0,    87,   151, 0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_NEGATIVE,     MMAL_PARAM_IMAGEFX_NEGATIVE,
		0,   0,    0,    0,   0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_EMBOSS,       MMAL_PARAM_IMAGEFX_EMBOSS,
		0,   0,    0,    0,   0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_SKETCH,       MMAL_PARAM_IMAGEFX_SKETCH,
		0,   0,    0,    0,   0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_SKY_BLUE,     MMAL_PARAM_IMAGEFX_PASTEL,
		0,   0,    0,    0,   0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_GRASS_GREEN,  MMAL_PARAM_IMAGEFX_WATERCOLOUR,
		0,   0,    0,    0,   0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_SKIN_WHITEN,  MMAL_PARAM_IMAGEFX_WASHEDOUT,
		0,   0,    0,    0,   0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_VIVID,        MMAL_PARAM_IMAGEFX_SATURATION,
		0,   0,    0,    0,   0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_AQUA,         MMAL_PARAM_IMAGEFX_NONE,
		1,   0,    171,  121, 0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_ART_FREEZE,   MMAL_PARAM_IMAGEFX_HATCH,
		0,   0,    0,    0,   0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_SILHOUETTE,   MMAL_PARAM_IMAGEFX_FILM,
		0,   0,    0,    0,   0, {0, 0, 0, 0, 0} },
	{  V4L2_COLORFX_SOLARIZATION, MMAL_PARAM_IMAGEFX_SOLARIZE,
		0,   0,    0,    0,   5, {1, 128, 160, 160, 48} },
	{  V4L2_COLORFX_ANTIQUE,      MMAL_PARAM_IMAGEFX_COLOURBALANCE,
		0,   0,    0,    0,   3, {108, 274, 238, 0, 0} },
	{  V4L2_COLORFX_SET_CBCR,     MMAL_PARAM_IMAGEFX_NONE,
		1,   1,    0,    0,   0, {0, 0, 0, 0, 0} }
};


/* control handlers*/

static int ctrl_set_rational(struct bm2835_mmal_dev *dev,
		      struct v4l2_ctrl *ctrl,
		      const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	struct  {
		s32 num;    /**< Numerator */
		s32 den;    /**< Denominator */
	} rational_value;
	struct vchiq_mmal_port *control;

	control = &dev->component[MMAL_COMPONENT_CAMERA]->control;

	rational_value.num = ctrl->val;
	rational_value.den = 100;

	return vchiq_mmal_port_parameter_set(dev->instance, control,
					     mmal_ctrl->mmal_id,
					     &rational_value,
					     sizeof(rational_value));
}

static int ctrl_set_value(struct bm2835_mmal_dev *dev,
		      struct v4l2_ctrl *ctrl,
		      const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	u32 u32_value;
	struct vchiq_mmal_port *control;

	control = &dev->component[MMAL_COMPONENT_CAMERA]->control;

	u32_value = ctrl->val;

	return vchiq_mmal_port_parameter_set(dev->instance, control,
					     mmal_ctrl->mmal_id,
					     &u32_value, sizeof(u32_value));
}

static int ctrl_set_rotate(struct bm2835_mmal_dev *dev,
		      struct v4l2_ctrl *ctrl,
		      const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	int ret;
	u32 u32_value;
	struct vchiq_mmal_component *camera;

	camera = dev->component[MMAL_COMPONENT_CAMERA];

	u32_value = ((ctrl->val % 360) / 90) * 90;

	ret = vchiq_mmal_port_parameter_set(dev->instance, &camera->output[0],
					    mmal_ctrl->mmal_id,
					    &u32_value, sizeof(u32_value));
	if (ret < 0)
		return ret;

	ret = vchiq_mmal_port_parameter_set(dev->instance, &camera->output[1],
					    mmal_ctrl->mmal_id,
					    &u32_value, sizeof(u32_value));
	if (ret < 0)
		return ret;

	ret = vchiq_mmal_port_parameter_set(dev->instance, &camera->output[2],
					    mmal_ctrl->mmal_id,
					    &u32_value, sizeof(u32_value));

	return ret;
}

static int ctrl_set_flip(struct bm2835_mmal_dev *dev,
		      struct v4l2_ctrl *ctrl,
		      const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	int ret;
	u32 u32_value;
	struct vchiq_mmal_component *camera;

	if (ctrl->id == V4L2_CID_HFLIP)
		dev->hflip = ctrl->val;
	else
		dev->vflip = ctrl->val;

	camera = dev->component[MMAL_COMPONENT_CAMERA];

	if (dev->hflip && dev->vflip)
		u32_value = MMAL_PARAM_MIRROR_BOTH;
	else if (dev->hflip)
		u32_value = MMAL_PARAM_MIRROR_HORIZONTAL;
	else if (dev->vflip)
		u32_value = MMAL_PARAM_MIRROR_VERTICAL;
	else
		u32_value = MMAL_PARAM_MIRROR_NONE;

	ret = vchiq_mmal_port_parameter_set(dev->instance, &camera->output[0],
					    mmal_ctrl->mmal_id,
					    &u32_value, sizeof(u32_value));
	if (ret < 0)
		return ret;

	ret = vchiq_mmal_port_parameter_set(dev->instance, &camera->output[1],
					    mmal_ctrl->mmal_id,
					    &u32_value, sizeof(u32_value));
	if (ret < 0)
		return ret;

	ret = vchiq_mmal_port_parameter_set(dev->instance, &camera->output[2],
					    mmal_ctrl->mmal_id,
					    &u32_value, sizeof(u32_value));

	return ret;

}

static int ctrl_set_exposure(struct bm2835_mmal_dev *dev,
		      struct v4l2_ctrl *ctrl,
		      const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	u32 u32_value;
	struct vchiq_mmal_port *control;

	control = &dev->component[MMAL_COMPONENT_CAMERA]->control;

	switch (ctrl->val) {
	case V4L2_EXPOSURE_AUTO:
		u32_value = MMAL_PARAM_EXPOSUREMODE_AUTO;
		break;

	case V4L2_EXPOSURE_MANUAL:
		u32_value = MMAL_PARAM_EXPOSUREMODE_OFF;
		break;

	case V4L2_EXPOSURE_SHUTTER_PRIORITY:
		u32_value = MMAL_PARAM_EXPOSUREMODE_SPORTS;
		break;

	case V4L2_EXPOSURE_APERTURE_PRIORITY:
		u32_value = MMAL_PARAM_EXPOSUREMODE_NIGHT;
		break;

	}

	/* todo: what about the other ten modes there are MMAL parameters for */
	return vchiq_mmal_port_parameter_set(dev->instance, control,
					     mmal_ctrl->mmal_id,
					     &u32_value, sizeof(u32_value));
}

static int ctrl_set_metering_mode(struct bm2835_mmal_dev *dev,
			   struct v4l2_ctrl *ctrl,
			   const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	u32 u32_value;
	struct vchiq_mmal_port *control;

	control = &dev->component[MMAL_COMPONENT_CAMERA]->control;

	switch (ctrl->val) {
	case V4L2_EXPOSURE_METERING_AVERAGE:
		u32_value = MMAL_PARAM_EXPOSUREMETERINGMODE_AVERAGE;
		break;

	case V4L2_EXPOSURE_METERING_CENTER_WEIGHTED:
		u32_value = MMAL_PARAM_EXPOSUREMETERINGMODE_BACKLIT;
		break;

	case V4L2_EXPOSURE_METERING_SPOT:
		u32_value = MMAL_PARAM_EXPOSUREMETERINGMODE_SPOT;
		break;

	/* todo matrix weighting not added to Linux API till 3.9
	case V4L2_EXPOSURE_METERING_MATRIX:
		u32_value = MMAL_PARAM_EXPOSUREMETERINGMODE_MATRIX;
		break;
	*/

	}

	return vchiq_mmal_port_parameter_set(dev->instance, control,
					     mmal_ctrl->mmal_id,
					     &u32_value, sizeof(u32_value));
}

static int ctrl_set_awb_mode(struct bm2835_mmal_dev *dev,
		      struct v4l2_ctrl *ctrl,
		      const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	u32 u32_value;
	struct vchiq_mmal_port *control;

	control = &dev->component[MMAL_COMPONENT_CAMERA]->control;

	switch (ctrl->val) {
	case V4L2_WHITE_BALANCE_MANUAL:
		u32_value = MMAL_PARAM_AWBMODE_OFF;
		break;

	case V4L2_WHITE_BALANCE_AUTO:
		u32_value = MMAL_PARAM_AWBMODE_AUTO;
		break;

	case V4L2_WHITE_BALANCE_INCANDESCENT:
		u32_value = MMAL_PARAM_AWBMODE_INCANDESCENT;
		break;

	case V4L2_WHITE_BALANCE_FLUORESCENT:
		u32_value = MMAL_PARAM_AWBMODE_FLUORESCENT;
		break;

	case V4L2_WHITE_BALANCE_FLUORESCENT_H:
		u32_value = MMAL_PARAM_AWBMODE_TUNGSTEN;
		break;

	case V4L2_WHITE_BALANCE_HORIZON:
		u32_value = MMAL_PARAM_AWBMODE_HORIZON;
		break;

	case V4L2_WHITE_BALANCE_DAYLIGHT:
		u32_value = MMAL_PARAM_AWBMODE_SUNLIGHT;
		break;

	case V4L2_WHITE_BALANCE_FLASH:
		u32_value = MMAL_PARAM_AWBMODE_FLASH;
		break;

	case V4L2_WHITE_BALANCE_CLOUDY:
		u32_value = MMAL_PARAM_AWBMODE_CLOUDY;
		break;

	case V4L2_WHITE_BALANCE_SHADE:
		u32_value = MMAL_PARAM_AWBMODE_SHADE;
		break;

	}

	return vchiq_mmal_port_parameter_set(dev->instance, control,
					     mmal_ctrl->mmal_id,
					     &u32_value, sizeof(u32_value));
}

static int ctrl_set_image_effect(struct bm2835_mmal_dev *dev,
		   struct v4l2_ctrl *ctrl,
		   const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	int ret = -EINVAL;
	int i, j;
	struct vchiq_mmal_port *control;
	struct mmal_parameter_imagefx_parameters imagefx;

	for (i = 0; i < ARRAY_SIZE(v4l2_to_mmal_effects_values); i++) {
		if (ctrl->val == v4l2_to_mmal_effects_values[i].v4l2_effect) {

			imagefx.effect =
				v4l2_to_mmal_effects_values[i].mmal_effect;
			imagefx.num_effect_params =
				v4l2_to_mmal_effects_values[i].num_effect_params;

			if (imagefx.num_effect_params > MMAL_MAX_IMAGEFX_PARAMETERS)
				imagefx.num_effect_params = MMAL_MAX_IMAGEFX_PARAMETERS;

			for (j = 0; j < imagefx.num_effect_params; j++)
				imagefx.effect_parameter[j] =
					v4l2_to_mmal_effects_values[i].effect_params[j];

			dev->colourfx.enable =
				v4l2_to_mmal_effects_values[i].col_fx_enable;
			if (!v4l2_to_mmal_effects_values[i].col_fx_fixed_cbcr) {
				dev->colourfx.u =
					v4l2_to_mmal_effects_values[i].u;
				dev->colourfx.v =
					v4l2_to_mmal_effects_values[i].v;
			}

			control = &dev->component[MMAL_COMPONENT_CAMERA]->control;

			ret = vchiq_mmal_port_parameter_set(
					dev->instance, control,
					MMAL_PARAMETER_IMAGE_EFFECT_PARAMETERS,
					&imagefx, sizeof(imagefx));
			if (ret)
				goto exit;

			ret = vchiq_mmal_port_parameter_set(
					dev->instance, control,
					MMAL_PARAMETER_COLOUR_EFFECT,
					&dev->colourfx, sizeof(dev->colourfx));
		}
	}

exit:
	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
		 "mmal_ctrl:%p ctrl id:0x%x ctrl val:%d imagefx:0x%x color_effect:%s u:%d v:%d ret %d(%d)\n",
				mmal_ctrl, ctrl->id, ctrl->val, imagefx.effect,
				dev->colourfx.enable ? "true" : "false",
				dev->colourfx.u, dev->colourfx.v,
				ret, (ret == 0 ? 0 : -EINVAL));
	return (ret == 0 ? 0 : EINVAL);
}

static int ctrl_set_colfx(struct bm2835_mmal_dev *dev,
		   struct v4l2_ctrl *ctrl,
		   const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	int ret = -EINVAL;
	struct vchiq_mmal_port *control;

	control = &dev->component[MMAL_COMPONENT_CAMERA]->control;

	dev->colourfx.enable = (ctrl->val & 0xff00) >> 8;
	dev->colourfx.enable = ctrl->val & 0xff;

	ret = vchiq_mmal_port_parameter_set(dev->instance, control,
					MMAL_PARAMETER_COLOUR_EFFECT,
					&dev->colourfx, sizeof(dev->colourfx));

	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
		 "After: mmal_ctrl:%p ctrl id:0x%x ctrl val:%d ret %d(%d)\n",
			mmal_ctrl, ctrl->id, ctrl->val, ret,
			(ret == 0 ? 0 : -EINVAL));
	return (ret == 0 ? 0 : EINVAL);
}

static int ctrl_set_bitrate(struct bm2835_mmal_dev *dev,
		   struct v4l2_ctrl *ctrl,
		   const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	int ret;
	struct vchiq_mmal_port *encoder_out;

	dev->capture.encode_bitrate = ctrl->val;

	encoder_out = &dev->component[MMAL_COMPONENT_VIDEO_ENCODE]->output[0];

	ret = vchiq_mmal_port_parameter_set(dev->instance, encoder_out,
					    mmal_ctrl->mmal_id,
					    &ctrl->val, sizeof(ctrl->val));
	ret = 0;
	return ret;
}

static int ctrl_set_bitrate_mode(struct bm2835_mmal_dev *dev,
		   struct v4l2_ctrl *ctrl,
		   const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	u32 bitrate_mode;
	struct vchiq_mmal_port *encoder_out;

	encoder_out = &dev->component[MMAL_COMPONENT_VIDEO_ENCODE]->output[0];

	dev->capture.encode_bitrate_mode = ctrl->val;
	switch (ctrl->val) {
	default:
	case V4L2_MPEG_VIDEO_BITRATE_MODE_VBR:
		bitrate_mode = MMAL_VIDEO_RATECONTROL_VARIABLE;
		break;
	case V4L2_MPEG_VIDEO_BITRATE_MODE_CBR:
		bitrate_mode = MMAL_VIDEO_RATECONTROL_CONSTANT;
		break;
	}

	vchiq_mmal_port_parameter_set(dev->instance, encoder_out,
					     mmal_ctrl->mmal_id,
					     &bitrate_mode,
					     sizeof(bitrate_mode));
	return 0;
}

static int ctrl_set_q_factor(struct bm2835_mmal_dev *dev,
		      struct v4l2_ctrl *ctrl,
		      const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl)
{
	u32 u32_value;
	struct vchiq_mmal_port *jpeg_out;

	jpeg_out = &dev->component[MMAL_COMPONENT_IMAGE_ENCODE]->output[0];

	u32_value = ctrl->val;

	return vchiq_mmal_port_parameter_set(dev->instance, jpeg_out,
					     mmal_ctrl->mmal_id,
					     &u32_value, sizeof(u32_value));
}

static int bm2835_mmal_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct bm2835_mmal_dev *dev =
		container_of(ctrl->handler, struct bm2835_mmal_dev,
			     ctrl_handler);
	const struct bm2835_mmal_v4l2_ctrl *mmal_ctrl = ctrl->priv;

	if ((mmal_ctrl == NULL) ||
	    (mmal_ctrl->id != ctrl->id) ||
	    (mmal_ctrl->setter == NULL)) {
		pr_warn("mmal_ctrl:%p ctrl id:%d\n", mmal_ctrl, ctrl->id);
		return -EINVAL;
	}

	return mmal_ctrl->setter(dev, ctrl, mmal_ctrl);
}

static const struct v4l2_ctrl_ops bm2835_mmal_ctrl_ops = {
	.s_ctrl = bm2835_mmal_s_ctrl,
};



static const struct bm2835_mmal_v4l2_ctrl v4l2_ctrls[V4L2_CTRL_COUNT] = {
	{
		V4L2_CID_SATURATION, MMAL_CONTROL_TYPE_STD,
		-100, 100, 0, 1, NULL,
		MMAL_PARAMETER_SATURATION, &ctrl_set_rational
	},
	{
		V4L2_CID_SHARPNESS, MMAL_CONTROL_TYPE_STD,
		-100, 100, 0, 1, NULL,
		MMAL_PARAMETER_SHARPNESS, &ctrl_set_rational
	},
	{
		V4L2_CID_CONTRAST, MMAL_CONTROL_TYPE_STD,
		-100, 100, 0, 1, NULL,
		MMAL_PARAMETER_CONTRAST, &ctrl_set_rational
	},
	{
		V4L2_CID_BRIGHTNESS, MMAL_CONTROL_TYPE_STD,
		0, 100, 50, 1, NULL,
		MMAL_PARAMETER_BRIGHTNESS, &ctrl_set_rational
	},
	{
		V4L2_CID_ISO_SENSITIVITY, MMAL_CONTROL_TYPE_INT_MENU,
		0, ARRAY_SIZE(iso_qmenu) - 1, 0, 1, iso_qmenu,
		MMAL_PARAMETER_ISO, &ctrl_set_value
	},
	{
		V4L2_CID_IMAGE_STABILIZATION, MMAL_CONTROL_TYPE_STD,
		0, 1, 0, 1, NULL,
		MMAL_PARAMETER_VIDEO_STABILISATION, &ctrl_set_value
	},
/*	{
		0, MMAL_CONTROL_TYPE_CLUSTER, 3, 1, 0, NULL, 0, NULL
	},
*/	{
		V4L2_CID_EXPOSURE_AUTO, MMAL_CONTROL_TYPE_STD_MENU,
		~0x03, 3, V4L2_EXPOSURE_AUTO, 0, NULL,
		MMAL_PARAMETER_EXPOSURE_MODE, &ctrl_set_exposure
	},
/* todo this needs mixing in with set exposure
	{
	       V4L2_CID_SCENE_MODE, MMAL_CONTROL_TYPE_STD_MENU,
	},
 */
	{
		V4L2_CID_AUTO_EXPOSURE_BIAS, MMAL_CONTROL_TYPE_INT_MENU,
		0, ARRAY_SIZE(ev_bias_qmenu) - 1,
		(ARRAY_SIZE(ev_bias_qmenu)+1)/2 - 1, 0, ev_bias_qmenu,
		MMAL_PARAMETER_EXPOSURE_COMP, &ctrl_set_value
	},
	{
		V4L2_CID_EXPOSURE_METERING,
		MMAL_CONTROL_TYPE_STD_MENU,
		~0x7, 2, V4L2_EXPOSURE_METERING_AVERAGE, 0, NULL,
		MMAL_PARAMETER_EXP_METERING_MODE, &ctrl_set_metering_mode
	},
	{
		V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE,
		MMAL_CONTROL_TYPE_STD_MENU,
		~0x3fe, 9, V4L2_WHITE_BALANCE_AUTO, 0, NULL,
		MMAL_PARAMETER_AWB_MODE, &ctrl_set_awb_mode
	},
	{
		V4L2_CID_COLORFX, MMAL_CONTROL_TYPE_STD_MENU,
		0, 15, V4L2_COLORFX_NONE, 0, NULL,
		MMAL_PARAMETER_IMAGE_EFFECT, &ctrl_set_image_effect
	},
	{
		V4L2_CID_COLORFX_CBCR, MMAL_CONTROL_TYPE_STD,
		0, 0xffff, 0x8080, 1, NULL,
		MMAL_PARAMETER_COLOUR_EFFECT, &ctrl_set_colfx
	},
	{
		V4L2_CID_ROTATE, MMAL_CONTROL_TYPE_STD,
		0, 360, 0, 90, NULL,
		MMAL_PARAMETER_ROTATION, &ctrl_set_rotate
	},
	{
		V4L2_CID_HFLIP, MMAL_CONTROL_TYPE_STD,
		0, 1, 0, 1, NULL,
		MMAL_PARAMETER_MIRROR, &ctrl_set_flip
	},
	{
		V4L2_CID_VFLIP, MMAL_CONTROL_TYPE_STD,
		0, 1, 0, 1, NULL,
		MMAL_PARAMETER_MIRROR, &ctrl_set_flip
	},
	{
		V4L2_CID_MPEG_VIDEO_BITRATE_MODE, MMAL_CONTROL_TYPE_STD_MENU,
		0, ARRAY_SIZE(bitrate_mode_qmenu) - 1,
		0, 0, bitrate_mode_qmenu,
		MMAL_PARAMETER_RATECONTROL, &ctrl_set_bitrate_mode
	},
	{
		V4L2_CID_MPEG_VIDEO_BITRATE, MMAL_CONTROL_TYPE_STD,
		25*1000, 25*1000*1000, 10*1000*1000, 25*1000, NULL,
		MMAL_PARAMETER_VIDEO_BIT_RATE, &ctrl_set_bitrate
	},
	{
		V4L2_CID_JPEG_COMPRESSION_QUALITY, MMAL_CONTROL_TYPE_STD,
		0, 100,
		30, 1, NULL,
		MMAL_PARAMETER_JPEG_Q_FACTOR, &ctrl_set_q_factor
	},
};

int bm2835_mmal_set_all_camera_controls(struct bm2835_mmal_dev *dev)
{
	int c;
	int ret;

	for (c = 0; c < V4L2_CTRL_COUNT; c++) {
		if ((dev->ctrls[c]) && (v4l2_ctrls[c].setter)) {
			ret = v4l2_ctrls[c].setter(dev, dev->ctrls[c],
						   &v4l2_ctrls[c]);
			if (ret)
				break;
		}
	}
	return ret;
}

int bm2835_mmal_init_controls(struct bm2835_mmal_dev *dev,
			      struct v4l2_ctrl_handler *hdl)
{
	int c;
	const struct bm2835_mmal_v4l2_ctrl *ctrl;

	v4l2_ctrl_handler_init(hdl, V4L2_CTRL_COUNT);

	for (c = 0; c < V4L2_CTRL_COUNT; c++) {
		ctrl = &v4l2_ctrls[c];

		switch (ctrl->type) {
		case MMAL_CONTROL_TYPE_STD:
			dev->ctrls[c] = v4l2_ctrl_new_std(hdl,
				&bm2835_mmal_ctrl_ops, ctrl->id,
				ctrl->min, ctrl->max, ctrl->step, ctrl->def);
			break;

		case MMAL_CONTROL_TYPE_STD_MENU:
			dev->ctrls[c] = v4l2_ctrl_new_std_menu(hdl,
			&bm2835_mmal_ctrl_ops, ctrl->id,
			ctrl->max, ctrl->min, ctrl->def);
			break;

		case MMAL_CONTROL_TYPE_INT_MENU:
			dev->ctrls[c] = v4l2_ctrl_new_int_menu(hdl,
				&bm2835_mmal_ctrl_ops, ctrl->id,
				ctrl->max, ctrl->def, ctrl->imenu);
			break;

		case MMAL_CONTROL_TYPE_CLUSTER:
			/* skip this entry when constructing controls */
			continue;
		}

		if (hdl->error)
			break;

		dev->ctrls[c]->priv = (void *)ctrl;
	}

	if (hdl->error) {
		pr_err("error adding control %d/%d id 0x%x\n", c,
			 V4L2_CTRL_COUNT, ctrl->id);
		return hdl->error;
	}

	for (c = 0; c < V4L2_CTRL_COUNT; c++) {
		ctrl = &v4l2_ctrls[c];

		switch (ctrl->type) {
		case MMAL_CONTROL_TYPE_CLUSTER:
			v4l2_ctrl_auto_cluster(ctrl->min,
					       &dev->ctrls[c+1],
					       ctrl->max,
					       ctrl->def);
			break;

		case MMAL_CONTROL_TYPE_STD:
		case MMAL_CONTROL_TYPE_STD_MENU:
		case MMAL_CONTROL_TYPE_INT_MENU:
			break;
		}

	}

	return 0;
}
