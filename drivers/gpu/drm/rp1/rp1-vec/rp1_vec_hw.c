// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM Driver for VEC output on Raspberry Pi RP1
 *
 * Copyright (c) 2023 Raspberry Pi Limited.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "rp1_vec.h"
#include "vec_regs.h"

#define BITS(field, val)    (((val) << (field ## _LSB)) & (field ## _BITS))
#define VEC_WRITE(reg, val) writel((val), vec->hw_base[RP1VEC_HW_BLOCK_VEC] + (reg ## _OFFSET))
#define VEC_READ(reg)	    readl(vec->hw_base[RP1VEC_HW_BLOCK_VEC] + (reg ## _OFFSET))

static void rp1vec_write_regs(struct rp1_vec *vec, u32 offset, u32 const *vals, u32 num)
{
	while (num--) {
		writel(*vals++, vec->hw_base[RP1VEC_HW_BLOCK_VEC] + offset);
		offset += 4;
	}
}

int rp1vec_hw_busy(struct rp1_vec *vec)
{
	/* Read the undocumented "pline_busy" flag */
	return VEC_READ(VEC_STATUS) & 1;
}

/* Table of supported input (in-memory/DMA) pixel formats. */
struct rp1vec_ipixfmt {
	u32 format; /* DRM format code				 */
	u32 mask;   /* RGB masks (10 bits each, left justified)	 */
	u32 shift;  /* RGB MSB positions in the memory word	 */
	u32 rgbsz;  /* Shifts used for scaling; also (BPP/8-1)	 */
};

#define MASK_RGB(r, g, b) \
	(BITS(VEC_IMASK_MASK_R, r) | BITS(VEC_IMASK_MASK_G, g) | BITS(VEC_IMASK_MASK_B, b))
#define SHIFT_RGB(r, g, b) \
	(BITS(VEC_SHIFT_SHIFT_R, r) | BITS(VEC_SHIFT_SHIFT_G, g) | BITS(VEC_SHIFT_SHIFT_B, b))

static const struct rp1vec_ipixfmt my_formats[] = {
	{
		.format = DRM_FORMAT_XRGB8888,
		.mask	= MASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift	= SHIFT_RGB(23, 15, 7),
		.rgbsz	= BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 3),
	},
	{
		.format = DRM_FORMAT_XBGR8888,
		.mask	= MASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift	= SHIFT_RGB(7, 15, 23),
		.rgbsz	= BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 3),
	},
	{
		.format = DRM_FORMAT_ARGB8888,
		.mask	= MASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift	= SHIFT_RGB(23, 15, 7),
		.rgbsz	= BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 3),
	},
	{
		.format = DRM_FORMAT_ABGR8888,
		.mask	= MASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift	= SHIFT_RGB(7, 15, 23),
		.rgbsz	= BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 3),
	},
	{
		.format = DRM_FORMAT_RGB888,
		.mask	= MASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift	= SHIFT_RGB(23, 15, 7),
		.rgbsz	= BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 2),
	},
	{
		.format = DRM_FORMAT_BGR888,
		.mask	= MASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift	= SHIFT_RGB(7, 15, 23),
		.rgbsz	= BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 2),
	},
	{
		.format = DRM_FORMAT_RGB565,
		.mask	= MASK_RGB(0x3e0, 0x3f0, 0x3e0),
		.shift	= SHIFT_RGB(15, 10, 4),
		.rgbsz	= BITS(VEC_RGBSZ_SCALE_R, 5) |
			  BITS(VEC_RGBSZ_SCALE_G, 6) |
			  BITS(VEC_RGBSZ_SCALE_B, 5) |
			  BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 1),
	}
};

/*
 * Hardware mode descriptions (@ 108 MHz VDAC clock)
 * See "vec_regs.h" for further descriptions of these registers and fields.
 * Driver should adjust some values for other TV standards and for pixel rate,
 * and must ensure that ((de_end - de_bgn) % rate) == 0.
 */

