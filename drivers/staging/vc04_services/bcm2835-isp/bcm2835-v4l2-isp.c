// SPDX-License-Identifier: GPL-2.0
/*
 * Broadcom BM2835 ISP driver
 *
 * Copyright © 2019 Raspberry Pi (Trading) Ltd.
 *
 * Author: Naushir Patuck @ Raspberry Pi
 *
 */

#include <linux/module.h>

#include <linux/platform_device.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/videobuf2-vmalloc.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-dma-sg.h>

#include "vchiq-mmal/mmal-encodings.h"
#include "vchiq-mmal/mmal-msg.h"
#include "vchiq-mmal/mmal-parameters.h"
#include "vchiq-mmal/mmal-vchiq.h"
//#include "mmal-parameters.h"
#include "bcm2835_isp_fmts.h"

static unsigned int debug = 3;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "activates debug info");

static unsigned int video_nr = 13;
module_param(video_nr, uint, 0644);
MODULE_PARM_DESC(video_nr, "base video device number");

#define dprintk(dev, fmt, arg...)                                              \
	v4l2_dbg(1, debug, &isp_dev->v4l2_dev, "%s: " fmt, __func__, ##arg)

#define BCM2835_ISP_NAME "bcm2835-isp"
#define BCM2835_ISP_ENTITY_NAME_LEN 32

#define BCM2835_ISP_NUM_NODE_GROUPS 1
#define BCM2835_ISP_NUM_OUTPUTS 1
#define BCM2835_ISP_NUM_CAPTURES 2
/* Add one for the stats output node */
#define BCM2835_ISP_NUM_NODES                                                  \
	(BCM2835_ISP_NUM_OUTPUTS + BCM2835_ISP_NUM_CAPTURES + 1)

#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480

#define V4L2_CID_BCM2835_ISP_PARAM (V4L2_CID_USER_BASE + 0x1000)

enum node_type {
	NODE_TYPE_OUTPUT = 0x0,
	NODE_TYPE_CAPTURE = 0x1,
	NODE_TYPE_STATS = 0x2
};

#define NODE_IS_OUTPUT(node)                                                   \
	(!(((node)->type) & (NODE_TYPE_CAPTURE | NODE_TYPE_STATS)))
#define NODE_IS_CAPTURE(node) (((node)->type) & NODE_TYPE_CAPTURE)
#define NODE_IS_STATS(node) (((node)->type) & NODE_TYPE_STATS)

#define INDEX_TO_NODE_TYPE(idx) ((idx) < BCM2835_ISP_NUM_NODES - 1 ?           \
		 ((idx) < BCM2835_ISP_NUM_OUTPUTS ? NODE_TYPE_OUTPUT :         \
		 NODE_TYPE_CAPTURE) : NODE_TYPE_STATS)

/* Per-queue, driver-specific private data */
struct bcm2835_isp_q_data {
	/*
	 * These parameters should be treated as gospel, with everything else
	 * being determined from them.
	 */
	/* Buffer width/height */
	unsigned int bytesperline;
	unsigned int height;
	/* Crop size used for selection handling */
	unsigned int crop_width;
	unsigned int crop_height;

	unsigned int sizeimage;
	unsigned int sequence;
	struct bcm2835_isp_fmt *fmt;
};

/* Structure to describe a single node /dev/video<N> which represents a single
 * input or output queue to the ISP device.
 */
struct bcm2835_isp_node {
	int vfl_dir;
	int id;
	enum node_type type;
	enum v4l2_buf_type v4l_type;
	const char *name;
	struct video_device vfd;
	struct media_pad pad;
	struct media_intf_devnode *intf_devnode;
	struct media_link *intf_link;
	struct bcm2835_isp_node_group *node_group;
	struct mutex node_lock;	/* top level device node lock */
	struct mutex queue_lock;
	int open;
	/* Remember that each node can open be opened once, so stuff related to
	 * the file handle can just be kept here.
	 */
	struct v4l2_fh fh;
	struct vb2_queue queue;
	struct v4l2_ctrl_handler hdl;

	/* The list of formats supported on input and output queues. */
	struct bcm2835_isp_fmt_list supported_fmts;

	struct bcm2835_isp_q_data q_data;
	enum v4l2_colorspace colorspace;
	unsigned int framerate_num;
	unsigned int framerate_denom;
};

#define node_get_bcm2835_isp(node) ((node)->node_group->isp_dev)

/* Node group structure, which comprises all the input and output nodes that a
 * single ISP client will need.
 */
struct bcm2835_isp_node_group {
	struct bcm2835_isp_dev *isp_dev;
	struct bcm2835_isp_node node[BCM2835_ISP_NUM_NODES];
	struct media_entity entity;
	struct media_pad pad[BCM2835_ISP_NUM_NODES];
	int param; /* this is just an example parameter */
	atomic_t num_streaming;
};

/* Structure representing the entire ISP device, comprising several input and
 * output nodes /dev/video<N>.
 */
struct bcm2835_isp_dev {
	struct v4l2_device v4l2_dev; /* does this belong in the node_group? */
	struct device *dev;
	struct media_device mdev;
	struct bcm2835_isp_node_group node_group[BCM2835_ISP_NUM_NODE_GROUPS];
	struct vchiq_mmal_instance *mmal_instance; /* MMAL handle. */
	struct vchiq_mmal_component *component;
	bool component_enabled;
	struct completion frame_cmplt;
	// Image pipeline controls.
	int r_gain;
	int b_gain;
};

struct bcm2835_isp_buffer {
	struct vb2_v4l2_buffer vb;
	struct mmal_buffer mmal;
};

static int set_wb_gains(struct bcm2835_isp_dev *isp_dev)
{
	struct mmal_parameter_awbgains gains;
	struct vchiq_mmal_port *control = &isp_dev->component->control;

	gains.r_gain.num = isp_dev->r_gain;
	gains.r_gain.num = isp_dev->b_gain;
	gains.r_gain.den = 1000;
	gains.b_gain.den = 1000;
	return vchiq_mmal_port_parameter_set(isp_dev->mmal_instance, control,
					     MMAL_PARAMETER_CUSTOM_AWB_GAINS,
					     &gains, sizeof(gains));
}

