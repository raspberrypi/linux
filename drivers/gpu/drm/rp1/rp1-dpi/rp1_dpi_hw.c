// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM Driver for DPI output on Raspberry Pi RP1
 *
 * Copyright (c) 2023 Raspberry Pi Limited.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/media-bus-format.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "rp1_dpi.h"

// --- DPI DMA REGISTERS ---

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

static unsigned int rp1dpi_hw_read(struct rp1_dpi *dpi, unsigned int reg)
{
	void __iomem *addr = dpi->hw_base[RP1DPI_HW_BLOCK_DPI] + reg;

	return readl(addr);
}

static void rp1dpi_hw_write(struct rp1_dpi *dpi, unsigned int reg, unsigned int val)
{
	void __iomem *addr = dpi->hw_base[RP1DPI_HW_BLOCK_DPI] + reg;

	writel(val, addr);
}

int rp1dpi_hw_busy(struct rp1_dpi *dpi)
{
	return (rp1dpi_hw_read(dpi, DPI_DMA_STATUS) & 0xF8F) ? 1 : 0;
}

/* Table of supported input (in-memory/DMA) pixel formats. */
struct rp1dpi_ipixfmt {
	u32 format; /* DRM format code                           */
	u32 mask;   /* RGB masks (10 bits each, left justified)  */
	u32 shift;  /* RGB MSB positions in the memory word      */
	u32 rgbsz;  /* Shifts used for scaling; also (BPP/8-1)   */
};

#define IMASK_RGB(r, g, b)	(BITS(DPI_DMA_IMASK_R, r)  | \
				 BITS(DPI_DMA_IMASK_G, g)  | \
				 BITS(DPI_DMA_IMASK_B, b))
#define OMASK_RGB(r, g, b)	(BITS(DPI_DMA_OMASK_R, r)  | \
				 BITS(DPI_DMA_OMASK_G, g)  | \
				 BITS(DPI_DMA_OMASK_B, b))
#define ISHIFT_RGB(r, g, b)	(BITS(DPI_DMA_SHIFT_IR, r) | \
				 BITS(DPI_DMA_SHIFT_IG, g) | \
				 BITS(DPI_DMA_SHIFT_IB, b))
#define OSHIFT_RGB(r, g, b)	(BITS(DPI_DMA_SHIFT_OR, r) | \
				 BITS(DPI_DMA_SHIFT_OG, g) | \
				 BITS(DPI_DMA_SHIFT_OB, b))

static const struct rp1dpi_ipixfmt my_formats[] = {
	{
	  .format = DRM_FORMAT_XRGB8888,
	  .mask	  = IMASK_RGB(0x3fc, 0x3fc, 0x3fc),
	  .shift  = ISHIFT_RGB(23, 15, 7),
	  .rgbsz  = BITS(DPI_DMA_RGBSZ_BPP, 3),
	},
	{
	  .format = DRM_FORMAT_XBGR8888,
	  .mask	  = IMASK_RGB(0x3fc, 0x3fc, 0x3fc),
	  .shift  = ISHIFT_RGB(7, 15, 23),
	  .rgbsz  = BITS(DPI_DMA_RGBSZ_BPP, 3),
	},
	{
	  .format = DRM_FORMAT_RGB888,
	  .mask	  = IMASK_RGB(0x3fc, 0x3fc, 0x3fc),
	  .shift  = ISHIFT_RGB(23, 15, 7),
	  .rgbsz  = BITS(DPI_DMA_RGBSZ_BPP, 2),
	},
	{
	  .format = DRM_FORMAT_BGR888,
	  .mask	  = IMASK_RGB(0x3fc, 0x3fc, 0x3fc),
	  .shift  = ISHIFT_RGB(7, 15, 23),
	  .rgbsz  = BITS(DPI_DMA_RGBSZ_BPP, 2),
	},
	{
	  .format = DRM_FORMAT_RGB565,
	  .mask	  = IMASK_RGB(0x3e0, 0x3f0, 0x3e0),
	  .shift  = ISHIFT_RGB(15, 10, 4),
	  .rgbsz  = BITS(DPI_DMA_RGBSZ_R, 5) | BITS(DPI_DMA_RGBSZ_G, 6) |
		    BITS(DPI_DMA_RGBSZ_B, 5) | BITS(DPI_DMA_RGBSZ_BPP, 1),
	},
	{
	  .format = DRM_FORMAT_BGR565,
	  .mask	  = IMASK_RGB(0x3e0, 0x3f0, 0x3e0),
	  .shift  = ISHIFT_RGB(4, 10, 15),
	  .rgbsz  = BITS(DPI_DMA_RGBSZ_R, 5) | BITS(DPI_DMA_RGBSZ_G, 6) |
		    BITS(DPI_DMA_RGBSZ_B, 5) | BITS(DPI_DMA_RGBSZ_BPP, 1),
	}
};

