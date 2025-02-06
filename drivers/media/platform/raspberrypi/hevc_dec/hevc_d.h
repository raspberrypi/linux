/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Raspberry Pi HEVC driver
 *
 * Copyright (C) 2024 Raspberry Pi Ltd
 *
 * Based on the Cedrus VPU driver, that is:
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 */

#ifndef _HEVC_D_H_
#define _HEVC_D_H_

#include <linux/clk.h>
#include <linux/platform_device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#define HEVC_D_DEC_ENV_COUNT 6
#define HEVC_D_P1BUF_COUNT 3
#define HEVC_D_P2BUF_COUNT 3

#define HEVC_D_NAME			"rpi-hevc-dec"

#define HEVC_D_CAPABILITY_UNTILED	BIT(0)
#define HEVC_D_CAPABILITY_H265_DEC	BIT(1)

#define HEVC_D_QUIRK_NO_DMA_OFFSET	BIT(0)

enum hevc_d_irq_status {
	HEVC_D_IRQ_NONE,
	HEVC_D_IRQ_ERROR,
	HEVC_D_IRQ_OK,
};

struct hevc_d_control {
	struct v4l2_ctrl_config cfg;
	unsigned char		required:1;
};

struct hevc_d_h265_run {
	u32 slice_ents;
	const struct v4l2_ctrl_hevc_sps			*sps;
	const struct v4l2_ctrl_hevc_pps			*pps;
	const struct v4l2_ctrl_hevc_decode_params	*dec;
	const struct v4l2_ctrl_hevc_slice_params	*slice_params;
	const struct v4l2_ctrl_hevc_scaling_matrix	*scaling_matrix;
};

struct hevc_d_run {
	struct vb2_v4l2_buffer	*src;
	struct vb2_v4l2_buffer	*dst;

	struct hevc_d_h265_run	h265;
};

struct hevc_d_buffer {
	struct v4l2_m2m_buffer          m2m_buf;
};

struct hevc_d_dec_state;
struct hevc_d_dec_env;

struct hevc_d_gptr {
	size_t size;
	__u8 *ptr;
	dma_addr_t addr;
	unsigned long attrs;
};

struct hevc_d_dev;
typedef void (*hevc_d_irq_callback)(struct hevc_d_dev *dev, void *ctx);

struct hevc_d_q_aux;
#define HEVC_D_AUX_ENT_COUNT VB2_MAX_FRAME

struct hevc_d_ctx {
	struct v4l2_fh			fh;
	struct hevc_d_dev		*dev;

	struct v4l2_pix_format_mplane	src_fmt;
	struct v4l2_pix_format_mplane	dst_fmt;
	int dst_fmt_set;

	int				src_stream_on;
	int				dst_stream_on;

	/*
	 * fatal_err is set if an error has occurred s.t. decode cannot
	 * continue (such as running out of CMA)
	 */
	int fatal_err;

	/* Lock for queue operations */
	struct mutex			ctx_mutex;

	struct v4l2_ctrl_handler	hdl;
	struct v4l2_ctrl		**ctrls;

	/*
	 * state contains stuff that is only needed in phase0
	 * it could be held in dec_env but that would be wasteful
	 */
	struct hevc_d_dec_state *state;
	struct hevc_d_dec_env *dec0;

	/* Spinlock protecting dec_free */
	spinlock_t dec_lock;
	struct hevc_d_dec_env *dec_free;

	struct hevc_d_dec_env *dec_pool;

	unsigned int p1idx;
	atomic_t p1out;

	unsigned int p2idx;
	struct hevc_d_gptr pu_bufs[HEVC_D_P2BUF_COUNT];
	struct hevc_d_gptr coeff_bufs[HEVC_D_P2BUF_COUNT];

	/* Spinlock protecting aux_free */
	spinlock_t aux_lock;
	struct hevc_d_q_aux *aux_free;

	struct hevc_d_q_aux *aux_ents[HEVC_D_AUX_ENT_COUNT];

	unsigned int colmv_stride;
	unsigned int colmv_picsize;
};

struct hevc_d_variant {
	unsigned int	capabilities;
	unsigned int	quirks;
	unsigned int	mod_rate;
};

struct hevc_d_hw_irq_ent;

#define HEVC_D_ICTL_ENABLE_UNLIMITED (-1)

struct hevc_d_hw_irq_ctrl {
	/* Spinlock protecting claim and tail */
	spinlock_t lock;
	struct hevc_d_hw_irq_ent *claim;
	struct hevc_d_hw_irq_ent *tail;

	/* Ent for pending irq - also prevents sched */
	struct hevc_d_hw_irq_ent *irq;
	/* Non-zero => do not start a new job - outer layer sched pending */
	int no_sched;
	/* Enable count. -1 always OK, 0 do not sched, +ve shed & count down */
	int enable;
	/* Thread CB requested */
	bool thread_reqed;
};

struct hevc_d_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	struct media_device	mdev;
	struct media_pad	pad[2];
	struct platform_device	*pdev;
	struct device		*dev;
	struct v4l2_m2m_dev	*m2m_dev;

	/* Device file mutex */
	struct mutex		dev_mutex;

	void __iomem		*base_irq;
	void __iomem		*base_h265;

	struct clk		*clock;
	unsigned long		max_clock_rate;

	int			cache_align;

	struct hevc_d_hw_irq_ctrl ic_active1;
	struct hevc_d_hw_irq_ctrl ic_active2;
};

struct v4l2_ctrl *hevc_d_find_ctrl(struct hevc_d_ctx *ctx, u32 id);
void *hevc_d_find_control_data(struct hevc_d_ctx *ctx, u32 id);

#endif
