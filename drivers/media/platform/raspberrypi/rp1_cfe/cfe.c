// SPDX-License-Identifier: GPL-2.0-only
/*
 * RP1 Camera Front End Driver
 *
 * Copyright (C) 2021-2022 - Raspberry Pi Ltd.
 *
 */

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/phy/phy.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/videodev2.h>

#include <media/v4l2-async.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "cfe.h"
#include "cfe_fmts.h"
#include "csi2.h"
#include "pisp_fe.h"
#include "pisp_fe_config.h"
#include "pisp_statistics.h"

#define CFE_MODULE_NAME	"rp1-cfe"
#define CFE_VERSION	"1.0"

bool cfe_debug_verbose;
module_param_named(verbose_debug, cfe_debug_verbose, bool, 0644);
MODULE_PARM_DESC(verbose_debug, "verbose debugging messages");

#define cfe_dbg_verbose(fmt, arg...)                          \
	do {                                                  \
		if (cfe_debug_verbose)                        \
			dev_dbg(&cfe->pdev->dev, fmt, ##arg); \
	} while (0)
#define cfe_dbg(fmt, arg...) dev_dbg(&cfe->pdev->dev, fmt, ##arg)
#define cfe_info(fmt, arg...) dev_info(&cfe->pdev->dev, fmt, ##arg)
#define cfe_err(fmt, arg...) dev_err(&cfe->pdev->dev, fmt, ##arg)

/* MIPICFG registers */
#define MIPICFG_CFG		0x004
#define MIPICFG_INTR		0x028
#define MIPICFG_INTE		0x02c
#define MIPICFG_INTF		0x030
#define MIPICFG_INTS		0x034

#define MIPICFG_CFG_SEL_CSI	BIT(0)

#define MIPICFG_INT_CSI_DMA	BIT(0)
#define MIPICFG_INT_CSI_HOST	BIT(2)
#define MIPICFG_INT_PISP_FE	BIT(4)

#define BPL_ALIGNMENT 16
#define MAX_BYTESPERLINE 0xffffff00
#define MAX_BUFFER_SIZE  0xffffff00
/*
 * Max width is therefore determined by the max stride divided by the number of
 * bits per pixel.
 *
 * However, to avoid overflow issues let's use a 16k maximum. This lets us
 * calculate 16k * 16k * 4 with 32bits. If we need higher maximums, a careful
 * review and adjustment of the code is needed so that it will deal with
 * overflows correctly.
 */
#define MAX_WIDTH 16384
#define MAX_HEIGHT MAX_WIDTH
/* Define a nominal minimum image size */
#define MIN_WIDTH 16
#define MIN_HEIGHT 16
/* Default size of the embedded buffer */
#define DEFAULT_EMBEDDED_SIZE 16384

const struct v4l2_mbus_framefmt cfe_default_format = {
	.width = 640,
	.height = 480,
	.code = MEDIA_BUS_FMT_SRGGB10_1X10,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_RAW,
	.ycbcr_enc = V4L2_YCBCR_ENC_601,
	.quantization = V4L2_QUANTIZATION_FULL_RANGE,
	.xfer_func = V4L2_XFER_FUNC_NONE,
};

const struct v4l2_mbus_framefmt cfe_default_meta_format = {
	.width = DEFAULT_EMBEDDED_SIZE,
	.height = 1,
	.code = MEDIA_BUS_FMT_SENSOR_DATA,
	.field = V4L2_FIELD_NONE,
};

enum node_ids {
	/* CSI2 HW output nodes first. */
	CSI2_CH0,
	CSI2_CH1,
	CSI2_CH2,
	CSI2_CH3,
	/* FE only nodes from here on. */
	FE_OUT0,
	FE_OUT1,
	FE_STATS,
	FE_CONFIG,
	NUM_NODES
};

struct node_description {
	unsigned int id;
	const char *name;
	unsigned int caps;
	unsigned int pad_flags;
	unsigned int link_pad;
};

/* Must match the ordering of enum ids */
static const struct node_description node_desc[NUM_NODES] = {
	[CSI2_CH0] = {
		.name = "csi2_ch0",
		.caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_META_CAPTURE,
		.pad_flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT,
		.link_pad = CSI2_NUM_CHANNELS + 0
	},
	/*
	 * TODO: This node should be named "csi2_ch1" and the caps should be set
	 * to both video and meta capture. However, to keep compatibility with
	 * the current libcamera, keep the name as "embedded" and support
	 * only meta capture.
	 */
	[CSI2_CH1] = {
		.name = "embedded",
		.caps = V4L2_CAP_META_CAPTURE,
		.pad_flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT,
		.link_pad = CSI2_NUM_CHANNELS + 1
	},
	[CSI2_CH2] = {
		.name = "csi2_ch2",
		.caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_META_CAPTURE,
		.pad_flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT,
		.link_pad = CSI2_NUM_CHANNELS + 2
	},
	[CSI2_CH3] = {
		.name = "csi2_ch3",
		.caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_META_CAPTURE,
		.pad_flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT,
		.link_pad = CSI2_NUM_CHANNELS + 3
	},
	[FE_OUT0] = {
		.name = "fe_image0",
		.caps = V4L2_CAP_VIDEO_CAPTURE,
		.pad_flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT,
		.link_pad = FE_OUTPUT0_PAD
	},
	[FE_OUT1] = {
		.name = "fe_image1",
		.caps = V4L2_CAP_VIDEO_CAPTURE,
		.pad_flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT,
		.link_pad = FE_OUTPUT1_PAD
	},
	[FE_STATS] = {
		.name = "fe_stats",
		.caps = V4L2_CAP_META_CAPTURE,
		.pad_flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT,
		.link_pad = FE_STATS_PAD
	},
	[FE_CONFIG] = {
		.name = "fe_config",
		.caps = V4L2_CAP_META_OUTPUT,
		.pad_flags = MEDIA_PAD_FL_SOURCE | MEDIA_PAD_FL_MUST_CONNECT,
		.link_pad = FE_CONFIG_PAD
	},
};

#define is_fe_node(node) (((node)->id) >= FE_OUT0)
#define is_csi2_node(node) (!is_fe_node(node))

#define node_supports_image_output(node) \
	(!!(node_desc[(node)->id].caps & V4L2_CAP_VIDEO_CAPTURE))
#define node_supports_meta_output(node) \
	(!!(node_desc[(node)->id].caps & V4L2_CAP_META_CAPTURE))
#define node_supports_image_input(node) \
	(!!(node_desc[(node)->id].caps & V4L2_CAP_VIDEO_OUTPUT))
#define node_supports_meta_input(node) \
	(!!(node_desc[(node)->id].caps & V4L2_CAP_META_OUTPUT))
#define node_supports_image(node) \
	(node_supports_image_output(node) || node_supports_image_input(node))
#define node_supports_meta(node) \
	(node_supports_meta_output(node) || node_supports_meta_input(node))

#define is_image_output_node(node) \
	((node)->buffer_queue.type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
#define is_image_input_node(node) \
	((node)->buffer_queue.type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
#define is_image_node(node) \
	(is_image_output_node(node) || is_image_input_node(node))
#define is_meta_output_node(node) \
	((node)->buffer_queue.type == V4L2_BUF_TYPE_META_CAPTURE)
#define is_meta_input_node(node) \
	((node)->buffer_queue.type == V4L2_BUF_TYPE_META_OUTPUT)
#define is_meta_node(node) \
	(is_meta_output_node(node) || is_meta_input_node(node))

/* To track state across all nodes. */
#define NUM_STATES		5
#define NODE_REGISTERED		BIT(0)
#define NODE_ENABLED		BIT(1)
#define NODE_STREAMING		BIT(2)
#define FS_INT			BIT(3)
#define FE_INT			BIT(4)

struct cfe_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct cfe_config_buffer {
	struct cfe_buffer buf;
	struct pisp_fe_config config;
};

static inline struct cfe_buffer *to_cfe_buffer(struct vb2_buffer *vb)
{
	return container_of(vb, struct cfe_buffer, vb.vb2_buf);
}

static inline
struct cfe_config_buffer *to_cfe_config_buffer(struct cfe_buffer *buf)
{
	return container_of(buf, struct cfe_config_buffer, buf);
}

struct cfe_node {
	unsigned int id;
	/* Pointer pointing to current v4l2_buffer */
	struct cfe_buffer *cur_frm;
	/* Pointer pointing to next v4l2_buffer */
	struct cfe_buffer *next_frm;
	/* Used to store current pixel format */
	struct v4l2_format vid_fmt;
	/* Used to store current meta format */
	struct v4l2_format meta_fmt;
	/* Buffer queue used in video-buf */
	struct vb2_queue buffer_queue;
	/* Queue of filled frames */
	struct list_head dma_queue;
	/* lock used to access this structure */
	struct mutex lock;
	/* Identifies video device for this channel */
	struct video_device video_dev;
	/* Pointer to the parent handle */
	struct cfe_device *cfe;
	struct media_pad pad;
	unsigned int fs_count;
	u64 ts;
};

struct cfe_device {
	struct dentry *debugfs;
	struct kref kref;

	/* V4l2 specific parameters */
	struct v4l2_async_connection *asd;

	/* peripheral base address */
	void __iomem *mipi_cfg_base;

	struct clk *clk;

	/* V4l2 device */
	struct v4l2_device v4l2_dev;
	struct media_device mdev;
	struct media_pipeline pipe;

	/* IRQ lock for node state and DMA queues */
	spinlock_t state_lock;
	bool job_ready;
	bool job_queued;

	/* parent device */
	struct platform_device *pdev;
	/* subdevice async Notifier */
	struct v4l2_async_notifier notifier;

	/* ptr to sub device */
	struct v4l2_subdev *sensor;

	struct cfe_node node[NUM_NODES];
	DECLARE_BITMAP(node_flags, NUM_STATES * NUM_NODES);

	struct csi2_device csi2;
	struct pisp_fe_device fe;

	int fe_csi2_channel;
};

static inline bool is_fe_enabled(struct cfe_device *cfe)
{
	return cfe->fe_csi2_channel != -1;
}

static inline struct cfe_device *to_cfe_device(struct v4l2_device *v4l2_dev)
{
	return container_of(v4l2_dev, struct cfe_device, v4l2_dev);
}

static inline u32 cfg_reg_read(struct cfe_device *cfe, u32 offset)
{
	return readl(cfe->mipi_cfg_base + offset);
}

static inline void cfg_reg_write(struct cfe_device *cfe, u32 offset, u32 val)
{
	writel(val, cfe->mipi_cfg_base + offset);
}

static bool check_state(struct cfe_device *cfe, unsigned long state,
			unsigned int node_id)
{
	unsigned long bit;

	for_each_set_bit(bit, &state, sizeof(state)) {
		if (!test_bit(bit + (node_id * NUM_STATES), cfe->node_flags))
			return false;
	}
	return true;
}

static void set_state(struct cfe_device *cfe, unsigned long state,
		      unsigned int node_id)
{
	unsigned long bit;

	for_each_set_bit(bit, &state, sizeof(state))
		set_bit(bit + (node_id * NUM_STATES), cfe->node_flags);
}

static void clear_state(struct cfe_device *cfe, unsigned long state,
			unsigned int node_id)
{
	unsigned long bit;

	for_each_set_bit(bit, &state, sizeof(state))
		clear_bit(bit + (node_id * NUM_STATES), cfe->node_flags);
}

static bool test_any_node(struct cfe_device *cfe, unsigned long cond)
{
	unsigned int i;

	for (i = 0; i < NUM_NODES; i++) {
		if (check_state(cfe, cond, i))
			return true;
	}

	return false;
}

static bool test_all_nodes(struct cfe_device *cfe, unsigned long precond,
			   unsigned long cond)
{
	unsigned int i;

	for (i = 0; i < NUM_NODES; i++) {
		if (check_state(cfe, precond, i)) {
			if (!check_state(cfe, cond, i))
				return false;
		}
	}

	return true;
}

static int mipi_cfg_regs_show(struct seq_file *s, void *data)
{
	struct cfe_device *cfe = s->private;
	int ret;

	ret = pm_runtime_resume_and_get(&cfe->pdev->dev);
	if (ret)
		return ret;

#define DUMP(reg) seq_printf(s, #reg " \t0x%08x\n", cfg_reg_read(cfe, reg))
	DUMP(MIPICFG_CFG);
	DUMP(MIPICFG_INTR);
	DUMP(MIPICFG_INTE);
	DUMP(MIPICFG_INTF);
	DUMP(MIPICFG_INTS);
#undef DUMP

	pm_runtime_put(&cfe->pdev->dev);

	return 0;
}

static int format_show(struct seq_file *s, void *data)
{
	struct cfe_device *cfe = s->private;
	unsigned int i;

	for (i = 0; i < NUM_NODES; i++) {
		struct cfe_node *node = &cfe->node[i];
		unsigned long sb, state = 0;

		for (sb = 0; sb < NUM_STATES; sb++) {
			if (check_state(cfe, BIT(sb), i))
				state |= BIT(sb);
		}

		seq_printf(s, "\nNode %u (%s) state: 0x%lx\n", i,
			   node_desc[i].name, state);

		if (node_supports_image(node))
			seq_printf(s, "format: " V4L2_FOURCC_CONV " 0x%x\n"
				      "resolution: %ux%u\nbpl: %u\nsize: %u\n",
				   V4L2_FOURCC_CONV_ARGS(node->vid_fmt.fmt.pix.pixelformat),
				   node->vid_fmt.fmt.pix.pixelformat,
				   node->vid_fmt.fmt.pix.width,
				   node->vid_fmt.fmt.pix.height,
				   node->vid_fmt.fmt.pix.bytesperline,
				   node->vid_fmt.fmt.pix.sizeimage);

		if (node_supports_meta(node))
			seq_printf(s, "format: " V4L2_FOURCC_CONV " 0x%x\nsize: %u\n",
				   V4L2_FOURCC_CONV_ARGS(node->meta_fmt.fmt.meta.dataformat),
				   node->meta_fmt.fmt.meta.dataformat,
				   node->meta_fmt.fmt.meta.buffersize);
	}

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mipi_cfg_regs);
DEFINE_SHOW_ATTRIBUTE(format);

/* Format setup functions */
const struct cfe_fmt *find_format_by_code(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		if (formats[i].code == code)
			return &formats[i];
	}

	return NULL;
}

const struct cfe_fmt *find_format_by_pix(u32 pixelformat)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		if (formats[i].fourcc == pixelformat)
			return &formats[i];
	}

	return NULL;
}