static int set_digital_gain(struct bcm2835_isp_dev *isp_dev, int gain)
{
	struct mmal_parameter_rational digital_gain;
	struct vchiq_mmal_port *control = &isp_dev->component->control;

	digital_gain.num = gain;
	digital_gain.den = 1000;
	return vchiq_mmal_port_parameter_set(isp_dev->mmal_instance, control,
					     MMAL_PARAMETER_DIGITAL_GAIN,
					     &digital_gain,
					     sizeof(digital_gain));
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

static struct bcm2835_isp_fmt *find_format(struct v4l2_format *f,
					   struct bcm2835_isp_node *node)
{
	struct bcm2835_isp_fmt *fmt;
	unsigned int k;
	struct bcm2835_isp_fmt_list *fmts = &node->supported_fmts;

	for (k = 0; k < fmts->num_entries; k++) {
		fmt = &fmts->list[k];
		if (fmt->fourcc == (NODE_IS_STATS(node) ? f->fmt.meta.dataformat
						: f->fmt.pix.pixelformat))
			break;
	}
	if (k == fmts->num_entries)
		return NULL;

	return &fmts->list[k];
}

static struct vchiq_mmal_port *get_port_data(struct bcm2835_isp_node *node)
{
	struct bcm2835_isp_dev *isp_dev = node_get_bcm2835_isp(node);

	if (!isp_dev->component)
		return NULL;

	switch (node->v4l_type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return &isp_dev->component->input[node->id];
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_META_CAPTURE:
		return &isp_dev->component->output[node->id];
	default:
		v4l2_err(&isp_dev->v4l2_dev, "%s: Invalid queue type %u\n",
			 __func__, node->v4l_type);
		break;
	}
	return NULL;
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

	/*
	 * Adding this means that the data must be framed correctly as one frame
	 * per buffer. The underlying decoder has no such requirement, but it
	 * will reduce latency as the bistream parser will be kicked immediately
	 * to parse the frame, rather than relying on its own heuristics for
	 * when to wake up.
	 */
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
	struct bcm2835_isp_node *node = port->cb_ctx;
	struct bcm2835_isp_dev *isp_dev = node_get_bcm2835_isp(node);
	struct bcm2835_isp_buffer *q_buf;
	struct vb2_v4l2_buffer *vb2;

	q_buf = container_of(mmal_buf, struct bcm2835_isp_buffer, mmal);
	vb2 = &q_buf->vb;
	v4l2_dbg(2, debug, &isp_dev->v4l2_dev, "%s: port:%s[%d], status:%d, buf:%p, dmabuf:%p, length:%lu, flags %u, pts %lld\n",
		 __func__, NODE_IS_OUTPUT(node) ? "input" : "output", node->id,
		 status, mmal_buf, mmal_buf->dma_buf, mmal_buf->length,
		 mmal_buf->mmal_flags, mmal_buf->pts);

	if (mmal_buf->cmd)
		v4l2_err(&isp_dev->v4l2_dev,
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
	vb2_set_plane_payload(&vb2->vb2_buf, 0, mmal_buf->length);
	vb2_buffer_done(&vb2->vb2_buf, VB2_BUF_STATE_DONE);

	if (!port->enabled)
		complete(&isp_dev->frame_cmplt);
}

static void setup_mmal_port_format(struct bcm2835_isp_node *node,
				   struct vchiq_mmal_port *port)
{
	struct bcm2835_isp_q_data *q_data = &node->q_data;

	port->format.encoding = q_data->fmt->mmal_fmt;
	/* Raw image format - set width/height */
	port->es.video.width = (q_data->bytesperline << 3) / q_data->fmt->depth;
	port->es.video.height = q_data->height;
	port->es.video.crop.width = q_data->crop_width;
	port->es.video.crop.height = q_data->crop_height;
	port->es.video.frame_rate.num = node->framerate_num;
	port->es.video.frame_rate.den = node->framerate_denom;
	port->es.video.crop.x = 0;
	port->es.video.crop.y = 0;
};

static int setup_mmal_component(struct bcm2835_isp_node *node)
{
	struct bcm2835_isp_dev *isp_dev = node_get_bcm2835_isp(node);
	unsigned int enable = 1;
	struct vchiq_mmal_port *port = get_port_data(node);
	int id = node->id;
	int ret;

	v4l2_dbg(2, debug, &isp_dev->v4l2_dev, "%s: setup %s[%d]\n", __func__,
		 node->name, id);

	vchiq_mmal_port_parameter_set(isp_dev->mmal_instance, port,
				      MMAL_PARAMETER_ZERO_COPY, &enable,
				      sizeof(enable));
	setup_mmal_port_format(node, port);
	ret = vchiq_mmal_port_set_format(isp_dev->mmal_instance, port);
	if (ret < 0) {
		v4l2_dbg(1, debug, &isp_dev->v4l2_dev,
			 "%s: vchiq_mmal_port_set_format ip port failed\n",
			 __func__);
		return ret;
	}

	ret = vchiq_mmal_port_set_format(isp_dev->mmal_instance,
					 &isp_dev->component->output[id]);
	if (ret < 0) {
		v4l2_dbg(1, debug, &isp_dev->v4l2_dev,
			 "%s: vchiq_mmal_port_set_format op port failed\n",
			 __func__);
		return ret;
	}

	if (node->q_data.sizeimage <
	    isp_dev->component->output[id].minimum_buffer.size) {
		v4l2_err(&isp_dev->v4l2_dev,
			 "buffer size mismatch sizeimage %u < min size %u\n",
			 node->q_data.sizeimage,
			 isp_dev->component->output[id].minimum_buffer.size);
		return -EINVAL;
	}

	v4l2_dbg(2, debug, &isp_dev->v4l2_dev,
		 "%s: component created as ril.isp\n", __func__);

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
	struct vchiq_mmal_port *port;
	unsigned int size;

	if (setup_mmal_component(node))
		return -EINVAL;

	size = node->q_data.sizeimage;
	if (size == 0) {
		v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
			  "Image size unset in queue_setup for node %p\n",
			  node);
		return -EINVAL;
	}

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = size;

	port = get_port_data(node);
	port->current_buffer.size = size;

	if (*nbuffers < port->minimum_buffer.num)
		*nbuffers = port->minimum_buffer.num;

	port->current_buffer.num = *nbuffers;

	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Image size %u, nbuffers %u for node %p\n", sizes[0],
		  *nbuffers, node);
	return 0;
}

static int bcm2835_isp_buf_init(struct vb2_buffer *vb)
{
	struct bcm2835_isp_node *node = vb2_get_drv_priv(vb->vb2_queue);
	struct bcm2835_isp_dev *isp_dev = node_get_bcm2835_isp(node);
	struct vb2_v4l2_buffer *vb2 = to_vb2_v4l2_buffer(vb);
	struct bcm2835_isp_buffer *buf =
		container_of(vb2, struct bcm2835_isp_buffer, vb);

	v4l2_dbg(2, debug, &isp_dev->v4l2_dev, "%s: vb %p\n", __func__, vb);

	buf->mmal.buffer = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	buf->mmal.buffer_size = vb2_plane_size(&buf->vb.vb2_buf, 0);
	mmal_vchi_buffer_init(isp_dev->mmal_instance, &buf->mmal);
	return 0;
}