struct rp1vec_hwmode {
	u16  max_rows_per_field;   /* active lines per field (including partial ones)  */
	u16  ref_vfp;		   /* nominal (vsync_start - vdisplay) when max height */
	bool interlaced;	   /* set for interlaced			       */
	bool first_field_odd;	   /* depends confusingly on line numbering convention */
	s16  scale_v;		   /* V scale in 2.8 format (for power-of-2 CIC rates) */
	s16  scale_u;		   /* U scale in 2.8 format (for power-of-2 CIC rates) */
	u16  scale_y;		   /* Y scale in 2.8 format (for power-of-2 CIC rates) */
	u16  de_end;		   /* end of horizontal Data Active period at 108MHz   */
	u16  de_bgn;		   /* start of horizontal Data Active period	       */
	u16  half_lines_per_field; /* number of half lines per field		       */
	s16  pedestal;		   /* pedestal (1024 = 100IRE) including FIR overshoot */
	u16  scale_luma;	   /* back end luma scaling in 1.15 format wrt DAC FSD */
	u16  scale_sync;	   /* back end sync scaling / blanking level as above  */
	u32  scale_burst_chroma;   /* back end { burst, chroma } scaling	       */
	u32  misc;		   /* Contents of the "EC" register except rate,shift  */
	u64  nco_freq;		   /* colour carrier frequency * (2**64) / 108MHz      */
	u32  timing_regs[14];	   /* other back end registers 0x84 .. 0xB8	       */
};