/*
 * Given the mbus code, find the 16 bit remapped code. Returns 0 if no remap
 * possible.
 */
u32 cfe_find_16bit_code(u32 code)
{
	const struct cfe_fmt *cfe_fmt;

	cfe_fmt = find_format_by_code(code);

	if (!cfe_fmt || !cfe_fmt->remap[CFE_REMAP_16BIT])
		return 0;

	cfe_fmt = find_format_by_pix(cfe_fmt->remap[CFE_REMAP_16BIT]);
	if (!cfe_fmt)
		return 0;

	return cfe_fmt->code;
}

/*
 * Given the mbus code, find the 8 bit compressed code. Returns 0 if no remap
 * possible.
 */
u32 cfe_find_compressed_code(u32 code)
{
	const struct cfe_fmt *cfe_fmt;

	cfe_fmt = find_format_by_code(code);

	if (!cfe_fmt || !cfe_fmt->remap[CFE_REMAP_COMPRESSED])
		return 0;

	cfe_fmt = find_format_by_pix(cfe_fmt->remap[CFE_REMAP_COMPRESSED]);
	if (!cfe_fmt)
		return 0;

	return cfe_fmt->code;
}

static int cfe_calc_format_size_bpl(struct cfe_device *cfe,
				    const struct cfe_fmt *fmt,
				    struct v4l2_format *f)
{
	unsigned int min_bytesperline;

	v4l_bound_align_image(&f->fmt.pix.width, MIN_WIDTH, MAX_WIDTH, 2,
			      &f->fmt.pix.height, MIN_HEIGHT, MAX_HEIGHT, 0, 0);

	min_bytesperline =
		ALIGN((f->fmt.pix.width * fmt->depth) >> 3, BPL_ALIGNMENT);

	if (f->fmt.pix.bytesperline > min_bytesperline &&
	    f->fmt.pix.bytesperline <= MAX_BYTESPERLINE)
		f->fmt.pix.bytesperline =
			ALIGN(f->fmt.pix.bytesperline, BPL_ALIGNMENT);
	else
		f->fmt.pix.bytesperline = min_bytesperline;

	f->fmt.pix.sizeimage = f->fmt.pix.height * f->fmt.pix.bytesperline;

	cfe_dbg("%s: " V4L2_FOURCC_CONV " size: %ux%u bpl:%u img_size:%u\n",
		__func__, V4L2_FOURCC_CONV_ARGS(f->fmt.pix.pixelformat),
		f->fmt.pix.width, f->fmt.pix.height,
		f->fmt.pix.bytesperline, f->fmt.pix.sizeimage);

	return 0;
}

static void cfe_schedule_next_csi2_job(struct cfe_device *cfe)
{
	struct cfe_buffer *buf;
	unsigned int i;
	dma_addr_t addr;

	for (i = 0; i < CSI2_NUM_CHANNELS; i++) {
		struct cfe_node *node = &cfe->node[i];
		unsigned int stride, size;

		if (!check_state(cfe, NODE_STREAMING, i))
			continue;

		buf = list_first_entry(&node->dma_queue, struct cfe_buffer,
				       list);
		node->next_frm = buf;
		list_del(&buf->list);

		cfe_dbg_verbose("%s: [%s] buffer:%p\n", __func__,
				node_desc[node->id].name, &buf->vb.vb2_buf);

		if (is_meta_node(node)) {
			size = node->meta_fmt.fmt.meta.buffersize;
			stride = 0;
		} else {
			size = node->vid_fmt.fmt.pix.sizeimage;
			stride = node->vid_fmt.fmt.pix.bytesperline;
		}

		addr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
		csi2_set_buffer(&cfe->csi2, node->id, addr, stride, size);
	}
}

static void cfe_schedule_next_pisp_job(struct cfe_device *cfe)
{
	struct vb2_buffer *vb2_bufs[FE_NUM_PADS] = { 0 };
	struct cfe_config_buffer *config_buf;
	struct cfe_buffer *buf;
	unsigned int i;

	for (i = CSI2_NUM_CHANNELS; i < NUM_NODES; i++) {
		struct cfe_node *node = &cfe->node[i];

		if (!check_state(cfe, NODE_STREAMING, i))
			continue;

		buf = list_first_entry(&node->dma_queue, struct cfe_buffer,
				       list);

		cfe_dbg_verbose("%s: [%s] buffer:%p\n", __func__,
				node_desc[node->id].name, &buf->vb.vb2_buf);

		node->next_frm = buf;
		vb2_bufs[node_desc[i].link_pad] = &buf->vb.vb2_buf;
		list_del(&buf->list);
	}

	config_buf = to_cfe_config_buffer(cfe->node[FE_CONFIG].next_frm);
	pisp_fe_submit_job(&cfe->fe, vb2_bufs, &config_buf->config);
}

static bool cfe_check_job_ready(struct cfe_device *cfe)
{
	unsigned int i;

	for (i = 0; i < NUM_NODES; i++) {
		struct cfe_node *node = &cfe->node[i];

		if (!check_state(cfe, NODE_ENABLED, i))
			continue;

		if (list_empty(&node->dma_queue)) {
			cfe_dbg_verbose("%s: [%s] has no buffer, unable to schedule job\n",
				__func__, node_desc[i].name);
			return false;
		}
	}

	return true;
}

