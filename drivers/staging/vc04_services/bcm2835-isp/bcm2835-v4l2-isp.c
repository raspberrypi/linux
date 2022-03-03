// SPDX-License-Identifier: GPL-2.0
/*
 * Broadcom BCM2835 ISP driver
 *
 * Copyright © 2019-2020 Raspberry Pi (Trading) Ltd.
 *
 * Author: Naushir Patuck (naush@raspberrypi.com)
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "vchiq-mmal/mmal-msg.h"
#include "vchiq-mmal/mmal-parameters.h"
#include "vchiq-mmal/mmal-vchiq.h"

#include "vc-sm-cma/vc_sm_knl.h"

#include "bcm2835-isp-ctrls.h"
#include "bcm2835-isp-fmts.h"

/*
 * We want to instantiate 2 independent instances allowing 2 simultaneous users
 * of the ISP hardware.
 */
#define BCM2835_ISP_NUM_INSTANCES 2

MODULE_IMPORT_NS(DMA_BUF);

static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "activates debug info");

static unsigned int video_nr[BCM2835_ISP_NUM_INSTANCES] = { 13, 20 };
module_param_array(video_nr, uint, NULL, 0644);
MODULE_PARM_DESC(video_nr, "base video device numbers");

#define BCM2835_ISP_NAME "bcm2835-isp"
#define BCM2835_ISP_ENTITY_NAME_LEN 32

#define BCM2835_ISP_NUM_OUTPUTS 1
#define BCM2835_ISP_NUM_CAPTURES 2
#define BCM2835_ISP_NUM_METADATA 1

#define BCM2835_ISP_NUM_NODES						\
		(BCM2835_ISP_NUM_OUTPUTS + BCM2835_ISP_NUM_CAPTURES +	\
		 BCM2835_ISP_NUM_METADATA)

/* Default frame dimension of 1280 pixels. */
#define DEFAULT_DIM 1280U
/*
 * Maximum frame dimension of 16384 pixels.  Even though the ISP runs in tiles,
 * have a sensible limit so that we do not create an excessive number of tiles
 * to process.
 */
#define MAX_DIM 16384U
/*
 * Minimum frame dimension of 64 pixels.  Anything lower, and the tiling
 * algorithm may not be able to cope when applying filter context.
 */
#define MIN_DIM 64U

/* Timeout for stop_streaming to allow all buffers to return */
#define COMPLETE_TIMEOUT (2 * HZ)

/* Per-queue, driver-specific private data */
struct bcm2835_isp_q_data {
	/*
	 * These parameters should be treated as gospel, with everything else
	 * being determined from them.
	 */
	unsigned int bytesperline;
	unsigned int width;
	unsigned int height;
	unsigned int sizeimage;
	enum v4l2_colorspace colorspace;
	const struct bcm2835_isp_fmt *fmt;
};

/*
 * Structure to describe a single node /dev/video<N> which represents a single
 * input or output queue to the ISP device.
 */
struct bcm2835_isp_node {
	int vfl_dir;
	unsigned int id;
	const char *name;
	struct vchiq_mmal_port *port;
	struct video_device vfd;
	struct media_pad pad;
	struct media_intf_devnode *intf_devnode;
	struct media_link *intf_link;
	struct mutex lock; /* top level device node lock */
	struct mutex queue_lock;

	struct vb2_queue queue;
	unsigned int sequence;

	/* The list of formats supported on the node. */
	struct bcm2835_isp_fmt const **supported_fmts;
	unsigned int num_supported_fmts;

	struct bcm2835_isp_q_data q_data;

	/* Parent device structure */
	struct bcm2835_isp_dev *dev;

	bool registered;
	bool media_node_registered;
};

/*
 * Structure representing the entire ISP device, comprising several input and
 * output nodes /dev/video<N>.
 */
struct bcm2835_isp_dev {
	struct v4l2_device v4l2_dev;
	struct device *dev;
	struct v4l2_ctrl_handler ctrl_handler;
	struct media_device mdev;
	struct media_entity entity;
	bool media_device_registered;
	bool media_entity_registered;
	struct vchiq_mmal_instance *mmal_instance;
	struct vchiq_mmal_component *component;
	struct completion frame_cmplt;

	struct bcm2835_isp_node node[BCM2835_ISP_NUM_NODES];
	struct media_pad pad[BCM2835_ISP_NUM_NODES];
	atomic_t num_streaming;

	/* Image pipeline controls. */
	int r_gain;
	int b_gain;
};

struct bcm2835_isp_buffer {
	struct vb2_v4l2_buffer vb;
	struct mmal_buffer mmal;
};

static
inline struct bcm2835_isp_dev *node_get_dev(struct bcm2835_isp_node *node)
{
	return node->dev;
}

static inline bool node_is_output(struct bcm2835_isp_node *node)
{
	return node->queue.type == V4L2_BUF_TYPE_VIDEO_OUTPUT;
}

static inline bool node_is_capture(struct bcm2835_isp_node *node)
{
	return node->queue.type == V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

static inline bool node_is_stats(struct bcm2835_isp_node *node)
{
	return node->queue.type == V4L2_BUF_TYPE_META_CAPTURE;
}

static inline enum v4l2_buf_type index_to_queue_type(int index)
{
	if (index < BCM2835_ISP_NUM_OUTPUTS)
		return V4L2_BUF_TYPE_VIDEO_OUTPUT;
	else if (index < BCM2835_ISP_NUM_OUTPUTS + BCM2835_ISP_NUM_CAPTURES)
		return V4L2_BUF_TYPE_VIDEO_CAPTURE;
	else
		return V4L2_BUF_TYPE_META_CAPTURE;
}

static int set_isp_param(struct bcm2835_isp_node *node, u32 parameter,
			 void *value, u32 value_size)
{
	struct bcm2835_isp_dev *dev = node_get_dev(node);

	return vchiq_mmal_port_parameter_set(dev->mmal_instance, node->port,
					     parameter, value, value_size);
}

static int set_wb_gains(struct bcm2835_isp_node *node)
{
	struct bcm2835_isp_dev *dev = node_get_dev(node);
	struct mmal_parameter_awbgains gains = {
		.r_gain = { dev->r_gain, 1000 },
		.b_gain = { dev->b_gain, 1000 }
	};

	return set_isp_param(node, MMAL_PARAMETER_CUSTOM_AWB_GAINS,
			     &gains, sizeof(gains));
}

static int set_digital_gain(struct bcm2835_isp_node *node, uint32_t gain)
{
	struct mmal_parameter_rational digital_gain = {
		.num = gain,
		.den = 1000
	};

	return set_isp_param(node, MMAL_PARAMETER_DIGITAL_GAIN,
			     &digital_gain, sizeof(digital_gain));
}

static const struct bcm2835_isp_fmt *get_fmt(u32 mmal_fmt)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(supported_formats); i++) {
		if (supported_formats[i].mmal_fmt == mmal_fmt)
			return &supported_formats[i];
	}
	return NULL;
}

static const
struct bcm2835_isp_fmt *find_format_by_fourcc(unsigned int fourcc,
					      struct bcm2835_isp_node *node)
{
	const struct bcm2835_isp_fmt *fmt;
	unsigned int i;

	for (i = 0; i < node->num_supported_fmts; i++) {
		fmt = node->supported_fmts[i];
		if (fmt->fourcc == fourcc)
			return fmt;
	}

	return NULL;
}

static const
struct bcm2835_isp_fmt *find_format(struct v4l2_format *f,
				    struct bcm2835_isp_node *node)
{
	return find_format_by_fourcc(node_is_stats(node) ?
				     f->fmt.meta.dataformat :
				     f->fmt.pix.pixelformat,
				     node);
}

/* vb2_to_mmal_buffer() - converts vb2 buffer header to MMAL
 *
 * Copies all the required fields from a VB2 buffer to the MMAL buffer header,
 * ready for sending to the VPU.
 */
static void vb2_to_mmal_buffer(struct mmal_buffer *buf,
			       struct vb2_v4l2_buffer *vb2)
{
	u64 pts;