static u32 set_output_format(u32 bus_format, u32 *shift, u32 *imask, u32 *rgbsz)
{
	switch (bus_format) {
	case MEDIA_BUS_FMT_RGB565_1X16:
		if (*shift == ISHIFT_RGB(15, 10, 4)) {
			/* When framebuffer is RGB565, we can output RGB565 */
			*shift = ISHIFT_RGB(15, 7, 0) | OSHIFT_RGB(19, 9, 0);
			*rgbsz &= DPI_DMA_RGBSZ_BPP_MASK;
			return OMASK_RGB(0x3fc, 0x3fc, 0);
		}

		/* due to a HW limitation, bit-depth is effectively RGB535 */
		*shift |= OSHIFT_RGB(19, 14, 6);
		*imask &= IMASK_RGB(0x3e0, 0x380, 0x3e0);
		*rgbsz = BITS(DPI_DMA_RGBSZ_G, 5) | (*rgbsz & DPI_DMA_RGBSZ_BPP_MASK);
		return OMASK_RGB(0x3e0, 0x39c, 0x3e0);

	case MEDIA_BUS_FMT_RGB666_1X18:
	case MEDIA_BUS_FMT_BGR666_1X18:
		/* due to a HW limitation, bit-depth is effectively RGB444 */
		*shift |= OSHIFT_RGB(23, 15, 7);
		*imask &= IMASK_RGB(0x3c0, 0x3c0, 0x3c0);
		*rgbsz = BITS(DPI_DMA_RGBSZ_R, 2) | (*rgbsz & DPI_DMA_RGBSZ_BPP_MASK);
		return OMASK_RGB(0x330, 0x3c0, 0x3c0);

	case MEDIA_BUS_FMT_RGB888_1X24:
	case MEDIA_BUS_FMT_BGR888_1X24:
	case MEDIA_BUS_FMT_RGB101010_1X30:
		/* The full 24 bits can be output. Note that RP1's internal wiring means
		 * that 8.8.8 to GPIO pads can share with 10.10.10 to the onboard VDAC.
		 */
		*shift |= OSHIFT_RGB(29, 19, 9);
		return OMASK_RGB(0x3fc, 0x3fc, 0x3fc);

	default:
		/* RGB666_1x24_CPADHI, BGR666_1X24_CPADHI and "RGB565_666" formats */
		*shift |= OSHIFT_RGB(27, 17, 7);
		*rgbsz &= DPI_DMA_RGBSZ_BPP_MASK;
		return OMASK_RGB(0x3f0, 0x3f0, 0x3f0);
	}
}

#define BUS_FMT_IS_BGR(fmt) (				       \
		((fmt) == MEDIA_BUS_FMT_BGR666_1X18)        || \
		((fmt) == MEDIA_BUS_FMT_BGR666_1X24_CPADHI) || \
		((fmt) == MEDIA_BUS_FMT_BGR888_1X24))

void rp1dpi_hw_setup(struct rp1_dpi *dpi,
		     u32 in_format, u32 bus_format, bool de_inv,
		    struct drm_display_mode const *mode)
{
	u32 shift, imask, omask, rgbsz;
	int i;

	pr_info("%s: in_fmt=\'%c%c%c%c\' bus_fmt=0x%x mode=%dx%d total=%dx%d %dkHz %cH%cV%cD%cC",
		__func__, in_format, in_format >> 8, in_format >> 16, in_format >> 24, bus_format,
		mode->hdisplay, mode->vdisplay,
		mode->htotal, mode->vtotal,
		mode->clock,
		(mode->flags & DRM_MODE_FLAG_NHSYNC) ? '-' : '+',
		(mode->flags & DRM_MODE_FLAG_NVSYNC) ? '-' : '+',
		de_inv ? '-' : '+',
		dpi->clk_inv ? '-' : '+');