static void cfe_prepare_next_job(struct cfe_device *cfe)
{
	cfe->job_queued = true;
	cfe_schedule_next_csi2_job(cfe);
	if (is_fe_enabled(cfe))
		cfe_schedule_next_pisp_job(cfe);

	/* Flag if another job is ready after this. */
	cfe->job_ready = cfe_check_job_ready(cfe);

	cfe_dbg_verbose("%s: end with scheduled job\n", __func__);
}

static void cfe_process_buffer_complete(struct cfe_node *node,
					enum vb2_buffer_state state)
{
	struct cfe_device *cfe = node->cfe;

	cfe_dbg_verbose("%s: [%s] buffer:%p\n", __func__,
			node_desc[node->id].name, &node->cur_frm->vb.vb2_buf);

	node->cur_frm->vb.sequence = node->fs_count - 1;
	vb2_buffer_done(&node->cur_frm->vb.vb2_buf, state);
}

static void cfe_queue_event_sof(struct cfe_node *node)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_FRAME_SYNC,
		.u.frame_sync.frame_sequence = node->fs_count - 1,
	};

	v4l2_event_queue(&node->video_dev, &event);
}

static void cfe_sof_isr_handler(struct cfe_node *node)
{
	struct cfe_device *cfe = node->cfe;
	bool matching_fs = true;
	unsigned int i;

	cfe_dbg_verbose("%s: [%s] seq %u\n", __func__, node_desc[node->id].name,
			node->fs_count);

	/*
	 * If the sensor is producing unexpected frame event ordering over a
	 * sustained period of time, guard against the possibility of coming
	 * here and orphaning the cur_frm if it's not been dequeued already.
	 * Unfortunately, there is not enough hardware state to tell if this
	 * may have occurred.
	 */
	if (WARN(node->cur_frm, "%s: [%s] Orphanded frame at seq %u\n",
		 __func__, node_desc[node->id].name, node->fs_count))
		cfe_process_buffer_complete(node, VB2_BUF_STATE_ERROR);

	node->cur_frm = node->next_frm;
	node->next_frm = NULL;
	node->fs_count++;

	node->ts = ktime_get_ns();
	for (i = 0; i < NUM_NODES; i++) {
		if (!check_state(cfe, NODE_STREAMING, i) || i == node->id)
			continue;
		/*
		 * This checks if any other node has seen a FS. If yes, use the
		 * same timestamp, eventually across all node buffers.
		 */
		if (cfe->node[i].fs_count >= node->fs_count)
			node->ts = cfe->node[i].ts;
		/*
		 * This checks if all other node have seen a matching FS. If
		 * yes, we can flag another job to be queued.
		 */
		if (matching_fs && cfe->node[i].fs_count != node->fs_count)
			matching_fs = false;
	}

	if (matching_fs)
		cfe->job_queued = false;

	if (node->cur_frm)
		node->cur_frm->vb.vb2_buf.timestamp = node->ts;

	set_state(cfe, FS_INT, node->id);
	clear_state(cfe, FE_INT, node->id);

	if (is_image_output_node(node))
		cfe_queue_event_sof(node);
}

static void cfe_eof_isr_handler(struct cfe_node *node)
{
	struct cfe_device *cfe = node->cfe;

	cfe_dbg_verbose("%s: [%s] seq %u\n", __func__, node_desc[node->id].name,
			node->fs_count - 1);

	if (node->cur_frm)
		cfe_process_buffer_complete(node, VB2_BUF_STATE_DONE);

	node->cur_frm = NULL;
	set_state(cfe, FE_INT, node->id);
	clear_state(cfe, FS_INT, node->id);
}

static irqreturn_t cfe_isr(int irq, void *dev)
{
	struct cfe_device *cfe = dev;
	unsigned int i;
	bool sof[NUM_NODES] = {0}, eof[NUM_NODES] = {0};
	u32 sts;

	sts = cfg_reg_read(cfe, MIPICFG_INTS);

	if (sts & MIPICFG_INT_CSI_DMA)
		csi2_isr(&cfe->csi2, sof, eof);

	if (sts & MIPICFG_INT_PISP_FE)
		pisp_fe_isr(&cfe->fe, sof + CSI2_NUM_CHANNELS,
			    eof + CSI2_NUM_CHANNELS);

	spin_lock(&cfe->state_lock);

	for (i = 0; i < NUM_NODES; i++) {
		struct cfe_node *node = &cfe->node[i];

		/*
		 * The check_state(NODE_STREAMING) is to ensure we do not loop
		 * over the CSI2_CHx nodes when the FE is active since they
		 * generate interrupts even though the node is not streaming.
		 */
		if (!check_state(cfe, NODE_STREAMING, i) ||
		    !(sof[i] || eof[i]))
			continue;

		/*
		 * There are 3 cases where we could get FS + FE_ACK at
		 * the same time:
		 * 1) FE of the current frame, and FS of the next frame.
		 * 2) FS + FE of the same frame.
		 * 3) FE of the current frame, and FS + FE of the next
		 *    frame. To handle this, see the sof handler below.
		 *
		 * (1) is handled implicitly by the ordering of the FE and FS
		 * handlers below.
		 */
		if (eof[i]) {
			/*
			 * The condition below tests for (2). Run the FS handler
			 * first before the FE handler, both for the current
			 * frame.
			 */
			if (sof[i] && !check_state(cfe, FS_INT, i)) {
				cfe_sof_isr_handler(node);
				sof[i] = false;
			}

			cfe_eof_isr_handler(node);
		}

		if (sof[i]) {
			/*
			 * The condition below tests for (3). In such cases, we
			 * come in here with FS flag set in the node state from
			 * the previous frame since it only gets cleared in
			 * eof_isr_handler(). Handle the FE for the previous
			 * frame first before the FS handler for the current
			 * frame.
			 */
			if (check_state(cfe, FS_INT, node->id) &&
			    !check_state(cfe, FE_INT, node->id)) {
				cfe_dbg("%s: [%s] Handling missing previous FE interrupt\n",
					__func__, node_desc[node->id].name);
				cfe_eof_isr_handler(node);
			}

			cfe_sof_isr_handler(node);
		}

		if (!cfe->job_queued && cfe->job_ready)
			cfe_prepare_next_job(cfe);
	}

	spin_unlock(&cfe->state_lock);

	return IRQ_HANDLED;
}

/*
 * Stream helpers
 */

static void cfe_start_channel(struct cfe_node *node)
{
	struct cfe_device *cfe = node->cfe;
	struct v4l2_subdev_state *state;
	struct v4l2_mbus_framefmt *source_fmt;
	const struct cfe_fmt *fmt;
	unsigned long flags;
	bool start_fe = is_fe_enabled(cfe) &&
			test_all_nodes(cfe, NODE_ENABLED, NODE_STREAMING);

	cfe_dbg("%s: [%s]\n", __func__, node_desc[node->id].name);

	state = v4l2_subdev_lock_and_get_active_state(&cfe->csi2.sd);

	if (start_fe) {
		unsigned int width, height;

		WARN_ON(!is_fe_enabled(cfe));
		cfe_dbg("%s: %s using csi2 channel %d\n",
			__func__, node_desc[FE_OUT0].name,
			cfe->fe_csi2_channel);

		source_fmt = v4l2_subdev_state_get_format(state,
							cfe->fe_csi2_channel);
		fmt = find_format_by_code(source_fmt->code);

		width = source_fmt->width;
		height = source_fmt->height;

		/* Must have a valid CSI2 datatype. */
		WARN_ON(!fmt->csi_dt);

		/*
		 * Start the associated CSI2 Channel as well.
		 *
		 * Must write to the ADDR register to latch the ctrl values
		 * even if we are connected to the front end. Once running,
		 * this is handled by the CSI2 AUTO_ARM mode.
		 */
		csi2_start_channel(&cfe->csi2, cfe->fe_csi2_channel,
				   CSI2_MODE_FE_STREAMING,
				   true, false, width, height);
		csi2_set_buffer(&cfe->csi2, cfe->fe_csi2_channel, 0, 0, -1);
		pisp_fe_start(&cfe->fe);
	}

	if (is_csi2_node(node)) {
		unsigned int width = 0, height = 0;

		u32 mode = CSI2_MODE_NORMAL;

		source_fmt = v4l2_subdev_state_get_format(state,
			node_desc[node->id].link_pad - CSI2_NUM_CHANNELS);
		fmt = find_format_by_code(source_fmt->code);

		/* Must have a valid CSI2 datatype. */
		WARN_ON(!fmt->csi_dt);

		if (is_image_output_node(node)) {
			width = source_fmt->width;
			height = source_fmt->height;

			if (node->vid_fmt.fmt.pix.pixelformat ==
					fmt->remap[CFE_REMAP_16BIT])
				mode = CSI2_MODE_REMAP;
			else if (node->vid_fmt.fmt.pix.pixelformat ==
					fmt->remap[CFE_REMAP_COMPRESSED]) {
				mode = CSI2_MODE_COMPRESSED;
				csi2_set_compression(&cfe->csi2, node->id,
						     CSI2_COMPRESSION_DELTA, 0,
						     0);
			}
		}
		/* Unconditionally start this CSI2 channel. */
		csi2_start_channel(&cfe->csi2, node->id,
				   mode,
				   /* Auto arm */
				   false,
				   /* Pack bytes */
				   is_meta_node(node) ? true : false,
				   width, height);
	}

	v4l2_subdev_unlock_state(state);

	spin_lock_irqsave(&cfe->state_lock, flags);
	if (cfe->job_ready && test_all_nodes(cfe, NODE_ENABLED, NODE_STREAMING))
		cfe_prepare_next_job(cfe);
	spin_unlock_irqrestore(&cfe->state_lock, flags);
}

