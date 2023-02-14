// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM Driver for DSI output on Raspberry Pi RP1
 *
 * Copyright (c) 2023 Raspberry Pi Limited.
 */

#include <linux/interrupt.h>
#include <linux/platform_device.h>

#include <drm/drm_fourcc.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "rp1_dsi.h"

// --- DPI DMA REGISTERS (derived from Argon firmware, via RP1 drivers/mipi, with corrections) ---

// Control
#define DPI_DMA_CONTROL				      0x0
#define DPI_DMA_CONTROL_ARM_SHIFT		      0
#define DPI_DMA_CONTROL_ARM_MASK		      BIT(DPI_DMA_CONTROL_ARM_SHIFT)
#define DPI_DMA_CONTROL_ALIGN16_SHIFT		      2
#define DPI_DMA_CONTROL_ALIGN16_MASK		      BIT(DPI_DMA_CONTROL_ALIGN16_SHIFT)
#define DPI_DMA_CONTROL_AUTO_REPEAT_SHIFT	      1
#define DPI_DMA_CONTROL_AUTO_REPEAT_MASK	      BIT(DPI_DMA_CONTROL_AUTO_REPEAT_SHIFT)
#define DPI_DMA_CONTROL_HIGH_WATER_SHIFT	      3
#define DPI_DMA_CONTROL_HIGH_WATER_MASK		      (0x1FF << DPI_DMA_CONTROL_HIGH_WATER_SHIFT)
#define DPI_DMA_CONTROL_DEN_POL_SHIFT		      12
#define DPI_DMA_CONTROL_DEN_POL_MASK		      BIT(DPI_DMA_CONTROL_DEN_POL_SHIFT)
#define DPI_DMA_CONTROL_HSYNC_POL_SHIFT		      13
#define DPI_DMA_CONTROL_HSYNC_POL_MASK		      BIT(DPI_DMA_CONTROL_HSYNC_POL_SHIFT)
#define DPI_DMA_CONTROL_VSYNC_POL_SHIFT		      14
#define DPI_DMA_CONTROL_VSYNC_POL_MASK		      BIT(DPI_DMA_CONTROL_VSYNC_POL_SHIFT)
#define DPI_DMA_CONTROL_COLORM_SHIFT		      15
#define DPI_DMA_CONTROL_COLORM_MASK		      BIT(DPI_DMA_CONTROL_COLORM_SHIFT)
#define DPI_DMA_CONTROL_SHUTDN_SHIFT		      16
#define DPI_DMA_CONTROL_SHUTDN_MASK		      BIT(DPI_DMA_CONTROL_SHUTDN_SHIFT)
#define DPI_DMA_CONTROL_HBP_EN_SHIFT		      17
#define DPI_DMA_CONTROL_HBP_EN_MASK		      BIT(DPI_DMA_CONTROL_HBP_EN_SHIFT)
#define DPI_DMA_CONTROL_HFP_EN_SHIFT		      18
#define DPI_DMA_CONTROL_HFP_EN_MASK		      BIT(DPI_DMA_CONTROL_HFP_EN_SHIFT)
#define DPI_DMA_CONTROL_VBP_EN_SHIFT		      19
#define DPI_DMA_CONTROL_VBP_EN_MASK		      BIT(DPI_DMA_CONTROL_VBP_EN_SHIFT)
#define DPI_DMA_CONTROL_VFP_EN_SHIFT		      20
#define DPI_DMA_CONTROL_VFP_EN_MASK		      BIT(DPI_DMA_CONTROL_VFP_EN_SHIFT)
#define DPI_DMA_CONTROL_HSYNC_EN_SHIFT		      21
#define DPI_DMA_CONTROL_HSYNC_EN_MASK		      BIT(DPI_DMA_CONTROL_HSYNC_EN_SHIFT)
#define DPI_DMA_CONTROL_VSYNC_EN_SHIFT		      22
#define DPI_DMA_CONTROL_VSYNC_EN_MASK		      BIT(DPI_DMA_CONTROL_VSYNC_EN_SHIFT)
#define DPI_DMA_CONTROL_FORCE_IMMED_SHIFT	      23
#define DPI_DMA_CONTROL_FORCE_IMMED_MASK	      BIT(DPI_DMA_CONTROL_FORCE_IMMED_SHIFT)
#define DPI_DMA_CONTROL_FORCE_DRAIN_SHIFT	      24
#define DPI_DMA_CONTROL_FORCE_DRAIN_MASK	      BIT(DPI_DMA_CONTROL_FORCE_DRAIN_SHIFT)
#define DPI_DMA_CONTROL_FORCE_EMPTY_SHIFT	      25
#define DPI_DMA_CONTROL_FORCE_EMPTY_MASK	      BIT(DPI_DMA_CONTROL_FORCE_EMPTY_SHIFT)