static int bcm2835_isp_buf_prepare(struct vb2_buffer *vb)
{
	struct bcm2835_isp_node *node = vb2_get_drv_priv(vb->vb2_queue);
	struct bcm2835_isp_dev *isp_dev = node_get_bcm2835_isp(node);
	struct vb2_v4l2_buffer *vb2 = to_vb2_v4l2_buffer(vb);
	struct bcm2835_isp_buffer *buf =
		container_of(vb2, struct bcm2835_isp_buffer, vb);
	struct dma_buf *dma_buf;
	int ret;

	v4l2_dbg(3, debug, &isp_dev->v4l2_dev, "%s: type: %d ptr %p\n",
		 __func__, vb->vb2_queue->type, vb);

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		if (vb2->field == V4L2_FIELD_ANY)
			vb2->field = V4L2_FIELD_NONE;
		if (vb2->field != V4L2_FIELD_NONE) {
			v4l2_err(&isp_dev->v4l2_dev,
				 "%s field isn't supported\n", __func__);
			return -EINVAL;
		}
	}

	if (vb2_plane_size(vb, 0) < node->q_data.sizeimage) {
		v4l2_err(&isp_dev->v4l2_dev,
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
			/* dmabuf either hasn't already been mapped, or it has
			 * changed.
			 */
			if (buf->mmal.dma_buf) {
				v4l2_err(&isp_dev->v4l2_dev,
					 "%s Buffer changed - why did the core not call cleanup?\n",
					 __func__);
				bcm2835_isp_mmal_buf_cleanup(&buf->mmal);
			}

			buf->mmal.dma_buf = dma_buf;
		} else {
			/* Already have a reference to the buffer, so release it
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
			v4l2_dbg(3, debug, &isp_dev->v4l2_dev,
				 "%s: exporting ptr %p to dmabuf %p\n",
				 __func__, vb, buf->mmal.dma_buf);
			if (ret)
				v4l2_err(&isp_dev->v4l2_dev,
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
	struct vb2_v4l2_buffer *vbuf =
		container_of(buf, struct vb2_v4l2_buffer, vb2_buf);
	struct bcm2835_isp_buffer *buffer =
		container_of(vbuf, struct bcm2835_isp_buffer, vb);
	struct bcm2835_isp_node *node = vb2_get_drv_priv(buf->vb2_queue);
	struct bcm2835_isp_dev *isp_dev = node_get_bcm2835_isp(node);

	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "%s: node %s[%d], buffer %p\n", __func__, node->name,
		  node->id, buffer);

	vb2_to_mmal_buffer(&buffer->mmal, &buffer->vb);
	v4l2_dbg(3, debug, &isp_dev->v4l2_dev,
		 "%s: node %s[%d] - submitting  mmal dmabuf %p\n", __func__,
		 node->name, node->id, buffer->mmal.dma_buf);
	vchiq_mmal_submit_buffer(isp_dev->mmal_instance, get_port_data(node),
				 &buffer->mmal);
}

static void bcm2835_isp_buffer_cleanup(struct vb2_buffer *vb)
{
	struct bcm2835_isp_node *node = vb2_get_drv_priv(vb->vb2_queue);
	struct bcm2835_isp_dev *isp_dev = node_get_bcm2835_isp(node);
	struct vb2_v4l2_buffer *vb2 = to_vb2_v4l2_buffer(vb);
	struct bcm2835_isp_buffer *buf =
		container_of(vb2, struct bcm2835_isp_buffer, vb);

	v4l2_dbg(2, debug, &isp_dev->v4l2_dev, "%s: ctx:%p, vb %p\n", __func__,
		 isp_dev, vb2);

	bcm2835_isp_mmal_buf_cleanup(&buf->mmal);
}

static int bcm2835_isp_node_start_streaming(struct vb2_queue *q,
					    unsigned int count)
{
	struct bcm2835_isp_node *node = vb2_get_drv_priv(q);
	struct bcm2835_isp_node_group *node_group = node->node_group;
	struct bcm2835_isp_dev *isp_dev = node_group->isp_dev;
	struct vchiq_mmal_port *port = get_port_data(node);
	int ret;

	v4l2_info(&isp_dev->v4l2_dev, "%s: node %s[%d] (count %u)\n", __func__,
		  node->name, node->id, count);

	if (!isp_dev->component_enabled) {
		ret = vchiq_mmal_component_enable(isp_dev->mmal_instance,
						  isp_dev->component);
		if (ret)
			v4l2_err(&isp_dev->v4l2_dev,
				 "%s: Failed enabling component, ret %d\n",
				 __func__, ret);
		isp_dev->component_enabled = true;
	}

	port->cb_ctx = node;
	ret = vchiq_mmal_port_enable(isp_dev->mmal_instance, port,
				     mmal_buffer_cb);
	if (ret == 0) {
		atomic_inc(&node_group->num_streaming);
	} else {
		v4l2_err(&isp_dev->v4l2_dev,
			 "%s: Failed enabling port, ret %d\n", __func__, ret);
	}
	return ret;
}

static void bcm2835_isp_node_stop_streaming(struct vb2_queue *q)
{
	struct bcm2835_isp_node *node = vb2_get_drv_priv(q);
	struct bcm2835_isp_node_group *node_group = node->node_group;
	struct bcm2835_isp_dev *isp_dev = node_group->isp_dev;
	struct vchiq_mmal_port *port = get_port_data(node);
	int ret, i;

	v4l2_info(&isp_dev->v4l2_dev, "%s: node %s[%d], mmal port %p\n",
		  __func__, node->name, node->id, port);

	init_completion(&isp_dev->frame_cmplt);

	/* Disable MMAL port - this will flush buffers back */
	ret = vchiq_mmal_port_disable(isp_dev->mmal_instance, port);
	if (ret)
		v4l2_err(&isp_dev->v4l2_dev,
			 "%s: Failed disabling %s port, ret %d\n", __func__,
			 V4L2_TYPE_IS_OUTPUT(node->v4l_type) ? "i/p" : "o/p",
			 ret);

	while (atomic_read(&port->buffers_with_vpu)) {
		v4l2_dbg(1, debug, &isp_dev->v4l2_dev,
			 "%s: Waiting for buffers to be returned - %d outstanding\n",
			 __func__, atomic_read(&port->buffers_with_vpu));
		ret = wait_for_completion_timeout(&isp_dev->frame_cmplt, HZ);
		if (ret <= 0) {
			v4l2_err(&isp_dev->v4l2_dev,
				 "%s: Timeout waiting for buffers to be returned - %d outstanding\n",
				 __func__,
				 atomic_read(&port->buffers_with_vpu));
			break;
		}
	}

	/*
	 * Release the VCSM handle here as otherwise REQBUFS(0) aborts because
	 * someone is using the dmabuf before giving the driver a chance to do
	 * anything about it.
	 */
	for (i = 0; i < q->num_buffers; i++) {
		struct vb2_v4l2_buffer *vb2 = to_vb2_v4l2_buffer(q->bufs[i]);
		struct bcm2835_isp_buffer *buf =
			container_of(vb2, struct bcm2835_isp_buffer, vb);
		bcm2835_isp_mmal_buf_cleanup(&buf->mmal);
	}

	atomic_dec(&node_group->num_streaming);
	/* If all ports disabled, then disable the component */
	if (atomic_read(&node_group->num_streaming) == 0) {
		ret = vchiq_mmal_component_disable(isp_dev->mmal_instance,
						   isp_dev->component);
		if (ret == 0) {
			isp_dev->component_enabled = false;
		} else {
			v4l2_err(&isp_dev->v4l2_dev,
				 "%s: Failed disabling component, ret %d\n",
				 __func__, ret);
		}
	}

	/* Simply wait for any vb2 buffers to finish. We could take steps to
	 * make them complete more quickly if we care, or even return them
	 * ourselves.
	 */
	vb2_wait_for_all_buffers(&node->queue);

	v4l2_info(&isp_dev->v4l2_dev, "%s: Done", __func__);
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

static int bcm2835_isp_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct bcm2835_isp_node *node =
		container_of(ctrl->handler, struct bcm2835_isp_node, hdl);
	struct bcm2835_isp_node_group *node_group = node->node_group;
	struct bcm2835_isp_dev *isp_dev = node_group->isp_dev;
	int ret = 0;

	v4l2_info(&isp_dev->v4l2_dev, "Ctrl id is %u\n", ctrl->id);
	switch (ctrl->id) {
	case V4L2_CID_RED_BALANCE:
		isp_dev->r_gain = ctrl->val;
		ret = set_wb_gains(isp_dev);
		break;
	case V4L2_CID_BLUE_BALANCE:
		isp_dev->b_gain = ctrl->val;
		ret = set_wb_gains(isp_dev);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = set_digital_gain(isp_dev, ctrl->val);
		break;
	case V4L2_CID_BCM2835_ISP_PARAM:
		node_group->param = ctrl->val;
		ret = 0;
		v4l2_info(&isp_dev->v4l2_dev, "Set param to %d\n",
			  node_group->param);
		break;
	default:
		v4l2_info(&isp_dev->v4l2_dev, "Unrecognised control\n");
		ret = -EINVAL;
	}
	return ret;
}

static const struct v4l2_ctrl_ops bcm2835_isp_ctrl_ops = {
	.s_ctrl = bcm2835_isp_s_ctrl,
};

static struct v4l2_ctrl_config bcm2835_isp_ctrl_param = {
	.ops	= &bcm2835_isp_ctrl_ops,
	.id	= V4L2_CID_BCM2835_ISP_PARAM,
	.name	= "Param",
	.type	= V4L2_CTRL_TYPE_INTEGER,
	.min	= 0,
	.max	= 999999,
	.step	= 1,
};

static struct bcm2835_isp_fmt *get_default_format(struct bcm2835_isp_node *node)
{
	return &node->supported_fmts.list[node->id];
}

static inline unsigned int get_bytesperline(int width,
					    struct bcm2835_isp_fmt *fmt)
{
	return ALIGN((width * fmt->depth) >> 3, fmt->bytesperline_align);
}

static inline unsigned int get_sizeimage(int bpl, int width, int height,
					 struct bcm2835_isp_fmt *fmt)
{
	return (bpl * height * fmt->size_multiplier_x2) >> 1;
}

/* Open one of the nodes /dev/video<N> associated with the ISP. Each node can be
 * opened only once.
 */
static int bcm2835_isp_open(struct file *file)
{
	struct bcm2835_isp_node *node = video_drvdata(file);
	struct bcm2835_isp_dev *isp_dev = node_get_bcm2835_isp(node);
	struct vb2_queue *queue;
	struct v4l2_ctrl_handler *hdl;
	int ret = 0;

	if (mutex_lock_interruptible(&node->node_lock))
		return -ERESTARTSYS;

	if (node->open) {
		ret = -EBUSY;
		goto unlock_return;
	}

	node->q_data.fmt = get_default_format(node);
	node->q_data.crop_width = DEFAULT_WIDTH;
	node->q_data.crop_height = DEFAULT_HEIGHT;
	node->q_data.height = DEFAULT_HEIGHT;
	node->q_data.bytesperline =
		get_bytesperline(DEFAULT_WIDTH, node->q_data.fmt);
	node->q_data.sizeimage = NODE_IS_STATS(node) ?
				get_port_data(node)->recommended_buffer.size :
				get_sizeimage(node->q_data.bytesperline,
					      node->q_data.crop_width,
					      node->q_data.height,
					      node->q_data.fmt);
	node->colorspace = V4L2_COLORSPACE_REC709;

	v4l2_info(&isp_dev->v4l2_dev, "Opening node %p (%s[%d])\n", node,
		  node->name, node->id);

	v4l2_fh_init(&node->fh, video_devdata(file));
	file->private_data = &node->fh;

	hdl = &node->hdl;
	v4l2_ctrl_handler_init(hdl, 4);
	bcm2835_isp_ctrl_param.def = 0;
	v4l2_ctrl_new_custom(hdl, &bcm2835_isp_ctrl_param, NULL);
	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		goto unlock_return;
	}
	node->fh.ctrl_handler = hdl;
	v4l2_ctrl_handler_setup(hdl);
	v4l2_fh_add(&node->fh);
	node->open = 1;

	queue = &node->queue;
	queue->type = node->v4l_type;
	queue->io_modes = VB2_MMAP | VB2_DMABUF; /* for now */
	queue->drv_priv = node;
	queue->ops = &bcm2835_isp_node_queue_ops;
	queue->mem_ops = &vb2_dma_contig_memops;
	queue->buf_struct_size = sizeof(struct bcm2835_isp_buffer);
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	queue->dev = isp_dev->dev;
	queue->lock = &node->queue_lock; /* get V4L2 to handle queue locking */
	/* Set some controls and defaults, but only on the VIDEO_OUTPUT node. */
	if (NODE_IS_OUTPUT(node)) {
		isp_dev->r_gain = 1000;
		isp_dev->b_gain = 1000;
		v4l2_ctrl_new_std(&node->hdl, &bcm2835_isp_ctrl_ops,
				  V4L2_CID_RED_BALANCE, 1, 7999, 1,
				  isp_dev->r_gain);
		v4l2_ctrl_new_std(&node->hdl, &bcm2835_isp_ctrl_ops,
				  V4L2_CID_BLUE_BALANCE, 1, 7999, 1,
				  isp_dev->b_gain);
		v4l2_ctrl_new_std(&node->hdl, &bcm2835_isp_ctrl_ops,
				  V4L2_CID_DIGITAL_GAIN, 1, 7999, 1, 1000);
	}

	ret = vb2_queue_init(queue);
	if (ret < 0) {
		v4l2_info(&isp_dev->v4l2_dev, "vb2_queue_init failed\n");
		v4l2_fh_del(&node->fh);
		v4l2_fh_exit(&node->fh);
		node->open = 0;
	}

unlock_return:
	mutex_unlock(&node->node_lock);
	return ret;
}