static void cfe_stop_channel(struct cfe_node *node, bool fe_stop)
{
	struct cfe_device *cfe = node->cfe;

	cfe_dbg("%s: [%s] fe_stop %u\n", __func__,
		node_desc[node->id].name, fe_stop);

	if (fe_stop) {
		csi2_stop_channel(&cfe->csi2, cfe->fe_csi2_channel);
		pisp_fe_stop(&cfe->fe);
	}

	if (is_csi2_node(node))
		csi2_stop_channel(&cfe->csi2, node->id);
}

static void cfe_return_buffers(struct cfe_node *node,
			       enum vb2_buffer_state state)
{
	struct cfe_device *cfe = node->cfe;
	struct cfe_buffer *buf, *tmp;
	unsigned long flags;

	cfe_dbg("%s: [%s]\n", __func__, node_desc[node->id].name);

	spin_lock_irqsave(&cfe->state_lock, flags);
	list_for_each_entry_safe(buf, tmp, &node->dma_queue, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}

	if (node->cur_frm)
		vb2_buffer_done(&node->cur_frm->vb.vb2_buf, state);
	if (node->next_frm && node->cur_frm != node->next_frm)
		vb2_buffer_done(&node->next_frm->vb.vb2_buf, state);

	node->cur_frm = NULL;
	node->next_frm = NULL;
	spin_unlock_irqrestore(&cfe->state_lock, flags);
}

/*
 * vb2 ops
 */

static int cfe_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			   unsigned int *nplanes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct cfe_node *node = vb2_get_drv_priv(vq);
	struct cfe_device *cfe = node->cfe;
	unsigned int size = is_image_node(node) ? node->vid_fmt.fmt.pix.sizeimage :
						  node->meta_fmt.fmt.meta.buffersize;

	cfe_dbg("%s: [%s] type:%u\n", __func__, node_desc[node->id].name,
		node->buffer_queue.type);

	if (vb2_get_num_buffers(vq) + *nbuffers < 3)
		*nbuffers = 3 - vb2_get_num_buffers(vq);

	if (*nplanes) {
		if (sizes[0] < size) {
			cfe_err("sizes[0] %i < size %u\n", sizes[0], size);
			return -EINVAL;
		}
		size = sizes[0];
	}

	*nplanes = 1;
	sizes[0] = size;

	return 0;
}

static int cfe_buffer_prepare(struct vb2_buffer *vb)
{
	struct cfe_node *node = vb2_get_drv_priv(vb->vb2_queue);
	struct cfe_device *cfe = node->cfe;
	struct cfe_buffer *buf = to_cfe_buffer(vb);
	unsigned long size;

	cfe_dbg_verbose("%s: [%s] buffer:%p\n", __func__,
			node_desc[node->id].name, vb);

	size = is_image_node(node) ? node->vid_fmt.fmt.pix.sizeimage :
				     node->meta_fmt.fmt.meta.buffersize;
	if (vb2_plane_size(vb, 0) < size) {
		cfe_err("data will not fit into plane (%lu < %lu)\n",
			vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, size);

	if (node->id == FE_CONFIG) {
		struct cfe_config_buffer *b = to_cfe_config_buffer(buf);
		void *addr = vb2_plane_vaddr(vb, 0);

		memcpy(&b->config, addr, sizeof(struct pisp_fe_config));
		return pisp_fe_validate_config(&cfe->fe, &b->config,
					       &cfe->node[FE_OUT0].vid_fmt,
					       &cfe->node[FE_OUT1].vid_fmt);
	}

	return 0;
}

static void cfe_buffer_queue(struct vb2_buffer *vb)
{
	struct cfe_node *node = vb2_get_drv_priv(vb->vb2_queue);
	struct cfe_device *cfe = node->cfe;
	struct cfe_buffer *buf = to_cfe_buffer(vb);
	unsigned long flags;

	cfe_dbg_verbose("%s: [%s] buffer:%p\n", __func__,
			node_desc[node->id].name, vb);

	spin_lock_irqsave(&cfe->state_lock, flags);

	list_add_tail(&buf->list, &node->dma_queue);

	if (!cfe->job_ready)
		cfe->job_ready = cfe_check_job_ready(cfe);

	if (!cfe->job_queued && cfe->job_ready &&
	    test_all_nodes(cfe, NODE_ENABLED, NODE_STREAMING)) {
		cfe_dbg("Preparing job immediately for channel %u\n",
			node->id);
		cfe_prepare_next_job(cfe);
	}

	spin_unlock_irqrestore(&cfe->state_lock, flags);
}

static u64 sensor_link_rate(struct cfe_device *cfe)
{
	struct v4l2_mbus_framefmt *source_fmt;
	struct v4l2_subdev_state *state;
	struct media_entity *entity;
	struct v4l2_subdev *subdev;
	const struct cfe_fmt *fmt;
	struct media_pad *pad;
	s64 link_freq;

	state = v4l2_subdev_lock_and_get_active_state(&cfe->csi2.sd);
	source_fmt = v4l2_subdev_state_get_format(state, 0);
	fmt = find_format_by_code(source_fmt->code);
	v4l2_subdev_unlock_state(state);

	/*
	 * Walk up the media graph to find either the sensor entity, or another
	 * entity that advertises the V4L2_CID_LINK_FREQ or V4L2_CID_PIXEL_RATE
	 * control through the subdev.
	 */
	entity = &cfe->csi2.sd.entity;
	while (1) {
		pad = &entity->pads[0];
		if (!(pad->flags & MEDIA_PAD_FL_SINK))
			goto err;

		pad = media_pad_remote_pad_first(pad);
		if (!pad || !is_media_entity_v4l2_subdev(pad->entity))
			goto err;

		entity = pad->entity;
		subdev = media_entity_to_v4l2_subdev(entity);
		if (entity->function == MEDIA_ENT_F_CAM_SENSOR ||
		    v4l2_ctrl_find(subdev->ctrl_handler, V4L2_CID_LINK_FREQ) ||
		    v4l2_ctrl_find(subdev->ctrl_handler, V4L2_CID_PIXEL_RATE))
			break;
	}

	link_freq = v4l2_get_link_freq(subdev->ctrl_handler, fmt->depth,
				       cfe->csi2.dphy.active_lanes * 2);
	if (link_freq < 0)
		goto err;

	/* x2 for DDR. */
	link_freq *= 2;
	cfe_info("Using a link rate of %lld Mbps\n", link_freq / (1000 * 1000));
	return link_freq;

err:
	cfe_err("Unable to determine sensor link rate, using 999 Mbps\n");
	return 999 * 1000000UL;
}

static int cfe_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct v4l2_mbus_config mbus_config = { 0 };
	struct cfe_node *node = vb2_get_drv_priv(vq);
	struct cfe_device *cfe = node->cfe;
	int ret;

	cfe_dbg("%s: [%s] begin.\n", __func__, node_desc[node->id].name);

	if (!check_state(cfe, NODE_ENABLED, node->id)) {
		cfe_err("%s node link is not enabled.\n",
			node_desc[node->id].name);
		ret = -EINVAL;
		goto err_streaming;
	}

	ret = pm_runtime_resume_and_get(&cfe->pdev->dev);
	if (ret < 0) {
		cfe_err("pm_runtime_resume_and_get failed\n");
		goto err_streaming;
	}

	/* When using the Frontend, we must enable the FE_CONFIG node. */
	if (is_fe_enabled(cfe) &&
	    !check_state(cfe, NODE_ENABLED, cfe->node[FE_CONFIG].id)) {
		cfe_err("FE enabled, but FE_CONFIG node is not\n");
		ret = -EINVAL;
		goto err_pm_put;
	}

	ret = media_pipeline_start(&node->pad, &cfe->pipe);
	if (ret < 0) {
		cfe_err("Failed to start media pipeline: %d\n", ret);
		goto err_pm_put;
	}

	clear_state(cfe, FS_INT | FE_INT, node->id);
	set_state(cfe, NODE_STREAMING, node->id);
	node->fs_count = 0;
	cfe_start_channel(node);

	if (!test_all_nodes(cfe, NODE_ENABLED, NODE_STREAMING)) {
		cfe_dbg("Not all nodes are set to streaming yet!\n");
		return 0;
	}

	cfg_reg_write(cfe, MIPICFG_CFG, MIPICFG_CFG_SEL_CSI);
	cfg_reg_write(cfe, MIPICFG_INTE, MIPICFG_INT_CSI_DMA | MIPICFG_INT_PISP_FE);

	ret = v4l2_subdev_call(cfe->sensor, pad, get_mbus_config, 0,
			       &mbus_config);
	if (ret < 0 && ret != -ENOIOCTLCMD) {
		cfe_err("g_mbus_config failed\n");
		goto err_pm_put;
	}

	cfe->csi2.dphy.active_lanes = mbus_config.bus.mipi_csi2.num_data_lanes;
	if (!cfe->csi2.dphy.active_lanes)
		cfe->csi2.dphy.active_lanes = cfe->csi2.dphy.max_lanes;
	if (cfe->csi2.dphy.active_lanes > cfe->csi2.dphy.max_lanes) {
		cfe_err("Device has requested %u data lanes, which is >%u configured in DT\n",
			cfe->csi2.dphy.active_lanes, cfe->csi2.dphy.max_lanes);
		ret = -EINVAL;
		goto err_disable_cfe;
	}

	cfe_dbg("Configuring CSI-2 block - %u data lanes\n", cfe->csi2.dphy.active_lanes);
	cfe->csi2.dphy.dphy_rate = sensor_link_rate(cfe) / 1000000UL;
	csi2_open_rx(&cfe->csi2);

	cfe_dbg("Starting sensor streaming\n");
	ret = v4l2_subdev_call(cfe->sensor, video, s_stream, 1);
	if (ret < 0) {
		cfe_err("stream on failed in subdev\n");
		goto err_disable_cfe;
	}

	cfe_dbg("%s: [%s] end.\n", __func__, node_desc[node->id].name);

	return 0;

err_disable_cfe:
	csi2_close_rx(&cfe->csi2);
	cfe_stop_channel(node, true);
	media_pipeline_stop(&node->pad);
err_pm_put:
	pm_runtime_put(&cfe->pdev->dev);
err_streaming:
	cfe_return_buffers(node, VB2_BUF_STATE_QUEUED);
	clear_state(cfe, NODE_STREAMING, node->id);

	return ret;
}