	buf->mmal_flags = 0;
	if (vb2->flags & V4L2_BUF_FLAG_KEYFRAME)
		buf->mmal_flags |= MMAL_BUFFER_HEADER_FLAG_KEYFRAME;

	/* Data must be framed correctly as one frame per buffer. */
	buf->mmal_flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_END;

	buf->length = vb2->vb2_buf.planes[0].bytesused;
	/*
	 * Minor ambiguity in the V4L2 spec as to whether passing in a 0 length
	 * buffer, or one with V4L2_BUF_FLAG_LAST set denotes end of stream.
	 * Handle either.
	 */
	if (!buf->length || vb2->flags & V4L2_BUF_FLAG_LAST)
		buf->mmal_flags |= MMAL_BUFFER_HEADER_FLAG_EOS;

	/* vb2 timestamps in nsecs, mmal in usecs */
	pts = vb2->vb2_buf.timestamp;
	do_div(pts, 1000);
	buf->pts = pts;
	buf->dts = MMAL_TIME_UNKNOWN;
}

static void mmal_buffer_cb(struct vchiq_mmal_instance *instance,
			   struct vchiq_mmal_port *port, int status,
			   struct mmal_buffer *mmal_buf)
{
	struct bcm2835_isp_buffer *q_buf;
	struct bcm2835_isp_node *node = port->cb_ctx;
	struct bcm2835_isp_dev *dev = node_get_dev(node);
	struct vb2_v4l2_buffer *vb2;

	q_buf = container_of(mmal_buf, struct bcm2835_isp_buffer, mmal);
	vb2 = &q_buf->vb;
	v4l2_dbg(2, debug, &dev->v4l2_dev,
		 "%s: port:%s[%d], status:%d, buf:%p, dmabuf:%p, length:%lu, flags %u, pts %lld\n",
		 __func__, node_is_output(node) ? "input" : "output", node->id,
		 status, mmal_buf, mmal_buf->dma_buf, mmal_buf->length,
		 mmal_buf->mmal_flags, mmal_buf->pts);

	if (mmal_buf->cmd)
		v4l2_err(&dev->v4l2_dev,
			 "%s: Unexpected event on output callback - %08x\n",
			 __func__, mmal_buf->cmd);

	if (status) {
		/* error in transfer */
		if (vb2) {
			/* there was a buffer with the error so return it */
			vb2_buffer_done(&vb2->vb2_buf, VB2_BUF_STATE_ERROR);
		}
		return;
	}

	/* vb2 timestamps in nsecs, mmal in usecs */
	vb2->vb2_buf.timestamp = mmal_buf->pts * 1000;
	vb2->sequence = node->sequence++;
	vb2_set_plane_payload(&vb2->vb2_buf, 0, mmal_buf->length);
	vb2_buffer_done(&vb2->vb2_buf, VB2_BUF_STATE_DONE);

	if (!port->enabled)
		complete(&dev->frame_cmplt);
}

struct colorspace_translation {
	enum v4l2_colorspace v4l2_value;
	u32 mmal_value;
};

static u32 translate_color_space(enum v4l2_colorspace color_space)
{
	static const struct colorspace_translation translations[] = {
		{ V4L2_COLORSPACE_DEFAULT, MMAL_COLOR_SPACE_UNKNOWN },
		{ V4L2_COLORSPACE_SMPTE170M, MMAL_COLOR_SPACE_ITUR_BT601 },
		{ V4L2_COLORSPACE_SMPTE240M, MMAL_COLOR_SPACE_SMPTE240M },
		{ V4L2_COLORSPACE_REC709, MMAL_COLOR_SPACE_ITUR_BT709 },
		/* V4L2_COLORSPACE_BT878 unavailable */
		{ V4L2_COLORSPACE_470_SYSTEM_M, MMAL_COLOR_SPACE_BT470_2_M },
		{ V4L2_COLORSPACE_470_SYSTEM_BG, MMAL_COLOR_SPACE_BT470_2_BG },
		{ V4L2_COLORSPACE_JPEG, MMAL_COLOR_SPACE_JPEG_JFIF },
		/*
		 * We don't have an encoding for SRGB as such, but VideoCore
		 * will do the right thing if it gets "unknown".
		 */
		{ V4L2_COLORSPACE_SRGB, MMAL_COLOR_SPACE_UNKNOWN },
		/* V4L2_COLORSPACE_OPRGB unavailable */
		/* V4L2_COLORSPACE_BT2020 unavailable */
		/* V4L2_COLORSPACE_RAW unavailable */
		/* V4L2_COLORSPACE_DCI_P3 unavailable */
	};

	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(translations); i++) {
		if (color_space == translations[i].v4l2_value)
			return translations[i].mmal_value;
	}

	return MMAL_COLOR_SPACE_UNKNOWN;
}

static void setup_mmal_port_format(struct bcm2835_isp_node *node,
				   struct vchiq_mmal_port *port)
{
	struct bcm2835_isp_q_data *q_data = &node->q_data;

	port->format.encoding = q_data->fmt->mmal_fmt;
	/* Raw image format - set width/height */
	port->es.video.width = (q_data->bytesperline << 3) / q_data->fmt->depth;
	port->es.video.height = q_data->height;
	port->es.video.crop.width = q_data->width;
	port->es.video.crop.height = q_data->height;
	port->es.video.crop.x = 0;
	port->es.video.crop.y = 0;
	port->es.video.color_space = translate_color_space(q_data->colorspace);
};

static int setup_mmal_port(struct bcm2835_isp_node *node)
{
	struct bcm2835_isp_dev *dev = node_get_dev(node);
	unsigned int enable = 1;
	int ret;

	v4l2_dbg(2, debug, &dev->v4l2_dev, "%s: setup %s[%d]\n", __func__,
		 node->name, node->id);

	vchiq_mmal_port_parameter_set(dev->mmal_instance, node->port,
				      MMAL_PARAMETER_ZERO_COPY, &enable,
				      sizeof(enable));
	setup_mmal_port_format(node, node->port);
	ret = vchiq_mmal_port_set_format(dev->mmal_instance, node->port);
	if (ret < 0) {
		v4l2_dbg(1, debug, &dev->v4l2_dev,
			 "%s: vchiq_mmal_port_set_format failed\n",
			 __func__);
		return ret;
	}

	if (node->q_data.sizeimage < node->port->minimum_buffer.size) {
		v4l2_err(&dev->v4l2_dev,
			 "buffer size mismatch sizeimage %u < min size %u\n",
			 node->q_data.sizeimage,
			 node->port->minimum_buffer.size);
		return -EINVAL;
	}

	return 0;
}

static int bcm2835_isp_mmal_buf_cleanup(struct mmal_buffer *mmal_buf)
{
	mmal_vchi_buffer_cleanup(mmal_buf);

	if (mmal_buf->dma_buf) {
		dma_buf_put(mmal_buf->dma_buf);
		mmal_buf->dma_buf = NULL;
	}

	return 0;
}

static int bcm2835_isp_node_queue_setup(struct vb2_queue *q,
					unsigned int *nbuffers,
					unsigned int *nplanes,
					unsigned int sizes[],
					struct device *alloc_devs[])
{
	struct bcm2835_isp_node *node = vb2_get_drv_priv(q);
	unsigned int size;

	if (setup_mmal_port(node))
		return -EINVAL;

	size = node->q_data.sizeimage;
	if (size == 0) {
		v4l2_info(&node_get_dev(node)->v4l2_dev,
			  "%s: Image size unset in queue_setup for node %s[%d]\n",
			  __func__, node->name, node->id);
		return -EINVAL;
	}

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = size;

	node->port->current_buffer.size = size;

	if (*nbuffers < node->port->minimum_buffer.num)
		*nbuffers = node->port->minimum_buffer.num;

	node->port->current_buffer.num = *nbuffers;

	v4l2_dbg(2, debug, &node_get_dev(node)->v4l2_dev,
		 "%s: Image size %u, nbuffers %u for node %s[%d]\n",
		 __func__, sizes[0], *nbuffers, node->name, node->id);
	return 0;
}

