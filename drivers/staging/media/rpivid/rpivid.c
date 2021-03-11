// SPDX-License-Identifier: GPL-2.0
/*
 * Raspberry Pi HEVC driver
 *
 * Copyright (C) 2020 Raspberry Pi (Trading) Ltd
 *
 * Based on the Cedrus VPU driver, that is:
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>

#include "rpivid.h"
#include "rpivid_video.h"
#include "rpivid_hw.h"
#include "rpivid_dec.h"

/*
 * Default /dev/videoN node number.
 * Deliberately avoid the very low numbers as these are often taken by webcams
 * etc, and simple apps tend to only go for /dev/video0.
 */
static int video_nr = 19;
module_param(video_nr, int, 0644);
MODULE_PARM_DESC(video_nr, "decoder video device number");

static const struct rpivid_control rpivid_ctrls[] = {
	{
		.cfg = {
			.id	= V4L2_CID_MPEG_VIDEO_HEVC_SPS,
		},
		.required	= true,
	},
	{
		.cfg = {
			.id	= V4L2_CID_MPEG_VIDEO_HEVC_PPS,
		},
		.required	= true,
	},
	{
		.cfg = {
			.id = V4L2_CID_MPEG_VIDEO_HEVC_SCALING_MATRIX,
		},
		.required	= false,
	},
	{
		.cfg = {
			.id	= V4L2_CID_MPEG_VIDEO_HEVC_SLICE_PARAMS,
		},
		.required	= true,
	},
	{
		.cfg = {
			.id	= V4L2_CID_MPEG_VIDEO_HEVC_DECODE_MODE,
			.max	= V4L2_MPEG_VIDEO_HEVC_DECODE_MODE_SLICE_BASED,
			.def	= V4L2_MPEG_VIDEO_HEVC_DECODE_MODE_SLICE_BASED,
		},
		.required	= false,
	},
	{
		.cfg = {
			.id	= V4L2_CID_MPEG_VIDEO_HEVC_START_CODE,
			.max	= V4L2_MPEG_VIDEO_HEVC_START_CODE_NONE,
			.def	= V4L2_MPEG_VIDEO_HEVC_START_CODE_NONE,
		},
		.required	= false,
	},
};

#define rpivid_ctrls_COUNT	ARRAY_SIZE(rpivid_ctrls)

struct v4l2_ctrl *rpivid_find_ctrl(struct rpivid_ctx *ctx, u32 id)
{
	unsigned int i;

	for (i = 0; ctx->ctrls[i]; i++)
		if (ctx->ctrls[i]->id == id)
			return ctx->ctrls[i];

	return NULL;
}

void *rpivid_find_control_data(struct rpivid_ctx *ctx, u32 id)
{
	struct v4l2_ctrl *const ctrl = rpivid_find_ctrl(ctx, id);

	return !ctrl ? NULL : ctrl->p_cur.p;
}

static int rpivid_init_ctrls(struct rpivid_dev *dev, struct rpivid_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->hdl;
	struct v4l2_ctrl *ctrl;
	unsigned int ctrl_size;
	unsigned int i;

	v4l2_ctrl_handler_init(hdl, rpivid_ctrls_COUNT);
	if (hdl->error) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize control handler\n");
		return hdl->error;
	}

	ctrl_size = sizeof(ctrl) * rpivid_ctrls_COUNT + 1;

	ctx->ctrls = kzalloc(ctrl_size, GFP_KERNEL);
	if (!ctx->ctrls)
		return -ENOMEM;

	for (i = 0; i < rpivid_ctrls_COUNT; i++) {
		ctrl = v4l2_ctrl_new_custom(hdl, &rpivid_ctrls[i].cfg,
					    NULL);
		if (hdl->error) {
			v4l2_err(&dev->v4l2_dev,
				 "Failed to create new custom control id=%#x\n",
				 rpivid_ctrls[i].cfg.id);

			v4l2_ctrl_handler_free(hdl);
			kfree(ctx->ctrls);
			return hdl->error;
		}

		ctx->ctrls[i] = ctrl;
	}

	ctx->fh.ctrl_handler = hdl;
	v4l2_ctrl_handler_setup(hdl);

	return 0;
}

static int rpivid_request_validate(struct media_request *req)
{
	struct media_request_object *obj;
	struct v4l2_ctrl_handler *parent_hdl, *hdl;
	struct rpivid_ctx *ctx = NULL;
	struct v4l2_ctrl *ctrl_test;
	unsigned int count;
	unsigned int i;

	list_for_each_entry(obj, &req->objects, list) {
		struct vb2_buffer *vb;

		if (vb2_request_object_is_buffer(obj)) {
			vb = container_of(obj, struct vb2_buffer, req_obj);
			ctx = vb2_get_drv_priv(vb->vb2_queue);

			break;
		}
	}

	if (!ctx)
		return -ENOENT;

	count = vb2_request_buffer_cnt(req);
	if (!count) {
		v4l2_info(&ctx->dev->v4l2_dev,
			  "No buffer was provided with the request\n");
		return -ENOENT;
	} else if (count > 1) {
		v4l2_info(&ctx->dev->v4l2_dev,
			  "More than one buffer was provided with the request\n");
		return -EINVAL;
	}

	parent_hdl = &ctx->hdl;

	hdl = v4l2_ctrl_request_hdl_find(req, parent_hdl);
	if (!hdl) {
		v4l2_info(&ctx->dev->v4l2_dev, "Missing codec control(s)\n");
		return -ENOENT;
	}

	for (i = 0; i < rpivid_ctrls_COUNT; i++) {
		if (!rpivid_ctrls[i].required)
			continue;

		ctrl_test =
			v4l2_ctrl_request_hdl_ctrl_find(hdl,
							rpivid_ctrls[i].cfg.id);
		if (!ctrl_test) {
			v4l2_info(&ctx->dev->v4l2_dev,
				  "Missing required codec control\n");
			return -ENOENT;
		}
	}

	v4l2_ctrl_request_hdl_put(hdl);

	return vb2_request_validate(req);
}