	/*
	 * Configure all DPI/DMA block registers, except base address.
	 * DMA will not actually start until a FB base address is specified
	 * using rp1dpi_hw_update().
	 */
	rp1dpi_hw_write(dpi, DPI_DMA_VISIBLE_AREA,
			BITS(DPI_DMA_VISIBLE_AREA_ROWSM1, mode->vdisplay - 1) |
			BITS(DPI_DMA_VISIBLE_AREA_COLSM1, mode->hdisplay - 1));

	rp1dpi_hw_write(dpi, DPI_DMA_SYNC_WIDTH,
			BITS(DPI_DMA_SYNC_WIDTH_ROWSM1, mode->vsync_end - mode->vsync_start - 1) |
			BITS(DPI_DMA_SYNC_WIDTH_COLSM1, mode->hsync_end - mode->hsync_start - 1));

	/* In these registers, "back porch" time includes sync width */
	rp1dpi_hw_write(dpi, DPI_DMA_BACK_PORCH,
			BITS(DPI_DMA_BACK_PORCH_ROWSM1, mode->vtotal - mode->vsync_start - 1) |
			BITS(DPI_DMA_BACK_PORCH_COLSM1, mode->htotal - mode->hsync_start - 1));

	rp1dpi_hw_write(dpi, DPI_DMA_FRONT_PORCH,
			BITS(DPI_DMA_FRONT_PORCH_ROWSM1, mode->vsync_start - mode->vdisplay - 1) |
			BITS(DPI_DMA_FRONT_PORCH_COLSM1, mode->hsync_start - mode->hdisplay - 1));

	/* Input to output pixel format conversion */
	for (i = 0; i < ARRAY_SIZE(my_formats); ++i) {
		if (my_formats[i].format == in_format)
			break;
	}
	if (i >= ARRAY_SIZE(my_formats)) {
		pr_err("%s: bad input format\n", __func__);
		i = 4;
	}
	if (BUS_FMT_IS_BGR(bus_format))
		i ^= 1;
	shift = my_formats[i].shift;
	imask = my_formats[i].mask;
	rgbsz = my_formats[i].rgbsz;
	omask = set_output_format(bus_format, &shift, &imask, &rgbsz);

	rp1dpi_hw_write(dpi, DPI_DMA_IMASK, imask);
	rp1dpi_hw_write(dpi, DPI_DMA_OMASK, omask);
	rp1dpi_hw_write(dpi, DPI_DMA_SHIFT, shift);
	rp1dpi_hw_write(dpi, DPI_DMA_RGBSZ, rgbsz);

	rp1dpi_hw_write(dpi, DPI_DMA_QOS,
			BITS(DPI_DMA_QOS_DQOS, 0x0) |
			BITS(DPI_DMA_QOS_ULEV, 0xb) |
			BITS(DPI_DMA_QOS_UQOS, 0x2) |
			BITS(DPI_DMA_QOS_LLEV, 0x8) |
			BITS(DPI_DMA_QOS_LQOS, 0x7));

	rp1dpi_hw_write(dpi, DPI_DMA_IRQ_FLAGS, -1);
	rp1dpi_hw_vblank_ctrl(dpi, 1);

	i = rp1dpi_hw_busy(dpi);
	if (i)
		pr_warn("%s: Unexpectedly busy at start!", __func__);