static int bcm2835_isp_release(struct file *file)
{
	struct bcm2835_isp_node *node = video_drvdata(file);

	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Releasing node %p (%s[%d])\n", node, node->name, node->id);

	/* TODO: make sure streamoff was called */

	mutex_lock(&node->node_lock);
	vb2_queue_release(&node->queue);

	v4l2_ctrl_handler_free(&node->hdl);
	v4l2_fh_del(&node->fh);
	v4l2_fh_exit(&node->fh);
	node->open = 0;
	mutex_unlock(&node->node_lock);

	return 0;
}

static unsigned int bcm2835_isp_poll(struct file *file, poll_table *wait)
{
	struct bcm2835_isp_node *node = video_drvdata(file);
	unsigned int ret;

	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Polling %p (%s[%d])\n", node, node->name, node->id);

	/* locking should be handled by the queue->lock? */
	ret = vb2_poll(&node->queue, file, wait);

	return ret;
}

static int bcm2835_isp_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct bcm2835_isp_node *node = video_drvdata(file);
	unsigned int ret;

	v4l2_dbg(1, debug, &node_get_bcm2835_isp(node)->v4l2_dev, "Mmap %p\n",
		 node);
	/* locking should be handled by the queue->lock? */
	ret = vb2_mmap(&node->queue, vma);
	return ret;
}

static const struct v4l2_file_operations bcm2835_isp_fops = {
	.owner		= THIS_MODULE,
	.open		= bcm2835_isp_open,
	.release	= bcm2835_isp_release,
	.poll		= bcm2835_isp_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap		= bcm2835_isp_mmap
};

static void populate_v4l_fmt(struct v4l2_format *f,
			     struct bcm2835_isp_node *node)
{
	struct bcm2835_isp_q_data *q_data = &node->q_data;

