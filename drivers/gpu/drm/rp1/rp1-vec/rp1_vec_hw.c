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

#define BITS(field, val) (((val) << (field ## _LSB)) & (field ## _BITS))

#define VEC_WRITE(reg, val) writel((val), vec->hw_base[RP1VEC_HW_BLOCK_VEC] + (reg ## _OFFSET))
#define VEC_READ(reg)	    readl(vec->hw_base[RP1VEC_HW_BLOCK_VEC] + (reg ## _OFFSET))

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
		.shift  = SHIFT_RGB(23, 15, 7),
		.rgbsz  = BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 3),
	},
	{
		.format = DRM_FORMAT_XBGR8888,
		.mask	= MASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift  = SHIFT_RGB(7, 15, 23),
		.rgbsz  = BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 3),
	},
	{
		.format = DRM_FORMAT_RGB888,
		.mask	= MASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift  = SHIFT_RGB(23, 15, 7),
		.rgbsz  = BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 2),
	},
	{
		.format = DRM_FORMAT_BGR888,
		.mask	= MASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift  = SHIFT_RGB(7, 15, 23),
		.rgbsz  = BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 2),
	},
	{
		.format = DRM_FORMAT_RGB565,
		.mask	= MASK_RGB(0x3e0, 0x3f0, 0x3e0),
		.shift  = SHIFT_RGB(15, 10, 4),
		.rgbsz  = BITS(VEC_RGBSZ_SCALE_R, 5) |
			  BITS(VEC_RGBSZ_SCALE_G, 6) |
			  BITS(VEC_RGBSZ_SCALE_B, 5) |
			  BITS(VEC_RGBSZ_BYTES_PER_PIXEL_MINUS1, 1),
	}
};

/*
 * Hardware mode descriptions (@ 108 MHz clock rate).
 * These rely largely on "canned" register settings.
 */

struct rp1vec_hwmode {
	u16  total_cols;	/* max active columns incl. padding and windowing   */
	u16  rows_per_field;	/* active lines per field (including partial ones)  */
	u16  ref_hfp;		/* nominal (hsync_start - hdisplay) when max width  */
	u16  ref_vfp;		/* nominal (vsync_start - vdisplay) when max height */
	bool interlaced;	/* set for interlaced				    */
	bool first_field_odd;	/* set for interlaced and 30fps			    */
	u32  yuv_scaling;	/* three 10-bit fields {Y, U, V} in 2.8 format	    */
	u32  back_end_regs[28]; /* All registers 0x80 .. 0xEC			    */
};

