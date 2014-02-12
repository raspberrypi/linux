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
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>
#include <media/v4l2-common.h>
#include <linux/delay.h>

#include "mmal-common.h"
#include "mmal-encodings.h"
#include "mmal-vchiq.h"
#include "mmal-msg.h"
#include "mmal-parameters.h"
#include "bcm2835-camera.h"

#define BM2835_MMAL_VERSION "0.0.2"
#define BM2835_MMAL_MODULE_NAME "bcm2835-v4l2"
#define MIN_WIDTH 16
#define MIN_HEIGHT 16
#define MAX_WIDTH 2592
#define MAX_HEIGHT 1944
#define MIN_BUFFER_SIZE (80*1024)

#define MAX_VIDEO_MODE_WIDTH 1280
#define MAX_VIDEO_MODE_HEIGHT 720

MODULE_DESCRIPTION("Broadcom 2835 MMAL video capture");
MODULE_AUTHOR("Vincent Sanders");
MODULE_LICENSE("GPL");
MODULE_VERSION(BM2835_MMAL_VERSION);

int bcm2835_v4l2_debug;
module_param_named(debug, bcm2835_v4l2_debug, int, 0644);
MODULE_PARM_DESC(bcm2835_v4l2_debug, "Debug level 0-2");

static struct bm2835_mmal_dev *gdev;	/* global device data */

#define FPS_MIN 1
#define FPS_MAX 90

/* timeperframe: min/max and default */
static const struct v4l2_fract
	tpf_min     = {.numerator = 1,		.denominator = FPS_MAX},
	tpf_max     = {.numerator = 1,	        .denominator = FPS_MIN},
	tpf_default = {.numerator = 1000,	.denominator = 30000};

/* video formats */
static struct mmal_fmt formats[] = {
	{
	 .name = "4:2:0, packed YUV",
	 .fourcc = V4L2_PIX_FMT_YUV420,
	 .flags = 0,
	 .mmal = MMAL_ENCODING_I420,
	 .depth = 12,
	 .mmal_component = MMAL_COMPONENT_CAMERA,
	 },
	{
	 .name = "4:2:2, packed, YUYV",
	 .fourcc = V4L2_PIX_FMT_YUYV,
	 .flags = 0,
	 .mmal = MMAL_ENCODING_YUYV,
	 .depth = 16,
	 .mmal_component = MMAL_COMPONENT_CAMERA,
	 },
	{
	 .name = "RGB24 (LE)",
	 .fourcc = V4L2_PIX_FMT_RGB24,
	 .flags = 0,
	 .mmal = MMAL_ENCODING_BGR24,
	 .depth = 24,
	 .mmal_component = MMAL_COMPONENT_CAMERA,
	 },
	{
	 .name = "JPEG",
	 .fourcc = V4L2_PIX_FMT_JPEG,
	 .flags = V4L2_FMT_FLAG_COMPRESSED,
	 .mmal = MMAL_ENCODING_JPEG,
	 .depth = 8,
	 .mmal_component = MMAL_COMPONENT_IMAGE_ENCODE,
	 },
	{
	 .name = "H264",
	 .fourcc = V4L2_PIX_FMT_H264,
	 .flags = V4L2_FMT_FLAG_COMPRESSED,
	 .mmal = MMAL_ENCODING_H264,
	 .depth = 8,
	 .mmal_component = MMAL_COMPONENT_VIDEO_ENCODE,
	 },
	{
	 .name = "MJPEG",
	 .fourcc = V4L2_PIX_FMT_MJPEG,
	 .flags = V4L2_FMT_FLAG_COMPRESSED,
	 .mmal = MMAL_ENCODING_MJPEG,
	 .depth = 8,
	 .mmal_component = MMAL_COMPONENT_VIDEO_ENCODE,
	 }
};

static struct mmal_fmt *get_format(struct v4l2_format *f)
{
	struct mmal_fmt *fmt;
	unsigned int k;

	for (k = 0; k < ARRAY_SIZE(formats); k++) {
		fmt = &formats[k];
		if (fmt->fourcc == f->fmt.pix.pixelformat)
			break;
	}

	if (k == ARRAY_SIZE(formats))
		return NULL;

	return &formats[k];
}

/* ------------------------------------------------------------------
	Videobuf queue operations
   ------------------------------------------------------------------*/

static int queue_setup(struct vb2_queue *vq, const struct v4l2_format *fmt,
		       unsigned int *nbuffers, unsigned int *nplanes,
		       unsigned int sizes[], void *alloc_ctxs[])
{
	struct bm2835_mmal_dev *dev = vb2_get_drv_priv(vq);
	unsigned long size;

	/* refuse queue setup if port is not configured */
	if (dev->capture.port == NULL) {
		v4l2_err(&dev->v4l2_dev,
			 "%s: capture port not configured\n", __func__);
		return -EINVAL;
	}

	size = dev->capture.port->current_buffer.size;
	if (size == 0) {
		v4l2_err(&dev->v4l2_dev,
			 "%s: capture port buffer size is zero\n", __func__);
		return -EINVAL;
	}

	if (*nbuffers < (dev->capture.port->current_buffer.num + 2))
		*nbuffers = (dev->capture.port->current_buffer.num + 2);

	*nplanes = 1;

	sizes[0] = size;

	/*
	 * videobuf2-vmalloc allocator is context-less so no need to set
	 * alloc_ctxs array.
	 */

	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev, "%s: dev:%p\n",
		 __func__, dev);

	return 0;
}

