// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * video stream multiplexer controlled via mux control
 *
 * Copyright (C) 2013 Pengutronix, Sascha Hauer <kernel@pengutronix.de>
 * Copyright (C) 2016-2017 Pengutronix, Philipp Zabel <kernel@pengutronix.de>
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mux/consumer.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

struct video_mux_asd {
	struct v4l2_async_connection base;
	unsigned int port;
};

static inline struct video_mux_asd *to_video_mux_asd(struct v4l2_async_connection *asd)
{
	return container_of(asd, struct video_mux_asd, base);
}

struct video_mux_pad_cfg {
	unsigned int num_lanes;
	bool non_continuous;
	struct v4l2_subdev *source;
};

struct video_mux {
	struct v4l2_subdev subdev;
	struct v4l2_async_notifier notifier;
	struct media_pad *pads;
	struct video_mux_pad_cfg *cfg;
	struct mux_control *mux;
	struct mutex lock;
	int active;
};

static const struct v4l2_mbus_framefmt video_mux_format_mbus_default = {
	.width = 1,
	.height = 1,
	.code = MEDIA_BUS_FMT_Y8_1X8,
	.field = V4L2_FIELD_NONE,
};

static inline struct video_mux *
notifier_to_video_mux(struct v4l2_async_notifier *n)
{
	return container_of(n, struct video_mux, notifier);
}

static inline struct video_mux *v4l2_subdev_to_video_mux(struct v4l2_subdev *sd)
{
	return container_of(sd, struct video_mux, subdev);
}

static int video_mux_link_setup(struct media_entity *entity,
				const struct media_pad *local,
				const struct media_pad *remote, u32 flags)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct v4l2_subdev *source_sd;
	struct v4l2_subdev_state *sd_state;
	struct video_mux *vmux = v4l2_subdev_to_video_mux(sd);
	u16 source_pad = entity->num_pads - 1;
	int ret = 0;

	/*
	 * The mux state is determined by the enabled sink pad link.
	 * Enabling or disabling the source pad link has no effect.
	 */
	if (local->flags & MEDIA_PAD_FL_SOURCE)
		return 0;

	dev_dbg(sd->dev, "link setup '%s':%d->'%s':%d[%d]",
		remote->entity->name, remote->index, local->entity->name,
		local->index, flags & MEDIA_LNK_FL_ENABLED);

	sd_state = v4l2_subdev_lock_and_get_active_state(sd);
	mutex_lock(&vmux->lock);

	if (flags & MEDIA_LNK_FL_ENABLED) {
		struct v4l2_mbus_framefmt *source_mbusformat;

		if (vmux->active == local->index)
			goto out;

		if (vmux->active >= 0) {
			ret = -EBUSY;
			goto out;
		}

		dev_dbg(sd->dev, "setting %d active\n", local->index);
		ret = mux_control_try_select(vmux->mux, local->index);
		if (ret < 0)
			goto out;
		vmux->active = local->index;

		/* Propagate the active format to the source */
		source_mbusformat = v4l2_subdev_state_get_format(sd_state,
							       source_pad);
		*source_mbusformat = *v4l2_subdev_state_get_format(sd_state,
								 vmux->active);

		source_sd = media_entity_to_v4l2_subdev(remote->entity);
		vmux->subdev.ctrl_handler = source_sd->ctrl_handler;

	} else {
		if (vmux->active != local->index)
			goto out;

		dev_dbg(sd->dev, "going inactive\n");
		mux_control_deselect(vmux->mux);
		vmux->active = -1;

		vmux->subdev.ctrl_handler = NULL;
	}

out:
	mutex_unlock(&vmux->lock);
	v4l2_subdev_unlock_state(sd_state);
	return ret;
}

static const struct media_entity_operations video_mux_ops = {
	.link_setup = video_mux_link_setup,
	.link_validate = v4l2_subdev_link_validate,
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
};