	if (NODE_IS_STATS(node)) {
		f->fmt.meta.dataformat = V4L2_META_FMT_STATS;
		f->fmt.meta.buffersize =
				   get_port_data(node)->minimum_buffer.size;
	} else {
		f->fmt.pix.width = q_data->crop_width;
		f->fmt.pix.height = q_data->height;
		f->fmt.pix.field = V4L2_FIELD_NONE;
		f->fmt.pix.pixelformat = q_data->fmt->fourcc;
		f->fmt.pix.bytesperline = q_data->bytesperline;
		f->fmt.pix.sizeimage = q_data->sizeimage;
		f->fmt.pix.colorspace = node->colorspace;
	}
}

static int populate_qdata_fmt(struct v4l2_format *f,
			      struct bcm2835_isp_node *node)
{
	struct bcm2835_isp_dev *isp_dev = node_get_bcm2835_isp(node);
	struct bcm2835_isp_q_data *q_data = &node->q_data;
	struct vchiq_mmal_port *port;
	int ret;

	if (!NODE_IS_STATS(node)) {
		v4l2_dbg(1, debug, &isp_dev->v4l2_dev,
			 "Setting pix format for type %d, wxh: %dx%d, fmt: %08x, size %u\n",
			 f->type, f->fmt.pix.width, f->fmt.pix.height,
			 f->fmt.pix.pixelformat, f->fmt.pix.sizeimage);

		q_data->fmt = find_format(f, node);
		q_data->crop_width = f->fmt.pix.width;
		q_data->height = f->fmt.pix.height;
		q_data->crop_height = f->fmt.pix.height;

		/*
		 * Copying the behaviour of vicodec which retains a single set
		 * of colorspace parameters for both input and output.
		 */
		node->colorspace = f->fmt.pix.colorspace;
		/* All parameters should have been set correctly by try_fmt */
		q_data->bytesperline = f->fmt.pix.bytesperline;
		q_data->sizeimage = f->fmt.pix.sizeimage;
	} else {
		v4l2_dbg(1, debug, &isp_dev->v4l2_dev,
			 "Setting meta format for fmt: %08x, size %u\n",
			 f->fmt.meta.dataformat, f->fmt.meta.buffersize);

		q_data->fmt = find_format(f, node);
		q_data->crop_width = 0;
		q_data->height = 0;
		q_data->bytesperline = 0;
		q_data->sizeimage = f->fmt.meta.buffersize;
	}

	v4l2_dbg(1, debug, &isp_dev->v4l2_dev, "Calculated bpl as %u, size %u\n",
		 q_data->bytesperline, q_data->sizeimage);

	/* If we have a component then setup the port as well */
	port = get_port_data(node);
	if (!port)
		return 0;

	setup_mmal_port_format(node, port);
	ret = vchiq_mmal_port_set_format(isp_dev->mmal_instance, port);
	if (ret) {
		v4l2_err(&isp_dev->v4l2_dev,
			 "%s: Failed vchiq_mmal_port_set_format on port, ret %d\n",
			 __func__, ret);
		ret = -EINVAL;
	}

	if (q_data->sizeimage < port->minimum_buffer.size) {
		v4l2_err(&isp_dev->v4l2_dev,
			 "%s: Current buffer size of %u < min buf size %u - driver mismatch to MMAL\n",
			 __func__, q_data->sizeimage,
			 port->minimum_buffer.size);
	}

	v4l2_dbg(1, debug, &isp_dev->v4l2_dev,
		 "Set format for type %d, wxh: %dx%d, fmt: %08x, size %u\n",
		 f->type, q_data->crop_width, q_data->height,
		 q_data->fmt->fourcc, q_data->sizeimage);

	return ret;
}

static int bcm2835_isp_node_querycap(struct file *file, void *priv,
				     struct v4l2_capability *cap)
{
	struct bcm2835_isp_node *node = video_drvdata(file);

	strscpy(cap->driver, BCM2835_ISP_NAME, sizeof(cap->driver));
	strscpy(cap->card, BCM2835_ISP_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 BCM2835_ISP_NAME);

	cap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OUTPUT |
			    V4L2_CAP_STREAMING | V4L2_CAP_DEVICE_CAPS;

	if (NODE_IS_CAPTURE(node))
		cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	else if (NODE_IS_OUTPUT(node))
		cap->device_caps = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING;
	else
		cap->device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;

	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Caps for node %p: %x and %x\n", priv, cap->capabilities,
		  cap->device_caps);
	return 0;
}

static int bcm2835_isp_node_g_fmt_vid_cap(struct file *file, void *priv,
					  struct v4l2_format *f)
{
	struct bcm2835_isp_node *node = video_drvdata(file);

	if (node->vfl_dir == VFL_DIR_TX || V4L2_TYPE_IS_OUTPUT(f->type)) {
		v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
			  "Cannot get capture format for output node %p\n",
			  node);
		return -EINVAL;
	}
	populate_v4l_fmt(f, node);
	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Get capture format for node %p\n", node);
	return 0;
}

static int bcm2835_isp_node_g_fmt_meta_cap(struct file *file, void *priv,
					   struct v4l2_format *f)
{
	struct bcm2835_isp_node *node = video_drvdata(file);

	if (!NODE_IS_STATS(node))
		return -EINVAL;
	populate_v4l_fmt(f, node);
	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Get meta format for node %p\n", node);
	return 0;
}

static int bcm2835_isp_node_g_fmt_vid_out(struct file *file, void *priv,
					  struct v4l2_format *f)
{
	struct bcm2835_isp_node *node = video_drvdata(file);

	if (node->vfl_dir == VFL_DIR_RX || !V4L2_TYPE_IS_OUTPUT(f->type)) {
		v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
			  "Cannot get output format for capture node %p\n",
			  node);
		return -EINVAL;
	}
	populate_v4l_fmt(f, node);
	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Get output format for node %p\n", node);
	return 0;
}

static int vidioc_try_fmt(struct bcm2835_isp_node *node, struct v4l2_format *f,
			  struct bcm2835_isp_fmt *fmt)
{
	f->fmt.pix.bytesperline = get_bytesperline(f->fmt.pix.width, fmt);
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.sizeimage = get_sizeimage(f->fmt.pix.bytesperline,
					     f->fmt.pix.width,
					     f->fmt.pix.height, fmt);
	return 0;
}

static int bcm2835_isp_node_try_fmt_vid_cap(struct file *file, void *priv,
					    struct v4l2_format *f)
{
	struct bcm2835_isp_fmt *fmt;
	struct bcm2835_isp_node *node = video_drvdata(file);

	fmt = find_format(f, node);
	if (!fmt) {
		f->fmt.pix.pixelformat = get_default_format(node)->fourcc;
		fmt = find_format(f, node);
	}
	return vidioc_try_fmt(node, f, fmt);
}

static int bcm2835_isp_node_try_fmt_meta_cap(struct file *file, void *priv,
					     struct v4l2_format *f)
{
	struct bcm2835_isp_fmt *fmt;
	struct bcm2835_isp_node *node = video_drvdata(file);

	if (!NODE_IS_STATS(node))
		return -EINVAL;

	fmt = find_format(f, node);
	if (!fmt) {
		f->fmt.meta.dataformat = V4L2_META_FMT_STATS;
		fmt = find_format(f, node);
	}
	f->fmt.meta.buffersize = get_port_data(node)->minimum_buffer.size;
	return 0;
}