static int bcm2835_isp_buf_init(struct vb2_buffer *vb)
{
	struct bcm2835_isp_node *node = vb2_get_drv_priv(vb->vb2_queue);
	struct bcm2835_isp_dev *dev = node_get_dev(node);
	struct vb2_v4l2_buffer *vb2 = to_vb2_v4l2_buffer(vb);
	struct bcm2835_isp_buffer *buf =
		container_of(vb2, struct bcm2835_isp_buffer, vb);

	v4l2_dbg(3, debug, &dev->v4l2_dev, "%s: vb %p\n", __func__, vb);

	buf->mmal.buffer = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	buf->mmal.buffer_size = vb2_plane_size(&buf->vb.vb2_buf, 0);
	mmal_vchi_buffer_init(dev->mmal_instance, &buf->mmal);
	return 0;
}

static int bcm2835_isp_buf_prepare(struct vb2_buffer *vb)
{
	struct bcm2835_isp_node *node = vb2_get_drv_priv(vb->vb2_queue);
	struct bcm2835_isp_dev *dev = node_get_dev(node);
	struct vb2_v4l2_buffer *vb2 = to_vb2_v4l2_buffer(vb);
	struct bcm2835_isp_buffer *buf =
		container_of(vb2, struct bcm2835_isp_buffer, vb);
	struct dma_buf *dma_buf;
	int ret;

	v4l2_dbg(3, debug, &dev->v4l2_dev, "%s: type: %d ptr %p\n",
		 __func__, vb->vb2_queue->type, vb);

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		if (vb2->field == V4L2_FIELD_ANY)
			vb2->field = V4L2_FIELD_NONE;
		if (vb2->field != V4L2_FIELD_NONE) {
			v4l2_err(&dev->v4l2_dev,
				 "%s field isn't supported\n", __func__);
			return -EINVAL;
		}
	}

	if (vb2_plane_size(vb, 0) < node->q_data.sizeimage) {
		v4l2_err(&dev->v4l2_dev,
			 "%s data will not fit into plane (%lu < %lu)\n",
			 __func__, vb2_plane_size(vb, 0),
			 (long)node->q_data.sizeimage);
		return -EINVAL;
	}

	if (!V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type))
		vb2_set_plane_payload(vb, 0, node->q_data.sizeimage);

	switch (vb->memory) {
	case VB2_MEMORY_DMABUF:
		dma_buf = dma_buf_get(vb->planes[0].m.fd);

		if (dma_buf != buf->mmal.dma_buf) {
			/*
			 * dmabuf either hasn't already been mapped, or it has
			 * changed.
			 */
			if (buf->mmal.dma_buf) {
				v4l2_err(&dev->v4l2_dev,
					 "%s Buffer changed - why did the core not call cleanup?\n",
					 __func__);
				bcm2835_isp_mmal_buf_cleanup(&buf->mmal);
			}

			buf->mmal.dma_buf = dma_buf;
		} else {
			/*
			 * Already have a reference to the buffer, so release it
			 * here.
			 */
			dma_buf_put(dma_buf);
		}
		ret = 0;
		break;
	case VB2_MEMORY_MMAP:
		/*
		 * We want to do this at init, but vb2_core_expbuf checks that
		 * the index < q->num_buffers, and q->num_buffers only gets
		 * updated once all the buffers are allocated.
		 */
		if (!buf->mmal.dma_buf) {
			ret = vb2_core_expbuf_dmabuf(vb->vb2_queue,
						     vb->vb2_queue->type,
						     vb->index, 0, O_CLOEXEC,
						     &buf->mmal.dma_buf);
			v4l2_dbg(3, debug, &dev->v4l2_dev,
				 "%s: exporting ptr %p to dmabuf %p\n",
				 __func__, vb, buf->mmal.dma_buf);
			if (ret)
				v4l2_err(&dev->v4l2_dev,
					 "%s: Failed to expbuf idx %d, ret %d\n",
					 __func__, vb->index, ret);
		} else {
			ret = 0;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static void bcm2835_isp_node_buffer_queue(struct vb2_buffer *buf)
{
	struct bcm2835_isp_node *node = vb2_get_drv_priv(buf->vb2_queue);
	struct vb2_v4l2_buffer *vbuf =
		container_of(buf, struct vb2_v4l2_buffer, vb2_buf);
	struct bcm2835_isp_buffer *buffer =
		container_of(vbuf, struct bcm2835_isp_buffer, vb);
	struct bcm2835_isp_dev *dev = node_get_dev(node);

	v4l2_dbg(3, debug, &dev->v4l2_dev, "%s: node %s[%d], buffer %p\n",
		 __func__, node->name, node->id, buffer);

	vb2_to_mmal_buffer(&buffer->mmal, &buffer->vb);
	v4l2_dbg(3, debug, &dev->v4l2_dev,
		 "%s: node %s[%d] - submitting  mmal dmabuf %p\n", __func__,
		 node->name, node->id, buffer->mmal.dma_buf);
	vchiq_mmal_submit_buffer(dev->mmal_instance, node->port, &buffer->mmal);
}

static void bcm2835_isp_buffer_cleanup(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vb2 = to_vb2_v4l2_buffer(vb);
	struct bcm2835_isp_buffer *buffer =
		container_of(vb2, struct bcm2835_isp_buffer, vb);

	bcm2835_isp_mmal_buf_cleanup(&buffer->mmal);
}

static int bcm2835_isp_node_start_streaming(struct vb2_queue *q,
					    unsigned int count)
{
	struct bcm2835_isp_node *node = vb2_get_drv_priv(q);
	struct bcm2835_isp_dev *dev = node_get_dev(node);
	int ret;

	v4l2_dbg(1, debug, &dev->v4l2_dev, "%s: node %s[%d] (count %u)\n",
		 __func__, node->name, node->id, count);

	ret = vchiq_mmal_component_enable(dev->mmal_instance, dev->component);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "%s: Failed enabling component, ret %d\n",
			 __func__, ret);
		return -EIO;
	}

	node->sequence = 0;
	node->port->cb_ctx = node;
	ret = vchiq_mmal_port_enable(dev->mmal_instance, node->port,
				     mmal_buffer_cb);
	if (!ret)
		atomic_inc(&dev->num_streaming);
	else
		v4l2_err(&dev->v4l2_dev,
			 "%s: Failed enabling port, ret %d\n", __func__, ret);

	return ret;
}

static void bcm2835_isp_node_stop_streaming(struct vb2_queue *q)
{
	struct bcm2835_isp_node *node = vb2_get_drv_priv(q);
	struct bcm2835_isp_dev *dev = node_get_dev(node);
	int ret;

	v4l2_dbg(1, debug, &dev->v4l2_dev, "%s: node %s[%d], mmal port %p\n",
		 __func__, node->name, node->id, node->port);

	init_completion(&dev->frame_cmplt);

	/* Disable MMAL port - this will flush buffers back */
	ret = vchiq_mmal_port_disable(dev->mmal_instance, node->port);
	if (ret)
		v4l2_err(&dev->v4l2_dev,
			 "%s: Failed disabling %s port, ret %d\n", __func__,
			 node_is_output(node) ? "i/p" : "o/p",
			 ret);

	while (atomic_read(&node->port->buffers_with_vpu)) {
		v4l2_dbg(1, debug, &dev->v4l2_dev,
			 "%s: Waiting for buffers to be returned - %d outstanding\n",
			 __func__, atomic_read(&node->port->buffers_with_vpu));
		ret = wait_for_completion_timeout(&dev->frame_cmplt,
						  COMPLETE_TIMEOUT);
		if (ret <= 0) {
			v4l2_err(&dev->v4l2_dev,
				 "%s: Timeout waiting for buffers to be returned - %d outstanding\n",
				 __func__,
				 atomic_read(&node->port->buffers_with_vpu));
			break;
		}
	}

	atomic_dec(&dev->num_streaming);
	/* If all ports disabled, then disable the component */
	if (atomic_read(&dev->num_streaming) == 0) {
		ret = vchiq_mmal_component_disable(dev->mmal_instance,
						   dev->component);
		if (ret) {
			v4l2_err(&dev->v4l2_dev,
				 "%s: Failed disabling component, ret %d\n",
				 __func__, ret);
		}
	}

	/*
	 * Simply wait for any vb2 buffers to finish. We could take steps to
	 * make them complete more quickly if we care, or even return them
	 * ourselves.
	 */
	vb2_wait_for_all_buffers(&node->queue);
}

