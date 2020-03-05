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

#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "rpivid.h"
#include "rpivid_hw.h"
#include "rpivid_video.h"
#include "rpivid_dec.h"

#define RPIVID_DECODE_SRC	BIT(0)
#define RPIVID_DECODE_DST	BIT(1)

#define RPIVID_MIN_WIDTH	16U
#define RPIVID_MIN_HEIGHT	16U
#define RPIVID_DEFAULT_WIDTH	1920U
#define RPIVID_DEFAULT_HEIGHT	1088U
#define RPIVID_MAX_WIDTH	4096U
#define RPIVID_MAX_HEIGHT	4096U

static inline struct rpivid_ctx *rpivid_file2ctx(struct file *file)
{
	return container_of(file->private_data, struct rpivid_ctx, fh);
}

/* constrain x to y,y*2 */
static inline unsigned int constrain2x(unsigned int x, unsigned int y)
{
	return (x < y) ?
			y :
			(x > y * 2) ? y : x;
}

size_t rpivid_round_up_size(const size_t x)
{
	/* Admit no size < 256 */
	const unsigned int n = x < 256 ? 8 : ilog2(x);

	return x >= (3 << n) ? 4 << n : (3 << n);
}

size_t rpivid_bit_buf_size(unsigned int w, unsigned int h, unsigned int bits_minus8)
{
	const size_t wxh = w * h;
	size_t bits_alloc;

	/* Annex A gives a min compression of 2 @ lvl 3.1
	 * (wxh <= 983040) and min 4 thereafter but avoid
	 * the odity of 983041 having a lower limit than
	 * 983040.
	 * Multiply by 3/2 for 4:2:0
	 */
	bits_alloc = wxh < 983040 ? wxh * 3 / 4 :
		wxh < 983040 * 2 ? 983040 * 3 / 4 :
		wxh * 3 / 8;
	/* Allow for bit depth */
	bits_alloc += (bits_alloc * bits_minus8) / 8;
	return rpivid_round_up_size(bits_alloc);
}

void rpivid_prepare_src_format(struct v4l2_pix_format_mplane *pix_fmt)
{
	size_t size;
	u32 w;
	u32 h;

	w = pix_fmt->width;
	h = pix_fmt->height;
	if (!w || !h) {
		w = RPIVID_DEFAULT_WIDTH;
		h = RPIVID_DEFAULT_HEIGHT;
	}
	if (w > RPIVID_MAX_WIDTH)
		w = RPIVID_MAX_WIDTH;
	if (h > RPIVID_MAX_HEIGHT)
		h = RPIVID_MAX_HEIGHT;

	if (!pix_fmt->plane_fmt[0].sizeimage ||
	    pix_fmt->plane_fmt[0].sizeimage > SZ_32M) {
		/* Unspecified or way too big - pick max for size */
		size = rpivid_bit_buf_size(w, h, 2);
	}
	/* Set a minimum */
	size = max_t(u32, SZ_4K, pix_fmt->plane_fmt[0].sizeimage);

	pix_fmt->pixelformat = V4L2_PIX_FMT_HEVC_SLICE;
	pix_fmt->width = w;
	pix_fmt->height = h;
	pix_fmt->num_planes = 1;
	pix_fmt->field = V4L2_FIELD_NONE;
	/* Zero bytes per line for encoded source. */
	pix_fmt->plane_fmt[0].bytesperline = 0;
	pix_fmt->plane_fmt[0].sizeimage = size;
}

