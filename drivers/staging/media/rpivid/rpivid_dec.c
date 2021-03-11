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

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "rpivid.h"
#include "rpivid_dec.h"

void rpivid_device_run(void *priv)
{
	struct rpivid_ctx *const ctx = priv;
	struct rpivid_dev *const dev = ctx->dev;
	struct rpivid_run run = {};
	struct media_request *src_req;

	run.src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run.dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	if (!run.src || !run.dst) {
		v4l2_err(&dev->v4l2_dev, "%s: Missing buffer: src=%p, dst=%p\n",
			 __func__, run.src, run.dst);
		goto fail;
	}

	/* Apply request(s) controls */
	src_req = run.src->vb2_buf.req_obj.req;
	if (!src_req) {
		v4l2_err(&dev->v4l2_dev, "%s: Missing request\n", __func__);
		goto fail;
	}

	v4l2_ctrl_request_setup(src_req, &ctx->hdl);

	switch (ctx->src_fmt.pixelformat) {
	case V4L2_PIX_FMT_HEVC_SLICE:
		run.h265.sps =
			rpivid_find_control_data(ctx,
						 V4L2_CID_MPEG_VIDEO_HEVC_SPS);
		run.h265.pps =
			rpivid_find_control_data(ctx,
						 V4L2_CID_MPEG_VIDEO_HEVC_PPS);
		run.h265.slice_params =
			rpivid_find_control_data(ctx,
						 V4L2_CID_MPEG_VIDEO_HEVC_SLICE_PARAMS);
		run.h265.scaling_matrix =
			rpivid_find_control_data(ctx,
						 V4L2_CID_MPEG_VIDEO_HEVC_SCALING_MATRIX);
		break;

	default:
		break;
	}

	v4l2_m2m_buf_copy_metadata(run.src, run.dst, true);

	dev->dec_ops->setup(ctx, &run);

	/* Complete request(s) controls */
	v4l2_ctrl_request_complete(src_req, &ctx->hdl);

	dev->dec_ops->trigger(ctx);
	return;

fail:
	/* We really shouldn't get here but tidy up what we can */
	v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx,
					 VB2_BUF_STATE_ERROR);
}
