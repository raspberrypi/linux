// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM Driver for DSI output on Raspberry Pi RP1
 *
 * Copyright (c) 2023 Raspberry Pi Limited.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/rp1_platform.h>

#include "rp1_vec.h"

// =============================================================================
// Register    : VIDEO_OUT_CFG_SEL
// JTAG access : synchronous
// Description : Selects source: VEC or DPI
#define VIDEO_OUT_CFG_SEL_OFFSET 0x00000000
#define VIDEO_OUT_CFG_SEL_BITS	 0x00000013
#define VIDEO_OUT_CFG_SEL_RESET	 0x00000000
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_SEL_PCLK_INV
// Description : Select dpi_pclk output port polarity inversion.
#define VIDEO_OUT_CFG_SEL_PCLK_INV_RESET  0x0
#define VIDEO_OUT_CFG_SEL_PCLK_INV_BITS	  0x00000010
#define VIDEO_OUT_CFG_SEL_PCLK_INV_MSB	  4
#define VIDEO_OUT_CFG_SEL_PCLK_INV_LSB	  4
#define VIDEO_OUT_CFG_SEL_PCLK_INV_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_SEL_PAD_MUX
// Description : VEC 1 DPI 0
#define VIDEO_OUT_CFG_SEL_PAD_MUX_RESET	 0x0
#define VIDEO_OUT_CFG_SEL_PAD_MUX_BITS	 0x00000002
#define VIDEO_OUT_CFG_SEL_PAD_MUX_MSB	 1
#define VIDEO_OUT_CFG_SEL_PAD_MUX_LSB	 1
#define VIDEO_OUT_CFG_SEL_PAD_MUX_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_SEL_VDAC_MUX
// Description : VEC 1 DPI 0
#define VIDEO_OUT_CFG_SEL_VDAC_MUX_RESET  0x0
#define VIDEO_OUT_CFG_SEL_VDAC_MUX_BITS	  0x00000001
#define VIDEO_OUT_CFG_SEL_VDAC_MUX_MSB	  0
#define VIDEO_OUT_CFG_SEL_VDAC_MUX_LSB	  0
#define VIDEO_OUT_CFG_SEL_VDAC_MUX_ACCESS "RW"
// =============================================================================
// Register    : VIDEO_OUT_CFG_VDAC_CFG
// JTAG access : synchronous
// Description : Configure SNPS VDAC
#define VIDEO_OUT_CFG_VDAC_CFG_OFFSET 0x00000004
#define VIDEO_OUT_CFG_VDAC_CFG_BITS   0x1fffffff
#define VIDEO_OUT_CFG_VDAC_CFG_RESET  0x0003ffff
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_VDAC_CFG_ENCTR
// Description : None
#define VIDEO_OUT_CFG_VDAC_CFG_ENCTR_RESET  0x0
#define VIDEO_OUT_CFG_VDAC_CFG_ENCTR_BITS   0x1c000000
#define VIDEO_OUT_CFG_VDAC_CFG_ENCTR_MSB    28
#define VIDEO_OUT_CFG_VDAC_CFG_ENCTR_LSB    26
#define VIDEO_OUT_CFG_VDAC_CFG_ENCTR_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_VDAC_CFG_ENSC
// Description : None
#define VIDEO_OUT_CFG_VDAC_CFG_ENSC_RESET  0x0
#define VIDEO_OUT_CFG_VDAC_CFG_ENSC_BITS   0x03800000
#define VIDEO_OUT_CFG_VDAC_CFG_ENSC_MSB	   25
#define VIDEO_OUT_CFG_VDAC_CFG_ENSC_LSB	   23
#define VIDEO_OUT_CFG_VDAC_CFG_ENSC_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_VDAC_CFG_ENDAC
// Description : None
#define VIDEO_OUT_CFG_VDAC_CFG_ENDAC_RESET  0x0
#define VIDEO_OUT_CFG_VDAC_CFG_ENDAC_BITS   0x00700000
#define VIDEO_OUT_CFG_VDAC_CFG_ENDAC_MSB    22
#define VIDEO_OUT_CFG_VDAC_CFG_ENDAC_LSB    20
#define VIDEO_OUT_CFG_VDAC_CFG_ENDAC_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_VDAC_CFG_ENVBG
// Description : None
#define VIDEO_OUT_CFG_VDAC_CFG_ENVBG_RESET  0x0
#define VIDEO_OUT_CFG_VDAC_CFG_ENVBG_BITS   0x00080000
#define VIDEO_OUT_CFG_VDAC_CFG_ENVBG_MSB    19
#define VIDEO_OUT_CFG_VDAC_CFG_ENVBG_LSB    19
#define VIDEO_OUT_CFG_VDAC_CFG_ENVBG_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_VDAC_CFG_ENEXTREF
// Description : None
#define VIDEO_OUT_CFG_VDAC_CFG_ENEXTREF_RESET  0x0
#define VIDEO_OUT_CFG_VDAC_CFG_ENEXTREF_BITS   0x00040000
#define VIDEO_OUT_CFG_VDAC_CFG_ENEXTREF_MSB    18
#define VIDEO_OUT_CFG_VDAC_CFG_ENEXTREF_LSB    18
#define VIDEO_OUT_CFG_VDAC_CFG_ENEXTREF_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_VDAC_CFG_DAC2GC
// Description : dac2 gain control
#define VIDEO_OUT_CFG_VDAC_CFG_DAC2GC_RESET  0x3f
#define VIDEO_OUT_CFG_VDAC_CFG_DAC2GC_BITS   0x0003f000
#define VIDEO_OUT_CFG_VDAC_CFG_DAC2GC_MSB    17
#define VIDEO_OUT_CFG_VDAC_CFG_DAC2GC_LSB    12
#define VIDEO_OUT_CFG_VDAC_CFG_DAC2GC_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_VDAC_CFG_DAC1GC
// Description : dac1 gain control
#define VIDEO_OUT_CFG_VDAC_CFG_DAC1GC_RESET  0x3f
#define VIDEO_OUT_CFG_VDAC_CFG_DAC1GC_BITS   0x00000fc0
#define VIDEO_OUT_CFG_VDAC_CFG_DAC1GC_MSB    11
#define VIDEO_OUT_CFG_VDAC_CFG_DAC1GC_LSB    6
#define VIDEO_OUT_CFG_VDAC_CFG_DAC1GC_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_VDAC_CFG_DAC0GC
// Description : dac0 gain control
#define VIDEO_OUT_CFG_VDAC_CFG_DAC0GC_RESET  0x3f
#define VIDEO_OUT_CFG_VDAC_CFG_DAC0GC_BITS   0x0000003f
#define VIDEO_OUT_CFG_VDAC_CFG_DAC0GC_MSB    5
#define VIDEO_OUT_CFG_VDAC_CFG_DAC0GC_LSB    0
#define VIDEO_OUT_CFG_VDAC_CFG_DAC0GC_ACCESS "RW"
// =============================================================================
// Register    : VIDEO_OUT_CFG_VDAC_STATUS
// JTAG access : synchronous
// Description : Read VDAC status
#define VIDEO_OUT_CFG_VDAC_STATUS_OFFSET 0x00000008
#define VIDEO_OUT_CFG_VDAC_STATUS_BITS	 0x00000017
#define VIDEO_OUT_CFG_VDAC_STATUS_RESET	 0x00000000
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_VDAC_STATUS_ENCTR3
// Description : None
#define VIDEO_OUT_CFG_VDAC_STATUS_ENCTR3_RESET	0x0
#define VIDEO_OUT_CFG_VDAC_STATUS_ENCTR3_BITS	0x00000010
#define VIDEO_OUT_CFG_VDAC_STATUS_ENCTR3_MSB	4
#define VIDEO_OUT_CFG_VDAC_STATUS_ENCTR3_LSB	4
#define VIDEO_OUT_CFG_VDAC_STATUS_ENCTR3_ACCESS "RO"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_VDAC_STATUS_CABLEOUT
// Description : None
#define VIDEO_OUT_CFG_VDAC_STATUS_CABLEOUT_RESET  "-"
#define VIDEO_OUT_CFG_VDAC_STATUS_CABLEOUT_BITS	  0x00000007
#define VIDEO_OUT_CFG_VDAC_STATUS_CABLEOUT_MSB	  2
#define VIDEO_OUT_CFG_VDAC_STATUS_CABLEOUT_LSB	  0
#define VIDEO_OUT_CFG_VDAC_STATUS_CABLEOUT_ACCESS "RO"
// =============================================================================
// Register    : VIDEO_OUT_CFG_MEM_PD
// JTAG access : synchronous
// Description : Control memory power down
#define VIDEO_OUT_CFG_MEM_PD_OFFSET 0x0000000c
#define VIDEO_OUT_CFG_MEM_PD_BITS   0x00000003
#define VIDEO_OUT_CFG_MEM_PD_RESET  0x00000000
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_MEM_PD_VEC
// Description : None
#define VIDEO_OUT_CFG_MEM_PD_VEC_RESET	0x0
#define VIDEO_OUT_CFG_MEM_PD_VEC_BITS	0x00000002
#define VIDEO_OUT_CFG_MEM_PD_VEC_MSB	1
#define VIDEO_OUT_CFG_MEM_PD_VEC_LSB	1
#define VIDEO_OUT_CFG_MEM_PD_VEC_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_MEM_PD_DPI
// Description : None
#define VIDEO_OUT_CFG_MEM_PD_DPI_RESET	0x0
#define VIDEO_OUT_CFG_MEM_PD_DPI_BITS	0x00000001
#define VIDEO_OUT_CFG_MEM_PD_DPI_MSB	0
#define VIDEO_OUT_CFG_MEM_PD_DPI_LSB	0
#define VIDEO_OUT_CFG_MEM_PD_DPI_ACCESS "RW"
// =============================================================================
// Register    : VIDEO_OUT_CFG_TEST_OVERRIDE
// JTAG access : synchronous
// Description : None
#define VIDEO_OUT_CFG_TEST_OVERRIDE_OFFSET 0x00000010
#define VIDEO_OUT_CFG_TEST_OVERRIDE_BITS   0xffffffff
#define VIDEO_OUT_CFG_TEST_OVERRIDE_RESET  0x00000000
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_TEST_OVERRIDE_PAD
// Description : None
#define VIDEO_OUT_CFG_TEST_OVERRIDE_PAD_RESET  0x0
#define VIDEO_OUT_CFG_TEST_OVERRIDE_PAD_BITS   0x80000000
#define VIDEO_OUT_CFG_TEST_OVERRIDE_PAD_MSB    31
#define VIDEO_OUT_CFG_TEST_OVERRIDE_PAD_LSB    31
#define VIDEO_OUT_CFG_TEST_OVERRIDE_PAD_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_TEST_OVERRIDE_VDAC
// Description : None
#define VIDEO_OUT_CFG_TEST_OVERRIDE_VDAC_RESET	0x0
#define VIDEO_OUT_CFG_TEST_OVERRIDE_VDAC_BITS	0x40000000
#define VIDEO_OUT_CFG_TEST_OVERRIDE_VDAC_MSB	30
#define VIDEO_OUT_CFG_TEST_OVERRIDE_VDAC_LSB	30
#define VIDEO_OUT_CFG_TEST_OVERRIDE_VDAC_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_TEST_OVERRIDE_RGBVAL
// Description : None
#define VIDEO_OUT_CFG_TEST_OVERRIDE_RGBVAL_RESET  0x00000000
#define VIDEO_OUT_CFG_TEST_OVERRIDE_RGBVAL_BITS	  0x3fffffff
#define VIDEO_OUT_CFG_TEST_OVERRIDE_RGBVAL_MSB	  29
#define VIDEO_OUT_CFG_TEST_OVERRIDE_RGBVAL_LSB	  0
#define VIDEO_OUT_CFG_TEST_OVERRIDE_RGBVAL_ACCESS "RW"
// =============================================================================
// Register    : VIDEO_OUT_CFG_INTR
// JTAG access : synchronous
// Description : Raw Interrupts
#define VIDEO_OUT_CFG_INTR_OFFSET 0x00000014
#define VIDEO_OUT_CFG_INTR_BITS	  0x00000003
#define VIDEO_OUT_CFG_INTR_RESET  0x00000000
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_INTR_DPI
// Description : None
#define VIDEO_OUT_CFG_INTR_DPI_RESET  0x0
#define VIDEO_OUT_CFG_INTR_DPI_BITS   0x00000002
#define VIDEO_OUT_CFG_INTR_DPI_MSB    1
#define VIDEO_OUT_CFG_INTR_DPI_LSB    1
#define VIDEO_OUT_CFG_INTR_DPI_ACCESS "RO"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_INTR_VEC
// Description : None
#define VIDEO_OUT_CFG_INTR_VEC_RESET  0x0
#define VIDEO_OUT_CFG_INTR_VEC_BITS   0x00000001
#define VIDEO_OUT_CFG_INTR_VEC_MSB    0
#define VIDEO_OUT_CFG_INTR_VEC_LSB    0
#define VIDEO_OUT_CFG_INTR_VEC_ACCESS "RO"
// =============================================================================
// Register    : VIDEO_OUT_CFG_INTE
// JTAG access : synchronous
// Description : Interrupt Enable
#define VIDEO_OUT_CFG_INTE_OFFSET 0x00000018
#define VIDEO_OUT_CFG_INTE_BITS	  0x00000003
#define VIDEO_OUT_CFG_INTE_RESET  0x00000000
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_INTE_DPI
// Description : None
#define VIDEO_OUT_CFG_INTE_DPI_RESET  0x0
#define VIDEO_OUT_CFG_INTE_DPI_BITS   0x00000002
#define VIDEO_OUT_CFG_INTE_DPI_MSB    1
#define VIDEO_OUT_CFG_INTE_DPI_LSB    1
#define VIDEO_OUT_CFG_INTE_DPI_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_INTE_VEC
// Description : None
#define VIDEO_OUT_CFG_INTE_VEC_RESET  0x0
#define VIDEO_OUT_CFG_INTE_VEC_BITS   0x00000001
#define VIDEO_OUT_CFG_INTE_VEC_MSB    0
#define VIDEO_OUT_CFG_INTE_VEC_LSB    0
#define VIDEO_OUT_CFG_INTE_VEC_ACCESS "RW"
// =============================================================================
// Register    : VIDEO_OUT_CFG_INTF
// JTAG access : synchronous
// Description : Interrupt Force
#define VIDEO_OUT_CFG_INTF_OFFSET 0x0000001c
#define VIDEO_OUT_CFG_INTF_BITS	  0x00000003
#define VIDEO_OUT_CFG_INTF_RESET  0x00000000
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_INTF_DPI
// Description : None
#define VIDEO_OUT_CFG_INTF_DPI_RESET  0x0
#define VIDEO_OUT_CFG_INTF_DPI_BITS   0x00000002
#define VIDEO_OUT_CFG_INTF_DPI_MSB    1
#define VIDEO_OUT_CFG_INTF_DPI_LSB    1
#define VIDEO_OUT_CFG_INTF_DPI_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_INTF_VEC
// Description : None
#define VIDEO_OUT_CFG_INTF_VEC_RESET  0x0
#define VIDEO_OUT_CFG_INTF_VEC_BITS   0x00000001
#define VIDEO_OUT_CFG_INTF_VEC_MSB    0
#define VIDEO_OUT_CFG_INTF_VEC_LSB    0
#define VIDEO_OUT_CFG_INTF_VEC_ACCESS "RW"
// =============================================================================
// Register    : VIDEO_OUT_CFG_INTS
// JTAG access : synchronous
// Description : Interrupt status after masking & forcing
#define VIDEO_OUT_CFG_INTS_OFFSET 0x00000020
#define VIDEO_OUT_CFG_INTS_BITS	  0x00000003
#define VIDEO_OUT_CFG_INTS_RESET  0x00000000
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_INTS_DPI
// Description : None
#define VIDEO_OUT_CFG_INTS_DPI_RESET  0x0
#define VIDEO_OUT_CFG_INTS_DPI_BITS   0x00000002
#define VIDEO_OUT_CFG_INTS_DPI_MSB    1
#define VIDEO_OUT_CFG_INTS_DPI_LSB    1
#define VIDEO_OUT_CFG_INTS_DPI_ACCESS "RO"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_INTS_VEC
// Description : None
#define VIDEO_OUT_CFG_INTS_VEC_RESET  0x0
#define VIDEO_OUT_CFG_INTS_VEC_BITS   0x00000001
#define VIDEO_OUT_CFG_INTS_VEC_MSB    0
#define VIDEO_OUT_CFG_INTS_VEC_LSB    0
#define VIDEO_OUT_CFG_INTS_VEC_ACCESS "RO"
// =============================================================================
// Register    : VIDEO_OUT_CFG_BLOCK_ID
// JTAG access : synchronous
// Description : Block Identifier
//		 Hexadecimal representation of "VOCF"
#define VIDEO_OUT_CFG_BLOCK_ID_OFFSET 0x00000024
#define VIDEO_OUT_CFG_BLOCK_ID_BITS   0xffffffff
#define VIDEO_OUT_CFG_BLOCK_ID_RESET  0x564f4346
#define VIDEO_OUT_CFG_BLOCK_ID_MSB    31
#define VIDEO_OUT_CFG_BLOCK_ID_LSB    0
#define VIDEO_OUT_CFG_BLOCK_ID_ACCESS "RO"
// =============================================================================
// Register    : VIDEO_OUT_CFG_INSTANCE_ID
// JTAG access : synchronous
// Description : Block Instance Identifier
#define VIDEO_OUT_CFG_INSTANCE_ID_OFFSET 0x00000028
#define VIDEO_OUT_CFG_INSTANCE_ID_BITS	 0x0000000f
#define VIDEO_OUT_CFG_INSTANCE_ID_RESET	 0x00000000
#define VIDEO_OUT_CFG_INSTANCE_ID_MSB	 3
#define VIDEO_OUT_CFG_INSTANCE_ID_LSB	 0
#define VIDEO_OUT_CFG_INSTANCE_ID_ACCESS "RO"
// =============================================================================
// Register    : VIDEO_OUT_CFG_RSTSEQ_AUTO
// JTAG access : synchronous
// Description : None
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_OFFSET 0x0000002c
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_BITS	 0x00000007
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_RESET	 0x00000007
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_AUTO_VEC
// Description : 1 = reset is controlled by the sequencer
//		 0 = reset is controlled by rstseq_ctrl
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_VEC_RESET  0x1
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_VEC_BITS   0x00000004
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_VEC_MSB    2
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_VEC_LSB    2
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_VEC_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_AUTO_DPI
// Description : 1 = reset is controlled by the sequencer
//		 0 = reset is controlled by rstseq_ctrl
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_DPI_RESET  0x1
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_DPI_BITS   0x00000002
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_DPI_MSB    1
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_DPI_LSB    1
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_DPI_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_AUTO_BUSADAPTER
// Description : 1 = reset is controlled by the sequencer
//		 0 = reset is controlled by rstseq_ctrl
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_BUSADAPTER_RESET  0x1
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_BUSADAPTER_BITS   0x00000001
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_BUSADAPTER_MSB    0
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_BUSADAPTER_LSB    0
#define VIDEO_OUT_CFG_RSTSEQ_AUTO_BUSADAPTER_ACCESS "RW"
// =============================================================================
// Register    : VIDEO_OUT_CFG_RSTSEQ_PARALLEL
// JTAG access : synchronous
// Description : None
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_OFFSET 0x00000030
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_BITS   0x00000007
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_RESET  0x00000006
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_PARALLEL_VEC
// Description : Is this reset parallel (i.e. not part of the sequence)
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_VEC_RESET	 0x1
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_VEC_BITS	 0x00000004
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_VEC_MSB	 2
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_VEC_LSB	 2
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_VEC_ACCESS "RO"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_PARALLEL_DPI
// Description : Is this reset parallel (i.e. not part of the sequence)
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_DPI_RESET	 0x1
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_DPI_BITS	 0x00000002
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_DPI_MSB	 1
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_DPI_LSB	 1
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_DPI_ACCESS "RO"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_PARALLEL_BUSADAPTER
// Description : Is this reset parallel (i.e. not part of the sequence)
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_BUSADAPTER_RESET	0x0
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_BUSADAPTER_BITS	0x00000001
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_BUSADAPTER_MSB	0
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_BUSADAPTER_LSB	0
#define VIDEO_OUT_CFG_RSTSEQ_PARALLEL_BUSADAPTER_ACCESS "RO"
// =============================================================================
// Register    : VIDEO_OUT_CFG_RSTSEQ_CTRL
// JTAG access : synchronous
// Description : None
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_OFFSET 0x00000034
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_BITS	 0x00000007
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_RESET	 0x00000000
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_CTRL_VEC
// Description : 1 = keep the reset asserted
//		 0 = keep the reset deasserted
//		 This is ignored if rstseq_auto=1
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_VEC_RESET  0x0
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_VEC_BITS   0x00000004
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_VEC_MSB    2
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_VEC_LSB    2
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_VEC_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_CTRL_DPI
// Description : 1 = keep the reset asserted
//		 0 = keep the reset deasserted
//		 This is ignored if rstseq_auto=1
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_DPI_RESET  0x0
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_DPI_BITS   0x00000002
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_DPI_MSB    1
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_DPI_LSB    1
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_DPI_ACCESS "RW"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_CTRL_BUSADAPTER
// Description : 1 = keep the reset asserted
//		 0 = keep the reset deasserted
//		 This is ignored if rstseq_auto=1
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_BUSADAPTER_RESET  0x0
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_BUSADAPTER_BITS   0x00000001
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_BUSADAPTER_MSB    0
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_BUSADAPTER_LSB    0
#define VIDEO_OUT_CFG_RSTSEQ_CTRL_BUSADAPTER_ACCESS "RW"
// =============================================================================
// Register    : VIDEO_OUT_CFG_RSTSEQ_TRIG
// JTAG access : synchronous
// Description : None
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_OFFSET 0x00000038
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_BITS	 0x00000007
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_RESET	 0x00000000
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_TRIG_VEC
// Description : Pulses the reset output
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_VEC_RESET  0x0
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_VEC_BITS   0x00000004
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_VEC_MSB    2
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_VEC_LSB    2
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_VEC_ACCESS "SC"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_TRIG_DPI
// Description : Pulses the reset output
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_DPI_RESET  0x0
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_DPI_BITS   0x00000002
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_DPI_MSB    1
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_DPI_LSB    1
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_DPI_ACCESS "SC"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_TRIG_BUSADAPTER
// Description : Pulses the reset output
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_BUSADAPTER_RESET  0x0
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_BUSADAPTER_BITS   0x00000001
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_BUSADAPTER_MSB    0
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_BUSADAPTER_LSB    0
#define VIDEO_OUT_CFG_RSTSEQ_TRIG_BUSADAPTER_ACCESS "SC"
// =============================================================================
// Register    : VIDEO_OUT_CFG_RSTSEQ_DONE
// JTAG access : synchronous
// Description : None
#define VIDEO_OUT_CFG_RSTSEQ_DONE_OFFSET 0x0000003c
#define VIDEO_OUT_CFG_RSTSEQ_DONE_BITS	 0x00000007
#define VIDEO_OUT_CFG_RSTSEQ_DONE_RESET	 0x00000000
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_DONE_VEC
// Description : Indicates the current state of the reset
#define VIDEO_OUT_CFG_RSTSEQ_DONE_VEC_RESET  0x0
#define VIDEO_OUT_CFG_RSTSEQ_DONE_VEC_BITS   0x00000004
#define VIDEO_OUT_CFG_RSTSEQ_DONE_VEC_MSB    2
#define VIDEO_OUT_CFG_RSTSEQ_DONE_VEC_LSB    2
#define VIDEO_OUT_CFG_RSTSEQ_DONE_VEC_ACCESS "RO"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_DONE_DPI
// Description : Indicates the current state of the reset
#define VIDEO_OUT_CFG_RSTSEQ_DONE_DPI_RESET  0x0
#define VIDEO_OUT_CFG_RSTSEQ_DONE_DPI_BITS   0x00000002
#define VIDEO_OUT_CFG_RSTSEQ_DONE_DPI_MSB    1
#define VIDEO_OUT_CFG_RSTSEQ_DONE_DPI_LSB    1
#define VIDEO_OUT_CFG_RSTSEQ_DONE_DPI_ACCESS "RO"
// -----------------------------------------------------------------------------
// Field       : VIDEO_OUT_CFG_RSTSEQ_DONE_BUSADAPTER
// Description : Indicates the current state of the reset
#define VIDEO_OUT_CFG_RSTSEQ_DONE_BUSADAPTER_RESET  0x0
#define VIDEO_OUT_CFG_RSTSEQ_DONE_BUSADAPTER_BITS   0x00000001
#define VIDEO_OUT_CFG_RSTSEQ_DONE_BUSADAPTER_MSB    0
#define VIDEO_OUT_CFG_RSTSEQ_DONE_BUSADAPTER_LSB    0
#define VIDEO_OUT_CFG_RSTSEQ_DONE_BUSADAPTER_ACCESS "RO"
// =============================================================================