static int buffer_prepare(struct vb2_buffer *vb)
{
	struct bm2835_mmal_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long size;

	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev, "%s: dev:%p\n",
		 __func__, dev);

	BUG_ON(dev->capture.port == NULL);
	BUG_ON(dev->capture.fmt == NULL);

	size = dev->capture.stride * dev->capture.height;
	if (vb2_plane_size(vb, 0) < size) {
		v4l2_err(&dev->v4l2_dev,
			 "%s data will not fit into plane (%lu < %lu)\n",
			 __func__, vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	return 0;
}

static inline bool is_capturing(struct bm2835_mmal_dev *dev)
{
	return dev->capture.camera_port ==
	    &dev->
	    component[MMAL_COMPONENT_CAMERA]->output[MMAL_CAMERA_PORT_CAPTURE];
}

static void buffer_cb(struct vchiq_mmal_instance *instance,
		      struct vchiq_mmal_port *port,
		      int status,
		      struct mmal_buffer *buf,
		      unsigned long length, u32 mmal_flags, s64 dts, s64 pts)
{
	struct bm2835_mmal_dev *dev = port->cb_ctx;

	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
		 "%s: status:%d, buf:%p, length:%lu, flags %u, pts %lld\n",
		 __func__, status, buf, length, mmal_flags, pts);

	if (status != 0) {
		/* error in transfer */
		if (buf != NULL) {
			/* there was a buffer with the error so return it */
			vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
		}
		return;
	} else if (length == 0) {
		/* stream ended */
		if (buf != NULL) {
			/* this should only ever happen if the port is
			 * disabled and there are buffers still queued
			 */
			vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
			pr_debug("Empty buffer");
		} else if (dev->capture.frame_count) {
			/* grab another frame */
			if (is_capturing(dev)) {
				pr_debug("Grab another frame");
				vchiq_mmal_port_parameter_set(
					instance,
					dev->capture.
					camera_port,
					MMAL_PARAMETER_CAPTURE,
					&dev->capture.
					frame_count,
					sizeof(dev->capture.frame_count));
			}
		} else {
			/* signal frame completion */
			complete(&dev->capture.frame_cmplt);
		}
	} else {
		if (dev->capture.frame_count) {
			if (dev->capture.vc_start_timestamp != -1 &&
			    pts != 0) {
				s64 runtime_us = pts -
				    dev->capture.vc_start_timestamp;
				u32 div = 0;
				u32 rem = 0;

				div =
				    div_u64_rem(runtime_us, USEC_PER_SEC, &rem);
				buf->vb.v4l2_buf.timestamp.tv_sec =
				    dev->capture.kernel_start_ts.tv_sec - 1 +
				    div;
				buf->vb.v4l2_buf.timestamp.tv_usec =
				    dev->capture.kernel_start_ts.tv_usec + rem;

				if (buf->vb.v4l2_buf.timestamp.tv_usec >=
				    USEC_PER_SEC) {
					buf->vb.v4l2_buf.timestamp.tv_sec++;
					buf->vb.v4l2_buf.timestamp.tv_usec -=
					    USEC_PER_SEC;
				}
				v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
					 "Convert start time %d.%06d and %llu "
					 "with offset %llu to %d.%06d\n",
					 (int)dev->capture.kernel_start_ts.
					 tv_sec,
					 (int)dev->capture.kernel_start_ts.
					 tv_usec,
					 dev->capture.vc_start_timestamp, pts,
					 (int)buf->vb.v4l2_buf.timestamp.tv_sec,
					 (int)buf->vb.v4l2_buf.timestamp.
					 tv_usec);
			} else {
				v4l2_get_timestamp(&buf->vb.v4l2_buf.timestamp);
			}

			vb2_set_plane_payload(&buf->vb, 0, length);
			vb2_buffer_done(&buf->vb, VB2_BUF_STATE_DONE);

			if (mmal_flags & MMAL_BUFFER_HEADER_FLAG_EOS &&
			    is_capturing(dev)) {
				v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
					 "Grab another frame as buffer has EOS");
				vchiq_mmal_port_parameter_set(
					instance,
					dev->capture.
					camera_port,
					MMAL_PARAMETER_CAPTURE,
					&dev->capture.
					frame_count,
					sizeof(dev->capture.frame_count));
			}
		} else {
			/* signal frame completion */
			complete(&dev->capture.frame_cmplt);
		}
	}
}

static int enable_camera(struct bm2835_mmal_dev *dev)
{
	int ret;
	if (!dev->camera_use_count) {
		ret = vchiq_mmal_component_enable(
				dev->instance,
				dev->component[MMAL_COMPONENT_CAMERA]);
		if (ret < 0) {
			v4l2_err(&dev->v4l2_dev,
				 "Failed enabling camera, ret %d\n", ret);
			return -EINVAL;
		}
	}
	dev->camera_use_count++;
	v4l2_dbg(1, bcm2835_v4l2_debug,
		 &dev->v4l2_dev, "enabled camera (refcount %d)\n",
			dev->camera_use_count);
	return 0;
}

static int disable_camera(struct bm2835_mmal_dev *dev)
{
	int ret;
	if (!dev->camera_use_count) {
		v4l2_err(&dev->v4l2_dev,
			 "Disabled the camera when already disabled\n");
		return -EINVAL;
	}
	dev->camera_use_count--;
	if (!dev->camera_use_count) {
		unsigned int i = 0xFFFFFFFF;
		v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
			 "Disabling camera\n");
		ret =
		    vchiq_mmal_component_disable(
				dev->instance,
				dev->component[MMAL_COMPONENT_CAMERA]);
		if (ret < 0) {
			v4l2_err(&dev->v4l2_dev,
				 "Failed disabling camera, ret %d\n", ret);
			return -EINVAL;
		}
		vchiq_mmal_port_parameter_set(
			dev->instance,
			&dev->component[MMAL_COMPONENT_CAMERA]->control,
			MMAL_PARAMETER_CAMERA_NUM, &i,
			sizeof(i));
	}
	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
		 "Camera refcount now %d\n", dev->camera_use_count);
	return 0;
}

static void buffer_queue(struct vb2_buffer *vb)
{
	struct bm2835_mmal_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct mmal_buffer *buf = container_of(vb, struct mmal_buffer, vb);
	int ret;

	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
		 "%s: dev:%p buf:%p\n", __func__, dev, buf);

	buf->buffer = vb2_plane_vaddr(&buf->vb, 0);
	buf->buffer_size = vb2_plane_size(&buf->vb, 0);

	ret = vchiq_mmal_submit_buffer(dev->instance, dev->capture.port, buf);
	if (ret < 0)
		v4l2_err(&dev->v4l2_dev, "%s: error submitting buffer\n",
			 __func__);
}

static int start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct bm2835_mmal_dev *dev = vb2_get_drv_priv(vq);
	int ret;
	int parameter_size;

	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev, "%s: dev:%p\n",
		 __func__, dev);

	/* ensure a format has actually been set */
	if (dev->capture.port == NULL)
		return -EINVAL;

	if (enable_camera(dev) < 0) {
		v4l2_err(&dev->v4l2_dev, "Failed to enable camera\n");
		return -EINVAL;
	}

	/*init_completion(&dev->capture.frame_cmplt); */

	/* enable frame capture */
	dev->capture.frame_count = 1;

	/* if the preview is not already running, wait for a few frames for AGC
	 * to settle down.
	 */
	if (!dev->component[MMAL_COMPONENT_PREVIEW]->enabled)
		msleep(300);

	/* enable the connection from camera to encoder (if applicable) */
	if (dev->capture.camera_port != dev->capture.port
	    && dev->capture.camera_port) {
		ret = vchiq_mmal_port_enable(dev->instance,
					     dev->capture.camera_port, NULL);
		if (ret) {
			v4l2_err(&dev->v4l2_dev,
				 "Failed to enable encode tunnel - error %d\n",
				 ret);
			return -1;
		}
	}

	/* Get VC timestamp at this point in time */
	parameter_size = sizeof(dev->capture.vc_start_timestamp);
	if (vchiq_mmal_port_parameter_get(dev->instance,
					  dev->capture.camera_port,
					  MMAL_PARAMETER_SYSTEM_TIME,
					  &dev->capture.vc_start_timestamp,
					  &parameter_size)) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to get VC start time - update your VC f/w\n");

		/* Flag to indicate just to rely on kernel timestamps */
		dev->capture.vc_start_timestamp = -1;
	} else
		v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
			 "Start time %lld size %d\n",
			 dev->capture.vc_start_timestamp, parameter_size);

	v4l2_get_timestamp(&dev->capture.kernel_start_ts);

	/* enable the camera port */
	dev->capture.port->cb_ctx = dev;
	ret =
	    vchiq_mmal_port_enable(dev->instance, dev->capture.port, buffer_cb);
	if (ret) {
		v4l2_err(&dev->v4l2_dev,
			"Failed to enable capture port - error %d. "
			"Disabling camera port again\n", ret);

		vchiq_mmal_port_disable(dev->instance,
					dev->capture.camera_port);
		if (disable_camera(dev) < 0) {
			v4l2_err(&dev->v4l2_dev, "Failed to disable camera");
			return -EINVAL;
		}
		return -1;
	}

	/* capture the first frame */
	vchiq_mmal_port_parameter_set(dev->instance,
				      dev->capture.camera_port,
				      MMAL_PARAMETER_CAPTURE,
				      &dev->capture.frame_count,
				      sizeof(dev->capture.frame_count));
	return 0;
}

