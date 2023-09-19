// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM Driver for DPI output on Raspberry Pi RP1
 * Functions to set up VIDEO_OUT_CFG registers
 * Copyright (c) 2023-2025 Raspberry Pi Limited.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/rp1_platform.h>

#include "rp1_dpi.h"

// =============================================================================
// Register    : VIDEO_OUT_CFG_SEL
// Description : Selects source: 0 => DPI, 1 =>VEC; optionally invert clock
#define VIDEO_OUT_CFG_SEL                    0x0000
#define VIDEO_OUT_CFG_SEL_BITS               0x00000013
#define VIDEO_OUT_CFG_SEL_RESET              0x00000000
#define VIDEO_OUT_CFG_SEL_PCLK_INV           BIT(4)
#define VIDEO_OUT_CFG_SEL_PAD_MUX            BIT(1)
#define VIDEO_OUT_CFG_SEL_VDAC_MUX           BIT(0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_VDAC_CFG
// Description : Configure SNPS VDAC
#define VIDEO_OUT_CFG_VDAC_CFG               0x0004
#define VIDEO_OUT_CFG_VDAC_CFG_BITS          0x1fffffff
#define VIDEO_OUT_CFG_VDAC_CFG_RESET         0x0003ffff
#define VIDEO_OUT_CFG_VDAC_CFG_ENCTR         GENMASK(28, 26)
#define VIDEO_OUT_CFG_VDAC_CFG_ENSC          GENMASK(25, 23)
#define VIDEO_OUT_CFG_VDAC_CFG_ENDAC         GENMASK(22, 20)
#define VIDEO_OUT_CFG_VDAC_CFG_ENVBG         BIT(19)
#define VIDEO_OUT_CFG_VDAC_CFG_ENEXTREF      BIT(18)
#define VIDEO_OUT_CFG_VDAC_CFG_DAC2GC        GENMASK(17, 12)
#define VIDEO_OUT_CFG_VDAC_CFG_DAC1GC        GENMASK(11, 6)
#define VIDEO_OUT_CFG_VDAC_CFG_DAC0GC        GENMASK(5, 0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_VDAC_STATUS
// Description : Read VDAC status
#define VIDEO_OUT_CFG_VDAC_STATUS            0x0008
#define VIDEO_OUT_CFG_VDAC_STATUS_BITS       0x00000017
#define VIDEO_OUT_CFG_VDAC_STATUS_RESET      0x00000000
#define VIDEO_OUT_CFG_VDAC_STATUS_ENCTR3     BIT(4)
#define VIDEO_OUT_CFG_VDAC_STATUS_CABLEOUT   GENMASK(2, 0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_MEM_PD
// Description : Control memory power down
#define VIDEO_OUT_CFG_MEM_PD                 0x000c
#define VIDEO_OUT_CFG_MEM_PD_BITS            0x00000003
#define VIDEO_OUT_CFG_MEM_PD_RESET           0x00000000
#define VIDEO_OUT_CFG_MEM_PD_VEC             BIT(1)
#define VIDEO_OUT_CFG_MEM_PD_DPI             BIT(0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_TEST_OVERRIDE
// Description : Allow forcing of output values
#define VIDEO_OUT_CFG_TEST_OVERRIDE          0x0010
#define VIDEO_OUT_CFG_TEST_OVERRIDE_BITS     0xffffffff
#define VIDEO_OUT_CFG_TEST_OVERRIDE_RESET    0x00000000
#define VIDEO_OUT_CFG_TEST_OVERRIDE_PAD      BIT(31)
#define VIDEO_OUT_CFG_TEST_OVERRIDE_VDAC     BIT(30)
#define VIDEO_OUT_CFG_TEST_OVERRIDE_RGBVAL   GENMASK(29, 0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_INTR
// Description : Raw Interrupts
#define VIDEO_OUT_CFG_INTR                   0x0014
#define VIDEO_OUT_CFG_INTR_BITS              0x00000003
#define VIDEO_OUT_CFG_INTR_RESET             0x00000000
#define VIDEO_OUT_CFG_INTR_DPI               BIT(1)
#define VIDEO_OUT_CFG_INTR_VEC               BIT(0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_INTE
// Description : Interrupt Enable
#define VIDEO_OUT_CFG_INTE                   0x0018
#define VIDEO_OUT_CFG_INTE_BITS              0x00000003
#define VIDEO_OUT_CFG_INTE_RESET             0x00000000
#define VIDEO_OUT_CFG_INTE_DPI               BIT(1)
#define VIDEO_OUT_CFG_INTE_VEC               BIT(0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_INTF
// Description : Interrupt Force
#define VIDEO_OUT_CFG_INTF                   0x001c
#define VIDEO_OUT_CFG_INTF_BITS              0x00000003
#define VIDEO_OUT_CFG_INTF_RESET             0x00000000
#define VIDEO_OUT_CFG_INTF_DPI               BIT(1)
#define VIDEO_OUT_CFG_INTF_VEC               BIT(0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_INTS
// Description : Interrupt status after masking & forcing
#define VIDEO_OUT_CFG_INTS                   0x0020
#define VIDEO_OUT_CFG_INTS_BITS              0x00000003
#define VIDEO_OUT_CFG_INTS_RESET             0x00000000
#define VIDEO_OUT_CFG_INTS_DPI               BIT(1)
#define VIDEO_OUT_CFG_INTS_VEC               BIT(0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_BLOCK_ID
// Description : Block Identifier
//               Hexadecimal representation of "VOCF"
#define VIDEO_OUT_CFG_BLOCK_ID               0x0024
#define VIDEO_OUT_CFG_BLOCK_ID_BITS          0xffffffff
#define VIDEO_OUT_CFG_BLOCK_ID_RESET         0x564f4346
// =============================================================================
// Register    : VIDEO_OUT_CFG_INSTANCE_ID
// Description : Block Instance Identifier
#define VIDEO_OUT_CFG_INSTANCE_ID            0x0028
#define VIDEO_OUT_CFG_INSTANCE_ID_BITS       0x0000000f
#define VIDEO_OUT_CFG_INSTANCE_ID_RESET      0x00000000
// =============================================================================
// Register    : VIDEO_OUT_CFG_RSTSEQ_AUTO
// Description : None
#define VIDEO_OUT_CFG_RSTSEQ_AUTO            0x002c
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_BITS       0x00000007
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_RESET      0x00000007
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_VEC        BIT(2)
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_DPI        BIT(1)
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_BUSADAPTER BIT(0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_RSTSEQ_PARALLEL
// Description : None
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL        0x0030
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_BITS   0x00000007
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_RESET  0x00000006
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_VEC    BIT(2)
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_DPI    BIT(1)
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_BUSADAPTER BIT(0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_RSTSEQ_CTRL
// Description : None
#define VIDEO_OUT_CFG_RSTSEQ_CTRL            0x0034
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_BITS       0x00000007
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_RESET      0x00000000
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_VEC        BIT(2)
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_DPI        BIT(1)
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_BUSADAPTER BIT(0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_RSTSEQ_TRIG
// Description : None
#define VIDEO_OUT_CFG_RSTSEQ_TRIG            0x0038
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_BITS       0x00000007
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_RESET      0x00000000
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_VEC        BIT(2)
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_DPI        BIT(1)
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_BUSADAPTER BIT(0)
// =============================================================================
// Register    : VIDEO_OUT_CFG_RSTSEQ_DONE
// Description : None
#define VIDEO_OUT_CFG_RSTSEQ_DONE            0x003c
#define VIDEO_OUT_CFG_RSTSEQ_DONE_BITS       0x00000007
#define VIDEO_OUT_CFG_RSTSEQ_DONE_RESET      0x00000000
#define VIDEO_OUT_CFG_RSTSEQ_DONE_VEC        BIT(2)
#define VIDEO_OUT_CFG_RSTSEQ_DONE_DPI        BIT(1)
#define VIDEO_OUT_CFG_RSTSEQ_DONE_BUSADAPTER BIT(0)
// =============================================================================

#define CFG_WRITE(reg, val)  writel((val),  dpi->hw_base[RP1DPI_HW_BLOCK_CFG] + (reg))
#define CFG_READ(reg)        readl(dpi->hw_base[RP1DPI_HW_BLOCK_CFG] + (reg))

void rp1dpi_vidout_setup(struct rp1_dpi *dpi, bool drive_negedge)
{
	/*
	 * We assume DPI and VEC can't be used at the same time (due to
	 * clashing requirements for PLL_VIDEO, and potentially for VDAC).
	 * We therefore leave VEC memories powered down.
	 */
	CFG_WRITE(VIDEO_OUT_CFG_MEM_PD, VIDEO_OUT_CFG_MEM_PD_VEC);
	CFG_WRITE(VIDEO_OUT_CFG_TEST_OVERRIDE,
		  VIDEO_OUT_CFG_TEST_OVERRIDE_VDAC);

	/* DPI->Pads; DPI->VDAC; optionally flip PCLK polarity */
	CFG_WRITE(VIDEO_OUT_CFG_SEL,
		  drive_negedge ? VIDEO_OUT_CFG_SEL_PCLK_INV : 0);

	/* disable VDAC */
	CFG_WRITE(VIDEO_OUT_CFG_VDAC_CFG, 0);

	/* enable DPI interrupt */
	CFG_WRITE(VIDEO_OUT_CFG_INTE, VIDEO_OUT_CFG_INTE_DPI);
}

void rp1dpi_vidout_poweroff(struct rp1_dpi *dpi)
{
	/* disable DPI interrupt */
	CFG_WRITE(VIDEO_OUT_CFG_INTE, 0);

	/* Ensure VDAC is turned off; power down DPI,VEC memories */
	CFG_WRITE(VIDEO_OUT_CFG_VDAC_CFG, 0);
	CFG_WRITE(VIDEO_OUT_CFG_MEM_PD, VIDEO_OUT_CFG_MEM_PD_BITS);
}
