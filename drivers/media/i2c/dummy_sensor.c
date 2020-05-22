// SPDX-License-Identifier: GPL-2.0
/*
 * A V4L2 driver for CSI2 sources with no control.
 * This can be of use when interfacing to an FPGA or other source that is
 * constantly streaming data, and the V4L2 receiver device just needs to be
 * configured with the image parameters for the incoming stream, or where
 * userspace has to send the relevant configuration
 *.
 * V4L2 controls are created for the base parameters that libcamera insists
 * exist, but they are all read-only with dummy values.
 *
 * Copyright (C) 2021, Raspberry Pi (Trading) Ltd.
 *
 * Based on Sony imx219 camera driver
 * Copyright (C) 2020 Raspberry Pi (Trading) Ltd.
 *
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <asm/unaligned.h>

/* Array of all the mbus formats that we'll accept */
u32 mbus_codes[] = {
	MEDIA_BUS_FMT_SBGGR8_1X8,
	MEDIA_BUS_FMT_SGBRG8_1X8,
	MEDIA_BUS_FMT_SGRBG8_1X8,
	MEDIA_BUS_FMT_SRGGB8_1X8,
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SBGGR12_1X12,
	MEDIA_BUS_FMT_SGBRG12_1X12,
	MEDIA_BUS_FMT_SGRBG12_1X12,
	MEDIA_BUS_FMT_SRGGB12_1X12,
	MEDIA_BUS_FMT_SBGGR14_1X14,
	MEDIA_BUS_FMT_SGBRG14_1X14,
	MEDIA_BUS_FMT_SGRBG14_1X14,
	MEDIA_BUS_FMT_SRGGB14_1X14,
	MEDIA_BUS_FMT_SBGGR16_1X16,
	MEDIA_BUS_FMT_SGBRG16_1X16,
	MEDIA_BUS_FMT_SGRBG16_1X16,
	MEDIA_BUS_FMT_SRGGB16_1X16,
	MEDIA_BUS_FMT_Y8_1X8,
	MEDIA_BUS_FMT_Y10_1X10,
	MEDIA_BUS_FMT_Y12_1X12,
	MEDIA_BUS_FMT_Y14_1X14,
	MEDIA_BUS_FMT_YUYV8_1X16,
	MEDIA_BUS_FMT_UYVY8_1X16,
	MEDIA_BUS_FMT_YVYU8_1X16,
	MEDIA_BUS_FMT_VYUY8_1X16
};

#define NUM_MBUS_CODES ARRAY_SIZE(mbus_codes)

#define MIN_WIDTH	16
#define MAX_WIDTH	16383
#define MIN_HEIGHT	16
#define MAX_HEIGHT	16383

#define DEFAULT_WIDTH	640
#define DEFAULT_HEIGHT	480
/* Default format will be the first entry in mbus_codes */

struct sensor {
	struct platform_device *pdev;
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_mbus_framefmt fmt;

	struct v4l2_ctrl_handler ctrl_handler;
	/*
	 * Mutex for serialized access:
	 * Protect sensor module set pad format and start/stop streaming safely.
	 */
	struct mutex mutex;
};

static inline struct sensor *to_sensor(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct sensor, sd);
}

static int sensor_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct sensor *sensor = to_sensor(sd);
	struct v4l2_mbus_framefmt *try_img_fmt =
		v4l2_subdev_get_try_format(sd, fh->pad, 0);
	struct v4l2_rect *try_crop;

	*try_img_fmt = sensor->fmt;

	/* Initialize try_crop rectangle. */
	try_crop = v4l2_subdev_get_try_crop(sd, fh->pad, 0);
	try_crop->top = 0;
	try_crop->left = 0;
	try_crop->width = try_img_fmt->width;
	try_crop->height = try_img_fmt->height;

	return 0;
}