static int video_mux_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct video_mux *vmux = v4l2_subdev_to_video_mux(sd);
	struct v4l2_subdev *upstream_sd;
	struct media_pad *pad;

	if (vmux->active == -1) {
		dev_err(sd->dev, "Can not start streaming on inactive mux\n");
		return -EINVAL;
	}

	pad = media_pad_remote_pad_first(&sd->entity.pads[vmux->active]);
	if (!pad) {
		dev_err(sd->dev, "Failed to find remote source pad\n");
		return -ENOLINK;
	}

	if (!is_media_entity_v4l2_subdev(pad->entity)) {
		dev_err(sd->dev, "Upstream entity is not a v4l2 subdev\n");
		return -ENODEV;
	}

	upstream_sd = media_entity_to_v4l2_subdev(pad->entity);

	return v4l2_subdev_call(upstream_sd, video, s_stream, enable);
}

static const struct v4l2_subdev_video_ops video_mux_subdev_video_ops = {
	.s_stream = video_mux_s_stream,
};

static int video_mux_set_format(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *sdformat)
{
	struct video_mux *vmux = v4l2_subdev_to_video_mux(sd);
	struct v4l2_mbus_framefmt *mbusformat, *source_mbusformat;
	struct media_pad *pad = &vmux->pads[sdformat->pad];
	u16 source_pad = sd->entity.num_pads - 1;

	mbusformat = v4l2_subdev_state_get_format(sd_state, sdformat->pad);
	if (!mbusformat)
		return -EINVAL;

	source_mbusformat = v4l2_subdev_state_get_format(sd_state, source_pad);
	if (!source_mbusformat)
		return -EINVAL;

	/* No size limitations except V4L2 compliance requirements */
	v4l_bound_align_image(&sdformat->format.width, 1, 65536, 0,
			      &sdformat->format.height, 1, 65536, 0, 0);

	/* All formats except LVDS and vendor specific formats are acceptable */
	switch (sdformat->format.code) {
	case MEDIA_BUS_FMT_RGB444_1X12:
	case MEDIA_BUS_FMT_RGB444_2X8_PADHI_BE:
	case MEDIA_BUS_FMT_RGB444_2X8_PADHI_LE:
	case MEDIA_BUS_FMT_RGB555_2X8_PADHI_BE:
	case MEDIA_BUS_FMT_RGB555_2X8_PADHI_LE:
	case MEDIA_BUS_FMT_RGB565_1X16:
	case MEDIA_BUS_FMT_BGR565_2X8_BE:
	case MEDIA_BUS_FMT_BGR565_2X8_LE:
	case MEDIA_BUS_FMT_RGB565_2X8_BE:
	case MEDIA_BUS_FMT_RGB565_2X8_LE:
	case MEDIA_BUS_FMT_RGB666_1X18:
	case MEDIA_BUS_FMT_RBG888_1X24:
	case MEDIA_BUS_FMT_RGB666_1X24_CPADHI:
	case MEDIA_BUS_FMT_BGR888_1X24:
	case MEDIA_BUS_FMT_GBR888_1X24:
	case MEDIA_BUS_FMT_RGB888_1X24:
	case MEDIA_BUS_FMT_RGB888_2X12_BE:
	case MEDIA_BUS_FMT_RGB888_2X12_LE:
	case MEDIA_BUS_FMT_ARGB8888_1X32:
	case MEDIA_BUS_FMT_RGB888_1X32_PADHI:
	case MEDIA_BUS_FMT_RGB101010_1X30:
	case MEDIA_BUS_FMT_RGB121212_1X36:
	case MEDIA_BUS_FMT_RGB161616_1X48:
	case MEDIA_BUS_FMT_Y8_1X8:
	case MEDIA_BUS_FMT_UV8_1X8:
	case MEDIA_BUS_FMT_UYVY8_1_5X8:
	case MEDIA_BUS_FMT_VYUY8_1_5X8:
	case MEDIA_BUS_FMT_YUYV8_1_5X8:
	case MEDIA_BUS_FMT_YVYU8_1_5X8:
	case MEDIA_BUS_FMT_UYVY8_2X8:
	case MEDIA_BUS_FMT_VYUY8_2X8:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_YVYU8_2X8:
	case MEDIA_BUS_FMT_Y10_1X10:
	case MEDIA_BUS_FMT_UYVY10_2X10:
	case MEDIA_BUS_FMT_VYUY10_2X10:
	case MEDIA_BUS_FMT_YUYV10_2X10:
	case MEDIA_BUS_FMT_YVYU10_2X10:
	case MEDIA_BUS_FMT_Y12_1X12:
	case MEDIA_BUS_FMT_UYVY12_2X12:
	case MEDIA_BUS_FMT_VYUY12_2X12:
	case MEDIA_BUS_FMT_YUYV12_2X12:
	case MEDIA_BUS_FMT_YVYU12_2X12:
	case MEDIA_BUS_FMT_UYVY8_1X16:
	case MEDIA_BUS_FMT_VYUY8_1X16:
	case MEDIA_BUS_FMT_YUYV8_1X16:
	case MEDIA_BUS_FMT_YVYU8_1X16:
	case MEDIA_BUS_FMT_YDYUYDYV8_1X16:
	case MEDIA_BUS_FMT_UYVY10_1X20:
	case MEDIA_BUS_FMT_VYUY10_1X20:
	case MEDIA_BUS_FMT_YUYV10_1X20:
	case MEDIA_BUS_FMT_YVYU10_1X20:
	case MEDIA_BUS_FMT_VUY8_1X24:
	case MEDIA_BUS_FMT_YUV8_1X24:
	case MEDIA_BUS_FMT_UYYVYY8_0_5X24:
	case MEDIA_BUS_FMT_UYVY12_1X24:
	case MEDIA_BUS_FMT_VYUY12_1X24:
	case MEDIA_BUS_FMT_YUYV12_1X24:
	case MEDIA_BUS_FMT_YVYU12_1X24:
	case MEDIA_BUS_FMT_YUV10_1X30:
	case MEDIA_BUS_FMT_UYYVYY10_0_5X30:
	case MEDIA_BUS_FMT_AYUV8_1X32:
	case MEDIA_BUS_FMT_UYYVYY12_0_5X36:
	case MEDIA_BUS_FMT_YUV12_1X36:
	case MEDIA_BUS_FMT_YUV16_1X48:
	case MEDIA_BUS_FMT_UYYVYY16_0_5X48:
	case MEDIA_BUS_FMT_JPEG_1X8:
	case MEDIA_BUS_FMT_AHSV8888_1X32:
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SBGGR12_1X12:
	case MEDIA_BUS_FMT_SGBRG12_1X12:
	case MEDIA_BUS_FMT_SGRBG12_1X12:
	case MEDIA_BUS_FMT_SRGGB12_1X12:
	case MEDIA_BUS_FMT_SBGGR14_1X14:
	case MEDIA_BUS_FMT_SGBRG14_1X14:
	case MEDIA_BUS_FMT_SGRBG14_1X14:
	case MEDIA_BUS_FMT_SRGGB14_1X14:
	case MEDIA_BUS_FMT_SBGGR16_1X16:
	case MEDIA_BUS_FMT_SGBRG16_1X16:
	case MEDIA_BUS_FMT_SGRBG16_1X16:
	case MEDIA_BUS_FMT_SRGGB16_1X16:
		break;
	default:
		sdformat->format.code = MEDIA_BUS_FMT_Y8_1X8;
		break;
	}
	if (sdformat->format.field == V4L2_FIELD_ANY)
		sdformat->format.field = V4L2_FIELD_NONE;

	mutex_lock(&vmux->lock);

	/* Source pad mirrors active sink pad, no limitations on sink pads */
	if ((pad->flags & MEDIA_PAD_FL_SOURCE) && vmux->active >= 0)
		sdformat->format = *v4l2_subdev_state_get_format(sd_state,
							       vmux->active);

	*mbusformat = sdformat->format;

	/* Propagate the format from an active sink to source */
	if ((pad->flags & MEDIA_PAD_FL_SINK) && (pad->index == vmux->active))
		*source_mbusformat = sdformat->format;

	mutex_unlock(&vmux->lock);

	return 0;
}