/* abort streaming and wait for last buffer */
static int stop_streaming(struct vb2_queue *vq)
{
	int ret;
	struct bm2835_mmal_dev *dev = vb2_get_drv_priv(vq);

	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev, "%s: dev:%p\n",
		 __func__, dev);

	init_completion(&dev->capture.frame_cmplt);
	dev->capture.frame_count = 0;

	/* ensure a format has actually been set */
	if (dev->capture.port == NULL)
		return -EINVAL;

	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev, "stopping capturing\n");

	/* stop capturing frames */
	vchiq_mmal_port_parameter_set(dev->instance,
				      dev->capture.camera_port,
				      MMAL_PARAMETER_CAPTURE,
				      &dev->capture.frame_count,
				      sizeof(dev->capture.frame_count));

	/* wait for last frame to complete */
	ret = wait_for_completion_timeout(&dev->capture.frame_cmplt, HZ);
	if (ret <= 0)
		v4l2_err(&dev->v4l2_dev,
			 "error %d waiting for frame completion\n", ret);

	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
		 "disabling connection\n");

	/* disable the connection from camera to encoder */
	ret = vchiq_mmal_port_disable(dev->instance, dev->capture.camera_port);
	if (!ret && dev->capture.camera_port != dev->capture.port) {
		v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
			 "disabling port\n");
		ret = vchiq_mmal_port_disable(dev->instance, dev->capture.port);
	} else if (dev->capture.camera_port != dev->capture.port) {
		v4l2_err(&dev->v4l2_dev, "port_disable failed, error %d\n",
			 ret);
	}

	if (disable_camera(dev) < 0) {
		v4l2_err(&dev->v4l2_dev, "Failed to disable camera");
		return -EINVAL;
	}

	return ret;
}

static void bm2835_mmal_lock(struct vb2_queue *vq)
{
	struct bm2835_mmal_dev *dev = vb2_get_drv_priv(vq);
	mutex_lock(&dev->mutex);
}

static void bm2835_mmal_unlock(struct vb2_queue *vq)
{
	struct bm2835_mmal_dev *dev = vb2_get_drv_priv(vq);
	mutex_unlock(&dev->mutex);
}

static struct vb2_ops bm2835_mmal_video_qops = {
	.queue_setup = queue_setup,
	.buf_prepare = buffer_prepare,
	.buf_queue = buffer_queue,
	.start_streaming = start_streaming,
	.stop_streaming = stop_streaming,
	.wait_prepare = bm2835_mmal_unlock,
	.wait_finish = bm2835_mmal_lock,
};

/* ------------------------------------------------------------------
	IOCTL operations
   ------------------------------------------------------------------*/

/* overlay ioctl */
static int vidioc_enum_fmt_vid_overlay(struct file *file, void *priv,
				       struct v4l2_fmtdesc *f)
{
	struct mmal_fmt *fmt;

	if (f->index >= ARRAY_SIZE(formats))
		return -EINVAL;

	fmt = &formats[f->index];

	strlcpy(f->description, fmt->name, sizeof(f->description));
	f->pixelformat = fmt->fourcc;
	f->flags = fmt->flags;

	return 0;
}

static int vidioc_g_fmt_vid_overlay(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct bm2835_mmal_dev *dev = video_drvdata(file);

	f->fmt.win = dev->overlay;

	return 0;
}

static int vidioc_try_fmt_vid_overlay(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	/* Only support one format so get the current one. */
	vidioc_g_fmt_vid_overlay(file, priv, f);

	/* todo: allow the size and/or offset to be changed. */
	return 0;
}

static int vidioc_s_fmt_vid_overlay(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct bm2835_mmal_dev *dev = video_drvdata(file);

	vidioc_try_fmt_vid_overlay(file, priv, f);

	dev->overlay = f->fmt.win;

	/* todo: program the preview port parameters */
	return 0;
}

static int vidioc_overlay(struct file *file, void *f, unsigned int on)
{
	int ret;
	struct bm2835_mmal_dev *dev = video_drvdata(file);
	struct vchiq_mmal_port *src;
	struct vchiq_mmal_port *dst;
	struct mmal_parameter_displayregion prev_config = {
		.set = MMAL_DISPLAY_SET_LAYER | MMAL_DISPLAY_SET_ALPHA |
		    MMAL_DISPLAY_SET_DEST_RECT | MMAL_DISPLAY_SET_FULLSCREEN,
		.layer = PREVIEW_LAYER,
		.alpha = 255,
		.fullscreen = 0,
		.dest_rect = {
			      .x = dev->overlay.w.left,
			      .y = dev->overlay.w.top,
			      .width = dev->overlay.w.width,
			      .height = dev->overlay.w.height,
			      },
	};

	if ((on && dev->component[MMAL_COMPONENT_PREVIEW]->enabled) ||
	    (!on && !dev->component[MMAL_COMPONENT_PREVIEW]->enabled))
		return 0;	/* already in requested state */

	src =
	    &dev->component[MMAL_COMPONENT_CAMERA]->
	    output[MMAL_CAMERA_PORT_PREVIEW];

	if (!on) {
		/* disconnect preview ports and disable component */
		ret = vchiq_mmal_port_disable(dev->instance, src);
		if (!ret)
			ret =
			    vchiq_mmal_port_connect_tunnel(dev->instance, src,
							   NULL);
		if (ret >= 0)
			ret = vchiq_mmal_component_disable(
					dev->instance,
					dev->component[MMAL_COMPONENT_PREVIEW]);

		disable_camera(dev);
		return ret;
	}

	/* set preview port format and connect it to output */
	dst = &dev->component[MMAL_COMPONENT_PREVIEW]->input[0];

	ret = vchiq_mmal_port_set_format(dev->instance, src);
	if (ret < 0)
		goto error;

	ret = vchiq_mmal_port_parameter_set(dev->instance, dst,
					    MMAL_PARAMETER_DISPLAYREGION,
					    &prev_config, sizeof(prev_config));
	if (ret < 0)
		goto error;

	if (enable_camera(dev) < 0)
		goto error;

	ret = vchiq_mmal_component_enable(
			dev->instance,
			dev->component[MMAL_COMPONENT_PREVIEW]);
	if (ret < 0)
		goto error;

	v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev, "connecting %p to %p\n",
		 src, dst);
	ret = vchiq_mmal_port_connect_tunnel(dev->instance, src, dst);
	if (!ret)
		ret = vchiq_mmal_port_enable(dev->instance, src, NULL);