static void cfe_stop_streaming(struct vb2_queue *vq)
{
	struct cfe_node *node = vb2_get_drv_priv(vq);
	struct cfe_device *cfe = node->cfe;
	unsigned long flags;
	bool fe_stop;

	cfe_dbg("%s: [%s] begin.\n", __func__, node_desc[node->id].name);

	spin_lock_irqsave(&cfe->state_lock, flags);
	fe_stop = is_fe_enabled(cfe) &&
		  test_all_nodes(cfe, NODE_ENABLED, NODE_STREAMING);

	cfe->job_ready = false;
	clear_state(cfe, NODE_STREAMING, node->id);
	spin_unlock_irqrestore(&cfe->state_lock, flags);

	cfe_stop_channel(node, fe_stop);

	if (!test_any_node(cfe, NODE_STREAMING)) {
		/* Stop streaming the sensor and disable the peripheral. */
		if (v4l2_subdev_call(cfe->sensor, video, s_stream, 0) < 0)
			cfe_err("stream off failed in subdev\n");

		csi2_close_rx(&cfe->csi2);

		cfg_reg_write(cfe, MIPICFG_INTE, 0);
	}

	media_pipeline_stop(&node->pad);

	/* Clear all queued buffers for the node */
	cfe_return_buffers(node, VB2_BUF_STATE_ERROR);

	pm_runtime_put(&cfe->pdev->dev);

	cfe_dbg("%s: [%s] end.\n", __func__, node_desc[node->id].name);
}

static const struct vb2_ops cfe_video_qops = {
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.queue_setup = cfe_queue_setup,
	.buf_prepare = cfe_buffer_prepare,
	.buf_queue = cfe_buffer_queue,
	.start_streaming = cfe_start_streaming,
	.stop_streaming = cfe_stop_streaming,
};

/*
 * v4l2 ioctl ops
 */

static int cfe_querycap(struct file *file, void *priv,
			struct v4l2_capability *cap)
{
	struct cfe_node *node = video_drvdata(file);
	struct cfe_device *cfe = node->cfe;

	strscpy(cap->driver, CFE_MODULE_NAME, sizeof(cap->driver));
	strscpy(cap->card, CFE_MODULE_NAME, sizeof(cap->card));

	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(&cfe->pdev->dev));

	cap->capabilities |= V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_META_CAPTURE |
			     V4L2_CAP_META_OUTPUT;

	return 0;
}

static int cfe_enum_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_fmtdesc *f)
{
	struct cfe_node *node = video_drvdata(file);
	struct cfe_device *cfe = node->cfe;
	unsigned int i, j;

	if (!node_supports_image_output(node))
		return -EINVAL;

	cfe_dbg("%s: [%s]\n", __func__, node_desc[node->id].name);

	for (i = 0, j = 0; i < ARRAY_SIZE(formats); i++) {
		if (f->mbus_code && formats[i].code != f->mbus_code)
			continue;

		if (formats[i].flags & CFE_FORMAT_FLAG_META_OUT ||
		    formats[i].flags & CFE_FORMAT_FLAG_META_CAP)
			continue;

		if (is_fe_node(node) &&
		    !(formats[i].flags & CFE_FORMAT_FLAG_FE_OUT))
			continue;

		if (j == f->index) {
			f->pixelformat = formats[i].fourcc;
			f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			return 0;
		}
		j++;
	}

	return -EINVAL;
}

static int cfe_g_fmt(struct file *file, void *priv,
		     struct v4l2_format *f)
{
	struct cfe_node *node = video_drvdata(file);
	struct cfe_device *cfe = node->cfe;

	cfe_dbg("%s: [%s]\n", __func__, node_desc[node->id].name);

	if (!node_supports_image(node))
		return -EINVAL;

	*f = node->vid_fmt;

	return 0;
}

static int try_fmt_vid_cap(struct cfe_node *node, struct v4l2_format *f)
{
	struct cfe_device *cfe = node->cfe;
	const struct cfe_fmt *fmt;

	cfe_dbg("%s: [%s] %ux%u, V4L2 pix " V4L2_FOURCC_CONV "\n",
		__func__, node_desc[node->id].name,
		f->fmt.pix.width, f->fmt.pix.height,
		V4L2_FOURCC_CONV_ARGS(f->fmt.pix.pixelformat));

	if (!node_supports_image_output(node))
		return -EINVAL;

	/*
	 * Default to a format that works for both CSI2 and FE.
	 */
	fmt = find_format_by_pix(f->fmt.pix.pixelformat);
	if (!fmt)
		fmt = find_format_by_code(MEDIA_BUS_FMT_SBGGR10_1X10);

	f->fmt.pix.pixelformat = fmt->fourcc;

	if (is_fe_node(node) && fmt->remap[CFE_REMAP_16BIT]) {
		f->fmt.pix.pixelformat = fmt->remap[CFE_REMAP_16BIT];
		fmt = find_format_by_pix(f->fmt.pix.pixelformat);
	}

	f->fmt.pix.field = V4L2_FIELD_NONE;

	cfe_calc_format_size_bpl(cfe, fmt, f);

	return 0;
}

static int cfe_s_fmt_vid_cap(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct cfe_node *node = video_drvdata(file);
	struct cfe_device *cfe = node->cfe;
	struct vb2_queue *q = &node->buffer_queue;
	int ret;

	cfe_dbg("%s: [%s]\n", __func__, node_desc[node->id].name);

	if (vb2_is_busy(q))
		return -EBUSY;

	ret = try_fmt_vid_cap(node, f);
	if (ret)
		return ret;

	node->vid_fmt = *f;

	cfe_dbg("%s: Set %ux%u, V4L2 pix " V4L2_FOURCC_CONV "\n", __func__,
		node->vid_fmt.fmt.pix.width, node->vid_fmt.fmt.pix.height,
		V4L2_FOURCC_CONV_ARGS(node->vid_fmt.fmt.pix.pixelformat));

	return 0;
}

static int cfe_try_fmt_vid_cap(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct cfe_node *node = video_drvdata(file);
	struct cfe_device *cfe = node->cfe;

	cfe_dbg("%s: [%s]\n", __func__, node_desc[node->id].name);

	return try_fmt_vid_cap(node, f);
}

static int cfe_enum_fmt_meta(struct file *file, void *priv,
			     struct v4l2_fmtdesc *f)
{
	struct cfe_node *node = video_drvdata(file);
	struct cfe_device *cfe = node->cfe;

	cfe_dbg("%s: [%s]\n", __func__, node_desc[node->id].name);

	if (!node_supports_meta(node) || f->index != 0)
		return -EINVAL;

	switch (node->id) {
	case CSI2_CH0...CSI2_CH3:
		f->pixelformat = V4L2_META_FMT_SENSOR_DATA;
		return 0;
	case FE_STATS:
		f->pixelformat = V4L2_META_FMT_RPI_FE_STATS;
		return 0;
	case FE_CONFIG:
		f->pixelformat = V4L2_META_FMT_RPI_FE_CFG;
		return 0;
	}

	return -EINVAL;
}

static int try_fmt_meta(struct cfe_node *node, struct v4l2_format *f)
{
	if (!node_supports_meta(node))
		return -EINVAL;

	switch (node->id) {
	case CSI2_CH0...CSI2_CH3:
		f->fmt.meta.dataformat = V4L2_META_FMT_SENSOR_DATA;
		if (!f->fmt.meta.buffersize)
			f->fmt.meta.buffersize = DEFAULT_EMBEDDED_SIZE;
		f->fmt.meta.buffersize =
			min_t(u32, f->fmt.meta.buffersize, MAX_BUFFER_SIZE);
		f->fmt.meta.buffersize =
			ALIGN(f->fmt.meta.buffersize, BPL_ALIGNMENT);
		return 0;
	case FE_STATS:
		f->fmt.meta.dataformat = V4L2_META_FMT_RPI_FE_STATS;
		f->fmt.meta.buffersize = sizeof(struct pisp_statistics);
		return 0;
	case FE_CONFIG:
		f->fmt.meta.dataformat = V4L2_META_FMT_RPI_FE_CFG;
		f->fmt.meta.buffersize = sizeof(struct pisp_fe_config);
		return 0;
	}

	return -EINVAL;
}

static int cfe_g_fmt_meta(struct file *file, void *priv, struct v4l2_format *f)
{
	struct cfe_node *node = video_drvdata(file);
	struct cfe_device *cfe = node->cfe;

	cfe_dbg("%s: [%s]\n", __func__, node_desc[node->id].name);

	if (!node_supports_meta(node))
		return -EINVAL;

	*f = node->meta_fmt;

	return 0;
}

static int cfe_s_fmt_meta(struct file *file, void *priv, struct v4l2_format *f)
{
	struct cfe_node *node = video_drvdata(file);
	struct cfe_device *cfe = node->cfe;
	struct vb2_queue *q = &node->buffer_queue;
	int ret;

	cfe_dbg("%s: [%s]\n", __func__, node_desc[node->id].name);

	if (vb2_is_busy(q))
		return -EBUSY;

	if (!node_supports_meta(node))
		return -EINVAL;

	ret = try_fmt_meta(node, f);
	if (ret)
		return ret;

	node->meta_fmt = *f;

	cfe_dbg("%s: Set " V4L2_FOURCC_CONV "\n", __func__,
		V4L2_FOURCC_CONV_ARGS(node->meta_fmt.fmt.meta.dataformat));

	return 0;
}