static int video_mux_init_state(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state)
{
	struct video_mux *vmux = v4l2_subdev_to_video_mux(sd);
	struct v4l2_mbus_framefmt *mbusformat;
	unsigned int i;

	mutex_lock(&vmux->lock);

	for (i = 0; i < sd->entity.num_pads; i++) {
		mbusformat = v4l2_subdev_state_get_format(sd_state, i);
		*mbusformat = video_mux_format_mbus_default;
	}

	mutex_unlock(&vmux->lock);

	return 0;
}

static int video_mux_get_mbus_config(struct v4l2_subdev *sd,
				     unsigned int pad,
				     struct v4l2_mbus_config *cfg)
{
	struct video_mux *vmux = v4l2_subdev_to_video_mux(sd);
	int ret;

	ret = v4l2_subdev_call(vmux->cfg[vmux->active].source, pad, get_mbus_config,
			       0, cfg);

	if (ret != -ENOIOCTLCMD)
		return ret;

	cfg->type = V4L2_MBUS_CSI2_DPHY;
	cfg->bus.mipi_csi2.num_data_lanes = vmux->cfg[vmux->active].num_lanes;

	/* Support for non-continuous CSI-2 clock is missing in pdate mode */
	if (vmux->cfg[vmux->active].non_continuous)
		cfg->bus.mipi_csi2.flags |= V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK;