static int bcm2835_isp_node_try_fmt_vid_out(struct file *file, void *priv,
					    struct v4l2_format *f)
{
	struct bcm2835_isp_fmt *fmt;
	struct bcm2835_isp_node *node = video_drvdata(file);

	fmt = find_format(f, node);
	if (!fmt) {
		f->fmt.pix.pixelformat = get_default_format(node)->fourcc;
		fmt = find_format(f, node);
	}

	if (!f->fmt.pix.colorspace)
		f->fmt.pix.colorspace = node->colorspace;

	return vidioc_try_fmt(node, f, fmt);
}

static int bcm2835_isp_node_s_fmt_vid_cap(struct file *file, void *priv,
					  struct v4l2_format *f)
{
	struct bcm2835_isp_node *node = video_drvdata(file);
	int ret;

	ret = bcm2835_isp_node_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Set capture format for node %p (%s[%d])\n",
		  node, node->name, node->id);
	return populate_qdata_fmt(f, node);
}

static int bcm2835_isp_node_s_fmt_meta_cap(struct file *file, void *priv,
					   struct v4l2_format *f)
{
	struct bcm2835_isp_node *node = video_drvdata(file);
	int ret;

	ret = bcm2835_isp_node_try_fmt_meta_cap(file, priv, f);
	if (ret)
		return ret;

	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Set meta format for node %p (%s[%d])\n",
		  node, node->name, node->id);
	return populate_qdata_fmt(f, node);
}

static int bcm2835_isp_node_s_fmt_vid_out(struct file *file, void *priv,
					  struct v4l2_format *f)
{
	struct bcm2835_isp_node *node = video_drvdata(file);
	int ret;

	ret = bcm2835_isp_node_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Set output format for node %p (%s[%d])\n",
		  node, node->name, node->id);
	return populate_qdata_fmt(f, node);
}

static int bcm2835_isp_node_streamon(struct file *file, void *priv,
				     enum v4l2_buf_type type)
{
	struct bcm2835_isp_node *node = video_drvdata(file);
	int ret;

	/* Do we need a node->stream_lock mutex? */
	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Stream on for node %p\n", node);

	/* locking should be handled by the queue->lock? */
	ret = vb2_streamon(&node->queue, type);

	return ret;
}

static int bcm2835_isp_node_streamoff(struct file *file, void *priv,
				      enum v4l2_buf_type type)
{
	struct bcm2835_isp_node *node = video_drvdata(file);

	/* Do we need a node->stream_lock mutex? */

	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Stream off for node %p\n", node);

	/* Do we care about the type? Each node has only one queue. */

	/* locking should be handled by the queue->lock? */
	vb2_streamoff(&node->queue, type); /* causes any buffers to be returned
					    */

	return 0;
}

static const struct v4l2_ioctl_ops bcm2835_isp_node_ioctl_ops = {
	.vidioc_querycap		= bcm2835_isp_node_querycap,
	.vidioc_g_fmt_vid_cap		= bcm2835_isp_node_g_fmt_vid_cap,
	.vidioc_g_fmt_vid_out		= bcm2835_isp_node_g_fmt_vid_out,
	.vidioc_g_fmt_meta_cap		= bcm2835_isp_node_g_fmt_meta_cap,
	.vidioc_s_fmt_vid_cap		= bcm2835_isp_node_s_fmt_vid_cap,
	.vidioc_s_fmt_vid_out		= bcm2835_isp_node_s_fmt_vid_out,
	.vidioc_s_fmt_meta_cap		= bcm2835_isp_node_s_fmt_meta_cap,
	.vidioc_try_fmt_vid_out		= bcm2835_isp_node_try_fmt_vid_out,
	.vidioc_try_fmt_vid_cap		= bcm2835_isp_node_try_fmt_meta_cap,

	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,

	.vidioc_streamon		= bcm2835_isp_node_streamon,
	.vidioc_streamoff		= bcm2835_isp_node_streamoff,
};

/* Register a device node /dev/video<N> to go along with one of the ISP's input
 * or output nodes.
 */
static int register_node(struct platform_device *pdev,
			 struct bcm2835_isp_node *node,
			 struct bcm2835_isp_node_group *node_group, int index)
{
	struct video_device *vfd;
	int ret;

	mutex_init(&node->node_lock);

	node->open = 0;
	node->type = INDEX_TO_NODE_TYPE(index);
	switch (node->type) {
	case NODE_TYPE_OUTPUT:
		node->v4l_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		node->id = index;
		node->vfl_dir = VFL_DIR_TX;
		node->name = "output";
		break;
	case NODE_TYPE_CAPTURE:
		node->v4l_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		/* First Capture node starts at id 0, etc. */
		node->id = index - BCM2835_ISP_NUM_OUTPUTS;
		node->vfl_dir = VFL_DIR_RX;
		node->name = "capture";
		break;
	case NODE_TYPE_STATS:
		node->v4l_type = V4L2_BUF_TYPE_META_CAPTURE;
		node->id = index - BCM2835_ISP_NUM_OUTPUTS;
		node->vfl_dir = VFL_DIR_RX;
		node->name = "stats";
		break;
	}
	node->node_group = node_group;
	vfd = &node->vfd;

	/* Initialise the the video node... */
	vfd->vfl_type	= VFL_TYPE_GRABBER;
	vfd->fops	= &bcm2835_isp_fops,
	vfd->ioctl_ops	= &bcm2835_isp_node_ioctl_ops,
	vfd->minor	= -1,
	vfd->release	= video_device_release_empty,
	vfd->queue	= &node->queue;
	vfd->lock	= &node->node_lock; /* get V4L2 to serialise our ioctls */
	vfd->v4l2_dev	= &node_group->isp_dev->v4l2_dev;
	vfd->vfl_dir	= node->vfl_dir;

	/* Define the device names */
	snprintf(vfd->name, sizeof(node->vfd.name), "%s-%s%d",
		 BCM2835_ISP_NAME, node->name, node->id);

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, video_nr + index);
	if (ret) {
		v4l2_err(&node_group->isp_dev->v4l2_dev,
			 "Failed to register video %s[%d] device node\n",
			 node->name, node->id);
		return ret;
	}

	video_set_drvdata(vfd, node);

	v4l2_info(&node_group->isp_dev->v4l2_dev,
		  "device node %p (%s[%d]) registered as /dev/video%d\n", node,
		  node->name, node->id, vfd->num);

	return 0;
}

/* Unregister one of the /dev/video<N> nodes associated with the ISP. */
static void unregister_node(struct bcm2835_isp_node *node)
{
	v4l2_info(&node_get_bcm2835_isp(node)->v4l2_dev,
		  "Unregistering node %p (%s[%d]) device node /dev/video%d\n",
		  node, node->name, node->id, node->vfd.num);
	video_unregister_device(&node->vfd);
	/* node->supported_fmts.list is free'd automatically
	 * as a managed resource.
	 */
	node->supported_fmts.list = NULL;
	node->supported_fmts.num_entries = 0;
}

/* Unregister the group of /dev/video<N> nodes that make up a single user of the
 * ISP.
 */