// IRQ_ENABLES
#define DPI_DMA_IRQ_EN				      0x04
#define DPI_DMA_IRQ_EN_DMA_READY_SHIFT		      0
#define DPI_DMA_IRQ_EN_DMA_READY_MASK		      BIT(DPI_DMA_IRQ_EN_DMA_READY_SHIFT)
#define DPI_DMA_IRQ_EN_UNDERFLOW_SHIFT		      1
#define DPI_DMA_IRQ_EN_UNDERFLOW_MASK		      BIT(DPI_DMA_IRQ_EN_UNDERFLOW_SHIFT)
#define DPI_DMA_IRQ_EN_FRAME_START_SHIFT	      2
#define DPI_DMA_IRQ_EN_FRAME_START_MASK		      BIT(DPI_DMA_IRQ_EN_FRAME_START_SHIFT)
#define DPI_DMA_IRQ_EN_AFIFO_EMPTY_SHIFT	      3
#define DPI_DMA_IRQ_EN_AFIFO_EMPTY_MASK		      BIT(DPI_DMA_IRQ_EN_AFIFO_EMPTY_SHIFT)
#define DPI_DMA_IRQ_EN_TE_SHIFT			      4
#define DPI_DMA_IRQ_EN_TE_MASK			      BIT(DPI_DMA_IRQ_EN_TE_SHIFT)
#define DPI_DMA_IRQ_EN_ERROR_SHIFT		      5
#define DPI_DMA_IRQ_EN_ERROR_MASK		      BIT(DPI_DMA_IRQ_EN_ERROR_SHIFT)
#define DPI_DMA_IRQ_EN_MATCH_SHIFT		      6
#define DPI_DMA_IRQ_EN_MATCH_MASK		      BIT(DPI_DMA_IRQ_EN_MATCH_SHIFT)
#define DPI_DMA_IRQ_EN_MATCH_LINE_SHIFT		      16
#define DPI_DMA_IRQ_EN_MATCH_LINE_MASK		      (0xFFF << DPI_DMA_IRQ_EN_MATCH_LINE_SHIFT)