error:
	return ret;
}

static int vidioc_g_fbuf(struct file *file, void *fh,
			 struct v4l2_framebuffer *a)
{
	/* The video overlay must stay within the framebuffer and can't be
	   positioned independently. */
	struct bm2835_mmal_dev *dev = video_drvdata(file);
	struct vchiq_mmal_port *preview_port =
		    &dev->component[MMAL_COMPONENT_CAMERA]->
		    output[MMAL_CAMERA_PORT_PREVIEW];
	a->flags = V4L2_FBUF_FLAG_OVERLAY;
	a->fmt.width = preview_port->es.video.width;
	a->fmt.height = preview_port->es.video.height;
	a->fmt.pixelformat = V4L2_PIX_FMT_YUV420;
	a->fmt.bytesperline = (preview_port->es.video.width * 3)>>1;
	a->fmt.sizeimage = (preview_port->es.video.width *
			       preview_port->es.video.height * 3)>>1;
	a->fmt.colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

/* input ioctls */
static int vidioc_enum_input(struct file *file, void *priv,
			     struct v4l2_input *inp)
{
	/* only a single camera input */
	if (inp->index != 0)
		return -EINVAL;

	inp->type = V4L2_INPUT_TYPE_CAMERA;
	sprintf(inp->name, "Camera %u", inp->index);
	return 0;
}

static int vidioc_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int vidioc_s_input(struct file *file, void *priv, unsigned int i)
{
	if (i != 0)
		return -EINVAL;

	return 0;
}

/* capture ioctls */
static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct bm2835_mmal_dev *dev = video_drvdata(file);
	u32 major;
	u32 minor;

	vchiq_mmal_version(dev->instance, &major, &minor);

	strcpy(cap->driver, "bm2835 mmal");
	snprintf(cap->card, sizeof(cap->card), "mmal service %d.%d",
		 major, minor);

	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", dev->v4l2_dev.name);
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OVERLAY |
	    V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct mmal_fmt *fmt;

	if (f->index >= ARRAY_SIZE(formats))
		return -EINVAL;

	fmt = &formats[f->index];

	strlcpy(f->description, fmt->name, sizeof(f->description));
	f->pixelformat = fmt->fourcc;
	f->flags = fmt->flags;

	return 0;
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct bm2835_mmal_dev *dev = video_drvdata(file);

	f->fmt.pix.width = dev->capture.width;
	f->fmt.pix.height = dev->capture.height;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.pixelformat = dev->capture.fmt->fourcc;
	f->fmt.pix.bytesperline =
	    (f->fmt.pix.width * dev->capture.fmt->depth) >> 3;
	f->fmt.pix.sizeimage = f->fmt.pix.height * f->fmt.pix.bytesperline;
	if (dev->capture.fmt->fourcc == V4L2_PIX_FMT_JPEG
	    && f->fmt.pix.sizeimage < (100 << 10)) {
		/* Need a minimum size for JPEG to account for EXIF. */
		f->fmt.pix.sizeimage = (100 << 10);
	}

	if (dev->capture.fmt->fourcc == V4L2_PIX_FMT_YUYV ||
	    dev->capture.fmt->fourcc == V4L2_PIX_FMT_UYVY)
		f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
	else
		f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	f->fmt.pix.priv = 0;

	v4l2_dump_pix_format(1, bcm2835_v4l2_debug, &dev->v4l2_dev, &f->fmt.pix,
			     __func__);
	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct bm2835_mmal_dev *dev = video_drvdata(file);
	struct mmal_fmt *mfmt;

	mfmt = get_format(f);
	if (!mfmt) {
		v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
			 "Fourcc format (0x%08x) unknown.\n",
			 f->fmt.pix.pixelformat);
		f->fmt.pix.pixelformat = formats[0].fourcc;
		mfmt = get_format(f);
	}

	f->fmt.pix.field = V4L2_FIELD_NONE;
	/* image must be a multiple of 32 pixels wide and 16 lines high */
	v4l_bound_align_image(&f->fmt.pix.width, 48, MAX_WIDTH, 5,
			      &f->fmt.pix.height, 32, MAX_HEIGHT, 4, 0);
	f->fmt.pix.bytesperline = (f->fmt.pix.width * mfmt->depth) >> 3;
	f->fmt.pix.sizeimage = f->fmt.pix.height * f->fmt.pix.bytesperline;
	if (f->fmt.pix.sizeimage < MIN_BUFFER_SIZE)
		f->fmt.pix.sizeimage = MIN_BUFFER_SIZE;

	if (mfmt->fourcc == V4L2_PIX_FMT_YUYV ||
	    mfmt->fourcc == V4L2_PIX_FMT_UYVY)
		f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
	else
		f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	f->fmt.pix.priv = 0;

	v4l2_dump_pix_format(1, bcm2835_v4l2_debug, &dev->v4l2_dev, &f->fmt.pix,
			     __func__);
	return 0;
}

static int mmal_setup_components(struct bm2835_mmal_dev *dev,
				 struct v4l2_format *f)
{
	int ret;
	struct vchiq_mmal_port *port = NULL, *camera_port = NULL;
	struct vchiq_mmal_component *encode_component = NULL;
	struct mmal_fmt *mfmt = get_format(f);

	BUG_ON(!mfmt);

	if (dev->capture.encode_component) {
		v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
			 "vid_cap - disconnect previous tunnel\n");

		/* Disconnect any previous connection */
		vchiq_mmal_port_connect_tunnel(dev->instance,
					       dev->capture.camera_port, NULL);
		dev->capture.camera_port = NULL;
		ret = vchiq_mmal_component_disable(dev->instance,
						   dev->capture.
						   encode_component);
		if (ret)
			v4l2_err(&dev->v4l2_dev,
				 "Failed to disable encode component %d\n",
				 ret);