static int sensor_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sensor *sensor =
		container_of(ctrl->handler, struct sensor, ctrl_handler);
	int ret;

	switch (ctrl->id) {
	default:
		dev_info(&sensor->pdev->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops sensor_ctrl_ops = {
	.s_ctrl = sensor_set_ctrl,
};

static int sensor_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= NUM_MBUS_CODES)
		return -EINVAL;
	if (code->pad)
		return -EINVAL;

	code->code = mbus_codes[code->index];

	return 0;
}

static int sensor_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index)
		return -EINVAL;
	if (fse->pad)
		return -EINVAL;

	fse->min_width = MIN_WIDTH;
	fse->max_width = MAX_WIDTH;
	fse->min_height = MIN_HEIGHT;
	fse->max_height = MAX_HEIGHT;

	return 0;
}

static int sensor_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_format *fmt)
{
	struct sensor *sensor = to_sensor(sd);

	if (fmt->pad)
		return -EINVAL;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt =
			v4l2_subdev_get_try_format(&sensor->sd, cfg,
						   fmt->pad);
		fmt->format = *try_fmt;
	} else {
		fmt->format = sensor->fmt;
	}

	return 0;
}

static int sensor_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_format *fmt)
{
	struct sensor *sensor = to_sensor(sd);
	struct v4l2_mbus_framefmt *format;
	int i;

	if (fmt->pad)
		return -EINVAL;

	for (i = 0; i < NUM_MBUS_CODES; i++)
		if (mbus_codes[i] == fmt->format.code)
			break;

	if (i >= NUM_MBUS_CODES)
		i = 0;

	fmt->format.code = mbus_codes[i];
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_SRGB;
	fmt->format.ycbcr_enc =
		V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->format.colorspace);
	fmt->format.quantization =
		V4L2_MAP_QUANTIZATION_DEFAULT(true, fmt->format.colorspace,
					      fmt->format.ycbcr_enc);
	fmt->format.xfer_func =
		V4L2_MAP_XFER_FUNC_DEFAULT(fmt->format.colorspace);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		format = v4l2_subdev_get_try_format(&sensor->sd, cfg, fmt->pad);
	else
		format = &sensor->fmt;

	*format = fmt->format;

	return 0;
}

static int sensor_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_selection *sel)
{
	struct sensor *sensor = to_sensor(sd);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = sensor->fmt.width;
		sel->r.height = sensor->fmt.height;

		return 0;
	}

	return -EINVAL;
}

static int sensor_set_stream(struct v4l2_subdev *sd, int enable)
{
	/*
	 * Don't need to do anything here, just assume the source is streaming
	 * already.
	 */
	return 0;
}

static const struct v4l2_subdev_core_ops sensor_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops sensor_video_ops = {
	.s_stream = sensor_set_stream,
};

static const struct v4l2_subdev_pad_ops sensor_pad_ops = {
	.enum_mbus_code = sensor_enum_mbus_code,
	.get_fmt = sensor_get_pad_format,
	.set_fmt = sensor_set_pad_format,
	.get_selection = sensor_get_selection,
	.enum_frame_size = sensor_enum_frame_size,
};

static const struct v4l2_subdev_ops sensor_subdev_ops = {
	.core = &sensor_core_ops,
	.video = &sensor_video_ops,
	.pad = &sensor_pad_ops,
};

static const struct v4l2_subdev_internal_ops sensor_internal_ops = {
	.open = sensor_open,
};

/* Initialize control handlers */
static int sensor_init_controls(struct sensor *sensor)
{
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct v4l2_ctrl *ctrl;
	int ret;

	ctrl_hdlr = &sensor->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 4);
	if (ret)
		return ret;

	mutex_init(&sensor->mutex);
	ctrl_hdlr->lock = &sensor->mutex;

	/* By default, PIXEL_RATE is read only */
	v4l2_ctrl_new_std(ctrl_hdlr, &sensor_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  1, 1, 1, 1);

	/* Initial vblank/hblank/exposure parameters based on current mode */
	ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &sensor_ctrl_ops, V4L2_CID_VBLANK,
				 1, 1, 1, 1);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &sensor_ctrl_ops, V4L2_CID_HBLANK,
				 1, 1, 1, 1);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &sensor_ctrl_ops, V4L2_CID_EXPOSURE,
				 1, 1, 1, 1);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&sensor->pdev->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	sensor->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	mutex_destroy(&sensor->mutex);

	return ret;
}