// IRQ_FLAGS
#define DPI_DMA_IRQ_FLAGS			      0x08
#define DPI_DMA_IRQ_FLAGS_DMA_READY_SHIFT	      0
#define DPI_DMA_IRQ_FLAGS_DMA_READY_MASK	      BIT(DPI_DMA_IRQ_FLAGS_DMA_READY_SHIFT)
#define DPI_DMA_IRQ_FLAGS_UNDERFLOW_SHIFT	      1
#define DPI_DMA_IRQ_FLAGS_UNDERFLOW_MASK	      BIT(DPI_DMA_IRQ_FLAGS_UNDERFLOW_SHIFT)
#define DPI_DMA_IRQ_FLAGS_FRAME_START_SHIFT	      2
#define DPI_DMA_IRQ_FLAGS_FRAME_START_MASK	      BIT(DPI_DMA_IRQ_FLAGS_FRAME_START_SHIFT)
#define DPI_DMA_IRQ_FLAGS_AFIFO_EMPTY_SHIFT	      3
#define DPI_DMA_IRQ_FLAGS_AFIFO_EMPTY_MASK	      BIT(DPI_DMA_IRQ_FLAGS_AFIFO_EMPTY_SHIFT)
#define DPI_DMA_IRQ_FLAGS_TE_SHIFT		      4
#define DPI_DMA_IRQ_FLAGS_TE_MASK		      BIT(DPI_DMA_IRQ_FLAGS_TE_SHIFT)
#define DPI_DMA_IRQ_FLAGS_ERROR_SHIFT		      5
#define DPI_DMA_IRQ_FLAGS_ERROR_MASK		      BIT(DPI_DMA_IRQ_FLAGS_ERROR_SHIFT)
#define DPI_DMA_IRQ_FLAGS_MATCH_SHIFT		      6
#define DPI_DMA_IRQ_FLAGS_MATCH_MASK		      BIT(DPI_DMA_IRQ_FLAGS_MATCH_SHIFT)

// QOS
#define DPI_DMA_QOS				      0xC
#define DPI_DMA_QOS_DQOS_SHIFT			      0
#define DPI_DMA_QOS_DQOS_MASK			      (0xF << DPI_DMA_QOS_DQOS_SHIFT)
#define DPI_DMA_QOS_ULEV_SHIFT			      4
#define DPI_DMA_QOS_ULEV_MASK			      (0xF << DPI_DMA_QOS_ULEV_SHIFT)
#define DPI_DMA_QOS_UQOS_SHIFT			      8
#define DPI_DMA_QOS_UQOS_MASK			      (0xF << DPI_DMA_QOS_UQOS_SHIFT)
#define DPI_DMA_QOS_LLEV_SHIFT			      12
#define DPI_DMA_QOS_LLEV_MASK			      (0xF << DPI_DMA_QOS_LLEV_SHIFT)
#define DPI_DMA_QOS_LQOS_SHIFT			      16
#define DPI_DMA_QOS_LQOS_MASK			      (0xF << DPI_DMA_QOS_LQOS_SHIFT)

// Panics
#define DPI_DMA_PANICS				     0x38
#define DPI_DMA_PANICS_UPPER_COUNT_SHIFT	     0
#define DPI_DMA_PANICS_UPPER_COUNT_MASK		     \
				(0x0000FFFF << DPI_DMA_PANICS_UPPER_COUNT_SHIFT)
#define DPI_DMA_PANICS_LOWER_COUNT_SHIFT	     16
#define DPI_DMA_PANICS_LOWER_COUNT_MASK		     \
				(0x0000FFFF << DPI_DMA_PANICS_LOWER_COUNT_SHIFT)

// DMA Address Lower:
#define DPI_DMA_DMA_ADDR_L			     0x10

// DMA Address Upper:
#define DPI_DMA_DMA_ADDR_H			     0x40

// DMA stride
#define DPI_DMA_DMA_STRIDE			     0x14

// Visible Area
#define DPI_DMA_VISIBLE_AREA			     0x18
#define DPI_DMA_VISIBLE_AREA_ROWSM1_SHIFT     0
#define DPI_DMA_VISIBLE_AREA_ROWSM1_MASK     (0x0FFF << DPI_DMA_VISIBLE_AREA_ROWSM1_SHIFT)
#define DPI_DMA_VISIBLE_AREA_COLSM1_SHIFT    16
#define DPI_DMA_VISIBLE_AREA_COLSM1_MASK     (0x0FFF << DPI_DMA_VISIBLE_AREA_COLSM1_SHIFT)

