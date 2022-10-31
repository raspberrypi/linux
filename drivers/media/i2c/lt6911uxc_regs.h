/* SPDX-License-Identifier: (GPL-2.0)
 *
 * lt6911uxc_regs.h - Lontium 4k60 HDMI-CSI bridge register definitions
 *
 * Copyright (c) 2020, Alexey Gromov <groo@zhaw.ch>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __LT6911UXC_REGS_H__
#define __LT6911UXC_REGS_H__

/* Control */
#define SW_BANK	0xFF

#define ENABLE_I2C	0x80EE
#define DISABLE_WD	0x8010

/* Resolution registers */
#define H_TOTAL_0P5	0x867C	/* horizontal half total pixel */
#define H_ACTIVE_0P5	0x8680	/* horizontal half active pixel */
#define H_FP_0P5	0x8678	/* horizontal half front porch pixel */
#define H_BP_0P5	0x8676	/* horizontal half back porch pixel */
#define H_SW_0P5	0x8672	/* hsync half length pixel  */
#define V_TOTAL		0x867A	/* vertical total lines */
#define V_ACTIVE	0x867E	/* vertical active lines */
#define V_BP		0x8674	/* vertical back porch lines */
#define V_FP		0x8675	/* vertical front porch lines */
#define V_SW		0x8671	/* vsync length lines */

#define SYNC_POL	0x8670	/* hsync/vsync polarity flags */
#define MASK_VSYNC_POL	(1 << 1)
#define MASK_HSYNC_POL	(1 << 0)

/* FPS registers */
#define MASK_FMI_FREQ2	0x0F

#define FM1_FREQ_IN2	0x8548
#define FM1_FREQ_IN1	0x8549
#define FM1_FREQ_IN0	0x854A

#define AD_HALF_PCLK	0x8540

/* MIPI-TX */
#define MIPI_TX_CTRL	0x811D
#define MIPI_LANES	0x86A2
#define MIPI_CLK_MODE	0xD468

/* Audio sample rate */
#define AUDIO_SR	0xB0AB

/* Interrupts */
#define INT_HDMI		0x86A3
#define INT_HDMI_STABLE		0x55
#define INT_HDMI_DISCONNECT	0x88

#define INT_AUDIO		0x86A5
#define INT_AUDIO_DISCONNECT	0x88
#define INT_AUDIO_SR_HIGH	0x55
#define INT_AUDIO_SR_LOW	0xAA

#endif  /* __LT6911UXC_REGS_H__ */
