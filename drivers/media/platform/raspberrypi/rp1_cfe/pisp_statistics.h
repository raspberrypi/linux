/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RP1 PiSP Front End statistics definitions
 *
 * Copyright (C) 2021 - Raspberry Pi Ltd.
 *
 */
#ifndef _PISP_FE_STATISTICS_H_
#define _PISP_FE_STATISTICS_H_

#define PISP_FLOATING_STATS_NUM_ZONES 4
#define PISP_AGC_STATS_NUM_BINS 1024
#define PISP_AGC_STATS_SIZE 16
#define PISP_AGC_STATS_NUM_ZONES (PISP_AGC_STATS_SIZE * PISP_AGC_STATS_SIZE)
#define PISP_AGC_STATS_NUM_ROW_SUMS 512

struct pisp_agc_statistics_zone {
	u64 Y_sum;
	u32 counted;
	u32 pad;
};

struct pisp_agc_statistics {
	u32 row_sums[PISP_AGC_STATS_NUM_ROW_SUMS];
	/*
	 * 32-bits per bin means an image (just less than) 16384x16384 pixels
	 * in size can weight every pixel from 0 to 15.
	 */
	u32 histogram[PISP_AGC_STATS_NUM_BINS];
	struct pisp_agc_statistics_zone floating[PISP_FLOATING_STATS_NUM_ZONES];
};

#define PISP_AWB_STATS_SIZE 32
#define PISP_AWB_STATS_NUM_ZONES (PISP_AWB_STATS_SIZE * PISP_AWB_STATS_SIZE)

struct pisp_awb_statistics_zone {
	u32 R_sum;
	u32 G_sum;
	u32 B_sum;
	u32 counted;
};

struct pisp_awb_statistics {
	struct pisp_awb_statistics_zone zones[PISP_AWB_STATS_NUM_ZONES];
	struct pisp_awb_statistics_zone floating[PISP_FLOATING_STATS_NUM_ZONES];
};

#define PISP_CDAF_STATS_SIZE 8
#define PISP_CDAF_STATS_NUM_FOMS (PISP_CDAF_STATS_SIZE * PISP_CDAF_STATS_SIZE)

struct pisp_cdaf_statistics {
	u64 foms[PISP_CDAF_STATS_NUM_FOMS];
	u64 floating[PISP_FLOATING_STATS_NUM_ZONES];
};

struct pisp_statistics {
	struct pisp_awb_statistics awb;
	struct pisp_agc_statistics agc;
	struct pisp_cdaf_statistics cdaf;
};

#endif /* _PISP_FE_STATISTICS_H_ */