// Sync width
#define DPI_DMA_SYNC_WIDTH   0x1C
#define DPI_DMA_SYNC_WIDTH_ROWSM1_SHIFT	 0
#define DPI_DMA_SYNC_WIDTH_ROWSM1_MASK	 (0x0FFF << DPI_DMA_SYNC_WIDTH_ROWSM1_SHIFT)
#define DPI_DMA_SYNC_WIDTH_COLSM1_SHIFT	 16
#define DPI_DMA_SYNC_WIDTH_COLSM1_MASK	 (0x0FFF << DPI_DMA_SYNC_WIDTH_COLSM1_SHIFT)

// Back porch
#define DPI_DMA_BACK_PORCH   0x20
#define DPI_DMA_BACK_PORCH_ROWSM1_SHIFT	 0
#define DPI_DMA_BACK_PORCH_ROWSM1_MASK	 (0x0FFF << DPI_DMA_BACK_PORCH_ROWSM1_SHIFT)
#define DPI_DMA_BACK_PORCH_COLSM1_SHIFT	 16
#define DPI_DMA_BACK_PORCH_COLSM1_MASK	 (0x0FFF << DPI_DMA_BACK_PORCH_COLSM1_SHIFT)

// Front porch
#define DPI_DMA_FRONT_PORCH  0x24
#define DPI_DMA_FRONT_PORCH_ROWSM1_SHIFT     0
#define DPI_DMA_FRONT_PORCH_ROWSM1_MASK	 (0x0FFF << DPI_DMA_FRONT_PORCH_ROWSM1_SHIFT)
#define DPI_DMA_FRONT_PORCH_COLSM1_SHIFT     16
#define DPI_DMA_FRONT_PORCH_COLSM1_MASK	 (0x0FFF << DPI_DMA_FRONT_PORCH_COLSM1_SHIFT)

// Input masks
#define DPI_DMA_IMASK	 0x2C
#define DPI_DMA_IMASK_R_SHIFT	 0
#define DPI_DMA_IMASK_R_MASK	 (0x3FF << DPI_DMA_IMASK_R_SHIFT)
#define DPI_DMA_IMASK_G_SHIFT	 10
#define DPI_DMA_IMASK_G_MASK	 (0x3FF << DPI_DMA_IMASK_G_SHIFT)
#define DPI_DMA_IMASK_B_SHIFT	 20
#define DPI_DMA_IMASK_B_MASK	 (0x3FF << DPI_DMA_IMASK_B_SHIFT)

// Output Masks
#define DPI_DMA_OMASK	 0x30
#define DPI_DMA_OMASK_R_SHIFT	 0
#define DPI_DMA_OMASK_R_MASK	 (0x3FF << DPI_DMA_OMASK_R_SHIFT)
#define DPI_DMA_OMASK_G_SHIFT	 10
#define DPI_DMA_OMASK_G_MASK	 (0x3FF << DPI_DMA_OMASK_G_SHIFT)
#define DPI_DMA_OMASK_B_SHIFT	 20
#define DPI_DMA_OMASK_B_MASK	 (0x3FF << DPI_DMA_OMASK_B_SHIFT)

// Shifts
#define DPI_DMA_SHIFT	 0x28
#define DPI_DMA_SHIFT_IR_SHIFT	 0
#define DPI_DMA_SHIFT_IR_MASK	 (0x1F << DPI_DMA_SHIFT_IR_SHIFT)
#define DPI_DMA_SHIFT_IG_SHIFT	 5
#define DPI_DMA_SHIFT_IG_MASK	 (0x1F << DPI_DMA_SHIFT_IG_SHIFT)
#define DPI_DMA_SHIFT_IB_SHIFT	 10
#define DPI_DMA_SHIFT_IB_MASK	 (0x1F << DPI_DMA_SHIFT_IB_SHIFT)
#define DPI_DMA_SHIFT_OR_SHIFT	 15
#define DPI_DMA_SHIFT_OR_MASK	 (0x1F << DPI_DMA_SHIFT_OR_SHIFT)
#define DPI_DMA_SHIFT_OG_SHIFT	 20
#define DPI_DMA_SHIFT_OG_MASK	 (0x1F << DPI_DMA_SHIFT_OG_SHIFT)
#define DPI_DMA_SHIFT_OB_SHIFT	 25
#define DPI_DMA_SHIFT_OB_MASK	 (0x1F << DPI_DMA_SHIFT_OB_SHIFT)