static void unregister_node_group(struct bcm2835_isp_node_group *node_group,
				  int num_nodes)
{
	int i;

	for (i = 0; i < num_nodes; i++)
		unregister_node(&node_group->node[i]);
}

static void media_controller_unregister_node_group(
	struct bcm2835_isp_node_group *node_group, int group, int num_nodes)
{
	int i;

	v4l2_info(&node_group->isp_dev->v4l2_dev,
		  "Unregister node group %p from media controller\n",
		  node_group);

	kfree(node_group->entity.name);
	node_group->entity.name = NULL;

	if (group)
		media_device_unregister_entity(&node_group->entity);

	for (i = 0; i < num_nodes; i++) {
		media_remove_intf_links(node_group->node[i].intf_link->intf);
		media_entity_remove_links(&node_group->node[i].vfd.entity);
		media_devnode_remove(node_group->node[i].intf_devnode);
		media_device_unregister_entity(&node_group->node[i].vfd.entity);
		kfree(node_group->node[i].vfd.entity.name);
	}
}

static void media_controller_unregister(struct bcm2835_isp_dev *isp_dev)
{
	int i;

	v4l2_info(&isp_dev->v4l2_dev, "Unregister from media controller\n");
	media_device_unregister(&isp_dev->mdev);

	for (i = 0; i < BCM2835_ISP_NUM_NODE_GROUPS; i++)
		media_controller_unregister_node_group(&isp_dev->node_group[i],
						       1,
						       BCM2835_ISP_NUM_NODES);

	media_device_cleanup(&isp_dev->mdev);
	isp_dev->v4l2_dev.mdev = NULL;
}