/* { NTSC, PAL, PAL-M } x { progressive, interlaced } */
static const struct rp1vec_hwmode rp1vec_hwmodes[3][2] = {
	{
		/* NTSC */
		{
			.max_rows_per_field = 240,
			.ref_vfp = 2,
			.interlaced = false,
			.first_field_odd = false,
			.scale_v = 0x0cf,
			.scale_u = 0x074,
			.scale_y = 0x107,
			.de_end	 = 0x1a4f,
			.de_bgn	 = 0x038f,
			.half_lines_per_field = 524, /* also works with 526/2 lines */
			.pedestal = 0x04c,
			.scale_luma = 0x8c9a,
			.scale_sync = 0x3851,
			.scale_burst_chroma = 0x11195561,
			.misc = 0x00090c00, /* 5-tap FIR, SEQ_EN, 4 fld sync */
			.nco_freq = 0x087c1f07c1f07c1f,
			.timing_regs = {
				0x03e10cc6, 0x0d6801fb, 0x023d034c, 0x00f80b6d,
				0x00000005, 0x0006000b, 0x000c0011, 0x000a0106,
				0x00000000, 0x00000000, 0x00000000, 0x00000000,
				0x00170106, 0x00000000
			},
		}, {
			.max_rows_per_field = 243,
			.ref_vfp = 3,
			.interlaced = true,
			.first_field_odd = true,
			.scale_v = 0x0cf,
			.scale_u = 0x074,
			.scale_y = 0x107,
			.de_end = 0x1a4f,
			.de_bgn = 0x038f,
			.half_lines_per_field = 525,
			.pedestal = 0x04c,
			.scale_luma = 0x8c9a,
			.scale_sync = 0x3851,
			.scale_burst_chroma = 0x11195561,
			.misc = 0x00094c02, /* 5-tap FIR, SEQ_EN, 2 flds, 4 fld sync, ilace */
			.nco_freq = 0x087c1f07c1f07c1f,
			.timing_regs = {
				0x03e10cc6, 0x0d6801fb, 0x023d034c, 0x00f80b6d,
				0x00000005, 0x0006000b, 0x000c0011, 0x000a0107,
				0x0111020d, 0x00000000, 0x00000000, 0x011c020d,
				0x00150106, 0x0107011b,
			},
		},
	}, {
		/* PAL */
		{
			.max_rows_per_field = 288,
			.ref_vfp = 2,
			.interlaced = false,
			.first_field_odd = false,
			.scale_v = 0x0e0,
			.scale_u = 0x07e,
			.scale_y = 0x11c,
			.de_end = 0x1ab6,
			.de_bgn = 0x03f6,
			.half_lines_per_field = 624,
			.pedestal = 0x00a, /* nonzero for max FIR overshoot after CIC */
			.scale_luma = 0x89d8,
			.scale_sync = 0x3c00,
			.scale_burst_chroma = 0x0caf53b5,
			.misc = 0x00091c01, /* 5-tap FIR, SEQ_EN, 8 fld sync, PAL */
			.nco_freq = 0x0a8262b2cc48c1d1,
			.timing_regs = {
				0x04660cee, 0x0d8001fb, 0x025c034f, 0x00fd0b84,
				0x026c0270, 0x00000004, 0x00050009, 0x00070135,
				0x00000000, 0x00000000, 0x00000000, 0x00000000,
				0x00170136, 0x00000000,
			},
		}, {
			.max_rows_per_field = 288,
			.ref_vfp = 5,
			.interlaced = true,
			.first_field_odd = false,
			.scale_v = 0x0e0,
			.scale_u = 0x07e,
			.scale_y = 0x11c,
			.de_end = 0x1ab6,
			.de_bgn = 0x03f6,
			.half_lines_per_field = 625,
			.pedestal = 0x00a,
			.scale_luma = 0x89d8,
			.scale_sync = 0x3c00,
			.scale_burst_chroma = 0x0caf53b5,
			.misc = 0x0009dc03, /* 5-tap FIR, SEQ_EN, 4 flds, 8 fld sync, ilace, PAL */
			.nco_freq = 0x0a8262b2cc48c1d1,
			.timing_regs = {
				0x04660cee, 0x0d8001fb, 0x025c034f, 0x00fd0b84,
				0x026c0270, 0x00000004, 0x00050009, 0x00070135,
				0x013f026d, 0x00060136, 0x0140026e, 0x0150026e,
				0x00180136, 0x026f0017,
			},
		},
	}, {
		/* PAL-M */
		{
			.max_rows_per_field = 240,
			.ref_vfp = 2,
			.interlaced = false,
			.first_field_odd = false,
			.scale_v = 0x0e0,
			.scale_u = 0x07e,
			.scale_y = 0x11c,
			.de_end = 0x1a4f,
			.de_bgn = 0x038f,
			.half_lines_per_field = 524,
			.pedestal = 0x00a,
			.scale_luma = 0x89d8,
			.scale_sync = 0x3851,
			.scale_burst_chroma = 0x0d5c53b5,
			.misc = 0x00091c01, /* 5-tap FIR, SEQ_EN, 8 fld sync PAL */
			.nco_freq = 0x0879bbf8d6d33ea8,
			.timing_regs = {
				0x03e10cc6, 0x0d6801fb, 0x023c034c, 0x00f80b6e,
				0x00000005, 0x0006000b, 0x000c0011, 0x000a0106,
				0x00000000, 0x00000000, 0x00000000, 0x00000000,
				0x00170106, 0x00000000,
			},
		}, {
			.max_rows_per_field = 243,
			.ref_vfp = 3,
			.interlaced = true,
			.first_field_odd = true,
			.scale_v = 0x0e0,
			.scale_u = 0x07e,
			.scale_y = 0x11c,
			.de_end = 0x1a4f,
			.de_bgn = 0x038f,
			.half_lines_per_field = 525,
			.pedestal = 0x00a,
			.scale_luma = 0x89d8,
			.scale_sync = 0x3851,
			.scale_burst_chroma = 0x0d5c53b5,
			.misc = 0x0009dc03, /* 5-tap FIR, SEQ_EN, 4 flds, 8 fld sync, ilace, PAL */
			.nco_freq = 0x0879bbf8d6d33ea8,
			.timing_regs = {
				0x03e10cc6, 0x0d6801fb, 0x023c034c, 0x00f80b6e,
				0x00140019, 0x00000005, 0x0006000b, 0x00090103,
				0x010f0209, 0x00080102, 0x010e020a, 0x0119020a,
				0x00120103, 0x01040118,
			},
		},
	},
};