		dev->capture.encode_component = NULL;
	}
	/* format dependant port setup */
	switch (mfmt->mmal_component) {
	case MMAL_COMPONENT_CAMERA:
		/* Make a further decision on port based on resolution */
		if (f->fmt.pix.width <= MAX_VIDEO_MODE_WIDTH
		    && f->fmt.pix.height <= MAX_VIDEO_MODE_HEIGHT)
			camera_port = port =
			    &dev->component[MMAL_COMPONENT_CAMERA]->
			    output[MMAL_CAMERA_PORT_VIDEO];
		else
			camera_port = port =
			    &dev->component[MMAL_COMPONENT_CAMERA]->
			    output[MMAL_CAMERA_PORT_CAPTURE];
		break;
	case MMAL_COMPONENT_IMAGE_ENCODE:
		encode_component = dev->component[MMAL_COMPONENT_IMAGE_ENCODE];
		port = &dev->component[MMAL_COMPONENT_IMAGE_ENCODE]->output[0];
		camera_port =
		    &dev->component[MMAL_COMPONENT_CAMERA]->
		    output[MMAL_CAMERA_PORT_CAPTURE];
		break;
	case MMAL_COMPONENT_VIDEO_ENCODE:
		encode_component = dev->component[MMAL_COMPONENT_VIDEO_ENCODE];
		port = &dev->component[MMAL_COMPONENT_VIDEO_ENCODE]->output[0];
		camera_port =
		    &dev->component[MMAL_COMPONENT_CAMERA]->
		    output[MMAL_CAMERA_PORT_VIDEO];
		break;
	default:
		break;
	}

	if (!port)
		return -EINVAL;

	if (encode_component)
		camera_port->format.encoding = MMAL_ENCODING_OPAQUE;
	else
		camera_port->format.encoding = mfmt->mmal;

	camera_port->format.encoding_variant = 0;
	camera_port->es.video.width = f->fmt.pix.width;
	camera_port->es.video.height = f->fmt.pix.height;
	camera_port->es.video.crop.x = 0;
	camera_port->es.video.crop.y = 0;
	camera_port->es.video.crop.width = f->fmt.pix.width;
	camera_port->es.video.crop.height = f->fmt.pix.height;
	camera_port->es.video.frame_rate.num = 0;
	camera_port->es.video.frame_rate.den = 1;

	ret = vchiq_mmal_port_set_format(dev->instance, camera_port);

	if (!ret
	    && camera_port ==
	    &dev->component[MMAL_COMPONENT_CAMERA]->
	    output[MMAL_CAMERA_PORT_VIDEO]) {
		bool overlay_enabled =
		    !!dev->component[MMAL_COMPONENT_PREVIEW]->enabled;
		struct vchiq_mmal_port *preview_port =
		    &dev->component[MMAL_COMPONENT_CAMERA]->
		    output[MMAL_CAMERA_PORT_PREVIEW];
		/* Preview and encode ports need to match on resolution */
		if (overlay_enabled) {
			/* Need to disable the overlay before we can update
			 * the resolution
			 */
			ret =
			    vchiq_mmal_port_disable(dev->instance,
						    preview_port);
			if (!ret)
				ret =
				    vchiq_mmal_port_connect_tunnel(
						dev->instance,
						preview_port,
						NULL);
		}
		preview_port->es.video.width = f->fmt.pix.width;
		preview_port->es.video.height = f->fmt.pix.height;
		preview_port->es.video.crop.x = 0;
		preview_port->es.video.crop.y = 0;
		preview_port->es.video.crop.width = f->fmt.pix.width;
		preview_port->es.video.crop.height = f->fmt.pix.height;
		preview_port->es.video.frame_rate.num =
					  dev->capture.timeperframe.denominator;
		preview_port->es.video.frame_rate.den =
					  dev->capture.timeperframe.numerator;
		ret = vchiq_mmal_port_set_format(dev->instance, preview_port);
		if (overlay_enabled) {
			ret = vchiq_mmal_port_connect_tunnel(
				dev->instance,
				preview_port,
				&dev->component[MMAL_COMPONENT_PREVIEW]->input[0]);
			if (!ret)
				ret = vchiq_mmal_port_enable(dev->instance,
							     preview_port,
							     NULL);
		}
	}

	if (ret) {
		v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
			 "%s failed to set format\n", __func__);
		/* ensure capture is not going to be tried */
		dev->capture.port = NULL;
	} else {
		if (encode_component) {
			v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
				 "vid_cap - set up encode comp\n");

			/* configure buffering */
			camera_port->current_buffer.size =
			    camera_port->recommended_buffer.size;
			camera_port->current_buffer.num =
			    camera_port->recommended_buffer.num;

			ret =
			    vchiq_mmal_port_connect_tunnel(
					dev->instance,
					camera_port,
					&encode_component->input[0]);
			if (ret) {
				v4l2_dbg(1, bcm2835_v4l2_debug,
					 &dev->v4l2_dev,
					 "%s failed to create connection\n",
					 __func__);
				/* ensure capture is not going to be tried */
				dev->capture.port = NULL;
			} else {
				port->es.video.width = f->fmt.pix.width;
				port->es.video.height = f->fmt.pix.height;
				port->es.video.crop.x = 0;
				port->es.video.crop.y = 0;
				port->es.video.crop.width = f->fmt.pix.width;
				port->es.video.crop.height = f->fmt.pix.height;
				port->es.video.frame_rate.num =
					  dev->capture.timeperframe.denominator;
				port->es.video.frame_rate.den =
					  dev->capture.timeperframe.numerator;

				port->format.encoding = mfmt->mmal;
				port->format.encoding_variant = 0;
				/* Set any encoding specific parameters */
				switch (mfmt->mmal_component) {
				case MMAL_COMPONENT_VIDEO_ENCODE:
					port->format.bitrate =
					    dev->capture.encode_bitrate;
					break;
				case MMAL_COMPONENT_IMAGE_ENCODE:
					/* Could set EXIF parameters here */
					break;
				default:
					break;
				}
				ret = vchiq_mmal_port_set_format(dev->instance,
								 port);
				if (ret)
					v4l2_dbg(1, bcm2835_v4l2_debug,
						 &dev->v4l2_dev,
						 "%s failed to set format\n",
						 __func__);
			}

			if (!ret) {
				ret = vchiq_mmal_component_enable(
						dev->instance,
						encode_component);
				if (ret) {
					v4l2_dbg(1, bcm2835_v4l2_debug,
					   &dev->v4l2_dev,
					   "%s Failed to enable encode components\n",
					   __func__);
				}
			}
			if (!ret) {
				/* configure buffering */
				port->current_buffer.num = 1;
				port->current_buffer.size =
				    f->fmt.pix.sizeimage;
				if (port->format.encoding ==
				    MMAL_ENCODING_JPEG) {
					v4l2_dbg(1, bcm2835_v4l2_debug,
					    &dev->v4l2_dev,
					    "JPG - buf size now %d was %d\n",
					    f->fmt.pix.sizeimage,
					    port->current_buffer.size);
					port->current_buffer.size =
					    (f->fmt.pix.sizeimage <
					     (100 << 10))
					    ? (100 << 10) : f->fmt.pix.
					    sizeimage;
				}
				v4l2_dbg(1, bcm2835_v4l2_debug,
					 &dev->v4l2_dev,
					 "vid_cap - cur_buf.size set to %d\n",
					 f->fmt.pix.sizeimage);
				port->current_buffer.alignment = 0;
			}
		} else {
			/* configure buffering */
			camera_port->current_buffer.num = 1;
			camera_port->current_buffer.size = f->fmt.pix.sizeimage;
			camera_port->current_buffer.alignment = 0;
		}

		if (!ret) {
			dev->capture.fmt = mfmt;
			dev->capture.stride = f->fmt.pix.bytesperline;
			dev->capture.width = camera_port->es.video.crop.width;
			dev->capture.height = camera_port->es.video.crop.height;

			/* select port for capture */
			dev->capture.port = port;
			dev->capture.camera_port = camera_port;
			dev->capture.encode_component = encode_component;
			v4l2_dbg(1, bcm2835_v4l2_debug,
				 &dev->v4l2_dev,
				"Set dev->capture.fmt %08X, %dx%d, stride %d",
				port->format.encoding,
				dev->capture.width, dev->capture.height,
				dev->capture.stride);
		}
	}

	/* todo: Need to convert the vchiq/mmal error into a v4l2 error. */
	return ret;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	int ret;
	struct bm2835_mmal_dev *dev = video_drvdata(file);
	struct mmal_fmt *mfmt;

	/* try the format to set valid parameters */
	ret = vidioc_try_fmt_vid_cap(file, priv, f);
	if (ret) {
		v4l2_err(&dev->v4l2_dev,
			 "vid_cap - vidioc_try_fmt_vid_cap failed\n");
		return ret;
	}

	/* if a capture is running refuse to set format */
	if (vb2_is_busy(&dev->capture.vb_vidq)) {
		v4l2_info(&dev->v4l2_dev, "%s device busy\n", __func__);
		return -EBUSY;
	}

	/* If the format is unsupported v4l2 says we should switch to
	 * a supported one and not return an error. */
	mfmt = get_format(f);
	if (!mfmt) {
		v4l2_dbg(1, bcm2835_v4l2_debug, &dev->v4l2_dev,
			 "Fourcc format (0x%08x) unknown.\n",
			 f->fmt.pix.pixelformat);
		f->fmt.pix.pixelformat = formats[0].fourcc;
		mfmt = get_format(f);
	}

	ret = mmal_setup_components(dev, f);
	if (ret != 0) {
		v4l2_err(&dev->v4l2_dev,
			 "%s: failed to setup mmal components: %d\n",
			 __func__, ret);
		ret = -EINVAL;
	}

	return ret;
}

