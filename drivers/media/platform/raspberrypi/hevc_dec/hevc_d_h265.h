/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Raspberry Pi HEVC driver
 *
 * Copyright (C) 2024 Raspberry Pi Ltd
 *
 */

#ifndef _HEVC_D_H265_H_
#define _HEVC_D_H265_H_
#include "hevc_d.h"

extern const struct v4l2_ctrl_ops hevc_d_hevc_sps_ctrl_ops;
extern const struct v4l2_ctrl_ops hevc_d_hevc_pps_ctrl_ops;

void hevc_d_h265_setup(struct hevc_d_ctx *ctx, struct hevc_d_run *run);
int hevc_d_h265_start(struct hevc_d_ctx *ctx);
void hevc_d_h265_stop(struct hevc_d_ctx *ctx);
void hevc_d_h265_trigger(struct hevc_d_ctx *ctx);

void hevc_d_device_run(void *priv);

#endif