static void sensor_free_controls(struct sensor *sensor)
{
	v4l2_ctrl_handler_free(sensor->sd.ctrl_handler);
	mutex_destroy(&sensor->mutex);
}

static int sensor_check_hwcfg(struct device *dev)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret = -EINVAL;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	if (v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg)) {
		dev_err(dev, "could not parse endpoint\n");
		goto error_out;
	}

	ret = 0;

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static int sensor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sensor *sensor;
	int ret;

	sensor = devm_kzalloc(&pdev->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->pdev = pdev;

	v4l2_subdev_init(&sensor->sd, &sensor_subdev_ops);
	/* the owner is the same as the i2c_client's driver owner */
	sensor->sd.owner = pdev->dev.driver->owner;
	sensor->sd.dev = &pdev->dev;
	v4l2_set_subdevdata(&sensor->sd, pdev);
	platform_set_drvdata(pdev, sensor);

	/* initialize name */
	snprintf(sensor->sd.name, sizeof(sensor->sd.name), "%s",
		 pdev->dev.driver->name);

	/* Check the hardware configuration in device tree */
	if (sensor_check_hwcfg(dev))
		return -EINVAL;

	sensor->fmt.width = DEFAULT_WIDTH;
	sensor->fmt.height = DEFAULT_HEIGHT;
	sensor->fmt.code = mbus_codes[0];
	sensor->fmt.field = V4L2_FIELD_NONE;
	sensor->fmt.colorspace = V4L2_COLORSPACE_SRGB;
	sensor->fmt.ycbcr_enc =
		V4L2_MAP_YCBCR_ENC_DEFAULT(sensor->fmt.colorspace);
	sensor->fmt.quantization =
		V4L2_MAP_QUANTIZATION_DEFAULT(true,
					      sensor->fmt.colorspace,
					      sensor->fmt.ycbcr_enc);
	sensor->fmt.xfer_func =
		V4L2_MAP_XFER_FUNC_DEFAULT(sensor->fmt.colorspace);

	ret = sensor_init_controls(sensor);
	if (ret)
		return ret;

	/* Initialize subdev */
	sensor->sd.internal_ops = &sensor_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pads */
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sensor->sd.entity, 1,
				     &sensor->pad);
	if (ret) {
		dev_err(dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor_common(&sensor->sd);
	if (ret < 0) {
		dev_err(dev, "failed to register sensor sub-device: %d\n", ret);
		goto error_media_entity;
	}

	return 0;

error_media_entity:
	media_entity_cleanup(&sensor->sd.entity);

error_handler_free:
	sensor_free_controls(sensor);

	return ret;
}

static int sensor_remove(struct platform_device *pdev)
{
	struct sensor *sensor = platform_get_drvdata(pdev);

	v4l2_async_unregister_subdev(&sensor->sd);
	media_entity_cleanup(&sensor->sd.entity);
	sensor_free_controls(sensor);

	return 0;
}

static const struct of_device_id sensor_dt_ids[] = {
	{ .compatible = "raspberrypi,dummy-csi2-sensor" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sensor_dt_ids);

static struct platform_driver sensor_driver = {
	.probe = sensor_probe,
	.remove = sensor_remove,
	.driver = {
		.name = "dummy_csi2_sensor",
		.of_match_table	= sensor_dt_ids,
	},
};

module_platform_driver(sensor_driver);

MODULE_AUTHOR("Dave Stevenson <dave.stevenson@raspberrypi.com>");
MODULE_DESCRIPTION("Dummy CSI-2 sensor driver");
MODULE_LICENSE("GPL v2");