static int cfe_try_fmt_meta(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct cfe_node *node = video_drvdata(file);
	struct cfe_device *cfe = node->cfe;

	cfe_dbg("%s: [%s]\n", __func__, node_desc[node->id].name);
	return try_fmt_meta(node, f);
}

static int cfe_enum_framesizes(struct file *file, void *priv,
			       struct v4l2_frmsizeenum *fsize)
{
	struct cfe_node *node = video_drvdata(file);
	struct cfe_device *cfe = node->cfe;
	const struct cfe_fmt *fmt;

	cfe_dbg("%s [%s]\n", __func__, node_desc[node->id].name);

	if (fsize->index > 0)
		return -EINVAL;

	/* check for valid format */
	fmt = find_format_by_pix(fsize->pixel_format);
	if (!fmt) {
		cfe_dbg("Invalid pixel code: %x\n", fsize->pixel_format);
		return -EINVAL;
	}

	/* TODO: Do we have limits on the step_width? */

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = MIN_WIDTH;
	fsize->stepwise.max_width = MAX_WIDTH;
	fsize->stepwise.step_width = 2;
	fsize->stepwise.min_height = MIN_HEIGHT;
	fsize->stepwise.max_height = MAX_HEIGHT;
	fsize->stepwise.step_height = 1;

	return 0;
}

static int cfe_vb2_ioctl_reqbufs(struct file *file, void *priv,
				 struct v4l2_requestbuffers *p)
{
	struct video_device *vdev = video_devdata(file);
	struct cfe_node *node = video_get_drvdata(vdev);
	struct cfe_device *cfe = node->cfe;
	int ret;

	cfe_dbg("%s: [%s] type:%u\n", __func__, node_desc[node->id].name,
		p->type);

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    p->type != V4L2_BUF_TYPE_META_CAPTURE &&
	    p->type != V4L2_BUF_TYPE_META_OUTPUT)
		return -EINVAL;

	ret = vb2_queue_change_type(vdev->queue, p->type);
	if (ret)
		return ret;

	return vb2_ioctl_reqbufs(file, priv, p);
}

static int cfe_vb2_ioctl_create_bufs(struct file *file, void *priv,
				     struct v4l2_create_buffers *p)
{
	struct video_device *vdev = video_devdata(file);
	struct cfe_node *node = video_get_drvdata(vdev);
	struct cfe_device *cfe = node->cfe;
	int ret;

	cfe_dbg("%s: [%s] type:%u\n", __func__, node_desc[node->id].name,
		p->format.type);

	if (p->format.type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    p->format.type != V4L2_BUF_TYPE_META_CAPTURE &&
	    p->format.type != V4L2_BUF_TYPE_META_OUTPUT)
		return -EINVAL;

	ret = vb2_queue_change_type(vdev->queue, p->format.type);
	if (ret)
		return ret;

	return vb2_ioctl_create_bufs(file, priv, p);
}

static int cfe_subscribe_event(struct v4l2_fh *fh,
			       const struct v4l2_event_subscription *sub)
{
	struct cfe_node *node = video_get_drvdata(fh->vdev);

	switch (sub->type) {
	case V4L2_EVENT_FRAME_SYNC:
		if (!node_supports_image_output(node))
			break;

		return v4l2_event_subscribe(fh, sub, 2, NULL);
	case V4L2_EVENT_SOURCE_CHANGE:
		if (!node_supports_image_output(node) &&
		    !node_supports_meta_output(node))
			break;

		return v4l2_event_subscribe(fh, sub, 4, NULL);
	}

	return v4l2_ctrl_subscribe_event(fh, sub);
}

static const struct v4l2_ioctl_ops cfe_ioctl_ops = {
	.vidioc_querycap = cfe_querycap,
	.vidioc_enum_fmt_vid_cap = cfe_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = cfe_g_fmt,
	.vidioc_s_fmt_vid_cap = cfe_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = cfe_try_fmt_vid_cap,

	.vidioc_enum_fmt_meta_cap = cfe_enum_fmt_meta,
	.vidioc_g_fmt_meta_cap = cfe_g_fmt_meta,
	.vidioc_s_fmt_meta_cap = cfe_s_fmt_meta,
	.vidioc_try_fmt_meta_cap = cfe_try_fmt_meta,

	.vidioc_enum_fmt_meta_out = cfe_enum_fmt_meta,
	.vidioc_g_fmt_meta_out = cfe_g_fmt_meta,
	.vidioc_s_fmt_meta_out = cfe_s_fmt_meta,
	.vidioc_try_fmt_meta_out = cfe_try_fmt_meta,

	.vidioc_enum_framesizes = cfe_enum_framesizes,

	.vidioc_reqbufs = cfe_vb2_ioctl_reqbufs,
	.vidioc_create_bufs = cfe_vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_subscribe_event = cfe_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static void cfe_notify(struct v4l2_subdev *sd, unsigned int notification,
		       void *arg)
{
	struct cfe_device *cfe = to_cfe_device(sd->v4l2_dev);
	unsigned int i;

	switch (notification) {
	case V4L2_DEVICE_NOTIFY_EVENT:
		for (i = 0; i < NUM_NODES; i++) {
			struct cfe_node *node = &cfe->node[i];

			if (check_state(cfe, NODE_REGISTERED, i))
				continue;

			v4l2_event_queue(&node->video_dev, arg);
		}
		break;
	default:
		break;
	}
}

/* cfe capture driver file operations */
static const struct v4l2_file_operations cfe_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
};

static int cfe_video_link_validate(struct media_link *link)
{
	struct video_device *vd = container_of(link->sink->entity,
					       struct video_device, entity);
	struct cfe_node *node = container_of(vd, struct cfe_node, video_dev);
	struct cfe_device *cfe = node->cfe;
	struct v4l2_mbus_framefmt *source_fmt;
	struct v4l2_subdev_state *state;
	struct v4l2_subdev *source_sd;
	int ret = 0;

	cfe_dbg("%s: [%s] link \"%s\":%u -> \"%s\":%u\n", __func__,
		node_desc[node->id].name,
		link->source->entity->name, link->source->index,
		link->sink->entity->name, link->sink->index);

	if (!media_entity_remote_source_pad_unique(link->sink->entity)) {
		cfe_err("video node %s pad not connected\n", vd->name);
		return -ENOTCONN;
	}

	source_sd = media_entity_to_v4l2_subdev(link->source->entity);

	state = v4l2_subdev_lock_and_get_active_state(source_sd);

	source_fmt = v4l2_subdev_state_get_format(state,
						link->source->index);
	if (!source_fmt) {
		ret = -EINVAL;
		goto out;
	}

	if (is_image_output_node(node)) {
		struct v4l2_pix_format *pix_fmt = &node->vid_fmt.fmt.pix;
		const struct cfe_fmt *fmt = NULL;
		unsigned int i;

		if (source_fmt->width != pix_fmt->width ||
		    source_fmt->height != pix_fmt->height) {
			cfe_err("Wrong width or height %ux%u (remote pad set to %ux%u)\n",
				pix_fmt->width, pix_fmt->height,
				source_fmt->width,
				source_fmt->height);
			ret = -EINVAL;
			goto out;
		}

		for (i = 0; i < ARRAY_SIZE(formats); i++) {
			if (formats[i].code == source_fmt->code &&
			    formats[i].fourcc == pix_fmt->pixelformat) {
				fmt = &formats[i];
				break;
			}
		}
		if (!fmt) {
			cfe_err("Format mismatch!\n");
			ret = -EINVAL;
			goto out;
		}
	} else if (is_csi2_node(node) && is_meta_output_node(node)) {
		struct v4l2_meta_format *meta_fmt = &node->meta_fmt.fmt.meta;
		const struct cfe_fmt *fmt;
		u32 source_size;

		fmt = find_format_by_code(source_fmt->code);
		if (!fmt || fmt->fourcc != meta_fmt->dataformat) {
			cfe_err("Metadata format mismatch!\n");
			ret = -EINVAL;
			goto out;
		}

		source_size = DIV_ROUND_UP(source_fmt->width * source_fmt->height * fmt->depth, 8);

		if (source_fmt->code != MEDIA_BUS_FMT_SENSOR_DATA) {
			cfe_err("Bad metadata mbus format\n");
			ret = -EINVAL;
			goto out;
		}

		if (source_size > meta_fmt->buffersize) {
			cfe_err("Metadata buffer too small: %u < %u\n",
				meta_fmt->buffersize, source_size);
			ret = -EINVAL;
			goto out;
		}
	}

out:
	v4l2_subdev_unlock_state(state);

	return ret;
}

static const struct media_entity_operations cfe_media_entity_ops = {
	.link_validate = cfe_video_link_validate,
};

static int cfe_video_link_notify(struct media_link *link, u32 flags,
				 unsigned int notification)
{
	struct media_device *mdev = link->graph_obj.mdev;
	struct cfe_device *cfe = container_of(mdev, struct cfe_device, mdev);
	struct media_entity *fe = &cfe->fe.sd.entity;
	struct media_entity *csi2 = &cfe->csi2.sd.entity;
	unsigned long lock_flags;
	unsigned int i;

	if (notification != MEDIA_DEV_NOTIFY_POST_LINK_CH)
		return 0;

	cfe_dbg("%s: %s[%u] -> %s[%u] 0x%x", __func__,
		link->source->entity->name, link->source->index,
		link->sink->entity->name, link->sink->index, flags);

	spin_lock_irqsave(&cfe->state_lock, lock_flags);

	for (i = 0; i < NUM_NODES; i++) {
		if (link->sink->entity != &cfe->node[i].video_dev.entity &&
		    link->source->entity != &cfe->node[i].video_dev.entity)
			continue;

		if (link->flags & MEDIA_LNK_FL_ENABLED)
			set_state(cfe, NODE_ENABLED, i);
		else
			clear_state(cfe, NODE_ENABLED, i);

		break;
	}

	spin_unlock_irqrestore(&cfe->state_lock, lock_flags);

	if (link->source->entity != csi2)
		return 0;
	if (link->sink->entity != fe)
		return 0;
	if (link->sink->index != 0)
		return 0;

	cfe->fe_csi2_channel = -1;
	if (link->flags & MEDIA_LNK_FL_ENABLED) {
		if (link->source->index == node_desc[CSI2_CH0].link_pad)
			cfe->fe_csi2_channel = CSI2_CH0;
		else if (link->source->index == node_desc[CSI2_CH1].link_pad)
			cfe->fe_csi2_channel = CSI2_CH1;
		else if (link->source->index == node_desc[CSI2_CH2].link_pad)
			cfe->fe_csi2_channel = CSI2_CH2;
		else if (link->source->index == node_desc[CSI2_CH3].link_pad)
			cfe->fe_csi2_channel = CSI2_CH3;
	}

	if (is_fe_enabled(cfe))
		cfe_dbg("%s: Found CSI2:%d -> FE:0 link\n", __func__,
			cfe->fe_csi2_channel);
	else
		cfe_dbg("%s: Unable to find CSI2:x -> FE:0 link\n", __func__);

	return 0;
}

