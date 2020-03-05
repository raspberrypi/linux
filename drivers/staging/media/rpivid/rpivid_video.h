/* SPDX-License-Identifier: GPL-2.0 */
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

#ifndef _RPIVID_VIDEO_H_
#define _RPIVID_VIDEO_H_

struct rpivid_format {
	u32		pixelformat;
	u32		directions;
	unsigned int	capabilities;
};

extern const struct v4l2_ioctl_ops rpivid_ioctl_ops;

int rpivid_queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq);

size_t rpivid_bit_buf_size(unsigned int w, unsigned int h, unsigned int bits_minus8);
size_t rpivid_round_up_size(const size_t x);

void rpivid_prepare_src_format(struct v4l2_pix_format_mplane *pix_fmt);

#endif
