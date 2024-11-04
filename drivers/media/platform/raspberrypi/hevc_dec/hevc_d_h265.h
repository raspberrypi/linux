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

#ifndef _HEVC_D_265_H_
#define _HEVC_D_265_H_

void hevc_d_h265_stop(struct hevc_d_ctx *ctx);
int hevc_d_h265_start(struct hevc_d_ctx *ctx);

void hevc_d_h265_run(void * priv);

#endif