static const struct vb2_ops bcm2835_isp_node_queue_ops = {
	.queue_setup		= bcm2835_isp_node_queue_setup,
	.buf_init		= bcm2835_isp_buf_init,
	.buf_prepare		= bcm2835_isp_buf_prepare,
	.buf_queue		= bcm2835_isp_node_buffer_queue,
	.buf_cleanup		= bcm2835_isp_buffer_cleanup,
	.start_streaming	= bcm2835_isp_node_start_streaming,
	.stop_streaming		= bcm2835_isp_node_stop_streaming,
};

static const
struct bcm2835_isp_fmt *get_default_format(struct bcm2835_isp_node *node)
{
	return node->supported_fmts[0];
}

static inline unsigned int get_bytesperline(int width,
					    const struct bcm2835_isp_fmt *fmt)
{
	/* GPU aligns 24bpp images to a multiple of 32 pixels (not bytes). */
	if (fmt->depth == 24)
		return ALIGN(width, 32) * 3;
	else
		return ALIGN((width * fmt->depth) >> 3, fmt->bytesperline_align);
}

static inline unsigned int get_sizeimage(int bpl, int width, int height,
					 const struct bcm2835_isp_fmt *fmt)
{
	return (bpl * height * fmt->size_multiplier_x2) >> 1;
}

static int bcm2835_isp_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct bcm2835_isp_dev *dev =
	      container_of(ctrl->handler, struct bcm2835_isp_dev, ctrl_handler);
	struct bcm2835_isp_node *node = &dev->node[0];
	int ret = 0;

	/*
	 * The ISP firmware driver will ensure these settings are applied on
	 * a frame boundary, so we are safe to write them as they come in.
	 *
	 * Note that the bcm2835_isp_* param structures are identical to the
	 * mmal-parameters.h definitions.  This avoids the need for unnecessary
	 * field-by-field copying between structures.
	 */
	switch (ctrl->id) {
	case V4L2_CID_RED_BALANCE:
		dev->r_gain = ctrl->val;
		ret = set_wb_gains(node);
		break;
	case V4L2_CID_BLUE_BALANCE:
		dev->b_gain = ctrl->val;
		ret = set_wb_gains(node);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = set_digital_gain(node, ctrl->val);
		break;
	case V4L2_CID_USER_BCM2835_ISP_CC_MATRIX:
		ret = set_isp_param(node, MMAL_PARAMETER_CUSTOM_CCM,
				    ctrl->p_new.p_u8,
				    sizeof(struct bcm2835_isp_custom_ccm));
		break;
	case V4L2_CID_USER_BCM2835_ISP_LENS_SHADING:
	{
		struct bcm2835_isp_lens_shading *v4l2_ls;
		struct mmal_parameter_lens_shading_v2 ls;
		struct dma_buf *dmabuf;
		void *vcsm_handle;

		v4l2_ls = (struct bcm2835_isp_lens_shading *)ctrl->p_new.p_u8;
		/*
		 * struct bcm2835_isp_lens_shading and struct
		 * mmal_parameter_lens_shading_v2 match so that we can do a
		 * simple memcpy here.
		 * Only the dmabuf to the actual table needs any manipulation.
		 */
		memcpy(&ls, v4l2_ls, sizeof(ls));

		dmabuf = dma_buf_get(v4l2_ls->dmabuf);
		if (IS_ERR_OR_NULL(dmabuf))
			return -EINVAL;

		ret = vc_sm_cma_import_dmabuf(dmabuf, &vcsm_handle);
		if (ret) {
			dma_buf_put(dmabuf);
			return -EINVAL;
		}

		ls.mem_handle_table = vc_sm_cma_int_handle(vcsm_handle);
		if (ls.mem_handle_table)
			/* The VPU will take a reference on the vcsm handle,
			 * which in turn will retain a reference on the dmabuf.
			 * This code can therefore safely release all
			 * references to the buffer.
			 */
			ret = set_isp_param(node,
					    MMAL_PARAMETER_LENS_SHADING_OVERRIDE,
					    &ls,
					    sizeof(ls));
		else
			ret = -EINVAL;

		vc_sm_cma_free(vcsm_handle);
		dma_buf_put(dmabuf);
		break;
	}
	case V4L2_CID_USER_BCM2835_ISP_BLACK_LEVEL:
		ret = set_isp_param(node, MMAL_PARAMETER_BLACK_LEVEL,
				    ctrl->p_new.p_u8,
				    sizeof(struct bcm2835_isp_black_level));
		break;
	case V4L2_CID_USER_BCM2835_ISP_GEQ:
		ret = set_isp_param(node, MMAL_PARAMETER_GEQ,
				    ctrl->p_new.p_u8,
				    sizeof(struct bcm2835_isp_geq));
		break;
	case V4L2_CID_USER_BCM2835_ISP_GAMMA:
		ret = set_isp_param(node, MMAL_PARAMETER_GAMMA,
				    ctrl->p_new.p_u8,
				    sizeof(struct bcm2835_isp_gamma));
		break;
	case V4L2_CID_USER_BCM2835_ISP_DENOISE:
		ret = set_isp_param(node, MMAL_PARAMETER_DENOISE,
				    ctrl->p_new.p_u8,
				    sizeof(struct bcm2835_isp_denoise));
		break;
	case V4L2_CID_USER_BCM2835_ISP_CDN:
		ret = set_isp_param(node, MMAL_PARAMETER_CDN,
				    ctrl->p_new.p_u8,
				    sizeof(struct bcm2835_isp_cdn));
		break;
	case V4L2_CID_USER_BCM2835_ISP_SHARPEN:
		ret = set_isp_param(node, MMAL_PARAMETER_SHARPEN,
				    ctrl->p_new.p_u8,
				    sizeof(struct bcm2835_isp_sharpen));
		break;
	case V4L2_CID_USER_BCM2835_ISP_DPC:
		ret = set_isp_param(node, MMAL_PARAMETER_DPC,
				    ctrl->p_new.p_u8,
				    sizeof(struct bcm2835_isp_dpc));
		break;
	default:
		v4l2_info(&dev->v4l2_dev, "Unrecognised control\n");
		ret = -EINVAL;
	}

	if (ret) {
		v4l2_err(&dev->v4l2_dev, "%s: Failed setting ctrl \"%s\" (%08x), err %d\n",
			 __func__, ctrl->name, ctrl->id, ret);
		ret = -EIO;
	}

	return ret;
}

static const struct v4l2_ctrl_ops bcm2835_isp_ctrl_ops = {
	.s_ctrl = bcm2835_isp_s_ctrl,
};

static const struct v4l2_file_operations bcm2835_isp_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.poll		= vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap		= vb2_fop_mmap
};

static int populate_qdata_fmt(struct v4l2_format *f,
			      struct bcm2835_isp_node *node)
{
	struct bcm2835_isp_dev *dev = node_get_dev(node);
	struct bcm2835_isp_q_data *q_data = &node->q_data;
	int ret;

	if (!node_is_stats(node)) {
		v4l2_dbg(1, debug, &dev->v4l2_dev,
			 "%s: Setting pix format for type %d, wxh: %ux%u, fmt: %08x, size %u\n",
			 __func__, f->type, f->fmt.pix.width, f->fmt.pix.height,
			 f->fmt.pix.pixelformat, f->fmt.pix.sizeimage);

		q_data->fmt = find_format(f, node);
		q_data->width = f->fmt.pix.width;
		q_data->height = f->fmt.pix.height;
		q_data->height = f->fmt.pix.height;

		/* All parameters should have been set correctly by try_fmt */
		q_data->bytesperline = f->fmt.pix.bytesperline;
		q_data->sizeimage = f->fmt.pix.sizeimage;

		/* We must indicate which of the allowed colour spaces we have. */
		q_data->colorspace = f->fmt.pix.colorspace;
	} else {
		v4l2_dbg(1, debug, &dev->v4l2_dev,
			 "%s: Setting meta format for fmt: %08x, size %u\n",
			 __func__, f->fmt.meta.dataformat,
			 f->fmt.meta.buffersize);

		q_data->fmt = find_format(f, node);
		q_data->width = 0;
		q_data->height = 0;
		q_data->bytesperline = 0;
		q_data->sizeimage = f->fmt.meta.buffersize;

		/* This won't mean anything for metadata, but may as well fill it in. */
		q_data->colorspace = V4L2_COLORSPACE_DEFAULT;
	}