/* Take any pix_format and make it valid */
static void rpivid_prepare_dst_format(struct v4l2_pix_format_mplane *pix_fmt)
{
	unsigned int width = pix_fmt->width;
	unsigned int height = pix_fmt->height;
	unsigned int sizeimage = pix_fmt->plane_fmt[0].sizeimage;
	unsigned int bytesperline = pix_fmt->plane_fmt[0].bytesperline;

	if (!width)
		width = RPIVID_DEFAULT_WIDTH;
	if (width > RPIVID_MAX_WIDTH)
		width = RPIVID_MAX_WIDTH;
	if (!height)
		height = RPIVID_DEFAULT_HEIGHT;
	if (height > RPIVID_MAX_HEIGHT)
		height = RPIVID_MAX_HEIGHT;

	/* For column formats set bytesperline to column height (stride2) */
	switch (pix_fmt->pixelformat) {
	default:
		pix_fmt->pixelformat = V4L2_PIX_FMT_NV12_COL128;
		fallthrough;
	case V4L2_PIX_FMT_NV12_COL128:
		/* Width rounds up to columns */
		width = ALIGN(width, 128);

		/* 16 aligned height - not sure we even need that */
		height = ALIGN(height, 16);
		/* column height
		 * Accept suggested shape if at least min & < 2 * min
		 */
		bytesperline = constrain2x(bytesperline, height * 3 / 2);

		/* image size
		 * Again allow plausible variation in case added padding is
		 * required
		 */
		sizeimage = constrain2x(sizeimage, bytesperline * width);
		break;

	case V4L2_PIX_FMT_NV12_10_COL128:
		/* width in pixels (3 pels = 4 bytes) rounded to 128 byte
		 * columns
		 */
		width = ALIGN(((width + 2) / 3), 32) * 3;

		/* 16-aligned height. */
		height = ALIGN(height, 16);

		/* column height
		 * Accept suggested shape if at least min & < 2 * min
		 */
		bytesperline = constrain2x(bytesperline, height * 3 / 2);

		/* image size
		 * Again allow plausible variation in case added padding is
		 * required
		 */
		sizeimage = constrain2x(sizeimage,
					bytesperline * width * 4 / 3);
		break;
	}

	pix_fmt->width = width;
	pix_fmt->height = height;

	pix_fmt->field = V4L2_FIELD_NONE;
	pix_fmt->plane_fmt[0].bytesperline = bytesperline;
	pix_fmt->plane_fmt[0].sizeimage = sizeimage;
	pix_fmt->num_planes = 1;
}

static int rpivid_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strscpy(cap->driver, RPIVID_NAME, sizeof(cap->driver));
	strscpy(cap->card, RPIVID_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", RPIVID_NAME);

	return 0;
}

static int rpivid_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	// Input formats

	// H.265 Slice only currently
	if (f->index == 0) {
		f->pixelformat = V4L2_PIX_FMT_HEVC_SLICE;
		return 0;
	}

	return -EINVAL;
}

static int rpivid_hevc_validate_sps(const struct v4l2_ctrl_hevc_sps * const sps)
{
	const unsigned int ctb_log2_size_y =
			sps->log2_min_luma_coding_block_size_minus3 + 3 +
			sps->log2_diff_max_min_luma_coding_block_size;
	const unsigned int min_tb_log2_size_y =
			sps->log2_min_luma_transform_block_size_minus2 + 2;
	const unsigned int max_tb_log2_size_y = min_tb_log2_size_y +
			sps->log2_diff_max_min_luma_transform_block_size;

	/* Local limitations */
	if (sps->pic_width_in_luma_samples < 32 ||
	    sps->pic_width_in_luma_samples > 4096)
		return 0;
	if (sps->pic_height_in_luma_samples < 32 ||
	    sps->pic_height_in_luma_samples > 4096)
		return 0;
	if (!(sps->bit_depth_luma_minus8 == 0 ||
	      sps->bit_depth_luma_minus8 == 2))
		return 0;
	if (sps->bit_depth_luma_minus8 != sps->bit_depth_chroma_minus8)
		return 0;
	if (sps->chroma_format_idc != 1)
		return 0;

	/*  Limits from H.265 7.4.3.2.1 */
	if (sps->log2_max_pic_order_cnt_lsb_minus4 > 12)
		return 0;
	if (sps->sps_max_dec_pic_buffering_minus1 > 15)
		return 0;
	if (sps->sps_max_num_reorder_pics >
				sps->sps_max_dec_pic_buffering_minus1)
		return 0;
	if (ctb_log2_size_y > 6)
		return 0;
	if (max_tb_log2_size_y > 5)
		return 0;
	if (max_tb_log2_size_y > ctb_log2_size_y)
		return 0;
	if (sps->max_transform_hierarchy_depth_inter >
				(ctb_log2_size_y - min_tb_log2_size_y))
		return 0;
	if (sps->max_transform_hierarchy_depth_intra >
				(ctb_log2_size_y - min_tb_log2_size_y))
		return 0;
	/* Check pcm stuff */
	if (sps->num_short_term_ref_pic_sets > 64)
		return 0;
	if (sps->num_long_term_ref_pics_sps > 32)
		return 0;
	return 1;
}

