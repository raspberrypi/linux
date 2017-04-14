/*
 * cxd2880_tnrdmd_dvbt.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * control functions for DVB-T
 *
 * Copyright (C) 2016, 2017 Sony Semiconductor Solutions Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; version 2 of the License.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "cxd2880_tnrdmd_dvbt.h"
#include "cxd2880_tnrdmd_dvbt_mon.h"

static enum cxd2880_ret x_tune_dvbt_demod_setting(struct cxd2880_tnrdmd
						  *tnr_dmd,
						  enum cxd2880_dtv_bandwidth
						  bandwidth,
						  enum cxd2880_tnrdmd_clockmode
						  clk_mode)
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x31,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	{
		u8 data_a[2] = { 0x52, 0x49 };
		u8 data_b[2] = { 0x5D, 0x55 };
		u8 data_c[2] = { 0x60, 0x00 };
		u8 *data = NULL;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x04) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		switch (clk_mode) {
		case CXD2880_TNRDMD_CLOCKMODE_A:
			data = data_a;
			break;
		case CXD2880_TNRDMD_CLOCKMODE_B:
			data = data_b;
			break;
		case CXD2880_TNRDMD_CLOCKMODE_C:
			data = data_c;
			break;
		default:
			return CXD2880_RESULT_ERROR_SW_STATE;
		}

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x65, data,
					    2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x5D,
				   0x07) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_SUB) {
		u8 data[2] = { 0x01, 0x01 };

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x00) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0xCE, data,
					    2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x04) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x5C,
				   0xFB) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xA4,
				   0x03) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x14) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xB0,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x25) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	{
		u8 data[2] = { 0x01, 0xF0 };

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0xF0, data,
					    2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if ((tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) ||
	    (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)) {
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x12) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x44,
					   0x00) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB) {
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x11) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x87,
					   0xD2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_SUB) {
		u8 data_a[3] = { 0x73, 0xCA, 0x49 };
		u8 data_b[3] = { 0xC8, 0x13, 0xAA };
		u8 data_c[3] = { 0xDC, 0x6C, 0x00 };
		u8 *data = NULL;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x04) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		switch (clk_mode) {
		case CXD2880_TNRDMD_CLOCKMODE_A:
			data = data_a;
			break;
		case CXD2880_TNRDMD_CLOCKMODE_B:
			data = data_b;
			break;
		case CXD2880_TNRDMD_CLOCKMODE_C:
			data = data_c;
			break;
		default:
			return CXD2880_RESULT_ERROR_SW_STATE;
		}

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x68, data,
					    3) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x04) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	switch (bandwidth) {
	case CXD2880_DTV_BW_8_MHZ:

		{
			u8 data_ac[5] = { 0x15, 0x00, 0x00, 0x00,
				0x00
			};
			u8 data_b[5] = { 0x14, 0x6A, 0xAA, 0xAA,
				0xAA
			};
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_ac;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x60,
						    data,
						    5) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x4A,
					   0x00) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		{
			u8 data_a[2] = { 0x01, 0x28 };
			u8 data_b[2] = { 0x11, 0x44 };
			u8 data_c[2] = { 0x15, 0x28 };
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
				data = data_a;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_c;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x7D,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data = 0;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = 0x35;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = 0x34;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x71,
						   data) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			u8 data_a[5] = { 0x30, 0x00, 0x00, 0x90,
				0x00
			};
			u8 data_b[5] = { 0x36, 0x71, 0x00, 0xA3,
				0x55
			};
			u8 data_c[5] = { 0x38, 0x00, 0x00, 0xA8,
				0x00
			};
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
				data = data_a;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_c;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x4B,
						    &data[0],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x51,
						    &data[2],
						    3) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data[4] = { 0xB3, 0x00, 0x01, 0x02 };

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x72,
						    &data[0],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x6B,
						    &data[2],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}
		break;

	case CXD2880_DTV_BW_7_MHZ:

		{
			u8 data_ac[5] = { 0x18, 0x00, 0x00, 0x00,
				0x00
			};
			u8 data_b[5] = { 0x17, 0x55, 0x55, 0x55,
				0x55
			};
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_ac;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x60,
						    data,
						    5) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x4A,
					   0x02) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		{
			u8 data_a[2] = { 0x12, 0x4C };
			u8 data_b[2] = { 0x1F, 0x15 };
			u8 data_c[2] = { 0x1F, 0xF8 };
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
				data = data_a;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_c;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x7D,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data = 0;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = 0x2F;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = 0x2E;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x71,
						   data) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			u8 data_a[5] = { 0x36, 0xDB, 0x00, 0xA4,
				0x92
			};
			u8 data_b[5] = { 0x3E, 0x38, 0x00, 0xBA,
				0xAA
			};
			u8 data_c[5] = { 0x40, 0x00, 0x00, 0xC0,
				0x00
			};
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
				data = data_a;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_c;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x4B,
						    &data[0],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x51,
						    &data[2],
						    3) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data[4] = { 0xB8, 0x00, 0x00, 0x03 };

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x72,
						    &data[0],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x6B,
						    &data[2],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}
		break;

	case CXD2880_DTV_BW_6_MHZ:

		{
			u8 data_ac[5] = { 0x1C, 0x00, 0x00, 0x00,
				0x00
			};
			u8 data_b[5] = { 0x1B, 0x38, 0xE3, 0x8E,
				0x38
			};
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_ac;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x60,
						    data,
						    5) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x4A,
					   0x04) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		{
			u8 data_a[2] = { 0x1F, 0xF8 };
			u8 data_b[2] = { 0x24, 0x43 };
			u8 data_c[2] = { 0x25, 0x4C };
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
				data = data_a;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_c;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x7D,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data = 0;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = 0x29;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = 0x2A;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x71,
						   data) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			u8 data_a[5] = { 0x40, 0x00, 0x00, 0xC0,
				0x00
			};
			u8 data_b[5] = { 0x48, 0x97, 0x00, 0xD9,
				0xC7
			};
			u8 data_c[5] = { 0x4A, 0xAA, 0x00, 0xDF,
				0xFF
			};
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
				data = data_a;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_c;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x4B,
						    &data[0],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x51,
						    &data[2],
						    3) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data[4] = { 0xBE, 0xAB, 0x00, 0x03 };

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x72,
						    &data[0],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x6B,
						    &data[2],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}
		break;

	case CXD2880_DTV_BW_5_MHZ:

		{
			u8 data_ac[5] = { 0x21, 0x99, 0x99, 0x99,
				0x99
			};
			u8 data_b[5] = { 0x20, 0xAA, 0xAA, 0xAA,
				0xAA
			};
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_ac;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x60,
						    data,
						    5) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x4A,
					   0x06) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		{
			u8 data_a[2] = { 0x26, 0x5D };
			u8 data_b[2] = { 0x2B, 0x84 };
			u8 data_c[2] = { 0x2C, 0xC2 };
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
				data = data_a;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_c;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x7D,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data = 0;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = 0x24;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = 0x23;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x71,
						   data) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			u8 data_a[5] = { 0x4C, 0xCC, 0x00, 0xE6,
				0x66
			};
			u8 data_b[5] = { 0x57, 0x1C, 0x01, 0x05,
				0x55
			};
			u8 data_c[5] = { 0x59, 0x99, 0x01, 0x0C,
				0xCC
			};
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
				data = data_a;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_c;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x4B,
						    &data[0],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x51,
						    &data[2],
						    3) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data[4] = { 0xC8, 0x01, 0x00, 0x03 };

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x72,
						    &data[0],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x6B,
						    &data[2],
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}
		break;

	default:
		return CXD2880_RESULT_ERROR_SW_STATE;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xFD,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret x_sleep_dvbt_demod_setting(struct cxd2880_tnrdmd
						   *tnr_dmd)
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x04) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x5C,
				   0xD8) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xA4,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB) {
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x11) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x87,
					   0x04) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret dvbt_set_profile(struct cxd2880_tnrdmd *tnr_dmd,
					 enum cxd2880_dvbt_profile profile)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x67,
				   (profile ==
				    CXD2880_DVBT_PROFILE_HP) ? 0x00 : 0x01) !=
	    CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_tune1(struct cxd2880_tnrdmd *tnr_dmd,
					   struct cxd2880_dvbt_tune_param
					   *tune_param)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!tune_param))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret =
	    cxd2880_tnrdmd_common_tune_setting1(tnr_dmd, CXD2880_DTV_SYS_DVBT,
						tune_param->center_freq_khz,
						tune_param->bandwidth, 0, 0);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret =
	    x_tune_dvbt_demod_setting(tnr_dmd, tune_param->bandwidth,
				      tnr_dmd->clk_mode);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret =
		    x_tune_dvbt_demod_setting(tnr_dmd->diver_sub,
					      tune_param->bandwidth,
					      tnr_dmd->diver_sub->clk_mode);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	ret = dvbt_set_profile(tnr_dmd, tune_param->profile);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_tune2(struct cxd2880_tnrdmd *tnr_dmd,
					   struct cxd2880_dvbt_tune_param
					   *tune_param)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!tune_param))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret =
	    cxd2880_tnrdmd_common_tune_setting2(tnr_dmd, CXD2880_DTV_SYS_DVBT,
						0);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	tnr_dmd->state = CXD2880_TNRDMD_STATE_ACTIVE;
	tnr_dmd->frequency_khz = tune_param->center_freq_khz;
	tnr_dmd->sys = CXD2880_DTV_SYS_DVBT;
	tnr_dmd->bandwidth = tune_param->bandwidth;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		tnr_dmd->diver_sub->state = CXD2880_TNRDMD_STATE_ACTIVE;
		tnr_dmd->diver_sub->frequency_khz = tune_param->center_freq_khz;
		tnr_dmd->diver_sub->sys = CXD2880_DTV_SYS_DVBT;
		tnr_dmd->diver_sub->bandwidth = tune_param->bandwidth;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_sleep_setting(struct cxd2880_tnrdmd
						   *tnr_dmd)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = x_sleep_dvbt_demod_setting(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret = x_sleep_dvbt_demod_setting(tnr_dmd->diver_sub);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_check_demod_lock(struct cxd2880_tnrdmd
						      *tnr_dmd,
						      enum
						      cxd2880_tnrdmd_lock_result
						      *lock)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	u8 sync_stat = 0;
	u8 ts_lock = 0;
	u8 unlock_detected = 0;
	u8 unlock_detected_sub = 0;

	if ((!tnr_dmd) || (!lock))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret =
	    cxd2880_tnrdmd_dvbt_mon_sync_stat(tnr_dmd, &sync_stat, &ts_lock,
					      &unlock_detected);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SINGLE) {
		if (sync_stat == 6)
			*lock = CXD2880_TNRDMD_LOCK_RESULT_LOCKED;
		else if (unlock_detected)
			*lock = CXD2880_TNRDMD_LOCK_RESULT_UNLOCKED;
		else
			*lock = CXD2880_TNRDMD_LOCK_RESULT_NOTDETECT;

		return ret;
	}

	if (sync_stat == 6) {
		*lock = CXD2880_TNRDMD_LOCK_RESULT_LOCKED;
		return ret;
	}

	ret =
	    cxd2880_tnrdmd_dvbt_mon_sync_stat_sub(tnr_dmd, &sync_stat,
						  &unlock_detected_sub);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (sync_stat == 6)
		*lock = CXD2880_TNRDMD_LOCK_RESULT_LOCKED;
	else if (unlock_detected && unlock_detected_sub)
		*lock = CXD2880_TNRDMD_LOCK_RESULT_UNLOCKED;
	else
		*lock = CXD2880_TNRDMD_LOCK_RESULT_NOTDETECT;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_check_ts_lock(struct cxd2880_tnrdmd
						   *tnr_dmd,
						   enum
						   cxd2880_tnrdmd_lock_result
						   *lock)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	u8 sync_stat = 0;
	u8 ts_lock = 0;
	u8 unlock_detected = 0;
	u8 unlock_detected_sub = 0;

	if ((!tnr_dmd) || (!lock))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret =
	    cxd2880_tnrdmd_dvbt_mon_sync_stat(tnr_dmd, &sync_stat, &ts_lock,
					      &unlock_detected);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SINGLE) {
		if (ts_lock)
			*lock = CXD2880_TNRDMD_LOCK_RESULT_LOCKED;
		else if (unlock_detected)
			*lock = CXD2880_TNRDMD_LOCK_RESULT_UNLOCKED;
		else
			*lock = CXD2880_TNRDMD_LOCK_RESULT_NOTDETECT;

		return ret;
	}

	if (ts_lock) {
		*lock = CXD2880_TNRDMD_LOCK_RESULT_LOCKED;
		return ret;
	} else if (!unlock_detected) {
		*lock = CXD2880_TNRDMD_LOCK_RESULT_NOTDETECT;
		return ret;
	}

	ret =
	    cxd2880_tnrdmd_dvbt_mon_sync_stat_sub(tnr_dmd, &sync_stat,
						  &unlock_detected_sub);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (unlock_detected && unlock_detected_sub)
		*lock = CXD2880_TNRDMD_LOCK_RESULT_UNLOCKED;
	else
		*lock = CXD2880_TNRDMD_LOCK_RESULT_NOTDETECT;

	return ret;
}