static const struct media_device_ops cfe_media_device_ops = {
	.link_notify = cfe_video_link_notify,
};

static void cfe_release(struct kref *kref)
{
	struct cfe_device *cfe = container_of(kref, struct cfe_device, kref);

	media_device_cleanup(&cfe->mdev);

	kfree(cfe);
}

static void cfe_put(struct cfe_device *cfe)
{
	kref_put(&cfe->kref, cfe_release);
}

static void cfe_get(struct cfe_device *cfe)
{
	kref_get(&cfe->kref);
}

static void cfe_node_release(struct video_device *vdev)
{
	struct cfe_node *node = video_get_drvdata(vdev);

	cfe_put(node->cfe);
}

static int cfe_register_node(struct cfe_device *cfe, int id)
{
	struct video_device *vdev;
	const struct cfe_fmt *fmt;
	struct vb2_queue *q;
	struct cfe_node *node = &cfe->node[id];
	int ret;

	node->cfe = cfe;
	node->id = id;

	if (node_supports_image(node)) {
		if (node_supports_image_output(node))
			node->vid_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		else
			node->vid_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

		fmt = find_format_by_code(cfe_default_format.code);
		if (!fmt) {
			cfe_err("Failed to find format code\n");
			return -EINVAL;
		}

		node->vid_fmt.fmt.pix.pixelformat = fmt->fourcc;
		v4l2_fill_pix_format(&node->vid_fmt.fmt.pix, &cfe_default_format);

		ret = try_fmt_vid_cap(node, &node->vid_fmt);
		if (ret)
			return ret;
	}

	if (node_supports_meta(node)) {
		if (node_supports_meta_output(node))
			node->meta_fmt.type = V4L2_BUF_TYPE_META_CAPTURE;
		else
			node->meta_fmt.type = V4L2_BUF_TYPE_META_OUTPUT;

		ret = try_fmt_meta(node, &node->meta_fmt);
		if (ret)
			return ret;
	}

	mutex_init(&node->lock);

	q = &node->buffer_queue;
	q->type = node_supports_image(node) ? node->vid_fmt.type :
					      node->meta_fmt.type;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = node;
	q->ops = &cfe_video_qops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->buf_struct_size = id == FE_CONFIG ? sizeof(struct cfe_config_buffer)
					     : sizeof(struct cfe_buffer);
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &node->lock;
	q->min_queued_buffers = 1;
	q->dev = &cfe->pdev->dev;

	ret = vb2_queue_init(q);
	if (ret) {
		cfe_err("vb2_queue_init() failed\n");
		return ret;
	}

	INIT_LIST_HEAD(&node->dma_queue);

	vdev = &node->video_dev;
	vdev->release = cfe_node_release;
	vdev->fops = &cfe_fops;
	vdev->ioctl_ops = &cfe_ioctl_ops;
	vdev->entity.ops = &cfe_media_entity_ops;
	vdev->v4l2_dev = &cfe->v4l2_dev;
	vdev->vfl_dir = (node_supports_image_output(node) ||
			 node_supports_meta_output(node)) ?
				VFL_DIR_RX :
				VFL_DIR_TX;
	vdev->queue = q;
	vdev->lock = &node->lock;
	vdev->device_caps = node_desc[id].caps;
	vdev->device_caps |= V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;

	/* Define the device names */
	snprintf(vdev->name, sizeof(vdev->name), "%s-%s", CFE_MODULE_NAME,
		 node_desc[id].name);

	video_set_drvdata(vdev, node);
	if (node->id == FE_OUT0)
		vdev->entity.flags |= MEDIA_ENT_FL_DEFAULT;
	node->pad.flags = node_desc[id].pad_flags;
	media_entity_pads_init(&vdev->entity, 1, &node->pad);

	if (!node_supports_image(node)) {
		v4l2_disable_ioctl(&node->video_dev,
				   VIDIOC_ENUM_FRAMEINTERVALS);
		v4l2_disable_ioctl(&node->video_dev,
				   VIDIOC_ENUM_FRAMESIZES);
	}

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		cfe_err("Unable to register video device %s\n", vdev->name);
		return ret;
	}

	cfe_info("Registered [%s] node id %d successfully as /dev/video%u\n",
		 vdev->name, id, vdev->num);

	/*
	 * Acquire a reference to cfe, which will be released when the video
	 * device will be unregistered and userspace will have closed all open
	 * file handles.
	 */
	cfe_get(cfe);
	set_state(cfe, NODE_REGISTERED, id);

	return 0;
}

static void cfe_unregister_nodes(struct cfe_device *cfe)
{
	unsigned int i;

	for (i = 0; i < NUM_NODES; i++) {
		struct cfe_node *node = &cfe->node[i];

		if (check_state(cfe, NODE_REGISTERED, i)) {
			clear_state(cfe, NODE_REGISTERED, i);
			video_unregister_device(&node->video_dev);
		}
	}
}

static int cfe_link_node_pads(struct cfe_device *cfe)
{
	unsigned int i, source_pad = 0;
	int ret;

	for (i = 0; i < CSI2_NUM_CHANNELS; i++) {
		struct cfe_node *node = &cfe->node[i];

		if (!check_state(cfe, NODE_REGISTERED, i))
			continue;

		/* Find next source pad */
		while (source_pad < cfe->sensor->entity.num_pads &&
		       !(cfe->sensor->entity.pads[source_pad].flags &
							MEDIA_PAD_FL_SOURCE))
			source_pad++;

		if (source_pad < cfe->sensor->entity.num_pads) {
			/* Sensor -> CSI2 */
			ret = media_create_pad_link(&cfe->sensor->entity, source_pad,
						    &cfe->csi2.sd.entity, i,
						    MEDIA_LNK_FL_IMMUTABLE |
						    MEDIA_LNK_FL_ENABLED);
			if (ret)
				return ret;

			/* Dealt with that source_pad, look at the next one next time */
			source_pad++;
		}

		/* CSI2 channel # -> /dev/video# */
		ret = media_create_pad_link(&cfe->csi2.sd.entity,
					    node_desc[i].link_pad,
					    &node->video_dev.entity, 0, 0);
		if (ret)
			return ret;

		if (node_supports_image(node)) {
			/* CSI2 channel # -> FE Input */
			ret = media_create_pad_link(&cfe->csi2.sd.entity,
						    node_desc[i].link_pad,
						    &cfe->fe.sd.entity,
						    FE_STREAM_PAD, 0);
			if (ret)
				return ret;
		}
	}

	for (; i < NUM_NODES; i++) {
		struct cfe_node *node = &cfe->node[i];
		struct media_entity *src, *dst;
		unsigned int src_pad, dst_pad;

		if (node_desc[i].pad_flags & MEDIA_PAD_FL_SINK) {
			/* FE -> /dev/video# */
			src = &cfe->fe.sd.entity;
			src_pad = node_desc[i].link_pad;
			dst = &node->video_dev.entity;
			dst_pad = 0;
		} else {
			/* /dev/video# -> FE */
			dst = &cfe->fe.sd.entity;
			dst_pad = node_desc[i].link_pad;
			src = &node->video_dev.entity;
			src_pad = 0;
		}

		ret = media_create_pad_link(src, src_pad, dst, dst_pad, 0);
		if (ret)
			return ret;
	}

	return 0;
}

static int cfe_probe_complete(struct cfe_device *cfe)
{
	unsigned int i;
	int ret;

	cfe->v4l2_dev.notify = cfe_notify;

	for (i = 0; i < NUM_NODES; i++) {
		ret = cfe_register_node(cfe, i);
		if (ret) {
			cfe_err("Unable to register video node %u.\n", i);
			goto unregister;
		}
	}

	ret = cfe_link_node_pads(cfe);
	if (ret) {
		cfe_err("Unable to link node pads.\n");
		goto unregister;
	}

	ret = v4l2_device_register_subdev_nodes(&cfe->v4l2_dev);
	if (ret) {
		cfe_err("Unable to register subdev nodes.\n");
		goto unregister;
	}

	return 0;

unregister:
	cfe_unregister_nodes(cfe);
	return ret;
}

static int cfe_async_bound(struct v4l2_async_notifier *notifier,
			   struct v4l2_subdev *subdev,
			   struct v4l2_async_connection *asd)
{
	struct cfe_device *cfe = to_cfe_device(notifier->v4l2_dev);

	if (cfe->sensor) {
		cfe_info("Rejecting subdev %s (Already set!!)", subdev->name);
		return 0;
	}

	cfe->sensor = subdev;
	cfe_info("Using sensor %s for capture\n", subdev->name);

	return 0;
}

static int cfe_async_complete(struct v4l2_async_notifier *notifier)
{
	struct cfe_device *cfe = to_cfe_device(notifier->v4l2_dev);

	return cfe_probe_complete(cfe);
}

static const struct v4l2_async_notifier_operations cfe_async_ops = {
	.bound = cfe_async_bound,
	.complete = cfe_async_complete,
};

