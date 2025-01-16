// SPDX-License-Identifier: GPL-2.0
/*
 * Raspberry Pi HEVC driver
 *
 * Copyright (C) 2025 Raspberry Pi Ltd
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

#include "hevc_d.h"
#include "hevc_d_h265.h"
#include "hevc_d_video.h"
#include "hevc_d_hw.h"

int hevc_d_v4l2_debug;
module_param_named(debug, hevc_d_v4l2_debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug level 0-2");

/*
 * Default /dev/videoN node number.
 * Deliberately avoid the very low numbers as these are often taken by webcams
 * etc, and simple apps tend to only go for /dev/video0.
 */
static int video_nr = 19;
module_param(video_nr, int, 0644);
MODULE_PARM_DESC(video_nr, "decoder video device number");

static const struct v4l2_ctrl_config hevc_d_ctrls[] = {
	{
		.id	= V4L2_CID_STATELESS_HEVC_SPS,
		.ops	= &hevc_d_hevc_sps_ctrl_ops,
	}, {
		.id	= V4L2_CID_STATELESS_HEVC_PPS,
		.ops	= &hevc_d_hevc_pps_ctrl_ops,
	}, {
		.id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX,
	}, {
		.id	= V4L2_CID_STATELESS_HEVC_DECODE_PARAMS,
	}, {
		.name	= "Slice param array",
		.id	= V4L2_CID_STATELESS_HEVC_SLICE_PARAMS,
		.type	= V4L2_CTRL_TYPE_HEVC_SLICE_PARAMS,
		.flags	= V4L2_CTRL_FLAG_DYNAMIC_ARRAY,
		.dims	= { 600 },
	}, {
		.id	= V4L2_CID_STATELESS_HEVC_DECODE_MODE,
		.min	= V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
		.max	= V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
		.def	= V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
	}, {
		.id	= V4L2_CID_STATELESS_HEVC_START_CODE,
		.min	= V4L2_STATELESS_HEVC_START_CODE_NONE,
		.max	= V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
		.def	= V4L2_STATELESS_HEVC_START_CODE_NONE,
	},
};

void *hevc_d_find_control_data(struct hevc_d_ctx *ctx, u32 id)
{
	struct v4l2_ctrl *const ctrl = v4l2_ctrl_find(ctx->fh.ctrl_handler, id);

	return ctrl ? ctrl->p_cur.p : NULL;
}

static int hevc_d_init_ctrls(struct hevc_d_dev *dev, struct hevc_d_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->hdl;
	struct v4l2_ctrl *ctrl;
	unsigned int i;

	v4l2_ctrl_handler_init(hdl, ARRAY_SIZE(hevc_d_ctrls));
	if (hdl->error) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize control handler\n");
		return hdl->error;
	}

	for (i = 0; i < ARRAY_SIZE(hevc_d_ctrls); i++) {
		ctrl = v4l2_ctrl_new_custom(hdl, &hevc_d_ctrls[i], ctx);
		if (hdl->error) {
			v4l2_err(&dev->v4l2_dev,
				 "Failed to create new custom control id=%#x\n",
				 hevc_d_ctrls[i].id);

			v4l2_ctrl_handler_free(hdl);
			return hdl->error;
		}
	}

	ctx->fh.ctrl_handler = hdl;
	v4l2_ctrl_handler_setup(hdl);

	return 0;
}

static int hevc_d_open(struct file *file)
{
	struct hevc_d_dev *dev = video_drvdata(file);
	struct hevc_d_ctx *ctx = NULL;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->ctx_mutex);

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->dev = dev;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx,
					    &hevc_d_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_free;
	}

	/* The only bit of format info that we can guess now is H265 src
	 * Everything else we need more info for
	 */
	hevc_d_prepare_src_format(&ctx->src_fmt);

	ret = hevc_d_init_ctrls(dev, ctx);
	if (ret)
		goto err_ctx;

	v4l2_fh_add(&ctx->fh, file);
	return 0;