	return 0;
};

static const struct v4l2_subdev_pad_ops video_mux_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = video_mux_set_format,
	.get_mbus_config = video_mux_get_mbus_config,
};

static const struct v4l2_subdev_ops video_mux_subdev_ops = {
	.pad = &video_mux_pad_ops,
	.video = &video_mux_subdev_video_ops,
};

static const struct v4l2_subdev_internal_ops video_mux_internal_ops = {
	.init_state = video_mux_init_state,
};

static int video_mux_notify_bound(struct v4l2_async_notifier *notifier,
				  struct v4l2_subdev *sd,
				  struct v4l2_async_connection *asd)
{
	struct video_mux *vmux = notifier_to_video_mux(notifier);
	unsigned int port = to_video_mux_asd(asd)->port;

	vmux->cfg[port].source = sd;

	return v4l2_create_fwnode_links(sd, &vmux->subdev);
}

static const struct v4l2_async_notifier_operations video_mux_notify_ops = {
	.bound = video_mux_notify_bound,
};

static int video_mux_async_register(struct video_mux *vmux,
				    unsigned int num_input_pads)
{
	unsigned int i;
	int ret;

	v4l2_async_subdev_nf_init(&vmux->notifier, &vmux->subdev);

	for (i = 0; i < num_input_pads; i++) {
		struct video_mux_asd *asd;
		struct fwnode_handle *ep, *remote_ep;

		ep = fwnode_graph_get_endpoint_by_id(
			dev_fwnode(vmux->subdev.dev), i, 0,
			FWNODE_GRAPH_ENDPOINT_NEXT);
		if (!ep)
			continue;

		/* Skip dangling endpoints for backwards compatibility */
		remote_ep = fwnode_graph_get_remote_endpoint(ep);
		if (!remote_ep) {
			fwnode_handle_put(ep);
			continue;
		}
		fwnode_handle_put(remote_ep);

		asd = v4l2_async_nf_add_fwnode_remote(&vmux->notifier, ep,
						      struct video_mux_asd);
		fwnode_handle_put(ep);

		if (IS_ERR(asd)) {
			ret = PTR_ERR(asd);
			/* OK if asd already exists */
			if (ret != -EEXIST)
				goto err_nf_cleanup;
		}

		asd->port = i;
	}

	vmux->notifier.ops = &video_mux_notify_ops;

	ret = v4l2_async_nf_register(&vmux->notifier);
	if (ret)
		goto err_nf_cleanup;

	ret = v4l2_async_register_subdev(&vmux->subdev);
	if (ret)
		goto err_nf_unregister;

	return 0;

err_nf_unregister:
	v4l2_async_nf_unregister(&vmux->notifier);
err_nf_cleanup:
	v4l2_async_nf_cleanup(&vmux->notifier);
	return ret;
}

