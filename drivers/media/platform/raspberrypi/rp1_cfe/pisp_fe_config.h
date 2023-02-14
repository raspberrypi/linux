/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * RP1 PiSP Front End Driver Configuration structures
 *
 * Copyright (C) 2021 - Raspberry Pi Ltd.
 *
 */
#ifndef _PISP_FE_CONFIG_
#define _PISP_FE_CONFIG_

#include "pisp_common.h"

#include "pisp_statistics.h"

#define PISP_FE_NUM_OUTPUTS 2

enum pisp_fe_enable {
	PISP_FE_ENABLE_INPUT = 0x000001,
	PISP_FE_ENABLE_DECOMPRESS = 0x000002,
	PISP_FE_ENABLE_DECOMPAND = 0x000004,
	PISP_FE_ENABLE_BLA = 0x000008,
	PISP_FE_ENABLE_DPC = 0x000010,
	PISP_FE_ENABLE_STATS_CROP = 0x000020,
	PISP_FE_ENABLE_DECIMATE = 0x000040,
	PISP_FE_ENABLE_BLC = 0x000080,
	PISP_FE_ENABLE_CDAF_STATS = 0x000100,
	PISP_FE_ENABLE_AWB_STATS = 0x000200,
	PISP_FE_ENABLE_RGBY = 0x000400,
	PISP_FE_ENABLE_LSC = 0x000800,
	PISP_FE_ENABLE_AGC_STATS = 0x001000,
	PISP_FE_ENABLE_CROP0 = 0x010000,
	PISP_FE_ENABLE_DOWNSCALE0 = 0x020000,
	PISP_FE_ENABLE_COMPRESS0 = 0x040000,
	PISP_FE_ENABLE_OUTPUT0 = 0x080000,
	PISP_FE_ENABLE_CROP1 = 0x100000,
	PISP_FE_ENABLE_DOWNSCALE1 = 0x200000,
	PISP_FE_ENABLE_COMPRESS1 = 0x400000,
	PISP_FE_ENABLE_OUTPUT1 = 0x800000
};

#define PISP_FE_ENABLE_CROP(i) (PISP_FE_ENABLE_CROP0 << (4 * (i)))
#define PISP_FE_ENABLE_DOWNSCALE(i) (PISP_FE_ENABLE_DOWNSCALE0 << (4 * (i)))
#define PISP_FE_ENABLE_COMPRESS(i) (PISP_FE_ENABLE_COMPRESS0 << (4 * (i)))
#define PISP_FE_ENABLE_OUTPUT(i) (PISP_FE_ENABLE_OUTPUT0 << (4 * (i)))

/*
 * We use the enable flags to show when blocks are "dirty", but we need some
 * extra ones too.
 */
enum pisp_fe_dirty {
	PISP_FE_DIRTY_GLOBAL = 0x0001,
	PISP_FE_DIRTY_FLOATING = 0x0002,
	PISP_FE_DIRTY_OUTPUT_AXI = 0x0004
};

struct pisp_fe_global_config {
	u32 enables;
	u8 bayer_order;
	u8 pad[3];
};

struct pisp_fe_input_axi_config {
	/* burst length minus one, in the range 0..15; OR'd with flags */
	u8 maxlen_flags;
	/* { prot[2:0], cache[3:0] } fields */
	u8 cache_prot;
	/* QoS (only 4 LS bits are used) */
	u16 qos;
};

struct pisp_fe_output_axi_config {
	/* burst length minus one, in the range 0..15; OR'd with flags */
	u8 maxlen_flags;
	/* { prot[2:0], cache[3:0] } fields */
	u8 cache_prot;
	/* QoS (4 bitfields of 4 bits each for different panic levels) */
	u16 qos;
	/*  For Panic mode: Output FIFO panic threshold */
	u16 thresh;
	/*  For Panic mode: Output FIFO statistics throttle threshold */
	u16 throttle;
};

struct pisp_fe_input_config {
	u8 streaming;
	u8 pad[3];
	struct pisp_image_format_config format;
	struct pisp_fe_input_axi_config axi;
	/* Extra cycles delay before issuing each burst request */
	u8 holdoff;
	u8 pad2[3];
};

struct pisp_fe_output_config {
	struct pisp_image_format_config format;
	u16 ilines;
	u8 pad[2];
};

struct pisp_fe_input_buffer_config {
	u32 addr_lo;
	u32 addr_hi;
	u16 frame_id;
	u16 pad;
};

#define PISP_FE_DECOMPAND_LUT_SIZE 65

struct pisp_fe_decompand_config {
	u16 lut[PISP_FE_DECOMPAND_LUT_SIZE];
	u16 pad;
};