// Scaling
#define DPI_DMA_RGBSZ	 0x34
#define DPI_DMA_RGBSZ_BPP_SHIFT	 16
#define DPI_DMA_RGBSZ_BPP_MASK	 (0x3 << DPI_DMA_RGBSZ_BPP_SHIFT)
#define DPI_DMA_RGBSZ_R_SHIFT	 0
#define DPI_DMA_RGBSZ_R_MASK	 (0xF << DPI_DMA_RGBSZ_R_SHIFT)
#define DPI_DMA_RGBSZ_G_SHIFT	 4
#define DPI_DMA_RGBSZ_G_MASK	 (0xF << DPI_DMA_RGBSZ_G_SHIFT)
#define DPI_DMA_RGBSZ_B_SHIFT	 8
#define DPI_DMA_RGBSZ_B_MASK	 (0xF << DPI_DMA_RGBSZ_B_SHIFT)

// Status
#define DPI_DMA_STATUS  0x3c

#define BITS(field, val) (((val) << (field ## _SHIFT)) & (field ## _MASK))

static unsigned int rp1dsi_dma_read(struct rp1_dsi *dsi, unsigned int reg)
{
	void __iomem *addr = dsi->hw_base[RP1DSI_HW_BLOCK_DMA] + reg;

	return readl(addr);
}

static void rp1dsi_dma_write(struct rp1_dsi *dsi, unsigned int reg, unsigned int val)
{
	void __iomem *addr = dsi->hw_base[RP1DSI_HW_BLOCK_DMA] + reg;

	writel(val, addr);
}

int rp1dsi_dma_busy(struct rp1_dsi *dsi)
{
	return (rp1dsi_dma_read(dsi, DPI_DMA_STATUS) & 0xF8F) ? 1 : 0;
}

/* Table of supported input (in-memory/DMA) pixel formats. */
struct rp1dsi_ipixfmt {
	u32 format; /* DRM format code                           */
	u32 mask;   /* RGB masks (10 bits each, left justified)  */
	u32 shift;  /* RGB MSB positions in the memory word      */
	u32 rgbsz;  /* Shifts used for scaling; also (BPP/8-1)   */
};

#define IMASK_RGB(r, g, b)	(BITS(DPI_DMA_IMASK_R, r) | \
				 BITS(DPI_DMA_IMASK_G, g) |  \
				 BITS(DPI_DMA_IMASK_B, b))
#define ISHIFT_RGB(r, g, b)	(BITS(DPI_DMA_SHIFT_IR, r) | \
				 BITS(DPI_DMA_SHIFT_IG, g) | \
				 BITS(DPI_DMA_SHIFT_IB, b))