static int video_mux_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct v4l2_fwnode_endpoint fwnode_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct device_node *ep;
	struct video_mux *vmux;
	unsigned int num_pads = 0;
	unsigned int i;
	int ret;

	vmux = devm_kzalloc(dev, sizeof(*vmux), GFP_KERNEL);
	if (!vmux)
		return -ENOMEM;

	platform_set_drvdata(pdev, vmux);

	v4l2_subdev_init(&vmux->subdev, &video_mux_subdev_ops);
	vmux->subdev.internal_ops = &video_mux_internal_ops;
	snprintf(vmux->subdev.name, sizeof(vmux->subdev.name), "%pOFn", np);
	vmux->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	vmux->subdev.dev = dev;

	/*
	 * The largest numbered port is the output port. It determines
	 * total number of pads.
	 */
	for_each_endpoint_of_node(np, ep) {
		struct of_endpoint endpoint;

		of_graph_parse_endpoint(ep, &endpoint);
		num_pads = max(num_pads, endpoint.port + 1);
	}

	if (num_pads < 2) {
		dev_err(dev, "Not enough ports %d\n", num_pads);
		return -EINVAL;
	}

	vmux->mux = devm_mux_control_get(dev, NULL);
	if (IS_ERR(vmux->mux)) {
		ret = PTR_ERR(vmux->mux);
		return dev_err_probe(dev, ret, "Failed to get mux\n");
	}

	mutex_init(&vmux->lock);
	vmux->active = -1;
	vmux->pads = devm_kcalloc(dev, num_pads, sizeof(*vmux->pads),
				  GFP_KERNEL);
	if (!vmux->pads)
		return -ENOMEM;

	vmux->cfg = devm_kcalloc(dev, num_pads, sizeof(*vmux->cfg), GFP_KERNEL);
	if (!vmux->cfg)
		return -ENOMEM;

	for (i = 0; i < num_pads; i++) {
		vmux->pads[i].flags = (i < num_pads - 1) ? MEDIA_PAD_FL_SINK
							 : MEDIA_PAD_FL_SOURCE;

		ep = of_graph_get_endpoint_by_regs(pdev->dev.of_node, i, 0);
		if (ep) {
			ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(ep), &fwnode_ep);
			if (!ret) {
				/* Get number of data lanes */
				vmux->cfg[i].num_lanes = fwnode_ep.bus.mipi_csi2.num_data_lanes;
				vmux->cfg[i].non_continuous = fwnode_ep.bus.mipi_csi2.flags &
							V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK;
			}
			of_node_put(ep);
		}
	}

	vmux->subdev.entity.function = MEDIA_ENT_F_VID_MUX;
	ret = media_entity_pads_init(&vmux->subdev.entity, num_pads,
				     vmux->pads);
	if (ret < 0)
		return ret;

	vmux->subdev.entity.ops = &video_mux_ops;

	ret = v4l2_subdev_init_finalize(&vmux->subdev);
	if (ret < 0)
		goto err_entity_cleanup;

	ret = video_mux_async_register(vmux, num_pads - 1);
	if (ret)
		goto err_subdev_cleanup;

	return 0;

err_subdev_cleanup:
	v4l2_subdev_cleanup(&vmux->subdev);
err_entity_cleanup:
	media_entity_cleanup(&vmux->subdev.entity);
	return ret;
}

static void video_mux_remove(struct platform_device *pdev)
{
	struct video_mux *vmux = platform_get_drvdata(pdev);
	struct v4l2_subdev *sd = &vmux->subdev;

	v4l2_async_nf_unregister(&vmux->notifier);
	v4l2_async_nf_cleanup(&vmux->notifier);
	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
}

static const struct of_device_id video_mux_dt_ids[] = {
	{ .compatible = "video-mux", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, video_mux_dt_ids);

static struct platform_driver video_mux_driver = {
	.probe		= video_mux_probe,
	.remove_new	= video_mux_remove,
	.driver		= {
		.of_match_table = video_mux_dt_ids,
		.name = "video-mux",
	},
};

module_platform_driver(video_mux_driver);

MODULE_DESCRIPTION("video stream multiplexer");
MODULE_AUTHOR("Sascha Hauer, Pengutronix");
MODULE_AUTHOR("Philipp Zabel, Pengutronix");
MODULE_LICENSE("GPL");
