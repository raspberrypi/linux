/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RP1 PiSP common definitions.
 *
 * Copyright (C) 2021 - Raspberry Pi Ltd.
 *
 */
#ifndef _PISP_COMMON_H_
#define _PISP_COMMON_H_

#include "pisp_types.h"

struct pisp_bla_config {
	u16 black_level_r;
	u16 black_level_gr;
	u16 black_level_gb;
	u16 black_level_b;
	u16 output_black_level;
	u8 pad[2];
};

struct pisp_wbg_config {
	u16 gain_r;
	u16 gain_g;
	u16 gain_b;
	u8 pad[2];
};

struct pisp_compress_config {
	/* value subtracted from incoming data */
	u16 offset;
	u8 pad;
	/* 1 => Companding; 2 => Delta (recommended); 3 => Combined (for HDR) */
	u8 mode;
};

struct pisp_decompress_config {
	/* value added to reconstructed data */
	u16 offset;
	u8 pad;
	/* 1 => Companding; 2 => Delta (recommended); 3 => Combined (for HDR) */
	u8 mode;
};

enum pisp_axi_flags {
	/*
	 * round down bursts to end at a 32-byte boundary, to align following
	 * bursts
	 */
	PISP_AXI_FLAG_ALIGN = 128,
	/* for FE writer: force WSTRB high, to pad output to 16-byte boundary */
	PISP_AXI_FLAG_PAD = 64,
	/* for FE writer: Use Output FIFO level to trigger "panic" */
	PISP_AXI_FLAG_PANIC = 32,
};

struct pisp_axi_config {
	/*
	 * burst length minus one, which must be in the range 0:15; OR'd with
	 * flags
	 */
	u8 maxlen_flags;
	/* { prot[2:0], cache[3:0] } fields, echoed on AXI bus */
	u8 cache_prot;
	/* QoS field(s) (4x4 bits for FE writer; 4 bits for other masters) */
	u16 qos;
};

#endif /* _PISP_COMMON_H_ */
