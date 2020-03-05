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

#ifndef _RPIVID_DEC_H_
#define _RPIVID_DEC_H_

void rpivid_device_run(void *priv);

#endif