	rp1dpi_hw_write(dpi, DPI_DMA_CONTROL,
			BITS(DPI_DMA_CONTROL_ARM,          !i) |
			BITS(DPI_DMA_CONTROL_AUTO_REPEAT,   1) |
			BITS(DPI_DMA_CONTROL_HIGH_WATER,  448) |
			BITS(DPI_DMA_CONTROL_DEN_POL,  de_inv) |
			BITS(DPI_DMA_CONTROL_HSYNC_POL, !!(mode->flags & DRM_MODE_FLAG_NHSYNC)) |
			BITS(DPI_DMA_CONTROL_VSYNC_POL, !!(mode->flags & DRM_MODE_FLAG_NVSYNC)) |
			BITS(DPI_DMA_CONTROL_COLORM,	   0) |
			BITS(DPI_DMA_CONTROL_SHUTDN,	   0) |
			BITS(DPI_DMA_CONTROL_HBP_EN,    (mode->htotal != mode->hsync_end))      |
			BITS(DPI_DMA_CONTROL_HFP_EN,    (mode->hsync_start != mode->hdisplay))  |
			BITS(DPI_DMA_CONTROL_VBP_EN,    (mode->vtotal != mode->vsync_end))      |
			BITS(DPI_DMA_CONTROL_VFP_EN,    (mode->vsync_start != mode->vdisplay))  |
			BITS(DPI_DMA_CONTROL_HSYNC_EN,  (mode->hsync_end != mode->hsync_start)) |
			BITS(DPI_DMA_CONTROL_VSYNC_EN,  (mode->vsync_end != mode->vsync_start)));
}

void rp1dpi_hw_update(struct rp1_dpi *dpi, dma_addr_t addr, u32 offset, u32 stride)
{
	u64 a = addr + offset;

	/*
	 * Update STRIDE, DMAH and DMAL only. When called after rp1dpi_hw_setup(),
	 * DMA starts immediately; if already running, the buffer will flip at
	 * the next vertical sync event.
	 */
	rp1dpi_hw_write(dpi, DPI_DMA_DMA_STRIDE, stride);
	rp1dpi_hw_write(dpi, DPI_DMA_DMA_ADDR_H, a >> 32);
	rp1dpi_hw_write(dpi, DPI_DMA_DMA_ADDR_L, a & 0xFFFFFFFFu);
}

void rp1dpi_hw_stop(struct rp1_dpi *dpi)
{
	u32 ctrl;

	/*
	 * Stop DMA by turning off the Auto-Repeat flag, and wait up to 100ms for
	 * the current and any queued frame to end. "Force drain" flags are not used,
	 * as they seem to prevent DMA from re-starting properly; it's safer to wait.
	 */
	reinit_completion(&dpi->finished);
	ctrl = rp1dpi_hw_read(dpi, DPI_DMA_CONTROL);
	ctrl &= ~(DPI_DMA_CONTROL_ARM_MASK | DPI_DMA_CONTROL_AUTO_REPEAT_MASK);
	rp1dpi_hw_write(dpi, DPI_DMA_CONTROL, ctrl);
	if (!wait_for_completion_timeout(&dpi->finished, HZ / 10))
		drm_err(&dpi->drm, "%s: timed out waiting for idle\n", __func__);
	rp1dpi_hw_write(dpi, DPI_DMA_IRQ_EN, 0);
}

void rp1dpi_hw_vblank_ctrl(struct rp1_dpi *dpi, int enable)
{
	rp1dpi_hw_write(dpi, DPI_DMA_IRQ_EN,
			BITS(DPI_DMA_IRQ_EN_AFIFO_EMPTY, 1)      |
			BITS(DPI_DMA_IRQ_EN_UNDERFLOW, 1)        |
			BITS(DPI_DMA_IRQ_EN_DMA_READY, !!enable) |
			BITS(DPI_DMA_IRQ_EN_MATCH_LINE, 4095));
}

irqreturn_t rp1dpi_hw_isr(int irq, void *dev)
{
	struct rp1_dpi *dpi = dev;
	u32 u = rp1dpi_hw_read(dpi, DPI_DMA_IRQ_FLAGS);

	if (u) {
		rp1dpi_hw_write(dpi, DPI_DMA_IRQ_FLAGS, u);
		if (dpi) {
			if (u & DPI_DMA_IRQ_FLAGS_UNDERFLOW_MASK)
				drm_err_ratelimited(&dpi->drm,
						    "Underflow! (panics=0x%08x)\n",
						    rp1dpi_hw_read(dpi, DPI_DMA_PANICS));
			if (u & DPI_DMA_IRQ_FLAGS_DMA_READY_MASK)
				drm_crtc_handle_vblank(&dpi->pipe.crtc);
			if (u & DPI_DMA_IRQ_FLAGS_AFIFO_EMPTY_MASK)
				complete(&dpi->finished);
		}
	}
	return u ? IRQ_HANDLED : IRQ_NONE;
}