static inline int is_sps_set(const struct v4l2_ctrl_hevc_sps * const sps)
{
	return sps && sps->pic_width_in_luma_samples != 0;
}

static u32 pixelformat_from_sps(const struct v4l2_ctrl_hevc_sps * const sps,
				const int index)
{
	u32 pf = 0;

	if (!is_sps_set(sps) || !rpivid_hevc_validate_sps(sps)) {
		/* Treat this as an error? For now return both */
		if (index == 0)
			pf = V4L2_PIX_FMT_NV12_COL128;
		else if (index == 1)
			pf = V4L2_PIX_FMT_NV12_10_COL128;
	} else if (index == 0) {
		if (sps->bit_depth_luma_minus8 == 0)
			pf = V4L2_PIX_FMT_NV12_COL128;
		else if (sps->bit_depth_luma_minus8 == 2)
			pf = V4L2_PIX_FMT_NV12_10_COL128;
	}

	return pf;
}

static struct v4l2_pix_format_mplane
rpivid_hevc_default_dst_fmt(struct rpivid_ctx * const ctx)
{
	const struct v4l2_ctrl_hevc_sps * const sps =
		rpivid_find_control_data(ctx, V4L2_CID_STATELESS_HEVC_SPS);
	struct v4l2_pix_format_mplane pix_fmt;

	memset(&pix_fmt, 0, sizeof(pix_fmt));
	if (is_sps_set(sps)) {
		pix_fmt.width = sps->pic_width_in_luma_samples;
		pix_fmt.height = sps->pic_height_in_luma_samples;
		pix_fmt.pixelformat = pixelformat_from_sps(sps, 0);
	}

	rpivid_prepare_dst_format(&pix_fmt);
	return pix_fmt;
}

static u32 rpivid_hevc_get_dst_pixelformat(struct rpivid_ctx * const ctx,
					   const int index)
{
	const struct v4l2_ctrl_hevc_sps * const sps =
		rpivid_find_control_data(ctx, V4L2_CID_STATELESS_HEVC_SPS);

	return pixelformat_from_sps(sps, index);
}

static int rpivid_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct rpivid_ctx * const ctx = rpivid_file2ctx(file);

	const u32 pf = rpivid_hevc_get_dst_pixelformat(ctx, f->index);

	if (pf == 0)
		return -EINVAL;

	f->pixelformat = pf;
	return 0;
}

/*
 * get dst format - sets it to default if otherwise unset
 * returns a pointer to the struct as a convienience
 */
static struct v4l2_pix_format_mplane *get_dst_fmt(struct rpivid_ctx *const ctx)
{
	if (!ctx->dst_fmt_set)
		ctx->dst_fmt = rpivid_hevc_default_dst_fmt(ctx);
	return &ctx->dst_fmt;
}

static int rpivid_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rpivid_ctx *ctx = rpivid_file2ctx(file);

	f->fmt.pix_mp = *get_dst_fmt(ctx);
	return 0;
}