	v4l2_dbg(1, debug, &dev->v4l2_dev,
		 "%s: Calculated bpl as %u, size %u\n", __func__,
		 q_data->bytesperline, q_data->sizeimage);

	setup_mmal_port_format(node, node->port);
	ret = vchiq_mmal_port_set_format(dev->mmal_instance, node->port);
	if (ret) {
		v4l2_err(&dev->v4l2_dev,
			 "%s: Failed vchiq_mmal_port_set_format on port, ret %d\n",
			 __func__, ret);
		ret = -EINVAL;
	}

	if (q_data->sizeimage < node->port->minimum_buffer.size) {
		v4l2_err(&dev->v4l2_dev,
			 "%s: Current buffer size of %u < min buf size %u - driver mismatch to MMAL\n",
			 __func__,
			 q_data->sizeimage,
			 node->port->minimum_buffer.size);
	}

	v4l2_dbg(1, debug, &dev->v4l2_dev,
		 "%s: Set format for type %d, wxh: %dx%d, fmt: %08x, size %u\n",
		 __func__, f->type, q_data->width, q_data->height,
		 q_data->fmt->fourcc, q_data->sizeimage);

	return ret;
}

static int bcm2835_isp_node_querycap(struct file *file, void *priv,
				     struct v4l2_capability *cap)
{
	strscpy(cap->driver, BCM2835_ISP_NAME, sizeof(cap->driver));
	strscpy(cap->card, BCM2835_ISP_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 BCM2835_ISP_NAME);

	return 0;
}

static int bcm2835_isp_node_g_fmt(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct bcm2835_isp_node *node = video_drvdata(file);

	if (f->type != node->queue.type)
		return -EINVAL;

	if (node_is_stats(node)) {
		f->fmt.meta.dataformat = V4L2_META_FMT_BCM2835_ISP_STATS;
		f->fmt.meta.buffersize =
			node->port->minimum_buffer.size;
	} else {
		struct bcm2835_isp_q_data *q_data = &node->q_data;

		f->fmt.pix.width = q_data->width;
		f->fmt.pix.height = q_data->height;
		f->fmt.pix.field = V4L2_FIELD_NONE;
		f->fmt.pix.pixelformat = q_data->fmt->fourcc;
		f->fmt.pix.bytesperline = q_data->bytesperline;
		f->fmt.pix.sizeimage = q_data->sizeimage;
		f->fmt.pix.colorspace = q_data->colorspace;
	}

	return 0;
}

static int bcm2835_isp_node_enum_fmt(struct file *file, void  *priv,
				     struct v4l2_fmtdesc *f)
{
	struct bcm2835_isp_node *node = video_drvdata(file);

	if (f->type != node->queue.type)
		return -EINVAL;

	if (f->index < node->num_supported_fmts) {
		/* Format found */
		f->pixelformat = node->supported_fmts[f->index]->fourcc;
		f->flags = 0;
		return 0;
	}

	return -EINVAL;
}

static int bcm2835_isp_enum_framesizes(struct file *file, void *priv,
				       struct v4l2_frmsizeenum *fsize)
{
	struct bcm2835_isp_node *node = video_drvdata(file);
	struct bcm2835_isp_dev *dev = node_get_dev(node);
	const struct bcm2835_isp_fmt *fmt;

	if (node_is_stats(node) || fsize->index)
		return -EINVAL;

	fmt = find_format_by_fourcc(fsize->pixel_format, node);
	if (!fmt) {
		v4l2_err(&dev->v4l2_dev, "Invalid pixel code: %x\n",
			 fsize->pixel_format);
		return -EINVAL;
	}

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = MIN_DIM;
	fsize->stepwise.max_width = MAX_DIM;
	fsize->stepwise.step_width = fmt->step_size;

	fsize->stepwise.min_height = MIN_DIM;
	fsize->stepwise.max_height = MAX_DIM;
	fsize->stepwise.step_height = fmt->step_size;

	return 0;
}

static int bcm2835_isp_node_try_fmt(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct bcm2835_isp_node *node = video_drvdata(file);
	const struct bcm2835_isp_fmt *fmt;

	if (f->type != node->queue.type)
		return -EINVAL;

	fmt = find_format(f, node);
	if (!fmt)
		fmt = get_default_format(node);

	if (!node_is_stats(node)) {
		int is_rgb;

		f->fmt.pix.width = max(min(f->fmt.pix.width, MAX_DIM),
				       MIN_DIM);
		f->fmt.pix.height = max(min(f->fmt.pix.height, MAX_DIM),
					MIN_DIM);

		f->fmt.pix.pixelformat = fmt->fourcc;

		/*
		 * Fill in the actual colour space when the requested one was
		 * not supported. This also catches the case when the "default"
		 * colour space was requested (as that's never in the mask).
		 */
		if (!(V4L2_COLORSPACE_MASK(f->fmt.pix.colorspace) & fmt->colorspace_mask))
			f->fmt.pix.colorspace = fmt->colorspace_default;
		/* In all cases, we only support the defaults for these: */
		f->fmt.pix.ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(f->fmt.pix.colorspace);
		f->fmt.pix.xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(f->fmt.pix.colorspace);
		/* RAW counts as sRGB here so that we get full range. */
		is_rgb = f->fmt.pix.colorspace == V4L2_COLORSPACE_SRGB ||
			f->fmt.pix.colorspace == V4L2_COLORSPACE_RAW;
		f->fmt.pix.quantization = V4L2_MAP_QUANTIZATION_DEFAULT(is_rgb, f->fmt.pix.colorspace,
									f->fmt.pix.ycbcr_enc);

		f->fmt.pix.bytesperline = get_bytesperline(f->fmt.pix.width,
							   fmt);
		f->fmt.pix.field = V4L2_FIELD_NONE;
		f->fmt.pix.sizeimage =
			get_sizeimage(f->fmt.pix.bytesperline, f->fmt.pix.width,
				      f->fmt.pix.height, fmt);
	} else {
		f->fmt.meta.dataformat = fmt->fourcc;
		f->fmt.meta.buffersize = node->port->minimum_buffer.size;
	}

	return 0;
}

static int bcm2835_isp_node_s_fmt(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct bcm2835_isp_node *node = video_drvdata(file);
	int ret;

	if (f->type != node->queue.type)
		return -EINVAL;

	ret = bcm2835_isp_node_try_fmt(file, priv, f);
	if (ret)
		return ret;

	v4l2_dbg(1, debug, &node_get_dev(node)->v4l2_dev,
		 "%s: Set format for node %s[%d]\n",
		 __func__, node->name, node->id);

	return populate_qdata_fmt(f, node);
}

static int bcm2835_isp_node_s_selection(struct file *file, void *fh,
					struct v4l2_selection *s)
{
	struct mmal_parameter_crop crop;
	struct bcm2835_isp_node *node = video_drvdata(file);
	struct bcm2835_isp_dev *dev = node_get_dev(node);

	/* This return value is required fro V4L2 compliance. */
	if (node_is_stats(node))
		return -ENOTTY;

	if (!s->r.width || !s->r.height)
		return -EINVAL;

	/* We can only set crop on the input. */
	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		/*
		 * Adjust the crop window if it goes outside of the frame
		 * dimensions.
		 */
		s->r.left = min((unsigned int)max(s->r.left, 0),
				node->q_data.width - MIN_DIM);
		s->r.top = min((unsigned int)max(s->r.top, 0),
			       node->q_data.height - MIN_DIM);
		s->r.width = max(min(s->r.width,
				     node->q_data.width - s->r.left), MIN_DIM);
		s->r.height = max(min(s->r.height,
				      node->q_data.height - s->r.top), MIN_DIM);
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
		/* Default (i.e. no) crop window. */
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = node->q_data.width;
		s->r.height = node->q_data.height;
		break;
	default:
		return -EINVAL;
	}

	crop.rect.x = s->r.left;
	crop.rect.y = s->r.top;
	crop.rect.width = s->r.width;
	crop.rect.height = s->r.height;

	return vchiq_mmal_port_parameter_set(dev->mmal_instance, node->port,
					     MMAL_PARAMETER_CROP,
					     &crop, sizeof(crop));
}