err_ctx:
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
err_free:
	mutex_destroy(&ctx->ctx_mutex);
	kfree(ctx);
	return ret;
}

static int hevc_d_release(struct file *file)
{
	struct hevc_d_ctx *ctx = container_of(file->private_data,
					      struct hevc_d_ctx, fh);

	v4l2_fh_del(&ctx->fh, file);

	v4l2_ctrl_handler_free(&ctx->hdl);

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

	v4l2_fh_exit(&ctx->fh);
	mutex_destroy(&ctx->ctx_mutex);

	kfree(ctx);
	return 0;
}

static void hevc_d_media_req_queue(struct media_request *req)
{
	media_request_mark_manual_completion(req);
	v4l2_m2m_request_queue(req);
}

static const struct v4l2_file_operations hevc_d_fops = {
	.owner		= THIS_MODULE,
	.open		= hevc_d_open,
	.release	= hevc_d_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device hevc_d_video_device = {
	.name		= HEVC_D_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &hevc_d_fops,
	.ioctl_ops	= &hevc_d_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
	.device_caps	= V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING,
};

static const struct v4l2_m2m_ops hevc_d_m2m_ops = {
	.device_run	= hevc_d_device_run,
};

static const struct media_device_ops hevc_d_m2m_media_ops = {
	.req_validate	= vb2_request_validate,
	.req_queue	= hevc_d_media_req_queue,
};

static int hevc_d_probe(struct platform_device *pdev)
{
	struct hevc_d_dev *dev;
	struct video_device *vfd;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->vfd = hevc_d_video_device;
	dev->dev = &pdev->dev;
	dev->pdev = pdev;

	ret = hevc_d_hw_probe(dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to probe hardware - %d\n", ret);
		return ret;
	}

	mutex_init(&dev->dev_mutex);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register V4L2 device\n");
		return ret;
	}

	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;

	snprintf(vfd->name, sizeof(vfd->name), "%s", hevc_d_video_device.name);
	video_set_drvdata(vfd, dev);

	ret = dma_set_mask_and_coherent(dev->dev, DMA_BIT_MASK(36));
	if (ret) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed dma_set_mask_and_coherent\n");
		goto err_v4l2;
	}

	dev->m2m_dev = v4l2_m2m_init(&hevc_d_m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize V4L2 M2M device\n");
		ret = PTR_ERR(dev->m2m_dev);

		goto err_v4l2;
	}

	dev->mdev.dev = &pdev->dev;
	strscpy(dev->mdev.model, HEVC_D_NAME, sizeof(dev->mdev.model));
	strscpy(dev->mdev.bus_info, "platform:" HEVC_D_NAME,
		sizeof(dev->mdev.bus_info));

	media_device_init(&dev->mdev);
	dev->mdev.ops = &hevc_d_m2m_media_ops;
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

static void hevc_d_remove(struct platform_device *pdev)
{
	struct hevc_d_dev *dev = platform_get_drvdata(pdev);

	media_device_unregister(&dev->mdev);
	v4l2_m2m_unregister_media_controller(dev->m2m_dev);
	media_device_cleanup(&dev->mdev);

	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);

	hevc_d_hw_remove(dev);
}

static const struct of_device_id hevc_d_dt_match[] = {
	{ .compatible = "brcm,bcm2711-hevc-dec", },
	{ .compatible = "brcm,bcm2712-hevc-dec", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hevc_d_dt_match);

static struct platform_driver hevc_d_driver = {
	.probe		= hevc_d_probe,
	.remove		= hevc_d_remove,
	.driver		= {
		.name = HEVC_D_NAME,
		.of_match_table	= of_match_ptr(hevc_d_dt_match),
	},
};
module_platform_driver(hevc_d_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Cox <john.cox@raspberrypi.com>");
MODULE_DESCRIPTION("Raspberry Pi HEVC V4L2 driver");