static int rpivid_g_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rpivid_ctx *ctx = rpivid_file2ctx(file);

	f->fmt.pix_mp = ctx->src_fmt;
	return 0;
}

static inline void copy_color(struct v4l2_pix_format_mplane *d,
			      const struct v4l2_pix_format_mplane *s)
{
	d->colorspace   = s->colorspace;
	d->xfer_func    = s->xfer_func;
	d->ycbcr_enc    = s->ycbcr_enc;
	d->quantization = s->quantization;
}

static int rpivid_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct rpivid_ctx *ctx = rpivid_file2ctx(file);
	const struct v4l2_ctrl_hevc_sps * const sps =
		rpivid_find_control_data(ctx, V4L2_CID_STATELESS_HEVC_SPS);
	u32 pixelformat;
	int i;

	for (i = 0; (pixelformat = pixelformat_from_sps(sps, i)) != 0; i++) {
		if (f->fmt.pix_mp.pixelformat == pixelformat)
			break;
	}

	// We don't have any way of finding out colourspace so believe
	// anything we are told - take anything set in src as a default
	if (f->fmt.pix_mp.colorspace == V4L2_COLORSPACE_DEFAULT)
		copy_color(&f->fmt.pix_mp, &ctx->src_fmt);

	f->fmt.pix_mp.pixelformat = pixelformat;
	rpivid_prepare_dst_format(&f->fmt.pix_mp);
	return 0;
}

static int rpivid_try_fmt_vid_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	rpivid_prepare_src_format(&f->fmt.pix_mp);
	return 0;
}

static int rpivid_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rpivid_ctx *ctx = rpivid_file2ctx(file);
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ret = rpivid_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	ctx->dst_fmt = f->fmt.pix_mp;
	ctx->dst_fmt_set = 1;

	return 0;
}

static int rpivid_s_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rpivid_ctx *ctx = rpivid_file2ctx(file);
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ret = rpivid_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	ctx->src_fmt = f->fmt.pix_mp;
	ctx->dst_fmt_set = 0;  // Setting src invalidates dst

	vq->subsystem_flags |=
		VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF;

	/* Propagate colorspace information to capture. */
	copy_color(&ctx->dst_fmt, &f->fmt.pix_mp);
	return 0;
}

const struct v4l2_ioctl_ops rpivid_ioctl_ops = {
	.vidioc_querycap		= rpivid_querycap,

	.vidioc_enum_fmt_vid_cap	= rpivid_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap_mplane	= rpivid_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap_mplane	= rpivid_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap_mplane	= rpivid_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out	= rpivid_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out_mplane	= rpivid_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out_mplane	= rpivid_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out_mplane	= rpivid_s_fmt_vid_out,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_try_decoder_cmd		= v4l2_m2m_ioctl_stateless_try_decoder_cmd,
	.vidioc_decoder_cmd		= v4l2_m2m_ioctl_stateless_decoder_cmd,

	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static int rpivid_queue_setup(struct vb2_queue *vq, unsigned int *nbufs,
			      unsigned int *nplanes, unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct rpivid_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *pix_fmt;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->src_fmt;
	else
		pix_fmt = get_dst_fmt(ctx);

	if (*nplanes) {
		if (sizes[0] < pix_fmt->plane_fmt[0].sizeimage)
			return -EINVAL;
	} else {
		sizes[0] = pix_fmt->plane_fmt[0].sizeimage;
		*nplanes = 1;
	}

	return 0;
}

static void rpivid_queue_cleanup(struct vb2_queue *vq, u32 state)
{
	struct rpivid_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		if (!vbuf)
			return;

		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req,
					   &ctx->hdl);
		v4l2_m2m_buf_done(vbuf, state);
	}
}

static int rpivid_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