static int bcm2835_isp_node_g_selection(struct file *file, void *fh,
					struct v4l2_selection *s)
{
	struct mmal_parameter_crop crop;
	struct bcm2835_isp_node *node = video_drvdata(file);
	struct bcm2835_isp_dev *dev = node_get_dev(node);
	u32 crop_size = sizeof(crop);
	int ret;

	/* We can only return out an input crop. */
	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		ret = vchiq_mmal_port_parameter_get(dev->mmal_instance,
						    node->port,
						    MMAL_PARAMETER_CROP,
						    &crop, &crop_size);
		if (!ret) {
			s->r.left = crop.rect.x;
			s->r.top = crop.rect.y;
			s->r.width = crop.rect.width;
			s->r.height = crop.rect.height;
		}
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		/* Default (i.e. no) crop window. */
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = node->q_data.width;
		s->r.height = node->q_data.height;
		ret = 0;
		break;
	default:
		ret =  -EINVAL;
	}

	return ret;
}

static int bcm3285_isp_subscribe_event(struct v4l2_fh *fh,
				       const struct v4l2_event_subscription *s)
{
	switch (s->type) {
	/* Cannot change source parameters dynamically at runtime. */
	case V4L2_EVENT_SOURCE_CHANGE:
		return -EINVAL;
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subscribe_event(fh, s);
	default:
		return v4l2_event_subscribe(fh, s, 4, NULL);
	}
}

static const struct v4l2_ioctl_ops bcm2835_isp_node_ioctl_ops = {
	.vidioc_querycap		= bcm2835_isp_node_querycap,
	.vidioc_g_fmt_vid_cap		= bcm2835_isp_node_g_fmt,
	.vidioc_g_fmt_vid_out		= bcm2835_isp_node_g_fmt,
	.vidioc_g_fmt_meta_cap		= bcm2835_isp_node_g_fmt,
	.vidioc_s_fmt_vid_cap		= bcm2835_isp_node_s_fmt,
	.vidioc_s_fmt_vid_out		= bcm2835_isp_node_s_fmt,
	.vidioc_s_fmt_meta_cap		= bcm2835_isp_node_s_fmt,
	.vidioc_try_fmt_vid_cap		= bcm2835_isp_node_try_fmt,
	.vidioc_try_fmt_vid_out		= bcm2835_isp_node_try_fmt,
	.vidioc_try_fmt_meta_cap	= bcm2835_isp_node_try_fmt,
	.vidioc_s_selection		= bcm2835_isp_node_s_selection,
	.vidioc_g_selection		= bcm2835_isp_node_g_selection,

	.vidioc_enum_fmt_vid_cap	= bcm2835_isp_node_enum_fmt,
	.vidioc_enum_fmt_vid_out	= bcm2835_isp_node_enum_fmt,
	.vidioc_enum_fmt_meta_cap	= bcm2835_isp_node_enum_fmt,
	.vidioc_enum_framesizes		= bcm2835_isp_enum_framesizes,

	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,

	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,

	.vidioc_subscribe_event		= bcm3285_isp_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

/*
 * Size of the array to provide to the VPU when asking for the list of supported
 * formats.
 *
 * The ISP component currently advertises 62 input formats, so add a small
 * overhead on that. Should the component advertise more formats then the excess
 * will be dropped and a warning logged.
 */
#define MAX_SUPPORTED_ENCODINGS 70

/* Populate node->supported_fmts with the formats supported by those ports. */
static int bcm2835_isp_get_supported_fmts(struct bcm2835_isp_node *node)
{
	struct bcm2835_isp_dev *dev = node_get_dev(node);
	struct bcm2835_isp_fmt const **list;
	unsigned int i, j, num_encodings;
	u32 fourccs[MAX_SUPPORTED_ENCODINGS];
	u32 param_size = sizeof(fourccs);
	int ret;

	ret = vchiq_mmal_port_parameter_get(dev->mmal_instance, node->port,
					    MMAL_PARAMETER_SUPPORTED_ENCODINGS,
					    &fourccs, &param_size);

	if (ret) {
		if (ret == MMAL_MSG_STATUS_ENOSPC) {
			v4l2_err(&dev->v4l2_dev,
				 "%s: port has more encodings than we provided space for. Some are dropped (%zu vs %u).\n",
				 __func__, param_size / sizeof(u32),
				 MAX_SUPPORTED_ENCODINGS);
			num_encodings = MAX_SUPPORTED_ENCODINGS;
		} else {
			v4l2_err(&dev->v4l2_dev, "%s: get_param ret %u.\n",
				 __func__, ret);
			return -EINVAL;
		}
	} else {
		num_encodings = param_size / sizeof(u32);
	}

	/*
	 * Assume at this stage that all encodings will be supported in V4L2.
	 * Any that aren't supported will waste a very small amount of memory.
	 */
	list = devm_kzalloc(dev->dev,
			    sizeof(struct bcm2835_isp_fmt *) * num_encodings,
			    GFP_KERNEL);
	if (!list)
		return -ENOMEM;
	node->supported_fmts = list;

	for (i = 0, j = 0; i < num_encodings; i++) {
		const struct bcm2835_isp_fmt *fmt = get_fmt(fourccs[i]);

		if (fmt) {
			list[j] = fmt;
			j++;
		}
	}
	node->num_supported_fmts = j;

	return 0;
}

/*
 * Register a device node /dev/video<N> to go along with one of the ISP's input
 * or output nodes.
 */
static int register_node(struct bcm2835_isp_dev *dev,
			 unsigned int instance,
			 struct bcm2835_isp_node *node,
			 int index)
{
	struct video_device *vfd;
	struct vb2_queue *queue;
	int ret;

	mutex_init(&node->lock);
	mutex_init(&node->queue_lock);

	node->dev = dev;
	vfd = &node->vfd;
	queue = &node->queue;
	queue->type = index_to_queue_type(index);
	/*
	 * Setup the node type-specific params.
	 *
	 * Only the OUTPUT node can set controls and crop windows. However,
	 * we must allow the s/g_selection ioctl on the stats node as v4l2
	 * compliance expects it to return a -ENOTTY, and the framework
	 * does not handle it if the ioctl is disabled.
	 */
	switch (queue->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		vfd->device_caps = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING;
		node->id = index;
		node->vfl_dir = VFL_DIR_TX;
		node->name = "output";
		node->port = &dev->component->input[node->id];
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		vfd->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
		/* First Capture node starts at id 0, etc. */
		node->id = index - BCM2835_ISP_NUM_OUTPUTS;
		node->vfl_dir = VFL_DIR_RX;
		node->name = "capture";
		node->port = &dev->component->output[node->id];
		v4l2_disable_ioctl(&node->vfd, VIDIOC_S_CTRL);
		v4l2_disable_ioctl(&node->vfd, VIDIOC_S_SELECTION);
		v4l2_disable_ioctl(&node->vfd, VIDIOC_G_SELECTION);
		break;
	case V4L2_BUF_TYPE_META_CAPTURE:
		vfd->device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
		node->id = index - BCM2835_ISP_NUM_OUTPUTS;
		node->vfl_dir = VFL_DIR_RX;
		node->name = "stats";
		node->port = &dev->component->output[node->id];
		v4l2_disable_ioctl(&node->vfd, VIDIOC_S_CTRL);
		v4l2_disable_ioctl(&node->vfd, VIDIOC_S_SELECTION);
		v4l2_disable_ioctl(&node->vfd, VIDIOC_G_SELECTION);
		break;
	}

	/* We use the selection API instead of the old crop API. */
	v4l2_disable_ioctl(vfd, VIDIOC_CROPCAP);
	v4l2_disable_ioctl(vfd, VIDIOC_G_CROP);
	v4l2_disable_ioctl(vfd, VIDIOC_S_CROP);

	ret = bcm2835_isp_get_supported_fmts(node);
	if (ret)
		return ret;

	/* Initialise the video node. */
	vfd->vfl_type	= VFL_TYPE_VIDEO;
	vfd->fops	= &bcm2835_isp_fops,
	vfd->ioctl_ops	= &bcm2835_isp_node_ioctl_ops,
	vfd->minor	= -1,
	vfd->release	= video_device_release_empty,
	vfd->queue	= &node->queue;
	vfd->lock	= &node->lock;
	vfd->v4l2_dev	= &dev->v4l2_dev;
	vfd->vfl_dir	= node->vfl_dir;

	node->q_data.fmt = get_default_format(node);
	node->q_data.width = DEFAULT_DIM;
	node->q_data.height = DEFAULT_DIM;
	node->q_data.bytesperline =
		get_bytesperline(DEFAULT_DIM, node->q_data.fmt);
	node->q_data.sizeimage = node_is_stats(node) ?
				 node->port->recommended_buffer.size :
				 get_sizeimage(node->q_data.bytesperline,
					       node->q_data.width,
					       node->q_data.height,
					       node->q_data.fmt);
	node->q_data.colorspace = node->q_data.fmt->colorspace_default;

	queue->io_modes = VB2_MMAP | VB2_DMABUF;
	queue->drv_priv = node;
	queue->ops = &bcm2835_isp_node_queue_ops;
	queue->mem_ops = &vb2_dma_contig_memops;
	queue->buf_struct_size = sizeof(struct bcm2835_isp_buffer);
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	queue->dev = dev->dev;
	queue->lock = &node->queue_lock;

	ret = vb2_queue_init(queue);
	if (ret < 0) {
		v4l2_info(&dev->v4l2_dev, "vb2_queue_init failed\n");
		return ret;
	}

	/* Set some controls and defaults, but only on the VIDEO_OUTPUT node. */
	if (node_is_output(node)) {
		unsigned int i;

		/* Use this ctrl template to assign custom ISP ctrls. */
		struct v4l2_ctrl_config ctrl_template = {
			.ops		= &bcm2835_isp_ctrl_ops,
			.type		= V4L2_CTRL_TYPE_U8,
			.def		= 0,
			.min		= 0x00,
			.max		= 0xff,
			.step		= 1,
		};

		/* 3 standard controls, and an array of custom controls */
		ret = v4l2_ctrl_handler_init(&dev->ctrl_handler,
					     3 + ARRAY_SIZE(custom_ctrls));
		if (ret) {
			v4l2_err(&dev->v4l2_dev, "ctrl_handler init failed (%d)\n",
				 ret);
			goto queue_cleanup;
		}

		dev->r_gain = 1000;
		dev->b_gain = 1000;

		v4l2_ctrl_new_std(&dev->ctrl_handler,  &bcm2835_isp_ctrl_ops,
				  V4L2_CID_RED_BALANCE, 1, 0xffff, 1,
				  dev->r_gain);

		v4l2_ctrl_new_std(&dev->ctrl_handler, &bcm2835_isp_ctrl_ops,
				  V4L2_CID_BLUE_BALANCE, 1, 0xffff, 1,
				  dev->b_gain);

		v4l2_ctrl_new_std(&dev->ctrl_handler, &bcm2835_isp_ctrl_ops,
				  V4L2_CID_DIGITAL_GAIN, 1, 0xffff, 1, 1000);

		for (i = 0; i < ARRAY_SIZE(custom_ctrls); i++) {
			ctrl_template.name = custom_ctrls[i].name;
			ctrl_template.id = custom_ctrls[i].id;
			ctrl_template.dims[0] = custom_ctrls[i].size;
			ctrl_template.flags = custom_ctrls[i].flags;
			v4l2_ctrl_new_custom(&dev->ctrl_handler,
					     &ctrl_template, NULL);
		}

		node->vfd.ctrl_handler = &dev->ctrl_handler;
		if (dev->ctrl_handler.error) {
			ret = dev->ctrl_handler.error;
			v4l2_err(&dev->v4l2_dev, "controls init failed (%d)\n",
				 ret);
			v4l2_ctrl_handler_free(&dev->ctrl_handler);
			goto ctrl_cleanup;
		}
	}

	/* Define the device names */
	snprintf(vfd->name, sizeof(node->vfd.name), "%s-%s%d", BCM2835_ISP_NAME,
		 node->name, node->id);

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, video_nr[instance]);
	if (ret) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to register video %s[%d] device node\n",
			 node->name, node->id);
		goto ctrl_cleanup;
	}

	node->registered = true;
	video_set_drvdata(vfd, node);

	v4l2_info(&dev->v4l2_dev,
		  "Device node %s[%d] registered as /dev/video%d\n",
		  node->name, node->id, vfd->num);

	return 0;

