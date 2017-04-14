/*
 * cxd2880_tnrdmd_dvbt2.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * control functions for DVB-T2
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

#include "cxd2880_tnrdmd_dvbt2.h"
#include "cxd2880_tnrdmd_dvbt2_mon.h"

static enum cxd2880_ret x_tune_dvbt2_demod_setting(struct cxd2880_tnrdmd
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
				   0x02) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x04) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x5D,
				   0x0B) != CXD2880_RESULT_OK)
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

	{
		u8 data[14] = { 0x07, 0x06, 0x01, 0xF0,
			0x00, 0x00, 0x04, 0xB0, 0x00, 0x00, 0x09, 0x9C, 0x0E,
			    0x4C
		};

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x20) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x8A,
					   data[0]) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x90,
					   data[1]) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x25) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0xF0, &data[2],
					    2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x2A) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xDC,
					   data[4]) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xDE,
					   data[5]) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x2D) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x73, &data[6],
					    4) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x8F, &data[10],
					    4) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	{
		u8 data_a_1[9] = { 0x52, 0x49, 0x2C, 0x51,
			0x51, 0x3D, 0x15, 0x29, 0x0C
		};
		u8 data_b_1[9] = { 0x5D, 0x55, 0x32, 0x5C,
			0x5C, 0x45, 0x17, 0x2E, 0x0D
		};
		u8 data_c_1[9] = { 0x60, 0x00, 0x34, 0x5E,
			0x5E, 0x47, 0x18, 0x2F, 0x0E
		};

		u8 data_a_2[13] = { 0x04, 0xE7, 0x94, 0x92,
			0x09, 0xCF, 0x7E, 0xD0, 0x49, 0xCD, 0xCD, 0x1F, 0x5B
		};
		u8 data_b_2[13] = { 0x05, 0x90, 0x27, 0x55,
			0x0B, 0x20, 0x8F, 0xD6, 0xEA, 0xC8, 0xC8, 0x23, 0x91
		};
		u8 data_c_2[13] = { 0x05, 0xB8, 0xD8, 0x00,
			0x0B, 0x72, 0x93, 0xF3, 0x00, 0xCD, 0xCD, 0x24, 0x95
		};

		u8 data_a_3[5] = { 0x0B, 0x6A, 0xC9, 0x03,
			0x33
		};
		u8 data_b_3[5] = { 0x01, 0x02, 0xE4, 0x03,
			0x39
		};
		u8 data_c_3[5] = { 0x01, 0x02, 0xEB, 0x03,
			0x3B
		};

		u8 *data_1 = NULL;
		u8 *data_2 = NULL;
		u8 *data_3 = NULL;

		switch (clk_mode) {
		case CXD2880_TNRDMD_CLOCKMODE_A:
			data_1 = data_a_1;
			data_2 = data_a_2;
			data_3 = data_a_3;
			break;
		case CXD2880_TNRDMD_CLOCKMODE_B:
			data_1 = data_b_1;
			data_2 = data_b_2;
			data_3 = data_b_3;
			break;
		case CXD2880_TNRDMD_CLOCKMODE_C:
			data_1 = data_c_1;
			data_2 = data_c_2;
			data_3 = data_c_3;
			break;
		default:
			return CXD2880_RESULT_ERROR_SW_STATE;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x04) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x1D,
					    &data_1[0], 3) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x22,
					   data_1[3]) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x24,
					   data_1[4]) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x26,
					   data_1[5]) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x29,
					    &data_1[6], 2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x2D,
					   data_1[8]) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_SUB) {
			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x2E,
						    &data_2[0],
						    6) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x35,
						    &data_2[6],
						    7) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x3C,
					    &data_3[0], 2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x56,
					    &data_3[2], 3) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	switch (bandwidth) {
	case CXD2880_DTV_BW_8_MHZ:

		{
			u8 data_ac[6] = { 0x15, 0x00, 0x00, 0x00,
				0x00, 0x00
			};
			u8 data_b[6] = { 0x14, 0x6A, 0xAA, 0xAA,
				0xAB, 0x00
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
						    CXD2880_IO_TGT_DMD, 0x10,
						    data,
						    6) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x4A,
					   0x00) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		{
			u8 data_a[2] = { 0x19, 0xD2 };
			u8 data_bc[2] = { 0x3F, 0xFF };
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
				data = data_a;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_bc;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x19,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data_a[2] = { 0x06, 0x2A };
			u8 data_b[2] = { 0x06, 0x29 };
			u8 data_c[2] = { 0x06, 0x28 };
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
						    CXD2880_IO_TGT_DMD, 0x1B,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			u8 data_a[9] = { 0x28, 0x00, 0x50, 0x00,
				0x60, 0x00, 0x00, 0x90, 0x00
			};
			u8 data_b[9] = { 0x2D, 0x5E, 0x5A, 0xBD,
				0x6C, 0xE3, 0x00, 0xA3, 0x55
			};
			u8 data_c[9] = { 0x2E, 0xAA, 0x5D, 0x55,
				0x70, 0x00, 0x00, 0xA8, 0x00
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
						    data,
						    9) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}
		break;

	case CXD2880_DTV_BW_7_MHZ:

		{
			u8 data_ac[6] = { 0x18, 0x00, 0x00, 0x00,
				0x00, 0x00
			};
			u8 data_b[6] = { 0x17, 0x55, 0x55, 0x55,
				0x55, 0x00
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
						    CXD2880_IO_TGT_DMD, 0x10,
						    data,
						    6) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x4A,
					   0x02) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		{
			u8 data[2] = { 0x3F, 0xFF };

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x19,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data_a[2] = { 0x06, 0x23 };
			u8 data_b[2] = { 0x06, 0x22 };
			u8 data_c[2] = { 0x06, 0x21 };
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
						    CXD2880_IO_TGT_DMD, 0x1B,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			u8 data_a[9] = { 0x2D, 0xB6, 0x5B, 0x6D,
				0x6D, 0xB6, 0x00, 0xA4, 0x92
			};
			u8 data_b[9] = { 0x33, 0xDA, 0x67, 0xB4,
				0x7C, 0x71, 0x00, 0xBA, 0xAA
			};
			u8 data_c[9] = { 0x35, 0x55, 0x6A, 0xAA,
				0x80, 0x00, 0x00, 0xC0, 0x00
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
						    data,
						    9) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}
		break;

	case CXD2880_DTV_BW_6_MHZ:

		{
			u8 data_ac[6] = { 0x1C, 0x00, 0x00, 0x00,
				0x00, 0x00
			};
			u8 data_b[6] = { 0x1B, 0x38, 0xE3, 0x8E,
				0x39, 0x00
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
						    CXD2880_IO_TGT_DMD, 0x10,
						    data,
						    6) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x4A,
					   0x04) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		{
			u8 data[2] = { 0x3F, 0xFF };

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x19,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data_a[2] = { 0x06, 0x1C };
			u8 data_b[2] = { 0x06, 0x1B };
			u8 data_c[2] = { 0x06, 0x1A };
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
						    CXD2880_IO_TGT_DMD, 0x1B,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			u8 data_a[9] = { 0x35, 0x55, 0x6A, 0xAA,
				0x80, 0x00, 0x00, 0xC0, 0x00
			};
			u8 data_b[9] = { 0x3C, 0x7E, 0x78, 0xFC,
				0x91, 0x2F, 0x00, 0xD9, 0xC7
			};
			u8 data_c[9] = { 0x3E, 0x38, 0x7C, 0x71,
				0x95, 0x55, 0x00, 0xDF, 0xFF
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
						    data,
						    9) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}
		break;

	case CXD2880_DTV_BW_5_MHZ:

		{
			u8 data_ac[6] = { 0x21, 0x99, 0x99, 0x99,
				0x9A, 0x00
			};
			u8 data_b[6] = { 0x20, 0xAA, 0xAA, 0xAA,
				0xAB, 0x00
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
						    CXD2880_IO_TGT_DMD, 0x10,
						    data,
						    6) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x4A,
					   0x06) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		{
			u8 data[2] = { 0x3F, 0xFF };

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x19,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data_a[2] = { 0x06, 0x15 };
			u8 data_b[2] = { 0x06, 0x15 };
			u8 data_c[2] = { 0x06, 0x14 };
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
						    CXD2880_IO_TGT_DMD, 0x1B,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			u8 data_a[9] = { 0x40, 0x00, 0x6A, 0xAA,
				0x80, 0x00, 0x00, 0xE6, 0x66
			};
			u8 data_b[9] = { 0x48, 0x97, 0x78, 0xFC,
				0x91, 0x2F, 0x01, 0x05, 0x55
			};
			u8 data_c[9] = { 0x4A, 0xAA, 0x7C, 0x71,
				0x95, 0x55, 0x01, 0x0C, 0xCC
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
						    data,
						    9) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}
		break;

	case CXD2880_DTV_BW_1_7_MHZ:

		{
			u8 data_a[6] = { 0x68, 0x0F, 0xA2, 0x32,
				0xCF, 0x03
			};
			u8 data_c[6] = { 0x68, 0x0F, 0xA2, 0x32,
				0xCF, 0x03
			};
			u8 data_b[6] = { 0x65, 0x2B, 0xA4, 0xCD,
				0xD8, 0x03
			};
			u8 *data = NULL;

			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
				data = data_a;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_C:
				data = data_c;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				data = data_b;
				break;
			default:
				return CXD2880_RESULT_ERROR_SW_STATE;
			}

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x10,
						    data,
						    6) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x4A,
					   0x03) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		{
			u8 data[2] = { 0x3F, 0xFF };

			if (tnr_dmd->io->write_regs(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x19,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		{
			u8 data_a[2] = { 0x06, 0x0C };
			u8 data_b[2] = { 0x06, 0x0C };
			u8 data_c[2] = { 0x06, 0x0B };
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
						    CXD2880_IO_TGT_DMD, 0x1B,
						    data,
						    2) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			u8 data_a[9] = { 0x40, 0x00, 0x6A, 0xAA,
				0x80, 0x00, 0x02, 0xC9, 0x8F
			};
			u8 data_b[9] = { 0x48, 0x97, 0x78, 0xFC,
				0x91, 0x2F, 0x03, 0x29, 0x5D
			};
			u8 data_c[9] = { 0x4A, 0xAA, 0x7C, 0x71,
				0x95, 0x55, 0x03, 0x40, 0x7D
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
						    data,
						    9) != CXD2880_RESULT_OK)
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

static enum cxd2880_ret x_sleep_dvbt2_demod_setting(struct cxd2880_tnrdmd
						    *tnr_dmd)
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		u8 data[] = { 0, 1, 0, 2,
			0, 4, 0, 8, 0, 16, 0, 32
		};

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x1D) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x47, data,
					    12) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret dvbt2_set_profile(struct cxd2880_tnrdmd *tnr_dmd,
					  enum cxd2880_dvbt2_profile profile)
{
	u8 t2_mode_tune_mode = 0;
	u8 seq_not2_dtime = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	{
		u8 dtime1 = 0;
		u8 dtime2 = 0;

		switch (tnr_dmd->clk_mode) {
		case CXD2880_TNRDMD_CLOCKMODE_A:
			dtime1 = 0x27;
			dtime2 = 0x0C;
			break;
		case CXD2880_TNRDMD_CLOCKMODE_B:
			dtime1 = 0x2C;
			dtime2 = 0x0D;
			break;
		case CXD2880_TNRDMD_CLOCKMODE_C:
			dtime1 = 0x2E;
			dtime2 = 0x0E;
			break;
		default:
			return CXD2880_RESULT_ERROR_SW_STATE;
		}

		switch (profile) {
		case CXD2880_DVBT2_PROFILE_BASE:
			t2_mode_tune_mode = 0x01;
			seq_not2_dtime = dtime2;
			break;

		case CXD2880_DVBT2_PROFILE_LITE:
			t2_mode_tune_mode = 0x05;
			seq_not2_dtime = dtime1;
			break;

		case CXD2880_DVBT2_PROFILE_ANY:
			t2_mode_tune_mode = 0x00;
			seq_not2_dtime = dtime1;
			break;

		default:
			return CXD2880_RESULT_ERROR_ARG;
		}
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x2E) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x10,
				   t2_mode_tune_mode) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x04) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x2C,
				   seq_not2_dtime) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_tune1(struct cxd2880_tnrdmd *tnr_dmd,
					    struct cxd2880_dvbt2_tune_param
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

	if ((tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) &&
	    (tune_param->profile == CXD2880_DVBT2_PROFILE_ANY))
		return CXD2880_RESULT_ERROR_NOSUPPORT;

	ret =
	    cxd2880_tnrdmd_common_tune_setting1(tnr_dmd, CXD2880_DTV_SYS_DVBT2,
						tune_param->center_freq_khz,
						tune_param->bandwidth, 0, 0);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret =
	    x_tune_dvbt2_demod_setting(tnr_dmd, tune_param->bandwidth,
				       tnr_dmd->clk_mode);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret =
		    x_tune_dvbt2_demod_setting(tnr_dmd->diver_sub,
					       tune_param->bandwidth,
					       tnr_dmd->diver_sub->clk_mode);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	ret = dvbt2_set_profile(tnr_dmd, tune_param->profile);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret =
		    dvbt2_set_profile(tnr_dmd->diver_sub, tune_param->profile);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	if (tune_param->data_plp_id == CXD2880_DVBT2_TUNE_PARAM_PLPID_AUTO) {
		ret = cxd2880_tnrdmd_dvbt2_set_plp_cfg(tnr_dmd, 1, 0);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	} else {
		ret =
		    cxd2880_tnrdmd_dvbt2_set_plp_cfg(tnr_dmd, 0,
					     (u8)(tune_param->data_plp_id));
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_tune2(struct cxd2880_tnrdmd *tnr_dmd,
					    struct cxd2880_dvbt2_tune_param
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

	{
		u8 en_fef_intmtnt_ctrl = 1;

		switch (tune_param->profile) {
		case CXD2880_DVBT2_PROFILE_BASE:
			en_fef_intmtnt_ctrl = tnr_dmd->en_fef_intmtnt_base;
			break;
		case CXD2880_DVBT2_PROFILE_LITE:
			en_fef_intmtnt_ctrl = tnr_dmd->en_fef_intmtnt_lite;
			break;
		case CXD2880_DVBT2_PROFILE_ANY:
			if (tnr_dmd->en_fef_intmtnt_base &&
			    tnr_dmd->en_fef_intmtnt_lite)
				en_fef_intmtnt_ctrl = 1;
			else
				en_fef_intmtnt_ctrl = 0;
			break;
		default:
			return CXD2880_RESULT_ERROR_ARG;
		}

		ret =
		    cxd2880_tnrdmd_common_tune_setting2(tnr_dmd,
							CXD2880_DTV_SYS_DVBT2,
							en_fef_intmtnt_ctrl);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	tnr_dmd->state = CXD2880_TNRDMD_STATE_ACTIVE;
	tnr_dmd->frequency_khz = tune_param->center_freq_khz;
	tnr_dmd->sys = CXD2880_DTV_SYS_DVBT2;
	tnr_dmd->bandwidth = tune_param->bandwidth;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		tnr_dmd->diver_sub->state = CXD2880_TNRDMD_STATE_ACTIVE;
		tnr_dmd->diver_sub->frequency_khz = tune_param->center_freq_khz;
		tnr_dmd->diver_sub->sys = CXD2880_DTV_SYS_DVBT2;
		tnr_dmd->diver_sub->bandwidth = tune_param->bandwidth;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_sleep_setting(struct cxd2880_tnrdmd
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

	ret = x_sleep_dvbt2_demod_setting(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret = x_sleep_dvbt2_demod_setting(tnr_dmd->diver_sub);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_check_demod_lock(struct cxd2880_tnrdmd
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
	    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_stat, &ts_lock,
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
	    cxd2880_tnrdmd_dvbt2_mon_sync_stat_sub(tnr_dmd, &sync_stat,
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

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_check_ts_lock(struct cxd2880_tnrdmd
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
	    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_stat, &ts_lock,
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
	    cxd2880_tnrdmd_dvbt2_mon_sync_stat_sub(tnr_dmd, &sync_stat,
						   &unlock_detected_sub);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (unlock_detected && unlock_detected_sub)
		*lock = CXD2880_TNRDMD_LOCK_RESULT_UNLOCKED;
	else
		*lock = CXD2880_TNRDMD_LOCK_RESULT_NOTDETECT;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_set_plp_cfg(struct cxd2880_tnrdmd
						  *tnr_dmd, u8 auto_plp,
						  u8 plp_id)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x23) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (!auto_plp) {
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xAF,
					   plp_id) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xAD,
				   auto_plp ? 0x00 : 0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_diver_fef_setting(struct cxd2880_tnrdmd
							*tnr_dmd)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SINGLE)
		return CXD2880_RESULT_OK;

	{
		struct cxd2880_dvbt2_ofdm ofdm;

		ret = cxd2880_tnrdmd_dvbt2_mon_ofdm(tnr_dmd, &ofdm);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (!ofdm.mixed)
			return CXD2880_RESULT_OK;
	}

	{
		u8 data[] = { 0, 8, 0, 16,
			0, 32, 0, 64, 0, 128, 1, 0
		};

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x1D) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x47, data,
					    12) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_check_l1post_valid(struct cxd2880_tnrdmd
							 *tnr_dmd,
							 u8 *l1_post_valid)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	u8 data;

	if ((!tnr_dmd) || (!l1_post_valid))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0B) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x86, &data,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	*l1_post_valid = data & 0x01;

	return ret;
}