/* { NTSC, PAL, PAL-M } x { progressive, interlaced } x { 13.5 MHz, 15.428571 MHz } */
static const struct rp1vec_hwmode rp1vec_hwmodes[3][2][2] = {
	{
		/* NTSC */
		{
			{
				.total_cols = 724,
				.rows_per_field = 240,
				.ref_hfp = 12,
				.ref_vfp = 2,
				.interlaced = false,
				.first_field_odd = false,
				.yuv_scaling = 0x1071d0cf,
				.back_end_regs = {
					0x039f1a3f, 0x03e10cc6, 0x0d6801fb, 0x023d034c,
					0x00f80b6d, 0x00000005, 0x0006000b, 0x000c0011,
					0x000a0106, 0x00000000, 0x00000000, 0x00000000,
					0x00000000, 0x00170106, 0x00000000, 0x004c020e,
					0x00000000, 0x007bffff, 0x38518c9a, 0x11195561,
					0x02000200, 0xc1f07c1f, 0x087c1f07, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x000801ec,
				},
			}, {
				.total_cols = 815,
				.rows_per_field = 240,
				.ref_hfp = 16,
				.ref_vfp = 2,
				.interlaced = false,
				.first_field_odd = false,
				.yuv_scaling = 0x1c131962,
				.back_end_regs = {
					0x03ce1a17, 0x03e10cc6, 0x0d6801fb, 0x023d034c,
					0x00f80b6d, 0x00000005, 0x0006000b, 0x000c0011,
					0x000a0106, 0x00000000, 0x00000000, 0x00000000,
					0x00000000, 0x00170106, 0x00000000, 0x004c020e,
					0x00000000, 0x007bffff, 0x38518c9a, 0x11195561,
					0x02000200, 0xc1f07c1f, 0x087c1f07, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x000801ac,
				},
			},
		}, {
			{
				.total_cols = 724,
				.rows_per_field = 243,
				.ref_hfp = 12,
				.ref_vfp = 3,
				.interlaced = true,
				.first_field_odd = true,
				.yuv_scaling = 0x1071d0cf,
				.back_end_regs = {
					0x039f1a3f, 0x03e10cc6, 0x0d6801fb, 0x023d034c,
					0x00f80b6d, 0x00000005, 0x0006000b, 0x000c0011,
					0x000a0107, 0x0111020d, 0x00000000, 0x00000000,
					0x011c020d, 0x00150106, 0x0107011b, 0x004c020d,
					0x00000000, 0x007bffff, 0x38518c9a, 0x11195561,
					0x02000200, 0xc1f07c1f, 0x087c1f07, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x00094dee,
				},
			}, {
				.total_cols = 815,
				.rows_per_field = 243,
				.ref_hfp = 16,
				.ref_vfp = 3,
				.interlaced = true,
				.first_field_odd = true,
				.yuv_scaling = 0x1c131962,
				.back_end_regs = {
					0x03ce1a17, 0x03e10cc6, 0x0d6801fb, 0x023d034c,
					0x00f80b6d, 0x00000005, 0x0006000b, 0x000c0011,
					0x000a0107, 0x0111020d, 0x00000000, 0x00000000,
					0x011c020d, 0x00150106, 0x0107011b, 0x004c020d,
					0x00000000, 0x007bffff, 0x38518c9a, 0x11195561,
					0x02000200, 0xc1f07c1f, 0x087c1f07, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x00094dae,
				},
			},
		},
	}, {
		/* PAL */
		{
			{
				.total_cols = 724,
				.rows_per_field = 288,
				.ref_hfp = 16,
				.ref_vfp = 2,
				.interlaced = false,
				.first_field_odd = false,
				.yuv_scaling = 0x11c1f8e0,
				.back_end_regs = {
					0x04061aa6, 0x046e0cee, 0x0d8001fb, 0x025c034f,
					0x00fd0b84, 0x026c0270, 0x00000004, 0x00050009,
					0x00070135, 0x00000000, 0x00000000, 0x00000000,
					0x00000000, 0x00170136, 0x00000000, 0x000a0270,
					0x00000000, 0x007bffff, 0x3b1389d8, 0x0caf53b5,
					0x02000200, 0xcc48c1d1, 0x0a8262b2, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x000801ed,
				},
			}, {
				.total_cols = 804,
				.rows_per_field = 288,
				.ref_hfp = 24,
				.ref_vfp = 2,
				.interlaced = false,
				.first_field_odd = false,
				.yuv_scaling = 0x1e635d7f,
				.back_end_regs = {
					0x045b1a57, 0x046e0cee, 0x0d8001fb, 0x025c034f,
					0x00fd0b84, 0x026c0270, 0x00000004, 0x00050009,
					0x00070135, 0x00000000, 0x00000000, 0x00000000,
					0x00000000, 0x00170136, 0x00000000, 0x000a0270,
					0x00000000, 0x007bffff, 0x3b1389d8, 0x0caf53b5,
					0x02000200, 0xcc48c1d1, 0x0a8262b2, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x000801ad,
				},
			},
		}, {
			{
				.total_cols = 724,
				.rows_per_field = 288,
				.ref_hfp = 16,
				.ref_vfp = 5,
				.interlaced = true,
				.first_field_odd = false,
				.yuv_scaling = 0x11c1f8e0,
				.back_end_regs = {
					0x04061aa6, 0x046e0cee, 0x0d8001fb, 0x025c034f,
					0x00fd0b84, 0x026c0270, 0x00000004, 0x00050009,
					0x00070135, 0x013f026d, 0x00060136, 0x0140026e,
					0x0150026e, 0x00180136, 0x026f0017, 0x000a0271,
					0x00000000, 0x007bffff, 0x3b1389d8, 0x0caf53b5,
					0x02000200, 0xcc48c1d1, 0x0a8262b2, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x0009ddef,
				},
			}, {
				.total_cols = 804,
				.rows_per_field = 288,
				.ref_hfp = 24,
				.ref_vfp = 5,
				.interlaced = true,
				.first_field_odd = false,
				.yuv_scaling = 0x1e635d7f,
				.back_end_regs = {
					0x045b1a57, 0x046e0cee, 0x0d8001fb, 0x025c034f,
					0x00fd0b84, 0x026c0270, 0x00000004, 0x00050009,
					0x00070135, 0x013f026d, 0x00060136, 0x0140026e,
					0x0150026e, 0x00180136, 0x026f0017, 0x000a0271,
					0x00000000, 0x007bffff, 0x3b1389d8, 0x0caf53b5,
					0x02000200, 0xcc48c1d1, 0x0a8262b2, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x0009ddaf,
				},
			},
		},
	}, {
		/* PAL-M */
		{
			{
				.total_cols = 724,
				.rows_per_field = 240,
				.ref_hfp = 12,
				.ref_vfp = 2,
				.interlaced = false,
				.first_field_odd = false,
				.yuv_scaling = 0x11c1f8e0,
				.back_end_regs = {
					0x039f1a3f, 0x03e10cc6, 0x0d6801fb, 0x023c034c,
					0x00f80b6e, 0x00000005, 0x0006000b, 0x000c0011,
					0x000a0106, 0x00000000, 0x00000000, 0x00000000,
					0x00000000, 0x00170106, 0x00000000, 0x000a020c,
					0x00000000, 0x007bffff, 0x385189d8, 0x0d5c53b5,
					0x02000200, 0xd6d33ea8, 0x0879bbf8, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x000801ed,
				},
			}, {
				.total_cols = 815,
				.rows_per_field = 240,
				.ref_hfp = 16,
				.ref_vfp = 2,
				.interlaced = false,
				.first_field_odd = false,
				.yuv_scaling = 0x1e635d7f,
				.back_end_regs = {
					0x03ce1a17, 0x03e10cc6, 0x0d6801fb, 0x023c034c,
					0x00f80b6e, 0x00000005, 0x0006000b, 0x000c0011,
					0x000a0106, 0x00000000, 0x00000000, 0x00000000,
					0x00000000, 0x00170106, 0x00000000, 0x000a020c,
					0x00000000, 0x007bffff, 0x385189d8, 0x0d5c53b5,
					0x02000200, 0xd6d33ea8, 0x0879bbf8, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x000801ad,
				},
			},
		}, {
			{
				.total_cols = 724,
				.rows_per_field = 243,
				.ref_hfp = 12,
				.ref_vfp = 3,
				.interlaced = true,
				.first_field_odd = true,
				.yuv_scaling = 0x11c1f8e0,
				.back_end_regs = {
					0x039f1a3f, 0x03e10cc6, 0x0d6801fb, 0x023c034c,
					0x00f80b6e, 0x00140019, 0x00000005, 0x0006000b,
					0x00090103, 0x010f0209, 0x00080102, 0x010e020a,
					0x0119020a, 0x00120103, 0x01040118, 0x000a020d,
					0x00000000, 0x007bffff, 0x385189d8, 0x0d5c53b5,
					0x02000200, 0xd6d33ea8, 0x0879bbf8, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x0009ddef,
				},
			}, {
				.total_cols = 815,
				.rows_per_field = 243,
				.ref_hfp = 16,
				.ref_vfp = 3,
				.interlaced = true,
				.first_field_odd = true,
				.yuv_scaling = 0x1e635d7f,
				.back_end_regs = {
					0x03ce1a17, 0x03e10cc6, 0x0d6801fb, 0x023c034c,
					0x00f80b6e, 0x00140019, 0x00000005, 0x0006000b,
					0x00090103, 0x010f0209, 0x00080102, 0x010e020a,
					0x0119020a, 0x00120103, 0x01040118, 0x000a020d,
					0x00000000, 0x007bffff, 0x385189d8, 0x0d5c53b5,
					0x02000200, 0xd6d33ea8, 0x0879bbf8, 0x00000000,
					0x0be20200, 0x20f0f800, 0x265c7f00, 0x0009ddaf,
				},
			},
		},
	},
};

