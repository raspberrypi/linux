/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Raspberry Pi PiSP common configuration definitions.
 *
 * Copyright (C) 2021 - Raspberry Pi (Trading) Ltd.
 *
 */
#ifndef _PISP_COMMON_H_
#define _PISP_COMMON_H_

#include <linux/types.h>

#include "pisp_types.h"

struct pisp_bla_config {
	uint16_t black_level_r;
	uint16_t black_level_gr;
	uint16_t black_level_gb;
	uint16_t black_level_b;
	uint16_t output_black_level;
	uint8_t pad[2];
};

struct pisp_wbg_config {
	uint16_t gain_r;
	uint16_t gain_g;
	uint16_t gain_b;
	uint8_t pad[2];
};

struct pisp_compress_config {
	/* value subtracted from incoming data */
	uint16_t offset;
	uint8_t pad;
	/* 1 => Companding; 2 => Delta (recommended); 3 => Combined (for HDR) */
	uint8_t mode;
};

struct pisp_decompress_config {
	/* value added to reconstructed data */
	uint16_t offset;
	uint8_t pad;
	/* 1 => Companding; 2 => Delta (recommended); 3 => Combined (for HDR) */
	uint8_t mode;
};

enum pisp_axi_flags {
	/* round down bursts to end at a 32-byte boundary, to align following bursts */
	PISP_AXI_FLAG_ALIGN = 128,
	 /* for FE writer: force WSTRB high, to pad output to 16-byte boundary */
	PISP_AXI_FLAG_PAD = 64,
	/* for FE writer: Use Output FIFO level to trigger "panic" */
	PISP_AXI_FLAG_PANIC = 32
};

struct pisp_axi_config {
	/* burst length minus one, which must be in the range 0:15; OR'd with flags */
	uint8_t maxlen_flags;
	/* { prot[2:0], cache[3:0] } fields, echoed on AXI bus */
	uint8_t cache_prot;
	/* QoS field(s) (4x4 bits for FE writer; 4 bits for other masters) */
	uint16_t qos;
};

#endif /* _PISP_COMMON_H_ */