static const struct rp1dsi_ipixfmt my_formats[] = {
	{
		.format = DRM_FORMAT_XRGB8888,
		.mask   = IMASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift  = ISHIFT_RGB(23, 15, 7),
		.rgbsz  = BITS(DPI_DMA_RGBSZ_BPP, 3),
	},
	{
		.format = DRM_FORMAT_XBGR8888,
		.mask   = IMASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift  = ISHIFT_RGB(7, 15, 23),
		.rgbsz  = BITS(DPI_DMA_RGBSZ_BPP, 3),
	},
	{
		.format = DRM_FORMAT_ARGB8888,
		.mask   = IMASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift  = ISHIFT_RGB(23, 15, 7),
		.rgbsz  = BITS(DPI_DMA_RGBSZ_BPP, 3),
	},
	{
		.format = DRM_FORMAT_ABGR8888,
		.mask   = IMASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift  = ISHIFT_RGB(7, 15, 23),
		.rgbsz  = BITS(DPI_DMA_RGBSZ_BPP, 3),
	},
	{
		.format = DRM_FORMAT_RGB888,
		.mask   = IMASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift  = ISHIFT_RGB(23, 15, 7),
		.rgbsz  = BITS(DPI_DMA_RGBSZ_BPP, 2),
	},
	{
		.format = DRM_FORMAT_BGR888,
		.mask   = IMASK_RGB(0x3fc, 0x3fc, 0x3fc),
		.shift  = ISHIFT_RGB(7, 15, 23),
		.rgbsz  = BITS(DPI_DMA_RGBSZ_BPP, 2),
	},
	{
		.format = DRM_FORMAT_RGB565,
		.mask   = IMASK_RGB(0x3e0, 0x3f0, 0x3e0),
		.shift  = ISHIFT_RGB(15, 10, 4),
		.rgbsz  = BITS(DPI_DMA_RGBSZ_R, 5) | BITS(DPI_DMA_RGBSZ_G, 6) |
			  BITS(DPI_DMA_RGBSZ_B, 5) | BITS(DPI_DMA_RGBSZ_BPP, 1),
	}
};

/* Choose the internal on-the-bus DPI format as expected by DSI Host. */
static u32 get_omask_oshift(enum mipi_dsi_pixel_format fmt, u32 *oshift)
{
	switch (fmt) {
	case MIPI_DSI_FMT_RGB565:
		*oshift = BITS(DPI_DMA_SHIFT_OR, 15) |
			  BITS(DPI_DMA_SHIFT_OG, 10) |
			  BITS(DPI_DMA_SHIFT_OB, 4);
		return BITS(DPI_DMA_OMASK_R, 0x3e0) |
		       BITS(DPI_DMA_OMASK_G, 0x3f0) |
		       BITS(DPI_DMA_OMASK_B, 0x3e0);
	case MIPI_DSI_FMT_RGB666_PACKED:
		*oshift = BITS(DPI_DMA_SHIFT_OR, 17) |
			  BITS(DPI_DMA_SHIFT_OG, 11) |
			  BITS(DPI_DMA_SHIFT_OB, 5);
		return BITS(DPI_DMA_OMASK_R, 0x3f0) |
		       BITS(DPI_DMA_OMASK_G, 0x3f0) |
		       BITS(DPI_DMA_OMASK_B, 0x3f0);
	case MIPI_DSI_FMT_RGB666:
		*oshift = BITS(DPI_DMA_SHIFT_OR, 21) |
			  BITS(DPI_DMA_SHIFT_OG, 13) |
			  BITS(DPI_DMA_SHIFT_OB, 5);
		return BITS(DPI_DMA_OMASK_R, 0x3f0) |
		       BITS(DPI_DMA_OMASK_G, 0x3f0) |
		       BITS(DPI_DMA_OMASK_B, 0x3f0);
	default:
		*oshift = BITS(DPI_DMA_SHIFT_OR, 23) |
			  BITS(DPI_DMA_SHIFT_OG, 15) |
			  BITS(DPI_DMA_SHIFT_OB, 7);
		return BITS(DPI_DMA_OMASK_R, 0x3fc) |
		       BITS(DPI_DMA_OMASK_G, 0x3fc) |
		       BITS(DPI_DMA_OMASK_B, 0x3fc);
	}
}

void rp1dsi_dma_setup(struct rp1_dsi *dsi,
		      u32 in_format, enum mipi_dsi_pixel_format out_format,
		     struct drm_display_mode const *mode)
{
	u32 oshift;
	int i;

	/*
	 * Configure all DSI/DPI/DMA block registers, except base address.
	 * DMA will not actually start until a FB base address is specified
	 * using rp1dsi_dma_update().
	 */