void rp1vec_hw_setup(struct rp1_vec *vec,
		     u32 in_format,
		     struct drm_display_mode const *mode,
		     int tvstd)
{
	unsigned int i, mode_family, mode_ilaced, mode_narrow;
	const struct rp1vec_hwmode *hwm;
	int w, h, hpad, vpad;

	/* Pick the appropriate "base" mode, which we may modify */
	mode_ilaced = !!(mode->flags & DRM_MODE_FLAG_INTERLACE);
	if (mode->vtotal >= 272 * (1 + mode_ilaced))
		mode_family = 1;
	else if (tvstd == DRM_MODE_TV_MODE_PAL_M || tvstd == DRM_MODE_TV_MODE_PAL)
		mode_family = 2;
	else
		mode_family = 0;
	mode_narrow = (mode->clock >= 14336);
	hwm = &rp1vec_hwmodes[mode_family][mode_ilaced][mode_narrow];
	dev_info(&vec->pdev->dev,
		 "%s: in_fmt=\'%c%c%c%c\' mode=%dx%d%s [%d%d%d] tvstd=%d",
		__func__, in_format, in_format >> 8, in_format >> 16, in_format >> 24,
		mode->hdisplay, mode->vdisplay, (mode_ilaced) ? "i" : "",
		mode_family, mode_ilaced, mode_narrow, tvstd);

	w = mode->hdisplay;
	h = mode->vdisplay >> mode_ilaced;
	if (w > hwm->total_cols)
		w = hwm->total_cols;
	if (h > hwm->rows_per_field)
		h = hwm->rows_per_field;

	/*
	 * Add padding so a framebuffer with the given dimensions and
	 * [hv]sync_start can be displayed in the chosen hardware mode.
	 *
	 *          |<----- mode->hsync_start ----->|
	 *          |<------ w ------>|             |
	 *          |                 |         >|--|<  ref_hfp
	 *                            |<- hpad ->|
	 * |<------------ total_cols ----------->|
	 *  ________FRAMEBUFFERCONTENTS__________
	 * '                                     `--\____/-<\/\/\>-'
	 */
	hpad = max(0, mode->hsync_start - hwm->ref_hfp - w);
	hpad = min(hpad, hwm->total_cols - w);
	vpad = max(0, ((mode->vsync_start - hwm->ref_vfp) >> mode_ilaced) - h);
	vpad = min(vpad, hwm->rows_per_field - h);

	/* Configure the hardware */
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
	VEC_WRITE(VEC_YUV_SCALING, hwm->yuv_scaling);
	VEC_WRITE(VEC_BACK_PORCH,
		  BITS(VEC_BACK_PORCH_HBP_MINUS1, hwm->total_cols - w - hpad - 1) |
		  BITS(VEC_BACK_PORCH_VBP_MINUS1, hwm->rows_per_field - h - vpad - 1));
	VEC_WRITE(VEC_FRONT_PORCH,
		  BITS(VEC_FRONT_PORCH_HFP_MINUS1, hpad - 1) |
		  BITS(VEC_FRONT_PORCH_VFP_MINUS1, vpad - 1));
	VEC_WRITE(VEC_MODE,
		  BITS(VEC_MODE_HIGH_WATER, 0xE0)			  |
		  BITS(VEC_MODE_ALIGN16, !((w | mode->hdisplay) & 15))	  |
		  BITS(VEC_MODE_VFP_EN, (vpad > 0))			  |
		  BITS(VEC_MODE_VBP_EN, (hwm->rows_per_field > h + vpad)) |
		  BITS(VEC_MODE_HFP_EN, (hpad > 0))			  |
		  BITS(VEC_MODE_HBP_EN, (hwm->total_cols > w + hpad))	  |
		  BITS(VEC_MODE_FIELDS_PER_FRAME_MINUS1, hwm->interlaced) |
		  BITS(VEC_MODE_FIRST_FIELD_ODD, hwm->first_field_odd));
	for (i = 0; i < ARRAY_SIZE(hwm->back_end_regs); ++i) {
		writel(hwm->back_end_regs[i],
		       vec->hw_base[RP1VEC_HW_BLOCK_VEC] + 0x80 + 4 * i);
	}

	/* Apply modifications */
	if (tvstd == DRM_MODE_TV_MODE_NTSC_J && mode_family == 0) {
		/* Reduce pedestal (not quite to zero, for FIR overshoot); increase gain */
		VEC_WRITE(VEC_DAC_BC,
			  BITS(VEC_DAC_BC_S11_PEDESTAL, 10) |
			  (hwm->back_end_regs[(0xBC - 0x80) / 4] & ~VEC_DAC_BC_S11_PEDESTAL_BITS));
		VEC_WRITE(VEC_DAC_C8,
			  BITS(VEC_DAC_C8_U16_SCALE_LUMA, 0x9400) |
			  (hwm->back_end_regs[(0xC8 - 0x80) / 4] &
							~VEC_DAC_C8_U16_SCALE_LUMA_BITS));
	} else if ((tvstd == DRM_MODE_TV_MODE_NTSC_443 || tvstd == DRM_MODE_TV_MODE_PAL) &&
		   mode_family != 1) {
		/* Change colour carrier frequency to 4433618.75 Hz; disable hard sync */
		VEC_WRITE(VEC_DAC_D4, 0xcc48c1d1);
		VEC_WRITE(VEC_DAC_D8, 0x0a8262b2);
		VEC_WRITE(VEC_DAC_EC,
			  hwm->back_end_regs[(0xEC - 0x80) / 4] & ~VEC_DAC_EC_SEQ_EN_BITS);
	} else if (tvstd == DRM_MODE_TV_MODE_PAL_N && mode_family == 1) {
		/* Change colour carrier frequency to 3582056.25 Hz */
		VEC_WRITE(VEC_DAC_D4, 0x9ce075f7);
		VEC_WRITE(VEC_DAC_D8, 0x087da511);
	}

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

	VEC_WRITE(VEC_IRQ_FLAGS, 0xffffffff);
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