/* System A, System E */
static const struct rp1vec_hwmode rp1vec_vintage_modes[2] = {
	{
		.max_rows_per_field = 190,
		.ref_vfp = 0,
		.interlaced = true,
		.first_field_odd = true,
		.scale_v = 0,
		.scale_u = 0,
		.scale_y = 0x11c,
		.de_end = 0x2920,
		.de_bgn = 0x06a0,
		.half_lines_per_field = 405,
		.pedestal = 0x00a,
		.scale_luma = 0x89d8,
		.scale_sync = 0x3c00,
		.scale_burst_chroma = 0,
		.misc = 0x00084002, /* 5-tap FIR, 2 fields, interlace */
		.nco_freq = 0,
		.timing_regs = {
			0x06f01430, 0x14d503cc, 0x00000000, 0x000010de,
			0x00000000, 0x00000007, 0x00000000, 0x00000000,
			0x00000000, 0x00000000, 0x00000000, 0x00d90195,
			0x000e00ca, 0x00cb00d8,
		},
	}, {
		.max_rows_per_field = 369,
		.ref_vfp = 6,
		.interlaced = true,
		.first_field_odd = true,
		.scale_v = 0,
		.scale_u = 0,
		.scale_y = 0x11c,
		.de_end = 0x145f,
		.de_bgn = 0x03a7,
		.half_lines_per_field = 819,
		.pedestal = 0x0010,
		.scale_luma = 0x89d8,
		.scale_sync = 0x3b13,
		.scale_burst_chroma = 0,
		.misc = 0x00084002, /* 5-tap FIR, 2 fields, interlace */
		.nco_freq = 0,
		.timing_regs = {
			0x03c10a08, 0x0a4d0114, 0x00000000, 0x000008a6,
			0x00000000, 0x00000000, 0x00000000, 0x00000000,
			0x00000000, 0x00000000, 0x00000000, 0x01c10330,
			0x00270196, 0x019701c0,
		},
	},
};

static const u32 rp1vec_fir_regs[4] = {
	0x00000000, 0x0be20200, 0x20f0f800, 0x265c7f00,
};

/*
 * Correction for the 4th order CIC filter's gain of (rate ** 4)
 * expressed as a right-shift and a reciprocal scale factor (Q12).
 * These arrays are indexed by [rate - 4] where 4 <= rate <= 16.
 */

static const int rp1vec_scale_table[13] = {
	4096, 6711, 6473, 6988,
	4096, 5114, 6711, 4584,
	6473, 4699, 6988, 5302,
	4096
};

static const u32 rp1vec_rate_shift_table[13] = {
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1,  3) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1,	7),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1,  4) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1,	9),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1,  5) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1, 10),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1,  6) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1, 11),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1,  7) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1, 11),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1,  8) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1, 12),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1,  9) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1, 13),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1, 10) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1, 13),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1, 11) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1, 14),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1, 12) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1, 14),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1, 13) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1, 15),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1, 14) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1, 15),
	BITS(VEC_DAC_EC_INTERP_RATE_MINUS1, 15) | BITS(VEC_DAC_EC_INTERP_SHIFT_MINUS1, 15),
};