ctrl_cleanup:
	if (node_is_output(node))
		v4l2_ctrl_handler_free(&dev->ctrl_handler);
queue_cleanup:
	vb2_queue_release(&node->queue);
	return ret;
}

/* Unregister one of the /dev/video<N> nodes associated with the ISP. */
static void bcm2835_unregister_node(struct bcm2835_isp_node *node)
{
	struct bcm2835_isp_dev *dev = node_get_dev(node);

	v4l2_info(&dev->v4l2_dev,
		  "Unregistering node %s[%d] device node /dev/video%d\n",
		  node->name, node->id, node->vfd.num);

	if (node->registered) {
		video_unregister_device(&node->vfd);
		if (node_is_output(node))
			v4l2_ctrl_handler_free(&dev->ctrl_handler);
		vb2_queue_release(&node->queue);
	}

	/*
	 * node->supported_fmts.list is free'd automatically
	 * as a managed resource.
	 */
	node->supported_fmts = NULL;
	node->num_supported_fmts = 0;
	node->vfd.ctrl_handler = NULL;
	node->registered = false;
}

static void media_controller_unregister(struct bcm2835_isp_dev *dev)
{
	unsigned int i;

	v4l2_info(&dev->v4l2_dev, "Unregister from media controller\n");

	if (dev->media_device_registered) {
		media_device_unregister(&dev->mdev);
		media_device_cleanup(&dev->mdev);
		dev->media_device_registered = false;
	}

	kfree(dev->entity.name);
	dev->entity.name = NULL;

	if (dev->media_entity_registered) {
		media_device_unregister_entity(&dev->entity);
		dev->media_entity_registered = false;
	}

	for (i = 0; i < BCM2835_ISP_NUM_NODES; i++) {
		struct bcm2835_isp_node *node = &dev->node[i];

		if (node->media_node_registered) {
			media_remove_intf_links(node->intf_link->intf);
			media_entity_remove_links(&dev->node[i].vfd.entity);
			media_devnode_remove(node->intf_devnode);
			media_device_unregister_entity(&node->vfd.entity);
			kfree(node->vfd.entity.name);
		}
		node->media_node_registered = false;
	}

	dev->v4l2_dev.mdev = NULL;
}