#define CFG_WRITE(reg, val)  writel((val),  vec->hw_base[RP1VEC_HW_BLOCK_CFG] + (reg ## _OFFSET))
#define CFG_READ(reg)	     readl(vec->hw_base[RP1VEC_HW_BLOCK_CFG] + (reg ## _OFFSET))

void rp1vec_vidout_setup(struct rp1_vec *vec)
{
	/*
	 * We assume DPI and VEC can't be used at the same time (due to
	 * clashing requirements for PLL_VIDEO, and potentially for VDAC).
	 * We therefore leave DPI memories powered down.
	 */
	CFG_WRITE(VIDEO_OUT_CFG_MEM_PD, VIDEO_OUT_CFG_MEM_PD_DPI_BITS);
	CFG_WRITE(VIDEO_OUT_CFG_TEST_OVERRIDE, 0x00000000);

	/* DPI->Pads; VEC->VDAC */
	CFG_WRITE(VIDEO_OUT_CFG_SEL, VIDEO_OUT_CFG_SEL_VDAC_MUX_BITS);

	/* configure VDAC for 1 channel, bandgap on, 1.28V swing */
	CFG_WRITE(VIDEO_OUT_CFG_VDAC_CFG, 0x0019ffff);

	/* enable VEC interrupt */
	CFG_WRITE(VIDEO_OUT_CFG_INTE, VIDEO_OUT_CFG_INTE_VEC_BITS);
}

void rp1vec_vidout_poweroff(struct rp1_vec *vec)
{
	/* disable VEC interrupt */
	CFG_WRITE(VIDEO_OUT_CFG_INTE, 0);

	/* Ensure VDAC is turned off; power down DPI,VEC memories */
	CFG_WRITE(VIDEO_OUT_CFG_VDAC_CFG, 0);
	CFG_WRITE(VIDEO_OUT_CFG_MEM_PD, VIDEO_OUT_CFG_MEM_PD_BITS);
}