void rp1vec_hw_setup(struct rp1_vec *vec,
		     u32 in_format,
		     struct drm_display_mode const *mode,
		     int tvstd)
{
	int i, mode_family, w, h;
	const struct rp1vec_hwmode *hwm;
	int wmax, hpad_r, vpad_b, rate, ref_2mid, usr_2mid;
	u32 misc;

	/* Input pixel format conversion */
	for (i = 0; i < ARRAY_SIZE(my_formats); ++i) {
		if (my_formats[i].format == in_format)
			break;
	}
	if (i >= ARRAY_SIZE(my_formats)) {
		dev_err(&vec->pdev->dev, "%s: bad input format\n", __func__);
		i = 0;
	}
	VEC_WRITE(VEC_IMASK, my_formats[i].mask);
	VEC_WRITE(VEC_SHIFT, my_formats[i].shift);
	VEC_WRITE(VEC_RGBSZ, my_formats[i].rgbsz);

	/* Pick an appropriate "base" mode, which we may modify.
	 * Note that this driver supports a limited selection of video modes.
	 * (A complete TV mode cannot be directly inferred from a DRM display mode:
	 * features such as chroma burst sequence, half-lines and equalizing pulses
	 * would be under-specified, and timings prone to rounding errors.)
	 */
	if (mode->vtotal == 405 || mode->vtotal == 819) {
		/* Systems A and E (interlaced only) */
		vec->fake_31khz = false;
		mode_family = 1;
		hwm = &rp1vec_vintage_modes[(mode->vtotal == 819) ? 1 : 0];
	} else {
		/* 525- and 625-line modes, with half-height and "fake" progressive variants */
		vec->fake_31khz = mode->vtotal >= 500 && !(mode->flags & DRM_MODE_FLAG_INTERLACE);
		h = (mode->vtotal >= 500) ? (mode->vtotal >> 1) : mode->vtotal;
		if (h >= 272)
			mode_family = 1; /* PAL-625 */
		else if (tvstd == DRM_MODE_TV_MODE_PAL_M || tvstd == DRM_MODE_TV_MODE_PAL)
			mode_family = 2; /* PAL-525 */
		else
			mode_family = 0; /* NTSC-525 */
		hwm = &rp1vec_hwmodes[mode_family][(mode->flags & DRM_MODE_FLAG_INTERLACE) ? 1 : 0];
	}

	/*
	 * Choose the upsampling rate (to 108MHz) in the range 4..16.
	 * Clip dimensions to the limits of the chosen hardware mode, then add
	 * padding as required, making some attempt to respect the DRM mode's
	 * display position (relative to H and V sync start). Note that "wmax"
	 * should be wider than the horizontal active region, to avoid boundary
	 * artifacts (e.g. wmax = 728, w = 720, active ~= 704 in Rec.601 modes).
	 */
	i = (vec->fake_31khz) ? (mode->clock >> 1) : mode->clock;
	rate = (i < (RP1VEC_VDAC_KHZ / 16)) ? 16 : max(4, (RP1VEC_VDAC_KHZ + 256) / i);
	wmax = min((hwm->de_end - hwm->de_bgn) / rate, 1020);
	w = min(mode->hdisplay, wmax);
	ref_2mid = (hwm->de_bgn + hwm->de_end) / rate + 4; /* + 4 for FIR delay */
	usr_2mid = (2 * (mode->htotal - mode->hsync_start) + w) * 2 * (hwm->timing_regs[1] >> 16) /
		(rate * mode->htotal);
	hpad_r = (wmax - w + ref_2mid - usr_2mid) >> 1;
	hpad_r = min(max(0, hpad_r), wmax - w);
	h = mode->vdisplay >> (hwm->interlaced || vec->fake_31khz);
	h = min(h, 0 + hwm->max_rows_per_field);
	vpad_b = ((mode->vsync_start - hwm->ref_vfp) >> (hwm->interlaced || vec->fake_31khz)) - h;
	vpad_b = min(max(0, vpad_b), hwm->max_rows_per_field - h);

	/* Configure the hardware "front end" (in the sysclock domain) */
	VEC_WRITE(VEC_APB_TIMEOUT, 0x38);
	VEC_WRITE(VEC_QOS,
		  BITS(VEC_QOS_DQOS, 0x0) |
		  BITS(VEC_QOS_ULEV, 0x8) |
		  BITS(VEC_QOS_UQOS, 0x2) |
		  BITS(VEC_QOS_LLEV, 0x4) |
		  BITS(VEC_QOS_LQOS, 0x7));
	VEC_WRITE(VEC_DMA_AREA,
		  BITS(VEC_DMA_AREA_COLS_MINUS1, w - 1) |
		  BITS(VEC_DMA_AREA_ROWS_PER_FIELD_MINUS1, h - 1));
	VEC_WRITE(VEC_YUV_SCALING,
		  BITS(VEC_YUV_SCALING_U10_SCALE_Y,
		       (hwm->scale_y * rp1vec_scale_table[rate - 4] + 2048) >> 12) |
		  BITS(VEC_YUV_SCALING_S10_SCALE_U,
		       (hwm->scale_u * rp1vec_scale_table[rate - 4] + 2048) >> 12) |
		  BITS(VEC_YUV_SCALING_S10_SCALE_V,
		       (hwm->scale_v * rp1vec_scale_table[rate - 4] + 2048) >> 12));
	VEC_WRITE(VEC_BACK_PORCH,
		  BITS(VEC_BACK_PORCH_HBP_MINUS1, wmax - w - hpad_r - 1) |
		  BITS(VEC_BACK_PORCH_VBP_MINUS1, hwm->max_rows_per_field - h - vpad_b - 1));
	VEC_WRITE(VEC_FRONT_PORCH,
		  BITS(VEC_FRONT_PORCH_HFP_MINUS1, hpad_r - 1) |
		  BITS(VEC_FRONT_PORCH_VFP_MINUS1, vpad_b - 1));
	VEC_WRITE(VEC_MODE,
		  BITS(VEC_MODE_HIGH_WATER, 0xE0)				|
		  BITS(VEC_MODE_ALIGN16, !((w | mode->hdisplay) & 15))		|
		  BITS(VEC_MODE_VFP_EN, (vpad_b > 0))				|
		  BITS(VEC_MODE_VBP_EN, (hwm->max_rows_per_field > h + vpad_b)) |
		  BITS(VEC_MODE_HFP_EN, (hpad_r > 0))				|
		  BITS(VEC_MODE_HBP_EN, (wmax > w + hpad_r))			|
		  BITS(VEC_MODE_FIELDS_PER_FRAME_MINUS1, hwm->interlaced)	|
		  BITS(VEC_MODE_FIRST_FIELD_ODD, hwm->first_field_odd));

	/* Configure the hardware "back end" (in the VDAC clock domain) */
	VEC_WRITE(VEC_DAC_80,
		  BITS(VEC_DAC_80_U14_DE_BGN, hwm->de_bgn) |
		  BITS(VEC_DAC_80_U14_DE_END, hwm->de_bgn + wmax * rate));
	rp1vec_write_regs(vec, 0x84, hwm->timing_regs, ARRAY_SIZE(hwm->timing_regs));
	VEC_WRITE(VEC_DAC_C0, 0x0); /* DAC control/status -- not wired up in RP1 */
	VEC_WRITE(VEC_DAC_C4, 0x007bffff); /* DAC control -- not wired up in RP1 */
	misc = hwm->half_lines_per_field;
	if (misc == 524 && (mode->vtotal >> vec->fake_31khz) == 263)
		misc += 2;
	if (tvstd == DRM_MODE_TV_MODE_NTSC_J && mode_family == 0) {
		/* NTSC-J modification: reduce pedestal and increase gain */
		VEC_WRITE(VEC_DAC_BC,
			  BITS(VEC_DAC_BC_U11_HALF_LINES_PER_FIELD, misc) |
			  BITS(VEC_DAC_BC_S11_PEDESTAL, 0x00a));
		VEC_WRITE(VEC_DAC_C8,
			  BITS(VEC_DAC_C8_U16_SCALE_LUMA, 0x9400) |
			  BITS(VEC_DAC_C8_U16_SCALE_SYNC, hwm->scale_sync));
	} else {
		VEC_WRITE(VEC_DAC_BC,
			  BITS(VEC_DAC_BC_U11_HALF_LINES_PER_FIELD, misc) |
			  BITS(VEC_DAC_BC_S11_PEDESTAL, hwm->pedestal));
		VEC_WRITE(VEC_DAC_C8,
			  BITS(VEC_DAC_C8_U16_SCALE_LUMA, hwm->scale_luma) |
			  BITS(VEC_DAC_C8_U16_SCALE_SYNC, hwm->scale_sync));
	}
	VEC_WRITE(VEC_DAC_CC, (tvstd >= DRM_MODE_TV_MODE_SECAM) ? 0 : hwm->scale_burst_chroma);
	VEC_WRITE(VEC_DAC_D0, 0x02000000); /* ADC offsets -- not needed in RP1? */
	misc = hwm->misc;
	if ((tvstd == DRM_MODE_TV_MODE_NTSC_443 || tvstd == DRM_MODE_TV_MODE_PAL) &&
	    mode_family != 1) {
		/* Change colour carrier frequency to 4433618.75 Hz; disable hard sync */
		VEC_WRITE(VEC_DAC_D4, 0xcc48c1d1);
		VEC_WRITE(VEC_DAC_D8, 0x0a8262b2);
		misc &= ~VEC_DAC_EC_SEQ_EN_BITS;
	} else if (tvstd == DRM_MODE_TV_MODE_PAL_N && mode_family == 1) {
		/* Change colour carrier frequency to 3582056.25 Hz */
		VEC_WRITE(VEC_DAC_D4, 0x9ce075f7);
		VEC_WRITE(VEC_DAC_D8, 0x087da511);
	} else {
		VEC_WRITE(VEC_DAC_D4, (u32)(hwm->nco_freq));
		VEC_WRITE(VEC_DAC_D8, (u32)(hwm->nco_freq >> 32));
	}
	VEC_WRITE(VEC_DAC_EC, misc | rp1vec_rate_shift_table[rate - 4]);
	rp1vec_write_regs(vec, 0xDC, rp1vec_fir_regs, ARRAY_SIZE(rp1vec_fir_regs));

	/* Set up interrupts and initialise VEC. It will start on the next rp1vec_hw_update() */
	VEC_WRITE(VEC_IRQ_FLAGS, 0xFFFFFFFFu);
	rp1vec_hw_vblank_ctrl(vec, 1);
	i = rp1vec_hw_busy(vec);
	if (i)
		dev_warn(&vec->pdev->dev,
			 "%s: VEC unexpectedly busy at start (0x%08x)",
			__func__, VEC_READ(VEC_STATUS));

	VEC_WRITE(VEC_CONTROL,
		  BITS(VEC_CONTROL_START_ARM, (!i)) |
		  BITS(VEC_CONTROL_AUTO_REPEAT, 1));
}