	rp1dsi_dma_write(dsi, DPI_DMA_VISIBLE_AREA,
			 BITS(DPI_DMA_VISIBLE_AREA_ROWSM1, mode->vdisplay - 1) |
			 BITS(DPI_DMA_VISIBLE_AREA_COLSM1, mode->hdisplay - 1));

	rp1dsi_dma_write(dsi, DPI_DMA_SYNC_WIDTH,
			 BITS(DPI_DMA_SYNC_WIDTH_ROWSM1, mode->vsync_end - mode->vsync_start - 1) |
			 BITS(DPI_DMA_SYNC_WIDTH_COLSM1, mode->hsync_end - mode->hsync_start - 1));

	/* In the DPIDMA registers, "back porch" time includes sync width */
	rp1dsi_dma_write(dsi, DPI_DMA_BACK_PORCH,
			 BITS(DPI_DMA_BACK_PORCH_ROWSM1, mode->vtotal - mode->vsync_start - 1) |
			 BITS(DPI_DMA_BACK_PORCH_COLSM1, mode->htotal - mode->hsync_start - 1));

	rp1dsi_dma_write(dsi, DPI_DMA_FRONT_PORCH,
			 BITS(DPI_DMA_FRONT_PORCH_ROWSM1, mode->vsync_start - mode->vdisplay - 1) |
			 BITS(DPI_DMA_FRONT_PORCH_COLSM1, mode->hsync_start - mode->hdisplay - 1));

	/* Input to output pixel format conversion */
	for (i = 0; i < ARRAY_SIZE(my_formats); ++i) {
		if (my_formats[i].format == in_format)
			break;
	}
	if (i >= ARRAY_SIZE(my_formats)) {
		drm_err(dsi->drm, "%s: bad input format\n", __func__);
		i = 0;
	}
	rp1dsi_dma_write(dsi, DPI_DMA_IMASK, my_formats[i].mask);
	rp1dsi_dma_write(dsi, DPI_DMA_OMASK, get_omask_oshift(out_format, &oshift));
	rp1dsi_dma_write(dsi, DPI_DMA_SHIFT, my_formats[i].shift | oshift);
	if (out_format == MIPI_DSI_FMT_RGB888)
		rp1dsi_dma_write(dsi, DPI_DMA_RGBSZ, my_formats[i].rgbsz);
	else
		rp1dsi_dma_write(dsi, DPI_DMA_RGBSZ, my_formats[i].rgbsz & DPI_DMA_RGBSZ_BPP_MASK);

	rp1dsi_dma_write(dsi, DPI_DMA_QOS,
			 BITS(DPI_DMA_QOS_DQOS, 0x0) |
			 BITS(DPI_DMA_QOS_ULEV, 0xb) |
			 BITS(DPI_DMA_QOS_UQOS, 0x2) |
			 BITS(DPI_DMA_QOS_LLEV, 0x8) |
			 BITS(DPI_DMA_QOS_LQOS, 0x7));

	rp1dsi_dma_write(dsi, DPI_DMA_IRQ_FLAGS, -1);
	rp1dsi_dma_vblank_ctrl(dsi, 1);

	i = rp1dsi_dma_busy(dsi);
	if (i)
		drm_err(dsi->drm, "RP1DSI: Unexpectedly busy at start!");

	rp1dsi_dma_write(dsi, DPI_DMA_CONTROL,
			 BITS(DPI_DMA_CONTROL_ARM, (i == 0)) |
			 BITS(DPI_DMA_CONTROL_AUTO_REPEAT, 1) |
			 BITS(DPI_DMA_CONTROL_HIGH_WATER, 448) |
			 BITS(DPI_DMA_CONTROL_DEN_POL, 0) |
			 BITS(DPI_DMA_CONTROL_HSYNC_POL, 0) |
			 BITS(DPI_DMA_CONTROL_VSYNC_POL, 0) |
			 BITS(DPI_DMA_CONTROL_COLORM, 0) |
			 BITS(DPI_DMA_CONTROL_SHUTDN, 0) |
			 BITS(DPI_DMA_CONTROL_HBP_EN, 1) |
			 BITS(DPI_DMA_CONTROL_HFP_EN, 1) |
			 BITS(DPI_DMA_CONTROL_VBP_EN, 1) |
			 BITS(DPI_DMA_CONTROL_VFP_EN, 1) |
			 BITS(DPI_DMA_CONTROL_HSYNC_EN, 1) |
			 BITS(DPI_DMA_CONTROL_VSYNC_EN, 1));
}