int vidioc_enum_framesizes(struct file *file, void *fh,
			   struct v4l2_frmsizeenum *fsize)
{
	static const struct v4l2_frmsize_stepwise sizes = {
		MIN_WIDTH, MAX_WIDTH, 2,
		MIN_HEIGHT, MAX_HEIGHT, 2
	};
	int i;

	if (fsize->index)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(formats); i++)
		if (formats[i].fourcc == fsize->pixel_format)
			break;
	if (i == ARRAY_SIZE(formats))
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = sizes;
	return 0;
}

/* timeperframe is arbitrary and continous */
static int vidioc_enum_frameintervals(struct file *file, void *priv,
					     struct v4l2_frmivalenum *fival)
{
	int i;

	if (fival->index)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(formats); i++)
		if (formats[i].fourcc == fival->pixel_format)
			break;
	if (i == ARRAY_SIZE(formats))
		return -EINVAL;

	/* regarding width & height - we support any within range */
	if (fival->width < MIN_WIDTH || fival->width > MAX_WIDTH ||
	    fival->height < MIN_HEIGHT || fival->height > MAX_HEIGHT)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_CONTINUOUS;

	/* fill in stepwise (step=1.0 is requred by V4L2 spec) */
	fival->stepwise.min  = tpf_min;
	fival->stepwise.max  = tpf_max;
	fival->stepwise.step = (struct v4l2_fract) {1, 1};

	return 0;
}

static int vidioc_g_parm(struct file *file, void *priv,
			  struct v4l2_streamparm *parm)
{
	struct bm2835_mmal_dev *dev = video_drvdata(file);

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	parm->parm.capture.capability   = V4L2_CAP_TIMEPERFRAME;
	parm->parm.capture.timeperframe = dev->capture.timeperframe;
	parm->parm.capture.readbuffers  = 1;
	return 0;
}

#define FRACT_CMP(a, OP, b)	\
	((u64)(a).numerator * (b).denominator  OP  \
	 (u64)(b).numerator * (a).denominator)

static int vidioc_s_parm(struct file *file, void *priv,
			  struct v4l2_streamparm *parm)
{
	struct bm2835_mmal_dev *dev = video_drvdata(file);
	struct v4l2_fract tpf;
	struct mmal_parameter_rational fps_param;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	tpf = parm->parm.capture.timeperframe;

	/* tpf: {*, 0} resets timing; clip to [min, max]*/
	tpf = tpf.denominator ? tpf : tpf_default;
	tpf = FRACT_CMP(tpf, <, tpf_min) ? tpf_min : tpf;
	tpf = FRACT_CMP(tpf, >, tpf_max) ? tpf_max : tpf;

	dev->capture.timeperframe = tpf;
	parm->parm.capture.timeperframe = tpf;
	parm->parm.capture.readbuffers  = 1;

	fps_param.num = 0;	/* Select variable fps, and then use
				 * FPS_RANGE to select the actual limits.
				 */
	fps_param.den = 1;
	set_framerate_params(dev);

	return 0;
}

static const struct v4l2_ioctl_ops camera0_ioctl_ops = {
	/* overlay */
	.vidioc_enum_fmt_vid_overlay = vidioc_enum_fmt_vid_overlay,
	.vidioc_g_fmt_vid_overlay = vidioc_g_fmt_vid_overlay,
	.vidioc_try_fmt_vid_overlay = vidioc_try_fmt_vid_overlay,
	.vidioc_s_fmt_vid_overlay = vidioc_s_fmt_vid_overlay,
	.vidioc_overlay = vidioc_overlay,
	.vidioc_g_fbuf = vidioc_g_fbuf,

	/* inputs */
	.vidioc_enum_input = vidioc_enum_input,
	.vidioc_g_input = vidioc_g_input,
	.vidioc_s_input = vidioc_s_input,

	/* capture */
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = vidioc_s_fmt_vid_cap,

	/* buffer management */
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_enum_framesizes = vidioc_enum_framesizes,
	.vidioc_enum_frameintervals = vidioc_enum_frameintervals,
	.vidioc_g_parm        = vidioc_g_parm,
	.vidioc_s_parm        = vidioc_s_parm,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ------------------------------------------------------------------
	Driver init/finalise
   ------------------------------------------------------------------*/

static const struct v4l2_file_operations camera0_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,	/* V4L2 ioctl handler */
	.mmap = vb2_fop_mmap,
};