static int media_controller_register_node(struct bcm2835_isp_dev *dev, int num)
{
	struct bcm2835_isp_node *node = &dev->node[num];
	struct media_entity *entity = &node->vfd.entity;
	int output = node_is_output(node);
	char *name;
	int ret;

	v4l2_info(&dev->v4l2_dev,
		  "Register %s node %d with media controller\n",
		  output ? "output" : "capture", num);
	entity->obj_type = MEDIA_ENTITY_TYPE_VIDEO_DEVICE;
	entity->function = MEDIA_ENT_F_IO_V4L;
	entity->info.dev.major = VIDEO_MAJOR;
	entity->info.dev.minor = node->vfd.minor;
	name = kmalloc(BCM2835_ISP_ENTITY_NAME_LEN, GFP_KERNEL);
	if (!name) {
		ret = -ENOMEM;
		goto error_no_mem;
	}
	snprintf(name, BCM2835_ISP_ENTITY_NAME_LEN, "%s0-%s%d",
		 BCM2835_ISP_NAME, output ? "output" : "capture", num);
	entity->name = name;
	node->pad.flags = output ? MEDIA_PAD_FL_SOURCE : MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(entity, 1, &node->pad);
	if (ret)
		goto error_pads_init;
	ret = media_device_register_entity(&dev->mdev, entity);
	if (ret)
		goto error_register_entity;

	node->intf_devnode = media_devnode_create(&dev->mdev,
						  MEDIA_INTF_T_V4L_VIDEO, 0,
						  VIDEO_MAJOR, node->vfd.minor);
	if (!node->intf_devnode) {
		ret = -ENOMEM;
		goto error_devnode_create;
	}

	node->intf_link = media_create_intf_link(entity,
						 &node->intf_devnode->intf,
						 MEDIA_LNK_FL_IMMUTABLE |
						 MEDIA_LNK_FL_ENABLED);
	if (!node->intf_link) {
		ret = -ENOMEM;
		goto error_create_intf_link;
	}

	if (output)
		ret = media_create_pad_link(entity, 0, &dev->entity, num,
					    MEDIA_LNK_FL_IMMUTABLE |
						    MEDIA_LNK_FL_ENABLED);
	else
		ret = media_create_pad_link(&dev->entity, num, entity, 0,
					    MEDIA_LNK_FL_IMMUTABLE |
					    MEDIA_LNK_FL_ENABLED);
	if (ret)
		goto error_create_pad_link;

	dev->node[num].media_node_registered = true;
	return 0;

error_create_pad_link:
	media_remove_intf_links(&node->intf_devnode->intf);
error_create_intf_link:
	media_devnode_remove(node->intf_devnode);
error_devnode_create:
	media_device_unregister_entity(&node->vfd.entity);
error_register_entity:
error_pads_init:
	kfree(entity->name);
	entity->name = NULL;
error_no_mem:
	if (ret)
		v4l2_info(&dev->v4l2_dev, "Error registering node\n");

	return ret;
}

static int media_controller_register(struct bcm2835_isp_dev *dev)
{
	char *name;
	unsigned int i;
	int ret;

	v4l2_dbg(2, debug, &dev->v4l2_dev, "Registering with media controller\n");
	dev->mdev.dev = dev->dev;
	strscpy(dev->mdev.model, "bcm2835-isp",
		sizeof(dev->mdev.model));
	strscpy(dev->mdev.bus_info, "platform:bcm2835-isp",
		sizeof(dev->mdev.bus_info));
	media_device_init(&dev->mdev);
	dev->v4l2_dev.mdev = &dev->mdev;

	v4l2_dbg(2, debug, &dev->v4l2_dev, "Register entity for nodes\n");

	name = kmalloc(BCM2835_ISP_ENTITY_NAME_LEN, GFP_KERNEL);
	if (!name) {
		ret = -ENOMEM;
		goto done;
	}
	snprintf(name, BCM2835_ISP_ENTITY_NAME_LEN, "bcm2835_isp0");
	dev->entity.name = name;
	dev->entity.obj_type = MEDIA_ENTITY_TYPE_BASE;
	dev->entity.function = MEDIA_ENT_F_PROC_VIDEO_SCALER;

	for (i = 0; i < BCM2835_ISP_NUM_NODES; i++) {
		dev->pad[i].flags = node_is_output(&dev->node[i]) ?
					MEDIA_PAD_FL_SINK : MEDIA_PAD_FL_SOURCE;
	}

	ret = media_entity_pads_init(&dev->entity, BCM2835_ISP_NUM_NODES,
				     dev->pad);
	if (ret)
		goto done;

	ret = media_device_register_entity(&dev->mdev, &dev->entity);
	if (ret)
		goto done;

	dev->media_entity_registered = true;
	for (i = 0; i < BCM2835_ISP_NUM_NODES; i++) {
		ret = media_controller_register_node(dev, i);
		if (ret)
			goto done;
	}

	ret = media_device_register(&dev->mdev);
	if (!ret)
		dev->media_device_registered = true;
done:
	return ret;
}

static void bcm2835_isp_remove_instance(struct bcm2835_isp_dev *dev)
{
	unsigned int i;

	media_controller_unregister(dev);

	for (i = 0; i < BCM2835_ISP_NUM_NODES; i++)
		bcm2835_unregister_node(&dev->node[i]);

	v4l2_device_unregister(&dev->v4l2_dev);

	if (dev->component)
		vchiq_mmal_component_finalise(dev->mmal_instance,
					      dev->component);

	vchiq_mmal_finalise(dev->mmal_instance);
}

static int bcm2835_isp_probe_instance(struct platform_device *pdev,
				      struct bcm2835_isp_dev **dev_int,
				      unsigned int instance)
{
	struct bcm2835_isp_dev *dev;
	unsigned int i;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	*dev_int = dev;
	dev->dev = &pdev->dev;

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		return ret;

	ret = vchiq_mmal_init(&dev->mmal_instance);
	if (ret) {
		v4l2_device_unregister(&dev->v4l2_dev);
		return ret;
	}

	ret = vchiq_mmal_component_init(dev->mmal_instance, "ril.isp",
					&dev->component);
	if (ret) {
		v4l2_err(&dev->v4l2_dev,
			 "%s: failed to create ril.isp component\n", __func__);
		return ret;
	}

	if (dev->component->inputs < BCM2835_ISP_NUM_OUTPUTS ||
	    dev->component->outputs < BCM2835_ISP_NUM_CAPTURES +
					BCM2835_ISP_NUM_METADATA) {
		v4l2_err(&dev->v4l2_dev,
			 "%s: ril.isp returned %d i/p (%d expected), %d o/p (%d expected) ports\n",
			  __func__, dev->component->inputs,
			  BCM2835_ISP_NUM_OUTPUTS,
			  dev->component->outputs,
			  BCM2835_ISP_NUM_CAPTURES + BCM2835_ISP_NUM_METADATA);
		return -EINVAL;
	}

	atomic_set(&dev->num_streaming, 0);

	for (i = 0; i < BCM2835_ISP_NUM_NODES; i++) {
		struct bcm2835_isp_node *node = &dev->node[i];

		ret = register_node(dev, instance, node, i);
		if (ret)
			return ret;
	}

	ret = media_controller_register(dev);
	if (ret)
		return ret;

	return 0;
}

static int bcm2835_isp_remove(struct platform_device *pdev)
{
	struct bcm2835_isp_dev **bcm2835_isp_instances;
	unsigned int i;

	bcm2835_isp_instances = platform_get_drvdata(pdev);
	for (i = 0; i < BCM2835_ISP_NUM_INSTANCES; i++) {
		if (bcm2835_isp_instances[i])
			bcm2835_isp_remove_instance(bcm2835_isp_instances[i]);
	}

	return 0;
}

static int bcm2835_isp_probe(struct platform_device *pdev)
{
	struct bcm2835_isp_dev **bcm2835_isp_instances;
	unsigned int i;
	int ret;

	bcm2835_isp_instances = devm_kzalloc(&pdev->dev,
					     sizeof(bcm2835_isp_instances) *
						      BCM2835_ISP_NUM_INSTANCES,
					     GFP_KERNEL);
	if (!bcm2835_isp_instances)
		return -ENOMEM;

	platform_set_drvdata(pdev, bcm2835_isp_instances);

	for (i = 0; i < BCM2835_ISP_NUM_INSTANCES; i++) {
		ret = bcm2835_isp_probe_instance(pdev,
						 &bcm2835_isp_instances[i], i);
		if (ret)
			goto error;
	}

	dev_info(&pdev->dev, "Loaded V4L2 %s\n", BCM2835_ISP_NAME);
	return 0;

error:
	bcm2835_isp_remove(pdev);

	return ret;
}

static struct platform_driver bcm2835_isp_pdrv = {
	.probe = bcm2835_isp_probe,
	.remove = bcm2835_isp_remove,
	.driver = {
			.name = BCM2835_ISP_NAME,
		  },
};

module_platform_driver(bcm2835_isp_pdrv);

MODULE_DESCRIPTION("BCM2835 ISP driver");
MODULE_AUTHOR("Naushir Patuck <naush@raspberrypi.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_ALIAS("platform:bcm2835-isp");