static int rpivid_open(struct file *file)
{
	struct rpivid_dev *dev = video_drvdata(file);
	struct rpivid_ctx *ctx = NULL;
	int ret;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		mutex_unlock(&dev->dev_mutex);
		return -ENOMEM;
	}

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->dev = dev;

	ret = rpivid_init_ctrls(dev, ctx);
	if (ret)
		goto err_free;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx,
					    &rpivid_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrls;
	}

	/* The only bit of format info that we can guess now is H265 src
	 * Everything else we need more info for
	 */
	ctx->src_fmt.pixelformat = RPIVID_SRC_PIXELFORMAT_DEFAULT;
	rpivid_prepare_src_format(&ctx->src_fmt);

	v4l2_fh_add(&ctx->fh);

	mutex_unlock(&dev->dev_mutex);

	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&ctx->hdl);
err_free:
	kfree(ctx);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

static int rpivid_release(struct file *file)
{
	struct rpivid_dev *dev = video_drvdata(file);
	struct rpivid_ctx *ctx = container_of(file->private_data,
					      struct rpivid_ctx, fh);

	mutex_lock(&dev->dev_mutex);

	v4l2_fh_del(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

	v4l2_ctrl_handler_free(&ctx->hdl);
	kfree(ctx->ctrls);

	v4l2_fh_exit(&ctx->fh);

	kfree(ctx);

	mutex_unlock(&dev->dev_mutex);

	return 0;
}

static const struct v4l2_file_operations rpivid_fops = {
	.owner		= THIS_MODULE,
	.open		= rpivid_open,
	.release	= rpivid_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device rpivid_video_device = {
	.name		= RPIVID_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &rpivid_fops,
	.ioctl_ops	= &rpivid_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
	.device_caps	= V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING,
};

static const struct v4l2_m2m_ops rpivid_m2m_ops = {
	.device_run	= rpivid_device_run,
};

static const struct media_device_ops rpivid_m2m_media_ops = {
	.req_validate	= rpivid_request_validate,
	.req_queue	= v4l2_m2m_request_queue,
};

static int rpivid_probe(struct platform_device *pdev)
{
	struct rpivid_dev *dev;
	struct video_device *vfd;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->vfd = rpivid_video_device;
	dev->dev = &pdev->dev;
	dev->pdev = pdev;

	ret = 0;
	ret = rpivid_hw_probe(dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to probe hardware\n");
		return ret;
	}

	dev->dec_ops = &rpivid_dec_ops_h265;

	mutex_init(&dev->dev_mutex);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register V4L2 device\n");
		return ret;
	}

	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;

	snprintf(vfd->name, sizeof(vfd->name), "%s", rpivid_video_device.name);
	video_set_drvdata(vfd, dev);

	dev->m2m_dev = v4l2_m2m_init(&rpivid_m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize V4L2 M2M device\n");
		ret = PTR_ERR(dev->m2m_dev);

		goto err_v4l2;
	}

	dev->mdev.dev = &pdev->dev;
	strscpy(dev->mdev.model, RPIVID_NAME, sizeof(dev->mdev.model));
	strscpy(dev->mdev.bus_info, "platform:" RPIVID_NAME,
		sizeof(dev->mdev.bus_info));

	media_device_init(&dev->mdev);
	dev->mdev.ops = &rpivid_m2m_media_ops;
	dev->v4l2_dev.mdev = &dev->mdev;

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, video_nr);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		goto err_m2m;
	}

	v4l2_info(&dev->v4l2_dev,
		  "Device registered as /dev/video%d\n", vfd->num);

	ret = v4l2_m2m_register_media_controller(dev->m2m_dev, vfd,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize V4L2 M2M media controller\n");
		goto err_video;
	}

	ret = media_device_register(&dev->mdev);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register media device\n");
		goto err_m2m_mc;
	}

	platform_set_drvdata(pdev, dev);

	return 0;

err_m2m_mc:
	v4l2_m2m_unregister_media_controller(dev->m2m_dev);
err_video:
	video_unregister_device(&dev->vfd);
err_m2m:
	v4l2_m2m_release(dev->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);

	return ret;
}

static int rpivid_remove(struct platform_device *pdev)
{
	struct rpivid_dev *dev = platform_get_drvdata(pdev);

	if (media_devnode_is_registered(dev->mdev.devnode)) {
		media_device_unregister(&dev->mdev);
		v4l2_m2m_unregister_media_controller(dev->m2m_dev);
		media_device_cleanup(&dev->mdev);
	}

	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);

	rpivid_hw_remove(dev);

	return 0;
}

static const struct of_device_id rpivid_dt_match[] = {
	{
		.compatible = "raspberrypi,rpivid-vid-decoder",
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rpivid_dt_match);

static struct platform_driver rpivid_driver = {
	.probe		= rpivid_probe,
	.remove		= rpivid_remove,
	.driver		= {
		.name = RPIVID_NAME,
		.of_match_table	= of_match_ptr(rpivid_dt_match),
	},
};
module_platform_driver(rpivid_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("John Cox <jc@kynesim.co.uk>");
MODULE_DESCRIPTION("Raspberry Pi HEVC V4L2 driver");