static struct video_device vdev_template = {
	.name = "camera0",
	.fops = &camera0_fops,
	.ioctl_ops = &camera0_ioctl_ops,
	.release = video_device_release_empty,
};

static int set_camera_parameters(struct vchiq_mmal_instance *instance,
				 struct vchiq_mmal_component *camera)
{
	int ret;
	struct mmal_parameter_camera_config cam_config = {
		.max_stills_w = MAX_WIDTH,
		.max_stills_h = MAX_HEIGHT,
		.stills_yuv422 = 1,
		.one_shot_stills = 1,
		.max_preview_video_w = 1920,
		.max_preview_video_h = 1088,
		.num_preview_video_frames = 3,
		.stills_capture_circular_buffer_height = 0,
		.fast_preview_resume = 0,
		.use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RAW_STC
	};

	ret = vchiq_mmal_port_parameter_set(instance, &camera->control,
					    MMAL_PARAMETER_CAMERA_CONFIG,
					    &cam_config, sizeof(cam_config));
	return ret;
}

/* MMAL instance and component init */
static int __init mmal_init(struct bm2835_mmal_dev *dev)
{
	int ret;
	struct mmal_es_format *format;

	ret = vchiq_mmal_init(&dev->instance);
	if (ret < 0)
		return ret;

	/* get the camera component ready */
	ret = vchiq_mmal_component_init(dev->instance, "ril.camera",
					&dev->component[MMAL_COMPONENT_CAMERA]);
	if (ret < 0)
		goto unreg_mmal;

	if (dev->component[MMAL_COMPONENT_CAMERA]->outputs <
	    MMAL_CAMERA_PORT_COUNT) {
		ret = -EINVAL;
		goto unreg_camera;
	}

	ret = set_camera_parameters(dev->instance,
				    dev->component[MMAL_COMPONENT_CAMERA]);
	if (ret < 0)
		goto unreg_camera;

	format =
	    &dev->component[MMAL_COMPONENT_CAMERA]->
	    output[MMAL_CAMERA_PORT_PREVIEW].format;

	format->encoding = MMAL_ENCODING_OPAQUE;
	format->encoding_variant = MMAL_ENCODING_I420;

	format->es->video.width = 1024;
	format->es->video.height = 768;
	format->es->video.crop.x = 0;
	format->es->video.crop.y = 0;
	format->es->video.crop.width = 1024;
	format->es->video.crop.height = 768;
	format->es->video.frame_rate.num = 0; /* Rely on fps_range */
	format->es->video.frame_rate.den = 1;

	format =
	    &dev->component[MMAL_COMPONENT_CAMERA]->
	    output[MMAL_CAMERA_PORT_VIDEO].format;

	format->encoding = MMAL_ENCODING_OPAQUE;
	format->encoding_variant = MMAL_ENCODING_I420;

	format->es->video.width = 1024;
	format->es->video.height = 768;
	format->es->video.crop.x = 0;
	format->es->video.crop.y = 0;
	format->es->video.crop.width = 1024;
	format->es->video.crop.height = 768;
	format->es->video.frame_rate.num = 0; /* Rely on fps_range */
	format->es->video.frame_rate.den = 1;

	format =
	    &dev->component[MMAL_COMPONENT_CAMERA]->
	    output[MMAL_CAMERA_PORT_CAPTURE].format;

	format->encoding = MMAL_ENCODING_OPAQUE;

	format->es->video.width = 2592;
	format->es->video.height = 1944;
	format->es->video.crop.x = 0;
	format->es->video.crop.y = 0;
	format->es->video.crop.width = 2592;
	format->es->video.crop.height = 1944;
	format->es->video.frame_rate.num = 0; /* Rely on fps_range */
	format->es->video.frame_rate.den = 1;

	dev->capture.width = format->es->video.width;
	dev->capture.height = format->es->video.height;
	dev->capture.fmt = &formats[0];
	dev->capture.encode_component = NULL;
	dev->capture.timeperframe = tpf_default;
	dev->capture.enc_profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
	dev->capture.enc_level = V4L2_MPEG_VIDEO_H264_LEVEL_4_0;

	/* get the preview component ready */
	ret = vchiq_mmal_component_init(
			dev->instance, "ril.video_render",
			&dev->component[MMAL_COMPONENT_PREVIEW]);
	if (ret < 0)
		goto unreg_camera;

	if (dev->component[MMAL_COMPONENT_PREVIEW]->inputs < 1) {
		ret = -EINVAL;
		pr_debug("too few input ports %d needed %d\n",
			 dev->component[MMAL_COMPONENT_PREVIEW]->inputs, 1);
		goto unreg_preview;
	}

	/* get the image encoder component ready */
	ret = vchiq_mmal_component_init(
		dev->instance, "ril.image_encode",
		&dev->component[MMAL_COMPONENT_IMAGE_ENCODE]);
	if (ret < 0)
		goto unreg_preview;

	if (dev->component[MMAL_COMPONENT_IMAGE_ENCODE]->inputs < 1) {
		ret = -EINVAL;
		v4l2_err(&dev->v4l2_dev, "too few input ports %d needed %d\n",
			 dev->component[MMAL_COMPONENT_IMAGE_ENCODE]->inputs,
			 1);
		goto unreg_image_encoder;
	}

	/* get the video encoder component ready */
	ret = vchiq_mmal_component_init(dev->instance, "ril.video_encode",
					&dev->
					component[MMAL_COMPONENT_VIDEO_ENCODE]);
	if (ret < 0)
		goto unreg_image_encoder;

	if (dev->component[MMAL_COMPONENT_VIDEO_ENCODE]->inputs < 1) {
		ret = -EINVAL;
		v4l2_err(&dev->v4l2_dev, "too few input ports %d needed %d\n",
			 dev->component[MMAL_COMPONENT_VIDEO_ENCODE]->inputs,
			 1);
		goto unreg_vid_encoder;
	}

	{
		struct vchiq_mmal_port *encoder_port =
			&dev->component[MMAL_COMPONENT_VIDEO_ENCODE]->output[0];
		encoder_port->format.encoding = MMAL_ENCODING_H264;
		ret = vchiq_mmal_port_set_format(dev->instance,
			encoder_port);
	}

	{
		unsigned int enable = 1;
		vchiq_mmal_port_parameter_set(
			dev->instance,
			&dev->component[MMAL_COMPONENT_VIDEO_ENCODE]->control,
			MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT,
			&enable, sizeof(enable));

		vchiq_mmal_port_parameter_set(dev->instance,
			&dev->component[MMAL_COMPONENT_VIDEO_ENCODE]->control,
			MMAL_PARAMETER_MINIMISE_FRAGMENTATION,
			&enable,
			sizeof(enable));
	}
	ret = bm2835_mmal_set_all_camera_controls(dev);
	if (ret < 0)
		goto unreg_vid_encoder;

	return 0;

unreg_vid_encoder:
	pr_err("Cleanup: Destroy video encoder\n");
	vchiq_mmal_component_finalise(
		dev->instance,
		dev->component[MMAL_COMPONENT_VIDEO_ENCODE]);

unreg_image_encoder:
	pr_err("Cleanup: Destroy image encoder\n");
	vchiq_mmal_component_finalise(
		dev->instance,
		dev->component[MMAL_COMPONENT_IMAGE_ENCODE]);

unreg_preview:
	pr_err("Cleanup: Destroy video render\n");
	vchiq_mmal_component_finalise(dev->instance,
				      dev->component[MMAL_COMPONENT_PREVIEW]);

unreg_camera:
	pr_err("Cleanup: Destroy camera\n");
	vchiq_mmal_component_finalise(dev->instance,
				      dev->component[MMAL_COMPONENT_CAMERA]);

unreg_mmal:
	vchiq_mmal_finalise(dev->instance);
	return ret;
}