static int of_cfe_connect_subdevs(struct cfe_device *cfe)
{
	struct platform_device *pdev = cfe->pdev;
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct device_node *node = pdev->dev.of_node;
	struct device_node *ep_node;
	struct device_node *sensor_node;
	unsigned int lane;
	int ret = -EINVAL;

	/* Get the local endpoint and remote device. */
	ep_node = of_graph_get_next_endpoint(node, NULL);
	if (!ep_node) {
		cfe_err("can't get next endpoint\n");
		return -EINVAL;
	}

	cfe_dbg("ep_node is %pOF\n", ep_node);

	sensor_node = of_graph_get_remote_port_parent(ep_node);
	if (!sensor_node) {
		cfe_err("can't get remote parent\n");
		goto cleanup_exit;
	}

	cfe_info("found subdevice %pOF\n", sensor_node);

	/* Parse the local endpoint and validate its configuration. */
	v4l2_fwnode_endpoint_parse(of_fwnode_handle(ep_node), &ep);

	cfe->csi2.multipacket_line =
		fwnode_property_present(of_fwnode_handle(ep_node),
					"multipacket-line");

	if (ep.bus_type != V4L2_MBUS_CSI2_DPHY) {
		cfe_err("endpoint node type != CSI2\n");
		return -EINVAL;
	}

	for (lane = 0; lane < ep.bus.mipi_csi2.num_data_lanes; lane++) {
		if (ep.bus.mipi_csi2.data_lanes[lane] != lane + 1) {
			cfe_err("subdevice %pOF: data lanes reordering not supported\n",
				sensor_node);
			goto cleanup_exit;
		}
	}

	cfe->csi2.dphy.max_lanes = ep.bus.mipi_csi2.num_data_lanes;
	cfe->csi2.bus_flags = ep.bus.mipi_csi2.flags;

	cfe_dbg("subdevice %pOF: %u data lanes, flags=0x%08x, multipacket_line=%u\n",
		sensor_node, cfe->csi2.dphy.max_lanes, cfe->csi2.bus_flags,
		cfe->csi2.multipacket_line);

	/* Initialize and register the async notifier. */
	v4l2_async_nf_init(&cfe->notifier, &cfe->v4l2_dev);
	cfe->notifier.ops = &cfe_async_ops;

	cfe->asd = v4l2_async_nf_add_fwnode(&cfe->notifier,
					    of_fwnode_handle(sensor_node),
					    struct v4l2_async_connection);
	if (IS_ERR(cfe->asd)) {
		cfe_err("Error adding subdevice: %d\n", ret);
		goto cleanup_exit;
	}

	ret = v4l2_async_nf_register(&cfe->notifier);
	if (ret) {
		cfe_err("Error registering async notifier: %d\n", ret);
		ret = -EINVAL;
	}

cleanup_exit:
	of_node_put(sensor_node);
	of_node_put(ep_node);

	return ret;
}

static int cfe_probe(struct platform_device *pdev)
{
	struct cfe_device *cfe;
	char debugfs_name[32];
	int ret;

	cfe = kzalloc(sizeof(*cfe), GFP_KERNEL);
	if (!cfe)
		return -ENOMEM;

	platform_set_drvdata(pdev, cfe);

	kref_init(&cfe->kref);
	cfe->pdev = pdev;
	cfe->fe_csi2_channel = -1;
	spin_lock_init(&cfe->state_lock);

	cfe->csi2.base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cfe->csi2.base)) {
		dev_err(&pdev->dev, "Failed to get dma io block\n");
		ret = PTR_ERR(cfe->csi2.base);
		goto err_cfe_put;
	}

	cfe->csi2.dphy.base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(cfe->csi2.dphy.base)) {
		dev_err(&pdev->dev, "Failed to get host io block\n");
		ret = PTR_ERR(cfe->csi2.dphy.base);
		goto err_cfe_put;
	}

	cfe->mipi_cfg_base = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(cfe->mipi_cfg_base)) {
		dev_err(&pdev->dev, "Failed to get mipi cfg io block\n");
		ret = PTR_ERR(cfe->mipi_cfg_base);
		goto err_cfe_put;
	}

	cfe->fe.base = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(cfe->fe.base)) {
		dev_err(&pdev->dev, "Failed to get pisp fe io block\n");
		ret = PTR_ERR(cfe->fe.base);
		goto err_cfe_put;
	}

	ret = platform_get_irq(pdev, 0);
	if (ret <= 0) {
		dev_err(&pdev->dev, "No IRQ resource\n");
		ret = -EINVAL;
		goto err_cfe_put;
	}

	ret = devm_request_irq(&pdev->dev, ret, cfe_isr, 0, "rp1-cfe", cfe);
	if (ret) {
		dev_err(&pdev->dev, "Unable to request interrupt\n");
		ret = -EINVAL;
		goto err_cfe_put;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(&pdev->dev, "DMA enable failed\n");
		goto err_cfe_put;
	}

	/* TODO: Enable clock only when running. */
	cfe->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(cfe->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(cfe->clk),
				     "clock not found\n");

	cfe->mdev.dev = &pdev->dev;
	cfe->mdev.ops = &cfe_media_device_ops;
	strscpy(cfe->mdev.model, CFE_MODULE_NAME, sizeof(cfe->mdev.model));
	strscpy(cfe->mdev.serial, "", sizeof(cfe->mdev.serial));
	snprintf(cfe->mdev.bus_info, sizeof(cfe->mdev.bus_info), "platform:%s",
		 dev_name(&pdev->dev));

	media_device_init(&cfe->mdev);

	cfe->v4l2_dev.mdev = &cfe->mdev;

	ret = v4l2_device_register(&pdev->dev, &cfe->v4l2_dev);
	if (ret) {
		cfe_err("Unable to register v4l2 device.\n");
		goto err_cfe_put;
	}

	snprintf(debugfs_name, sizeof(debugfs_name), "rp1-cfe:%s",
		 dev_name(&pdev->dev));
	cfe->debugfs = debugfs_create_dir(debugfs_name, NULL);
	debugfs_create_file("format", 0444, cfe->debugfs, cfe, &format_fops);
	debugfs_create_file("regs", 0444, cfe->debugfs, cfe,
			    &mipi_cfg_regs_fops);

	/* Enable the block power domain */
	pm_runtime_enable(&pdev->dev);

	ret = pm_runtime_resume_and_get(&cfe->pdev->dev);
	if (ret)
		goto err_runtime_disable;

	cfe->csi2.v4l2_dev = &cfe->v4l2_dev;
	ret = csi2_init(&cfe->csi2, cfe->debugfs);
	if (ret) {
		cfe_err("Failed to init csi2 (%d)\n", ret);
		goto err_runtime_put;
	}

	cfe->fe.v4l2_dev = &cfe->v4l2_dev;
	ret = pisp_fe_init(&cfe->fe, cfe->debugfs);
	if (ret) {
		cfe_err("Failed to init pisp fe (%d)\n", ret);
		goto err_csi2_uninit;
	}

	cfe->mdev.hw_revision = cfe->fe.hw_revision;
	ret = media_device_register(&cfe->mdev);
	if (ret < 0) {
		cfe_err("Unable to register media-controller device.\n");
		goto err_pisp_fe_uninit;
	}

	ret = of_cfe_connect_subdevs(cfe);
	if (ret) {
		cfe_err("Failed to connect subdevs\n");
		goto err_media_unregister;
	}

	pm_runtime_put(&cfe->pdev->dev);

	return 0;

err_media_unregister:
	media_device_unregister(&cfe->mdev);
err_pisp_fe_uninit:
	pisp_fe_uninit(&cfe->fe);
err_csi2_uninit:
	csi2_uninit(&cfe->csi2);
err_runtime_put:
	pm_runtime_put(&cfe->pdev->dev);
err_runtime_disable:
	pm_runtime_disable(&pdev->dev);
	debugfs_remove(cfe->debugfs);
	v4l2_device_unregister(&cfe->v4l2_dev);
err_cfe_put:
	cfe_put(cfe);

	return ret;
}

static int cfe_remove(struct platform_device *pdev)
{
	struct cfe_device *cfe = platform_get_drvdata(pdev);

	debugfs_remove(cfe->debugfs);

	v4l2_async_nf_unregister(&cfe->notifier);
	media_device_unregister(&cfe->mdev);
	cfe_unregister_nodes(cfe);

	pisp_fe_uninit(&cfe->fe);
	csi2_uninit(&cfe->csi2);

	pm_runtime_disable(&pdev->dev);

	v4l2_device_unregister(&cfe->v4l2_dev);

	cfe_put(cfe);

	return 0;
}

static int cfe_runtime_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct cfe_device *cfe = platform_get_drvdata(pdev);

	clk_disable_unprepare(cfe->clk);

	return 0;
}

static int cfe_runtime_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct cfe_device *cfe = platform_get_drvdata(pdev);
	int ret;

	ret = clk_prepare_enable(cfe->clk);
	if (ret) {
		dev_err(dev, "Unable to enable clock\n");
		return ret;
	}

	return 0;
}

static const struct dev_pm_ops cfe_pm_ops = {
	SET_RUNTIME_PM_OPS(cfe_runtime_suspend, cfe_runtime_resume, NULL)
	SET_LATE_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static const struct of_device_id cfe_of_match[] = {
	{ .compatible = "raspberrypi,rp1-cfe" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, cfe_of_match);

static struct platform_driver cfe_driver = {
	.probe		= cfe_probe,
	.remove		= cfe_remove,
	.driver = {
		.name	= CFE_MODULE_NAME,
		.of_match_table = cfe_of_match,
		.pm = &cfe_pm_ops,
	},
};

module_platform_driver(cfe_driver);

MODULE_AUTHOR("Naushir Patuck <naush@raspberrypi.com>");
MODULE_DESCRIPTION("RP1 Camera Front End driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(CFE_VERSION);