static int
media_controller_register_node(struct bcm2835_isp_node_group *node_group, int i,
			       int group_num)
{
	struct bcm2835_isp_node *node = &node_group->node[i];
	struct media_entity *entity = &node->vfd.entity;
	int output = NODE_IS_OUTPUT(node);
	int ret;
	char *name;

	v4l2_info(&node_group->isp_dev->v4l2_dev,
		  "Register %s node %d with media controller\n",
		  output ? "output" : "capture", i);
	entity->obj_type = MEDIA_ENTITY_TYPE_VIDEO_DEVICE;
	entity->function = MEDIA_ENT_F_IO_V4L;
	entity->info.dev.major = VIDEO_MAJOR;
	entity->info.dev.minor = node->vfd.minor;
	name = kmalloc(BCM2835_ISP_ENTITY_NAME_LEN, GFP_KERNEL);
	if (!name) {
		ret = -ENOMEM;
		goto error_no_mem;
	}
	snprintf(name, BCM2835_ISP_ENTITY_NAME_LEN, "%s%d-%s%d", BCM2835_ISP_NAME,
		 group_num, output ? "output" : "capture", i);
	entity->name = name;
	node->pad.flags = output ? MEDIA_PAD_FL_SOURCE : MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(entity, 1, &node->pad);
	if (ret)
		goto error_pads_init;
	ret = media_device_register_entity(&node_group->isp_dev->mdev, entity);
	if (ret)
		goto error_register_entity;

	node->intf_devnode = media_devnode_create(&node_group->isp_dev->mdev,
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
		ret = media_create_pad_link(entity, 0, &node_group->entity, i,
					    MEDIA_LNK_FL_IMMUTABLE |
						    MEDIA_LNK_FL_ENABLED);
	else
		ret = media_create_pad_link(&node_group->entity, i, entity, 0,
					    MEDIA_LNK_FL_IMMUTABLE |
						    MEDIA_LNK_FL_ENABLED);
	if (ret)
		goto error_create_pad_link;

	return 0;

error_create_pad_link:
	media_remove_intf_links(&node->intf_devnode->intf);
error_create_intf_link:
	media_devnode_remove(node->intf_devnode);
error_devnode_create:
error_register_entity:
error_pads_init:
	kfree(entity->name);
	entity->name = NULL;
error_no_mem:
	if (ret)
		v4l2_info(&node_group->isp_dev->v4l2_dev,
			  "Error registering node\n");
	return ret;
}

static int media_controller_register(struct bcm2835_isp_dev *isp_dev)
{
	int num_registered = 0;
	int num_groups_registered = 0;
	int group_registered = 0;
	int ret;
	int i;

	v4l2_info(&isp_dev->v4l2_dev, "Registering with media controller\n");
	isp_dev->mdev.dev = isp_dev->dev;
	strscpy(isp_dev->mdev.model, "bcm2835_isp",
		sizeof(isp_dev->mdev.model));
	strscpy(isp_dev->mdev.bus_info, "platform:bcm2835_isp",
		sizeof(isp_dev->mdev.bus_info));
	media_device_init(&isp_dev->mdev);
	isp_dev->v4l2_dev.mdev = &isp_dev->mdev;

	for (; num_groups_registered < BCM2835_ISP_NUM_NODE_GROUPS;
	     num_groups_registered++) {
		struct bcm2835_isp_node_group *node_group =
			&isp_dev->node_group[num_groups_registered];
		char *name = kmalloc(BCM2835_ISP_ENTITY_NAME_LEN, GFP_KERNEL);

		v4l2_info(&isp_dev->v4l2_dev,
			  "Register entity for node group %d\n",
			  num_groups_registered);
		node_group->entity.name = name;
		if (!name) {
			ret = -ENOMEM;
			goto done;
		}
		snprintf(name, BCM2835_ISP_ENTITY_NAME_LEN, "bcm2835_isp%d",
			 num_groups_registered);
		node_group->entity.obj_type = MEDIA_ENTITY_TYPE_BASE;
		node_group->entity.function =
			MEDIA_ENT_F_PROC_VIDEO_SCALER;
		for (i = 0; i < BCM2835_ISP_NUM_NODES; i++)
			node_group->pad[i].flags =
				NODE_IS_OUTPUT(&node_group->node[i]) ?
					MEDIA_PAD_FL_SINK :
					MEDIA_PAD_FL_SOURCE;
		ret = media_entity_pads_init(&node_group->entity,
					     BCM2835_ISP_NUM_NODES,
					     node_group->pad);
		if (ret)
			goto done;
		ret = media_device_register_entity(&isp_dev->mdev,
						   &node_group->entity);
		if (ret)
			goto done;
		group_registered = 1;

		for (; num_registered < BCM2835_ISP_NUM_NODES;
		     num_registered++) {
			ret = media_controller_register_node(
				node_group, num_registered,
				num_groups_registered);
			if (ret)
				goto done;
		}

		num_registered = 0;
		group_registered = 0;
	}

	ret = media_device_register(&isp_dev->mdev);
	if (ret)
		goto done;

done:
	if (ret) {
		if (num_groups_registered < BCM2835_ISP_NUM_NODE_GROUPS)
			media_controller_unregister_node_group(
				&isp_dev->node_group[num_groups_registered],
				group_registered, num_registered);
		while (--num_groups_registered >= 0)
			media_controller_unregister_node_group(
				&isp_dev->node_group[num_groups_registered], 1,
				BCM2835_ISP_NUM_NODES);
	}

	return ret;
}

/* Size of the array to provide to the VPU when asking for the list of supported
 * formats.
 * The ISP component currently advertises 33 input formats, so add a small
 * overhead on that.
 */
#define MAX_SUPPORTED_ENCODINGS 40

/* Populate node->supported_fmts with the formats supported by those ports. */
static int bcm2835_isp_get_supported_fmts(struct bcm2835_isp_node *node)
{
	struct bcm2835_isp_dev *isp_dev = node_get_bcm2835_isp(node);
	struct bcm2835_isp_fmt *list;
	u32 fourccs[MAX_SUPPORTED_ENCODINGS];
	u32 param_size = sizeof(fourccs);
	unsigned int i, j, num_encodings;
	int ret;

	ret = vchiq_mmal_port_parameter_get(isp_dev->mmal_instance,
					    get_port_data(node),
					    MMAL_PARAMETER_SUPPORTED_ENCODINGS,
					    &fourccs, &param_size);

	if (ret) {
		if (ret == MMAL_MSG_STATUS_ENOSPC) {
			v4l2_err(&isp_dev->v4l2_dev,
				 "%s: port has more encoding than we provided space for. Some are dropped.\n",
				 __func__);
			num_encodings = MAX_SUPPORTED_ENCODINGS;
		} else {
			v4l2_err(&isp_dev->v4l2_dev, "%s: get_param ret %u.\n",
				 __func__, ret);
			return -EINVAL;
		}
	} else {
		num_encodings = param_size / sizeof(u32);
	}

	/* Assume at this stage that all encodings will be supported in V4L2.
	 * Any that aren't supported will waste a very small amount of memory.
	 */
	list = devm_kzalloc(isp_dev->dev,
			    sizeof(struct bcm2835_isp_fmt) * num_encodings,
			    GFP_KERNEL);
	if (!list)
		return -ENOMEM;
	node->supported_fmts.list = list;

	for (i = 0, j = 0; i < num_encodings; i++) {
		const struct bcm2835_isp_fmt *fmt = get_fmt(fourccs[i]);

		if (fmt) {
			list[j] = *fmt;
			j++;
		}
	}
	node->supported_fmts.num_entries = j;

	param_size = sizeof(fourccs);
	ret = vchiq_mmal_port_parameter_get(isp_dev->mmal_instance,
					    get_port_data(node),
					    MMAL_PARAMETER_SUPPORTED_ENCODINGS,
					    &fourccs, &param_size);

	if (ret) {
		if (ret == MMAL_MSG_STATUS_ENOSPC) {
			v4l2_err(&isp_dev->v4l2_dev,
				 "%s: port has more encoding than we provided space for. Some are dropped.\n",
				 __func__);
			num_encodings = MAX_SUPPORTED_ENCODINGS;
		} else {
			return -EINVAL;
		}
	} else {
		num_encodings = param_size / sizeof(u32);
	}
	/* Assume at this stage that all encodings will be supported in V4L2. */
	list = devm_kzalloc(isp_dev->dev,
			    sizeof(struct bcm2835_isp_fmt) * num_encodings,
			    GFP_KERNEL);
	if (!list)
		return -ENOMEM;
	node->supported_fmts.list = list;

	for (i = 0, j = 0; i < num_encodings; i++) {
		const struct bcm2835_isp_fmt *fmt = get_fmt(fourccs[i]);

		if (fmt) {
			list[j] = *fmt;
			j++;
		}
	}
	node->supported_fmts.num_entries = j;
	return 0;
}

static int bcm2835_isp_probe(struct platform_device *pdev)
{
	struct bcm2835_isp_dev *isp_dev;
	int ret;
	int num_nodes_registered = 0;
	int num_groups_registered = 0;

	isp_dev = devm_kzalloc(&pdev->dev, sizeof(*isp_dev), GFP_KERNEL);
	if (!isp_dev)
		return -ENOMEM;

	isp_dev->dev = &pdev->dev;
	ret = v4l2_device_register(&pdev->dev, &isp_dev->v4l2_dev);
	if (ret)
		return ret;
	ret = vchiq_mmal_init(&isp_dev->mmal_instance);
	if (ret)
		return ret;
	ret = vchiq_mmal_component_init(isp_dev->mmal_instance, "ril.isp",
					&isp_dev->component);
	if (ret) {
		v4l2_err(&isp_dev->v4l2_dev,
			 "%s: failed to create ril.isp component\n", __func__);
		goto vchiq_finalise;
	}

	for (; num_groups_registered < BCM2835_ISP_NUM_NODE_GROUPS;
	     num_groups_registered++) {
		struct bcm2835_isp_node_group *node_group =
			&isp_dev->node_group[num_groups_registered];
		node_group->isp_dev = isp_dev;
		atomic_set(&node_group->num_streaming, 0);
		v4l2_info(&isp_dev->v4l2_dev, "Register nodes for group %d\n",
			  num_groups_registered);

		for (; num_nodes_registered < BCM2835_ISP_NUM_NODES;
		     num_nodes_registered++) {
			ret = register_node(pdev,
				&node_group->node[num_nodes_registered],
				node_group, num_nodes_registered);
			if (ret)
				goto done;
			ret = bcm2835_isp_get_supported_fmts(
				&node_group->node[num_nodes_registered]);
			if (ret)
				goto done;
		}

		num_nodes_registered = 0;
	}

	ret = media_controller_register(isp_dev);
	if (ret)
		goto vchiq_finalise;

	platform_set_drvdata(pdev, isp_dev);
	v4l2_info(&isp_dev->v4l2_dev, "Loaded V4L2 %s\n", BCM2835_ISP_NAME);
	return 0;

done:
	if (ret) {
		if (num_groups_registered < BCM2835_ISP_NUM_NODE_GROUPS)
			unregister_node_group(
				&isp_dev->node_group[num_groups_registered],
				num_nodes_registered);
		while (--num_groups_registered >= 0)
			unregister_node_group(
				&isp_dev->node_group[num_groups_registered],
				BCM2835_ISP_NUM_NODES);

		media_device_cleanup(&isp_dev->mdev);
		isp_dev->v4l2_dev.mdev = NULL;

		v4l2_device_unregister(&isp_dev->v4l2_dev);
	}
vchiq_finalise:
	if (isp_dev->component)
		vchiq_mmal_component_finalise(isp_dev->mmal_instance,
					      isp_dev->component);
	vchiq_mmal_finalise(isp_dev->mmal_instance);

	return ret;
}

static int bcm2835_isp_remove(struct platform_device *pdev)
{
	int i;
	struct bcm2835_isp_dev *isp_dev = platform_get_drvdata(pdev);

	media_controller_unregister(isp_dev);

	for (i = 0; i < BCM2835_ISP_NUM_NODE_GROUPS; i++)
		unregister_node_group(&isp_dev->node_group[i],
				      BCM2835_ISP_NUM_NODES);

	v4l2_device_unregister(&isp_dev->v4l2_dev);

	if (isp_dev->component)
		vchiq_mmal_component_finalise(isp_dev->mmal_instance,
					      isp_dev->component);

	vchiq_mmal_finalise(isp_dev->mmal_instance);

	return 0;
}

static struct platform_driver bcm2835_isp_pdrv = {
	.probe = bcm2835_isp_probe,
	.remove = bcm2835_isp_remove,
	.driver = {
			.name = BCM2835_ISP_NAME,
			.owner = THIS_MODULE,
		  },
};

module_platform_driver(bcm2835_isp_pdrv);

MODULE_DESCRIPTION("BCM2835 ISP driver");
MODULE_AUTHOR("Naushir Patuck");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_ALIAS("platform:bcm2835-isp");