static int __init bm2835_mmal_init_device(struct bm2835_mmal_dev *dev,
					  struct video_device *vfd)
{
	int ret;

	*vfd = vdev_template;

	vfd->v4l2_dev = &dev->v4l2_dev;

	vfd->lock = &dev->mutex;

	vfd->queue = &dev->capture.vb_vidq;

	set_bit(V4L2_FL_USE_FH_PRIO, &vfd->flags);

	/* video device needs to be able to access instance data */
	video_set_drvdata(vfd, dev);

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, -1);
	if (ret < 0)
		return ret;

	v4l2_info(vfd->v4l2_dev, "V4L2 device registered as %s\n",
		  video_device_node_name(vfd));

	return 0;
}

static struct v4l2_format default_v4l2_format = {
	.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG,
	.fmt.pix.width = 1024,
	.fmt.pix.bytesperline = 1024 * 3 / 2,
	.fmt.pix.height = 768,
	.fmt.pix.sizeimage = 1<<18,
};

static int __init bm2835_mmal_init(void)
{
	int ret;
	struct bm2835_mmal_dev *dev;
	struct vb2_queue *q;

	dev = kzalloc(sizeof(*gdev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	/* setup device defaults */
	dev->overlay.w.left = 150;
	dev->overlay.w.top = 50;
	dev->overlay.w.width = 1024;
	dev->overlay.w.height = 768;
	dev->overlay.clipcount = 0;
	dev->overlay.field = V4L2_FIELD_NONE;

	dev->capture.fmt = &formats[3]; /* JPEG */

	/* v4l device registration */
	snprintf(dev->v4l2_dev.name, sizeof(dev->v4l2_dev.name),
		 "%s", BM2835_MMAL_MODULE_NAME);
	ret = v4l2_device_register(NULL, &dev->v4l2_dev);
	if (ret)
		goto free_dev;

	/* setup v4l controls */
	ret = bm2835_mmal_init_controls(dev, &dev->ctrl_handler);
	if (ret < 0)
		goto unreg_dev;
	dev->v4l2_dev.ctrl_handler = &dev->ctrl_handler;

	/* mmal init */
	ret = mmal_init(dev);
	if (ret < 0)
		goto unreg_dev;

	/* initialize queue */
	q = &dev->capture.vb_vidq;
	memset(q, 0, sizeof(*q));
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	q->drv_priv = dev;
	q->buf_struct_size = sizeof(struct mmal_buffer);
	q->ops = &bm2835_mmal_video_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_type = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	ret = vb2_queue_init(q);
	if (ret < 0)
		goto unreg_dev;

	/* v4l2 core mutex used to protect all fops and v4l2 ioctls. */
	mutex_init(&dev->mutex);

	/* initialise video devices */
	ret = bm2835_mmal_init_device(dev, &dev->vdev);
	if (ret < 0)
		goto unreg_dev;

	ret = mmal_setup_components(dev, &default_v4l2_format);
	if (ret < 0) {
		v4l2_err(&dev->v4l2_dev,
			 "%s: could not setup components\n", __func__);
		goto unreg_dev;
	}

	v4l2_info(&dev->v4l2_dev,
		  "Broadcom 2835 MMAL video capture ver %s loaded.\n",
		  BM2835_MMAL_VERSION);

	gdev = dev;
	return 0;

unreg_dev:
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	v4l2_device_unregister(&dev->v4l2_dev);

free_dev:
	kfree(dev);

	v4l2_err(&dev->v4l2_dev,
		 "%s: error %d while loading driver\n",
		 BM2835_MMAL_MODULE_NAME, ret);

	return ret;
}

static void __exit bm2835_mmal_exit(void)
{
	if (!gdev)
		return;

	v4l2_info(&gdev->v4l2_dev, "unregistering %s\n",
		  video_device_node_name(&gdev->vdev));

	video_unregister_device(&gdev->vdev);

	if (gdev->capture.encode_component) {
		v4l2_dbg(1, bcm2835_v4l2_debug, &gdev->v4l2_dev,
			 "mmal_exit - disconnect tunnel\n");
		vchiq_mmal_port_connect_tunnel(gdev->instance,
					       gdev->capture.camera_port, NULL);
		vchiq_mmal_component_disable(gdev->instance,
					     gdev->capture.encode_component);
	}
	vchiq_mmal_component_disable(gdev->instance,
				     gdev->component[MMAL_COMPONENT_CAMERA]);

	vchiq_mmal_component_finalise(gdev->instance,
				      gdev->
				      component[MMAL_COMPONENT_VIDEO_ENCODE]);

	vchiq_mmal_component_finalise(gdev->instance,
				      gdev->
				      component[MMAL_COMPONENT_IMAGE_ENCODE]);

	vchiq_mmal_component_finalise(gdev->instance,
				      gdev->component[MMAL_COMPONENT_PREVIEW]);

	vchiq_mmal_component_finalise(gdev->instance,
				      gdev->component[MMAL_COMPONENT_CAMERA]);

	vchiq_mmal_finalise(gdev->instance);

	v4l2_ctrl_handler_free(&gdev->ctrl_handler);

	v4l2_device_unregister(&gdev->v4l2_dev);

	kfree(gdev);
}

module_init(bm2835_mmal_init);
module_exit(bm2835_mmal_exit);