static int rpivid_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct rpivid_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *pix_fmt;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->src_fmt;
	else
		pix_fmt = &ctx->dst_fmt;

	if (vb2_plane_size(vb, 0) < pix_fmt->plane_fmt[0].sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, pix_fmt->plane_fmt[0].sizeimage);

	return 0;
}

/* Only stops the clock if streaom off on both output & capture */
static void stop_clock(struct rpivid_dev *dev, struct rpivid_ctx *ctx)
{
	if (ctx->src_stream_on ||
	    ctx->dst_stream_on)
		return;

	clk_set_min_rate(dev->clock, 0);
	clk_disable_unprepare(dev->clock);
}

/* Always starts the clock if it isn't already on this ctx */
static int start_clock(struct rpivid_dev *dev, struct rpivid_ctx *ctx)
{
	int rv;

	rv = clk_set_min_rate(dev->clock, dev->max_clock_rate);
	if (rv) {
		dev_err(dev->dev, "Failed to set clock rate\n");
		return rv;
	}

	rv = clk_prepare_enable(dev->clock);
	if (rv) {
		dev_err(dev->dev, "Failed to enable clock\n");
		return rv;
	}

	return 0;
}

static int rpivid_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct rpivid_ctx *ctx = vb2_get_drv_priv(vq);
	struct rpivid_dev *dev = ctx->dev;
	int ret = 0;

	if (!V4L2_TYPE_IS_OUTPUT(vq->type)) {
		ctx->dst_stream_on = 1;
		goto ok;
	}

	if (ctx->src_fmt.pixelformat != V4L2_PIX_FMT_HEVC_SLICE) {
		ret = -EINVAL;
		goto fail_cleanup;
	}

	if (ctx->src_stream_on)
		goto ok;

	ret = start_clock(dev, ctx);
	if (ret)
		goto fail_cleanup;

	if (dev->dec_ops->start)
		ret = dev->dec_ops->start(ctx);
	if (ret)
		goto fail_stop_clock;

	ctx->src_stream_on = 1;
ok:
	return 0;

fail_stop_clock:
	stop_clock(dev, ctx);
fail_cleanup:
	v4l2_err(&dev->v4l2_dev, "%s: qtype=%d: FAIL\n", __func__, vq->type);
	rpivid_queue_cleanup(vq, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void rpivid_stop_streaming(struct vb2_queue *vq)
{
	struct rpivid_ctx *ctx = vb2_get_drv_priv(vq);
	struct rpivid_dev *dev = ctx->dev;

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		ctx->src_stream_on = 0;
		if (dev->dec_ops->stop)
			dev->dec_ops->stop(ctx);
	} else {
		ctx->dst_stream_on = 0;
	}

	rpivid_queue_cleanup(vq, VB2_BUF_STATE_ERROR);

	vb2_wait_for_all_buffers(vq);

	stop_clock(dev, ctx);
}

static void rpivid_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rpivid_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void rpivid_buf_request_complete(struct vb2_buffer *vb)
{
	struct rpivid_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->hdl);
}

static struct vb2_ops rpivid_qops = {
	.queue_setup		= rpivid_queue_setup,
	.buf_prepare		= rpivid_buf_prepare,
	.buf_queue		= rpivid_buf_queue,
	.buf_out_validate	= rpivid_buf_out_validate,
	.buf_request_complete	= rpivid_buf_request_complete,
	.start_streaming	= rpivid_start_streaming,
	.stop_streaming		= rpivid_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

int rpivid_queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq)
{
	struct rpivid_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct rpivid_buffer);
	src_vq->ops = &rpivid_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->ctx_mutex;
	src_vq->dev = ctx->dev->dev;
	src_vq->supports_requests = true;
	src_vq->requires_requests = true;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct rpivid_buffer);
	dst_vq->min_buffers_needed = 1;
	dst_vq->ops = &rpivid_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->ctx_mutex;
	dst_vq->dev = ctx->dev->dev;

	return vb2_queue_init(dst_vq);
}