void rp1dsi_dma_update(struct rp1_dsi *dsi, dma_addr_t addr, u32 offset, u32 stride)
{
	/*
	 * Update STRIDE, DMAH and DMAL only. When called after rp1dsi_dma_setup(),
	 * DMA starts immediately; if already running, the buffer will flip at
	 * the next vertical sync event.
	 */
	u64 a = addr + offset;

	rp1dsi_dma_write(dsi, DPI_DMA_DMA_STRIDE, stride);
	rp1dsi_dma_write(dsi, DPI_DMA_DMA_ADDR_H, a >> 32);
	rp1dsi_dma_write(dsi, DPI_DMA_DMA_ADDR_L, a & 0xFFFFFFFFu);
}

void rp1dsi_dma_stop(struct rp1_dsi *dsi)
{
	/*
	 * Stop DMA by turning off the Auto-Repeat flag, and wait up to 100ms for
	 * the current and any queued frame to end. "Force drain" flags are not used,
	 * as they seem to prevent DMA from re-starting properly; it's safer to wait.
	 */
	u32 ctrl;

	reinit_completion(&dsi->finished);
	ctrl = rp1dsi_dma_read(dsi, DPI_DMA_CONTROL);
	ctrl &= ~(DPI_DMA_CONTROL_ARM_MASK | DPI_DMA_CONTROL_AUTO_REPEAT_MASK);
	rp1dsi_dma_write(dsi, DPI_DMA_CONTROL, ctrl);
	if (!wait_for_completion_timeout(&dsi->finished, HZ / 10))
		drm_err(dsi->drm, "%s: timed out waiting for idle\n", __func__);
	rp1dsi_dma_write(dsi, DPI_DMA_IRQ_EN, 0);
}

void rp1dsi_dma_vblank_ctrl(struct rp1_dsi *dsi, int enable)
{
	rp1dsi_dma_write(dsi, DPI_DMA_IRQ_EN,
			 BITS(DPI_DMA_IRQ_EN_AFIFO_EMPTY, 1)      |
			 BITS(DPI_DMA_IRQ_EN_UNDERFLOW, 1)        |
			 BITS(DPI_DMA_IRQ_EN_DMA_READY, !!enable) |
			 BITS(DPI_DMA_IRQ_EN_MATCH_LINE, 4095));
}

irqreturn_t rp1dsi_dma_isr(int irq, void *dev)
{
	struct rp1_dsi *dsi = dev;
	u32 u = rp1dsi_dma_read(dsi, DPI_DMA_IRQ_FLAGS);

	if (u) {
		rp1dsi_dma_write(dsi, DPI_DMA_IRQ_FLAGS, u);
		if (dsi) {
			if (u & DPI_DMA_IRQ_FLAGS_UNDERFLOW_MASK)
				drm_err_ratelimited(dsi->drm,
						    "Underflow! (panics=0x%08x)\n",
						    rp1dsi_dma_read(dsi, DPI_DMA_PANICS));
			if (u & DPI_DMA_IRQ_FLAGS_DMA_READY_MASK)
				drm_crtc_handle_vblank(&dsi->pipe.crtc);
			if (u & DPI_DMA_IRQ_FLAGS_AFIFO_EMPTY_MASK)
				complete(&dsi->finished);
		}
	}
	return u ? IRQ_HANDLED : IRQ_NONE;
}