void rp1vec_hw_update(struct rp1_vec *vec, dma_addr_t addr, u32 offset, u32 stride)
{
	/*
	 * Update STRIDE, DMAH and DMAL only. When called after rp1vec_hw_setup(),
	 * DMA starts immediately; if already running, the buffer will flip at
	 * the next vertical sync event.
	 */
	u64 a = addr + offset;

	if (vec->fake_31khz) {
		a += stride;
		stride *= 2;
	}
	VEC_WRITE(VEC_DMA_STRIDE, stride);
	VEC_WRITE(VEC_DMA_ADDR_H, a >> 32);
	VEC_WRITE(VEC_DMA_ADDR_L, a & 0xFFFFFFFFu);
}

void rp1vec_hw_stop(struct rp1_vec *vec)
{
	/*
	 * Stop DMA by turning off the Auto-Repeat flag, and wait up to 100ms for
	 * the current and any queued frame to end. "Force drain" flags are not used,
	 * as they seem to prevent DMA from re-starting properly; it's safer to wait.
	 */

	reinit_completion(&vec->finished);
	VEC_WRITE(VEC_CONTROL, 0);
	if (!wait_for_completion_timeout(&vec->finished, HZ / 10))
		drm_err(&vec->drm, "%s: timed out waiting for idle\n", __func__);
	VEC_WRITE(VEC_IRQ_ENABLES, 0);
}

void rp1vec_hw_vblank_ctrl(struct rp1_vec *vec, int enable)
{
	VEC_WRITE(VEC_IRQ_ENABLES,
		  BITS(VEC_IRQ_ENABLES_DONE, 1) |
		  BITS(VEC_IRQ_ENABLES_DMA, (enable ? 1 : 0)) |
		  BITS(VEC_IRQ_ENABLES_MATCH_ROW, 1023));
}

irqreturn_t rp1vec_hw_isr(int irq, void *dev)
{
	struct rp1_vec *vec = dev;
	u32 u = VEC_READ(VEC_IRQ_FLAGS);

	if (u) {
		VEC_WRITE(VEC_IRQ_FLAGS, u);
		if (u & VEC_IRQ_FLAGS_DMA_BITS)
			drm_crtc_handle_vblank(&vec->pipe.crtc);
		if (u & VEC_IRQ_FLAGS_DONE_BITS)
			complete(&vec->finished);
	}
	return u ? IRQ_HANDLED : IRQ_NONE;
}