struct pisp_fe_dpc_config {
	u8 coeff_level;
	u8 coeff_range;
	u8 coeff_range2;
#define PISP_FE_DPC_FLAG_FOLDBACK 1
#define PISP_FE_DPC_FLAG_VFLAG 2
	u8 flags;
};

#define PISP_FE_LSC_LUT_SIZE 16

struct pisp_fe_lsc_config {
	u8 shift;
	u8 pad0;
	u16 scale;
	u16 centre_x;
	u16 centre_y;
	u16 lut[PISP_FE_LSC_LUT_SIZE];
};

struct pisp_fe_rgby_config {
	u16 gain_r;
	u16 gain_g;
	u16 gain_b;
	u8 maxflag;
	u8 pad;
};

struct pisp_fe_agc_stats_config {
	u16 offset_x;
	u16 offset_y;
	u16 size_x;
	u16 size_y;
	/* each weight only 4 bits */
	u8 weights[PISP_AGC_STATS_NUM_ZONES / 2];
	u16 row_offset_x;
	u16 row_offset_y;
	u16 row_size_x;
	u16 row_size_y;
	u8 row_shift;
	u8 float_shift;
	u8 pad1[2];
};

struct pisp_fe_awb_stats_config {
	u16 offset_x;
	u16 offset_y;
	u16 size_x;
	u16 size_y;
	u8 shift;
	u8 pad[3];
	u16 r_lo;
	u16 r_hi;
	u16 g_lo;
	u16 g_hi;
	u16 b_lo;
	u16 b_hi;
};

struct pisp_fe_floating_stats_region {
	u16 offset_x;
	u16 offset_y;
	u16 size_x;
	u16 size_y;
};

struct pisp_fe_floating_stats_config {
	struct pisp_fe_floating_stats_region
					regions[PISP_FLOATING_STATS_NUM_ZONES];
};

#define PISP_FE_CDAF_NUM_WEIGHTS 8

struct pisp_fe_cdaf_stats_config {
	u16 noise_constant;
	u16 noise_slope;
	u16 offset_x;
	u16 offset_y;
	u16 size_x;
	u16 size_y;
	u16 skip_x;
	u16 skip_y;
	u32 mode;
};

struct pisp_fe_stats_buffer_config {
	u32 addr_lo;
	u32 addr_hi;
};

struct pisp_fe_crop_config {
	u16 offset_x;
	u16 offset_y;
	u16 width;
	u16 height;
};

enum pisp_fe_downscale_flags {
	DOWNSCALE_BAYER =
		1, /* downscale the four Bayer components independently... */
	DOWNSCALE_BIN =
		2 /* ...without trying to preserve their spatial relationship */
};

struct pisp_fe_downscale_config {
	u8 xin;
	u8 xout;
	u8 yin;
	u8 yout;
	u8 flags; /* enum pisp_fe_downscale_flags */
	u8 pad[3];
	u16 output_width;
	u16 output_height;
};

struct pisp_fe_output_buffer_config {
	u32 addr_lo;
	u32 addr_hi;
};

/* Each of the two output channels/branches: */
struct pisp_fe_output_branch_config {
	struct pisp_fe_crop_config crop;
	struct pisp_fe_downscale_config downscale;
	struct pisp_compress_config compress;
	struct pisp_fe_output_config output;
	u32 pad;
};

/* And finally one to rule them all: */
struct pisp_fe_config {
	/* I/O configuration: */
	struct pisp_fe_stats_buffer_config stats_buffer;
	struct pisp_fe_output_buffer_config output_buffer[PISP_FE_NUM_OUTPUTS];
	struct pisp_fe_input_buffer_config input_buffer;
	/* processing configuration: */
	struct pisp_fe_global_config global;
	struct pisp_fe_input_config input;
	struct pisp_decompress_config decompress;
	struct pisp_fe_decompand_config decompand;
	struct pisp_bla_config bla;
	struct pisp_fe_dpc_config dpc;
	struct pisp_fe_crop_config stats_crop;
	u32 spare1; /* placeholder for future decimate configuration */
	struct pisp_bla_config blc;
	struct pisp_fe_rgby_config rgby;
	struct pisp_fe_lsc_config lsc;
	struct pisp_fe_agc_stats_config agc_stats;
	struct pisp_fe_awb_stats_config awb_stats;
	struct pisp_fe_cdaf_stats_config cdaf_stats;
	struct pisp_fe_floating_stats_config floating_stats;
	struct pisp_fe_output_axi_config output_axi;
	struct pisp_fe_output_branch_config ch[PISP_FE_NUM_OUTPUTS];
	/* non-register fields: */
	u32 dirty_flags; /* these use pisp_fe_enable */
	u32 dirty_flags_extra; /* these use pisp_fe_dirty */
};

#endif /* _PISP_FE_CONFIG_ */
