/*
 * cxd2880_tnrdmd.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * common control functions
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

#include "cxd2880_common.h"
#include "cxd2880_stdlib.h"
#include "cxd2880_tnrdmd.h"
#include "cxd2880_tnrdmd_mon.h"
#include "cxd2880_tnrdmd_dvbt.h"
#include "cxd2880_tnrdmd_dvbt2.h"

static enum cxd2880_ret p_init1(struct cxd2880_tnrdmd *tnr_dmd)
{
	u8 data = 0;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if ((tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SINGLE) ||
	    (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN)) {
		switch (tnr_dmd->create_param.ts_output_if) {
		case CXD2880_TNRDMD_TSOUT_IF_TS:
			data = 0x00;
			break;
		case CXD2880_TNRDMD_TSOUT_IF_SPI:
			data = 0x01;
			break;
		case CXD2880_TNRDMD_TSOUT_IF_SDIO:
			data = 0x02;
			break;
		default:
			return CXD2880_RESULT_ERROR_ARG;
		}
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x10,
					   data) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x11,
				   0x16) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	switch (tnr_dmd->chip_id) {
	case CXD2880_TNRDMD_CHIP_ID_CXD2880_ES1_0X:
		data = 0x1A;
		break;
	case CXD2880_TNRDMD_CHIP_ID_CXD2880_ES1_11:
		data = 0x16;
		break;
	default:
		return CXD2880_RESULT_ERROR_NOSUPPORT;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x10,
				   data) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->create_param.en_internal_ldo)
		data = 0x01;
	else
		data = 0x00;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x11,
				   data) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x13,
				   data) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x12,
				   data) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	switch (tnr_dmd->chip_id) {
	case CXD2880_TNRDMD_CHIP_ID_CXD2880_ES1_0X:
		data = 0x01;
		break;
	case CXD2880_TNRDMD_CHIP_ID_CXD2880_ES1_11:
		data = 0x00;
		break;
	default:
		return CXD2880_RESULT_ERROR_NOSUPPORT;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x69,
				   data) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret p_init2(struct cxd2880_tnrdmd *tnr_dmd)
{
	u8 data[6] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = tnr_dmd->create_param.xosc_cap;
	data[1] = tnr_dmd->create_param.xosc_i;
	switch (tnr_dmd->create_param.xtal_share_type) {
	case CXD2880_TNRDMD_XTAL_SHARE_NONE:
		data[2] = 0x01;
		data[3] = 0x00;
		break;
	case CXD2880_TNRDMD_XTAL_SHARE_EXTREF:
		data[2] = 0x00;
		data[3] = 0x00;
		break;
	case CXD2880_TNRDMD_XTAL_SHARE_MASTER:
		data[2] = 0x01;
		data[3] = 0x01;
		break;
	case CXD2880_TNRDMD_XTAL_SHARE_SLAVE:
		data[2] = 0x00;
		data[3] = 0x01;
		break;
	default:
		return CXD2880_RESULT_ERROR_ARG;
	}
	data[4] = 0x06;
	data[5] = 0x00;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x13, data,
				    6) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret p_init3(struct cxd2880_tnrdmd *tnr_dmd)
{
	u8 data[2] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	switch (tnr_dmd->diver_mode) {
	case CXD2880_TNRDMD_DIVERMODE_SINGLE:
		data[0] = 0x00;
		break;
	case CXD2880_TNRDMD_DIVERMODE_MAIN:
		data[0] = 0x03;
		break;
	case CXD2880_TNRDMD_DIVERMODE_SUB:
		data[0] = 0x02;
		break;
	default:
		return CXD2880_RESULT_ERROR_ARG;
	}

	data[1] = 0x01;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x1F, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret rf_init1(struct cxd2880_tnrdmd *tnr_dmd)
{
	u8 data[80] = { 0 };
	u8 addr = 0;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x01;
	data[1] = 0x00;
	data[2] = 0x01;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x21, data,
				    3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x01;
	data[1] = 0x01;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x17, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->create_param.stationary_use) {
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x1A,
					   0x06) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x4F,
				   0x18) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x61,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x71,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x9D,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x7D,
				   0x02) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x8F,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x8B,
				   0xC6) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x9A,
				   0x03) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x1C,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x00;
	if ((tnr_dmd->create_param.is_cxd2881gg) &&
	    (tnr_dmd->create_param.xtal_share_type ==
		CXD2880_TNRDMD_XTAL_SHARE_SLAVE))
		data[1] = 0x00;
	else
		data[1] = 0x1F;
	data[2] = 0x0A;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0xB5, data,
				    3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xB9,
				   0x07) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x33,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xC1,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xC4,
				   0x1E) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->chip_id == CXD2880_TNRDMD_CHIP_ID_CXD2880_ES1_0X) {
		data[0] = 0x34;
		data[1] = 0x2C;
	} else {
		data[0] = 0x2F;
		data[1] = 0x25;
	}
	data[2] = 0x15;
	data[3] = 0x19;
	data[4] = 0x1B;
	data[5] = 0x15;
	data[6] = 0x19;
	data[7] = 0x1B;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0xD9, data,
				    8) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x11) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x6C;
	data[1] = 0x10;
	data[2] = 0xA6;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x44, data,
				    3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x16;
	data[1] = 0xA8;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x50, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x00;
	data[1] = 0x22;
	data[2] = 0x00;
	data[3] = 0x88;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x62, data,
				    4) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x74,
				   0x75) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x05;
	data[1] = 0x05;
	data[2] = 0x05;
	data[3] = 0x05;
	data[4] = 0x05;
	data[5] = 0x05;
	data[6] = 0x05;
	data[7] = 0x05;
	data[8] = 0x05;
	data[9] = 0x04;
	data[10] = 0x04;
	data[11] = 0x04;
	data[12] = 0x03;
	data[13] = 0x03;
	data[14] = 0x03;
	data[15] = 0x04;
	data[16] = 0x04;
	data[17] = 0x05;
	data[18] = 0x05;
	data[19] = 0x05;
	data[20] = 0x02;
	data[21] = 0x02;
	data[22] = 0x02;
	data[23] = 0x02;
	data[24] = 0x02;
	data[25] = 0x02;
	data[26] = 0x02;
	data[27] = 0x02;
	data[28] = 0x02;
	data[29] = 0x03;
	data[30] = 0x02;
	data[31] = 0x01;
	data[32] = 0x01;
	data[33] = 0x01;
	data[34] = 0x02;
	data[35] = 0x02;
	data[36] = 0x03;
	data[37] = 0x04;
	data[38] = 0x04;
	data[39] = 0x04;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x7F, data,
				    40) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x16) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x00;
	data[1] = 0x71;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x10, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x23,
				   0x89) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0xFF;
	data[1] = 0x00;
	data[2] = 0x00;
	data[3] = 0x00;
	data[4] = 0x00;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x27, data,
				    5) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x00;
	data[3] = 0x00;
	data[4] = 0x00;
	data[5] = 0x01;
	data[6] = 0x00;
	data[7] = 0x01;
	data[8] = 0x00;
	data[9] = 0x02;
	data[10] = 0x00;
	data[11] = 0x63;
	data[12] = 0x00;
	data[13] = 0x00;
	data[14] = 0x00;
	data[15] = 0x03;
	data[16] = 0x00;
	data[17] = 0x04;
	data[18] = 0x00;
	data[19] = 0x04;
	data[20] = 0x00;
	data[21] = 0x06;
	data[22] = 0x00;
	data[23] = 0x06;
	data[24] = 0x00;
	data[25] = 0x08;
	data[26] = 0x00;
	data[27] = 0x09;
	data[28] = 0x00;
	data[29] = 0x0B;
	data[30] = 0x00;
	data[31] = 0x0B;
	data[32] = 0x00;
	data[33] = 0x0D;
	data[34] = 0x00;
	data[35] = 0x0D;
	data[36] = 0x00;
	data[37] = 0x0F;
	data[38] = 0x00;
	data[39] = 0x0F;
	data[40] = 0x00;
	data[41] = 0x0F;
	data[42] = 0x00;
	data[43] = 0x10;
	data[44] = 0x00;
	data[45] = 0x79;
	data[46] = 0x00;
	data[47] = 0x00;
	data[48] = 0x00;
	data[49] = 0x02;
	data[50] = 0x00;
	data[51] = 0x00;
	data[52] = 0x00;
	data[53] = 0x03;
	data[54] = 0x00;
	data[55] = 0x01;
	data[56] = 0x00;
	data[57] = 0x03;
	data[58] = 0x00;
	data[59] = 0x03;
	data[60] = 0x00;
	data[61] = 0x03;
	data[62] = 0x00;
	data[63] = 0x04;
	data[64] = 0x00;
	data[65] = 0x04;
	data[66] = 0x00;
	data[67] = 0x06;
	data[68] = 0x00;
	data[69] = 0x05;
	data[70] = 0x00;
	data[71] = 0x07;
	data[72] = 0x00;
	data[73] = 0x07;
	data[74] = 0x00;
	data[75] = 0x08;
	data[76] = 0x00;
	data[77] = 0x0A;
	data[78] = 0x03;
	data[79] = 0xE0;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x3A, data,
				    80) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	data[0] = 0x03;
	data[1] = 0xE0;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0xBC, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x51,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xC5,
				   0x07) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x11) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x70,
				   0xE9) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x76,
				   0x0A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x78,
				   0x32) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x7A,
				   0x46) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x7C,
				   0x86) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x7E,
				   0xA4) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xE1,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->create_param.stationary_use) {
		data[0] = 0x06;
		data[1] = 0x07;
		data[2] = 0x1A;
	} else {
		data[0] = 0x00;
		data[1] = 0x08;
		data[2] = 0x19;
	}
	data[3] = 0x0E;
	data[4] = 0x09;
	data[5] = 0x0E;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x12) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	for (addr = 0x10; addr < 0x9F; addr += 6) {
		if (tnr_dmd->lna_thrs_tbl_air) {
			u8 idx = 0;

			idx = (addr - 0x10) / 6;
			data[0] =
			    tnr_dmd->lna_thrs_tbl_air->thrs[idx].off_on;
			data[1] =
			    tnr_dmd->lna_thrs_tbl_air->thrs[idx].on_off;
		}
		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_SYS, addr,
					    data,
					    6) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	data[0] = 0x00;
	data[1] = 0x08;
	if (tnr_dmd->create_param.stationary_use)
		data[2] = 0x1A;
	else
		data[2] = 0x19;
	data[3] = 0x0E;
	data[4] = 0x09;
	data[5] = 0x0E;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x13) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	for (addr = 0x10; addr < 0xCF; addr += 6) {
		if (tnr_dmd->lna_thrs_tbl_cable) {
			u8 idx = 0;

			idx = (addr - 0x10) / 6;
			data[0] =
			    tnr_dmd->lna_thrs_tbl_cable->thrs[idx].off_on;
			data[1] =
			    tnr_dmd->lna_thrs_tbl_cable->thrs[idx].on_off;
		}
		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_SYS, addr,
					    data,
					    6) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x11) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x08;
	data[1] = 0x09;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0xBD, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x08;
	data[1] = 0x09;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0xC4, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x20;
	data[1] = 0x20;
	data[2] = 0x30;
	data[3] = 0x41;
	data[4] = 0x50;
	data[5] = 0x5F;
	data[6] = 0x6F;
	data[7] = 0x80;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0xC9, data,
				    8) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x14) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x15;
	data[1] = 0x18;
	data[2] = 0x00;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x10, data,
				    3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x15,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x16) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x00;
	data[1] = 0x09;
	data[2] = 0x00;
	data[3] = 0x08;
	data[4] = 0x00;
	data[5] = 0x07;
	data[6] = 0x00;
	data[7] = 0x06;
	data[8] = 0x00;
	data[9] = 0x05;
	data[10] = 0x00;
	data[11] = 0x03;
	data[12] = 0x00;
	data[13] = 0x02;
	data[14] = 0x00;
	data[15] = 0x00;
	data[16] = 0x00;
	data[17] = 0x78;
	data[18] = 0x00;
	data[19] = 0x00;
	data[20] = 0x00;
	data[21] = 0x06;
	data[22] = 0x00;
	data[23] = 0x08;
	data[24] = 0x00;
	data[25] = 0x08;
	data[26] = 0x00;
	data[27] = 0x0C;
	data[28] = 0x00;
	data[29] = 0x0C;
	data[30] = 0x00;
	data[31] = 0x0D;
	data[32] = 0x00;
	data[33] = 0x0F;
	data[34] = 0x00;
	data[35] = 0x0E;
	data[36] = 0x00;
	data[37] = 0x0E;
	data[38] = 0x00;
	data[39] = 0x10;
	data[40] = 0x00;
	data[41] = 0x0F;
	data[42] = 0x00;
	data[43] = 0x0E;
	data[44] = 0x00;
	data[45] = 0x10;
	data[46] = 0x00;
	data[47] = 0x0F;
	data[48] = 0x00;
	data[49] = 0x0E;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x12, data,
				    50) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	CXD2880_SLEEP(1);

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x0A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x10, data,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if ((data[0] & 0x01) == 0x00)
		return CXD2880_RESULT_ERROR_HW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x25,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	CXD2880_SLEEP(1);

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x0A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x11, data,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if ((data[0] & 0x01) == 0x00)
		return CXD2880_RESULT_ERROR_HW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x02,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x21,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0xE1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x8F,
				   0x16) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x67,
				   0x60) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x6A,
				   0x0F) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x6C,
				   0x17) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x00;
	data[1] = 0xFE;
	data[2] = 0xEE;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_DMD, 0x6E, data,
				    3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0xA1;
	data[1] = 0x8B;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_DMD, 0x8D, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x08;
	data[1] = 0x09;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_DMD, 0x77, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->create_param.stationary_use) {
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x80,
					   0xAA) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0xE2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x41,
				   0xA0) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x4B,
				   0x68) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x21,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x10,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x25,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	CXD2880_SLEEP(1);

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x1A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x10, data,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if ((data[0] & 0x01) == 0x00)
		return CXD2880_RESULT_ERROR_HW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x14,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x26,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret rf_init2(struct cxd2880_tnrdmd *tnr_dmd)
{
	u8 data[5] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x40;
	data[1] = 0x40;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0xEA, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	CXD2880_SLEEP(1);

	data[0] = 0x00;
	if (tnr_dmd->chip_id == CXD2880_TNRDMD_CHIP_ID_CXD2880_ES1_0X)
		data[1] = 0x00;
	else
		data[1] = 0x01;
	data[2] = 0x01;
	data[3] = 0x03;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x30, data,
				    4) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x14) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x1B,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x21,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0xE1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xD3,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x21,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret x_tune1(struct cxd2880_tnrdmd *tnr_dmd,
				enum cxd2880_dtv_sys sys, u32 freq_khz,
				enum cxd2880_dtv_bandwidth bandwidth,
				u8 is_cable, int shift_frequency_khz)
{
	u8 data[11] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x10,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x0E;
	data[3] = 0x00;
	data[4] = 0x03;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0xE7, data,
				    5) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	data[0] = 0x1F;
	data[1] = 0x80;
	data[2] = 0x18;
	data[3] = 0x00;
	data[4] = 0x07;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0xE7, data,
				    5) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	CXD2880_SLEEP(1);

	data[0] = 0x72;
	data[1] = 0x81;
	data[3] = 0x1D;
	data[4] = 0x6F;
	data[5] = 0x7E;
	data[7] = 0x1C;
	switch (sys) {
	case CXD2880_DTV_SYS_DVBT:
	case CXD2880_DTV_SYS_ISDBT:
	case CXD2880_DTV_SYS_ISDBTSB:
	case CXD2880_DTV_SYS_ISDBTMM_A:
	case CXD2880_DTV_SYS_ISDBTMM_B:
		data[2] = 0x94;
		data[6] = 0x91;
		break;
	case CXD2880_DTV_SYS_DVBT2:
		data[2] = 0x96;
		data[6] = 0x93;
		break;
	default:
		return CXD2880_RESULT_ERROR_ARG;
	}
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x44, data,
				    8) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x62,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x15) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x03;
	data[1] = 0xE2;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x1E, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	data[0] = (u8)(is_cable ? 0x01 : 0x00);
	data[1] = 0x00;
	data[2] = 0x6B;
	data[3] = 0x4D;

	switch (bandwidth) {
	case CXD2880_DTV_BW_1_7_MHZ:
		data[4] = 0x03;
		break;
	case CXD2880_DTV_BW_5_MHZ:
	case CXD2880_DTV_BW_6_MHZ:
		data[4] = 0x00;
		break;
	case CXD2880_DTV_BW_7_MHZ:
		data[4] = 0x01;
		break;
	case CXD2880_DTV_BW_8_MHZ:
		data[4] = 0x02;
		break;
	default:
		return CXD2880_RESULT_ERROR_ARG;
	}

	data[5] = 0x00;

	freq_khz += shift_frequency_khz;

	data[6] = (u8)((freq_khz >> 16) & 0x0F);
	data[7] = (u8)((freq_khz >> 8) & 0xFF);
	data[8] = (u8)(freq_khz & 0xFF);
	data[9] = 0xFF;
	data[10] = 0xFE;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x52, data,
				    11) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret x_tune2(struct cxd2880_tnrdmd *tnr_dmd,
				enum cxd2880_dtv_bandwidth bandwidth,
				enum cxd2880_tnrdmd_clockmode clk_mode,
				int shift_frequency_khz)
{
	u8 data[3] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x11) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	data[0] = 0x01;
	data[1] = 0x0E;
	data[2] = 0x01;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x2D, data,
				    3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x1A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x29,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x2C, data,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x60,
				   data[0]) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x62,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x11) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x2D,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x2F,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x10,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x21,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (shift_frequency_khz != 0) {
		int shift_freq = 0;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0xE1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x60, data,
					   2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		shift_freq = shift_frequency_khz * 1000;

		switch (clk_mode) {
		case CXD2880_TNRDMD_CLOCKMODE_A:
		case CXD2880_TNRDMD_CLOCKMODE_C:
		default:
			if (shift_freq >= 0)
				shift_freq = (shift_freq + 183 / 2) / 183;
			else
				shift_freq = (shift_freq - 183 / 2) / 183;
			break;
		case CXD2880_TNRDMD_CLOCKMODE_B:
			if (shift_freq >= 0)
				shift_freq = (shift_freq + 178 / 2) / 178;
			else
				shift_freq = (shift_freq - 178 / 2) / 178;
			break;
		}

		shift_freq +=
		    cxd2880_convert2s_complement((data[0] << 8) | data[1], 16);

		if (shift_freq > 32767)
			shift_freq = 32767;
		else if (shift_freq < -32768)
			shift_freq = -32768;

		data[0] = (u8)(((u32)shift_freq >> 8) & 0xFF);
		data[1] = (u8)((u32)shift_freq & 0xFF);

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x60, data,
					    2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x69, data,
					   1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		shift_freq = -shift_frequency_khz;

		if (bandwidth == CXD2880_DTV_BW_1_7_MHZ) {
			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
			case CXD2880_TNRDMD_CLOCKMODE_C:
			default:
				if (shift_freq >= 0)
					shift_freq =
					    (shift_freq * 1000 +
					     17578 / 2) / 17578;
				else
					shift_freq =
					    (shift_freq * 1000 -
					     17578 / 2) / 17578;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				if (shift_freq >= 0)
					shift_freq =
					    (shift_freq * 1000 +
					     17090 / 2) / 17090;
				else
					shift_freq =
					    (shift_freq * 1000 -
					     17090 / 2) / 17090;
				break;
			}
		} else {
			switch (clk_mode) {
			case CXD2880_TNRDMD_CLOCKMODE_A:
			case CXD2880_TNRDMD_CLOCKMODE_C:
			default:
				if (shift_freq >= 0)
					shift_freq =
					    (shift_freq * 1000 +
					     35156 / 2) / 35156;
				else
					shift_freq =
					    (shift_freq * 1000 -
					     35156 / 2) / 35156;
				break;
			case CXD2880_TNRDMD_CLOCKMODE_B:
				if (shift_freq >= 0)
					shift_freq =
					    (shift_freq * 1000 +
					     34180 / 2) / 34180;
				else
					shift_freq =
					    (shift_freq * 1000 -
					     34180 / 2) / 34180;
				break;
			}
		}

		shift_freq += cxd2880_convert2s_complement(data[0], 8);

		if (shift_freq > 127)
			shift_freq = 127;
		else if (shift_freq < -128)
			shift_freq = -128;

		data[0] = (u8)((u32)shift_freq & 0xFF);

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x69,
					   data[0]) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->create_param.stationary_use) {
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0xE1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x8A,
					   0x87) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x21,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret x_tune3(struct cxd2880_tnrdmd *tnr_dmd,
				enum cxd2880_dtv_sys sys,
				u8 en_fef_intmtnt_ctrl)
{
	u8 data[6] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x21,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0xE2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x41,
				   0xA0) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x21,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xFE,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if ((sys == CXD2880_DTV_SYS_DVBT2) && en_fef_intmtnt_ctrl) {
		data[0] = 0x01;
		data[1] = 0x01;
		data[2] = 0x01;
		data[3] = 0x01;
		data[4] = 0x01;
		data[5] = 0x01;
	} else {
		data[0] = 0x00;
		data[1] = 0x00;
		data[2] = 0x00;
		data[3] = 0x00;
		data[4] = 0x00;
		data[5] = 0x00;
	}
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0xEF, data,
				    6) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x2D) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if ((sys == CXD2880_DTV_SYS_DVBT2) && en_fef_intmtnt_ctrl)
		data[0] = 0x00;
	else
		data[0] = 0x01;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xB1,
				   data[0]) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret x_tune4(struct cxd2880_tnrdmd *tnr_dmd)
{
	u8 data[2] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	{
		if (tnr_dmd->diver_sub->io->write_reg(tnr_dmd->diver_sub->io,
						      CXD2880_IO_TGT_SYS, 0x00,
						      0x00) !=
		    CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		data[0] = 0x14;
		data[1] = 0x00;
		if (tnr_dmd->diver_sub->io->write_regs(tnr_dmd->diver_sub->io,
						       CXD2880_IO_TGT_SYS, 0x55,
						       data,
						       2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	{
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x00,
					   0x00) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		data[0] = 0x0B;
		data[1] = 0xFF;
		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_SYS, 0x53, data,
					    2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x57,
					   0x01) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		data[0] = 0x0B;
		data[1] = 0xFF;
		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_SYS, 0x55, data,
					    2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	{
		if (tnr_dmd->diver_sub->io->write_reg(tnr_dmd->diver_sub->io,
						      CXD2880_IO_TGT_SYS, 0x00,
						      0x00) !=
		    CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		data[0] = 0x14;
		data[1] = 0x00;
		if (tnr_dmd->diver_sub->io->write_regs(tnr_dmd->diver_sub->io,
						       CXD2880_IO_TGT_SYS, 0x53,
						       data,
						       2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		if (tnr_dmd->diver_sub->io->write_reg(tnr_dmd->diver_sub->io,
						      CXD2880_IO_TGT_SYS, 0x57,
						      0x02) !=
		    CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xFE,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->diver_sub->io->write_reg(tnr_dmd->diver_sub->io,
					      CXD2880_IO_TGT_DMD, 0x00,
					      0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->diver_sub->io->write_reg(tnr_dmd->diver_sub->io,
					      CXD2880_IO_TGT_DMD, 0xFE,
					      0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret x_sleep1(struct cxd2880_tnrdmd *tnr_dmd)
{
	u8 data[3] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	{
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x00,
					   0x00) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x57,
					   0x03) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		data[0] = 0x00;
		data[1] = 0x00;
		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_SYS, 0x53, data,
					    2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	{
		if (tnr_dmd->diver_sub->io->write_reg(tnr_dmd->diver_sub->io,
						      CXD2880_IO_TGT_SYS, 0x00,
						      0x00) !=
		    CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		data[0] = 0x1F;
		data[1] = 0xFF;
		data[2] = 0x03;
		if (tnr_dmd->diver_sub->io->write_regs(tnr_dmd->diver_sub->io,
						       CXD2880_IO_TGT_SYS, 0x55,
						       data,
						       3) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		data[0] = 0x00;
		data[1] = 0x00;
		if (tnr_dmd->diver_sub->io->write_regs(tnr_dmd->diver_sub->io,
						       CXD2880_IO_TGT_SYS, 0x53,
						       data,
						       2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	{
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x00,
					   0x00) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		data[0] = 0x1F;
		data[1] = 0xFF;
		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_SYS, 0x55, data,
					    2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret x_sleep2(struct cxd2880_tnrdmd *tnr_dmd)
{
	u8 data = 0;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x2D) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xB1,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	CXD2880_SLEEP(1);

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xB2, &data,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if ((data & 0x01) == 0x00)
		return CXD2880_RESULT_ERROR_HW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xF4,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xF3,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xF2,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xF1,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xF0,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xEF,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret x_sleep3(struct cxd2880_tnrdmd *tnr_dmd)
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xFD,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret x_sleep4(struct cxd2880_tnrdmd *tnr_dmd)
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x21,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0xE2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x41,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x21,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret spll_reset(struct cxd2880_tnrdmd *tnr_dmd,
				   enum cxd2880_tnrdmd_clockmode clockmode)
{
	u8 data[4] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x29,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x28,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x27,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x26,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x10,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x27,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x22,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	switch (clockmode) {
	case CXD2880_TNRDMD_CLOCKMODE_A:
		data[0] = 0x00;
		break;

	case CXD2880_TNRDMD_CLOCKMODE_B:
		data[0] = 0x01;
		break;

	case CXD2880_TNRDMD_CLOCKMODE_C:
		data[0] = 0x02;
		break;

	default:
		return CXD2880_RESULT_ERROR_ARG;
	}
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x30,
				   data[0]) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x22,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	CXD2880_SLEEP(2);

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x0A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x10, data,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if ((data[0] & 0x01) == 0x00)
		return CXD2880_RESULT_ERROR_HW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x27,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	CXD2880_SLEEP(1);

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x10,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x00;
	data[3] = 0x00;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x26, data,
				    4) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret t_power_x(struct cxd2880_tnrdmd *tnr_dmd, u8 on)
{
	u8 data[3] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x29,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x28,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x27,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x10,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x27,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x25,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (on) {
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x2B,
					   0x01) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		CXD2880_SLEEP(1);

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x00,
					   0x0A) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x12, data,
					   1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		if ((data[0] & 0x01) == 0)
			return CXD2880_RESULT_ERROR_HW_STATE;
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x00,
					   0x00) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x2A,
					   0x00) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	} else {
		data[0] = 0x03;
		data[1] = 0x00;
		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_SYS, 0x2A, data,
					    2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		CXD2880_SLEEP(1);

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x00,
					   0x0A) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x13, data,
					   1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
		if ((data[0] & 0x01) == 0)
			return CXD2880_RESULT_ERROR_HW_STATE;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x25,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	CXD2880_SLEEP(1);

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x0A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x11, data,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if ((data[0] & 0x01) == 0)
		return CXD2880_RESULT_ERROR_HW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x27,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	CXD2880_SLEEP(1);

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x10,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = 0x00;
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x27, data,
				    3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

struct cxd2880_tnrdmd_ts_clk_cfg {
	u8 srl_clk_mode;
	u8 srl_duty_mode;
	u8 ts_clk_period;
};

static enum cxd2880_ret set_ts_clk_mode_and_freq(struct cxd2880_tnrdmd *tnr_dmd,
						 enum cxd2880_dtv_sys sys)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	u8 backwards_compatible = 0;
	struct cxd2880_tnrdmd_ts_clk_cfg ts_clk_cfg;

	const struct cxd2880_tnrdmd_ts_clk_cfg srl_ts_clk_stgs[2][2] = {
	{
		{3, 1, 8,},
		{0, 2, 16,}
	},
	{
		{1, 1, 8,},
		{2, 2, 16,}
	}
	};

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	{
		u8 ts_rate_ctrl_off = 0;
		u8 ts_in_off = 0;
		u8 ts_clk_manaul_on = 0;

		if ((sys == CXD2880_DTV_SYS_ISDBT) ||
		    (sys == CXD2880_DTV_SYS_ISDBTSB) ||
		    (sys == CXD2880_DTV_SYS_ISDBTMM_A) ||
		    (sys == CXD2880_DTV_SYS_ISDBTMM_B)) {
			backwards_compatible = 0;
			ts_rate_ctrl_off = 1;
			ts_in_off = 0;
		} else if (tnr_dmd->is_ts_backwards_compatible_mode) {
			backwards_compatible = 1;
			ts_rate_ctrl_off = 1;
			ts_in_off = 1;
		} else {
			backwards_compatible = 0;
			ts_rate_ctrl_off = 0;
			ts_in_off = 0;
		}

		if (tnr_dmd->ts_byte_clk_manual_setting) {
			ts_clk_manaul_on = 1;
			ts_rate_ctrl_off = 0;
		}

		ret =
		    cxd2880_io_set_reg_bits(tnr_dmd->io, CXD2880_IO_TGT_DMD,
					    0xD3, ts_rate_ctrl_off, 0x01);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		ret =
		    cxd2880_io_set_reg_bits(tnr_dmd->io, CXD2880_IO_TGT_DMD,
					    0xDE, ts_in_off, 0x01);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		ret =
		    cxd2880_io_set_reg_bits(tnr_dmd->io, CXD2880_IO_TGT_DMD,
					    0xDA, ts_clk_manaul_on, 0x01);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	ts_clk_cfg =
	    srl_ts_clk_stgs[tnr_dmd->srl_ts_clk_mod_cnts]
			[(u8)tnr_dmd->srl_ts_clk_frq];

	if (tnr_dmd->ts_byte_clk_manual_setting)
		ts_clk_cfg.ts_clk_period = tnr_dmd->ts_byte_clk_manual_setting;

	ret =
	    cxd2880_io_set_reg_bits(tnr_dmd->io, CXD2880_IO_TGT_DMD, 0xC4,
				    ts_clk_cfg.srl_clk_mode, 0x03);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret =
	    cxd2880_io_set_reg_bits(tnr_dmd->io, CXD2880_IO_TGT_DMD, 0xD1,
				    ts_clk_cfg.srl_duty_mode, 0x03);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = tnr_dmd->io->write_reg(tnr_dmd->io,
				     CXD2880_IO_TGT_DMD, 0xD9,
				     ts_clk_cfg.ts_clk_period);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	{
		u8 data = (u8)(backwards_compatible ? 0x00 : 0x01);

		if (sys == CXD2880_DTV_SYS_DVBT) {
			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x00,
						   0x10) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;

			ret =
			    cxd2880_io_set_reg_bits(tnr_dmd->io,
						    CXD2880_IO_TGT_DMD, 0x66,
						    data, 0x01);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		}
	}

	return ret;
}

static enum cxd2880_ret pid_ftr_setting(struct cxd2880_tnrdmd *tnr_dmd,
					struct cxd2880_tnrdmd_pid_ftr_cfg
					*pid_ftr_cfg)
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (!pid_ftr_cfg) {
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x50,
					   0x02) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	} else {
		u8 data[65];

		data[0] = (u8)(pid_ftr_cfg->is_negative ? 0x01 : 0x00);
		{
			int i = 0;

			for (i = 0; i < 32; i++) {
				if (pid_ftr_cfg->pid_cfg[i].is_en) {
					data[1 + (i * 2)] =
					    (u8)((u8)
					    (pid_ftr_cfg->pid_cfg[i].pid
					     >> 8) | 0x20);
					data[2 + (i * 2)] =
					    (u8)(pid_ftr_cfg->pid_cfg[i].pid
					     & 0xFF);
				} else {
					data[1 + (i * 2)] = 0x00;
					data[2 + (i * 2)] = 0x00;
				}
			}
		}
		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_DMD, 0x50, data,
					    65) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret load_cfg_mem(struct cxd2880_tnrdmd *tnr_dmd)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	u8 i;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	for (i = 0; i < tnr_dmd->cfg_mem_last_entry; i++) {
		ret = tnr_dmd->io->write_reg(tnr_dmd->io,
					     tnr_dmd->cfg_mem[i].tgt,
					     0x00, tnr_dmd->cfg_mem[i].bank);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		ret = cxd2880_io_set_reg_bits(tnr_dmd->io,
					      tnr_dmd->cfg_mem[i].tgt,
					      tnr_dmd->cfg_mem[i].address,
					      tnr_dmd->cfg_mem[i].value,
					      tnr_dmd->cfg_mem[i].bit_mask);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return ret;
}

static enum cxd2880_ret set_cfg_mem(struct cxd2880_tnrdmd *tnr_dmd,
				    enum cxd2880_io_tgt tgt,
				    u8 bank, u8 address, u8 value, u8 bit_mask)
{
	u8 i;
	u8 value_stored = 0;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	for (i = 0; i < tnr_dmd->cfg_mem_last_entry; i++) {
		if ((value_stored == 0) &&
		    (tnr_dmd->cfg_mem[i].tgt == tgt) &&
		    (tnr_dmd->cfg_mem[i].bank == bank) &&
		    (tnr_dmd->cfg_mem[i].address == address)) {
			tnr_dmd->cfg_mem[i].value &= ~bit_mask;
			tnr_dmd->cfg_mem[i].value |= (value & bit_mask);

			tnr_dmd->cfg_mem[i].bit_mask |= bit_mask;

			value_stored = 1;
		}
	}

	if (value_stored == 0) {
		if (tnr_dmd->cfg_mem_last_entry <
		    CXD2880_TNRDMD_MAX_CFG_MEM_COUNT) {
			tnr_dmd->cfg_mem[tnr_dmd->cfg_mem_last_entry].tgt = tgt;
			tnr_dmd->cfg_mem[tnr_dmd->cfg_mem_last_entry].bank =
			    bank;
			tnr_dmd->cfg_mem[tnr_dmd->cfg_mem_last_entry].address =
			    address;
			tnr_dmd->cfg_mem[tnr_dmd->cfg_mem_last_entry].value =
			    (value & bit_mask);
			tnr_dmd->cfg_mem[tnr_dmd->cfg_mem_last_entry].bit_mask =
			    bit_mask;
			tnr_dmd->cfg_mem_last_entry++;
		} else {
			return CXD2880_RESULT_ERROR_OVERFLOW;
		}
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_create(struct cxd2880_tnrdmd *tnr_dmd,
				       struct cxd2880_io *io,
				       struct cxd2880_tnrdmd_create_param
				       *create_param)
{
	if ((!tnr_dmd) || (!io) || (!create_param))
		return CXD2880_RESULT_ERROR_ARG;

	cxd2880_memset(tnr_dmd, 0, sizeof(struct cxd2880_tnrdmd));

	tnr_dmd->io = io;
	tnr_dmd->create_param = *create_param;

	tnr_dmd->diver_mode = CXD2880_TNRDMD_DIVERMODE_SINGLE;
	tnr_dmd->diver_sub = NULL;

	tnr_dmd->srl_ts_clk_mod_cnts = 1;
	tnr_dmd->en_fef_intmtnt_base = 1;
	tnr_dmd->en_fef_intmtnt_lite = 1;
	tnr_dmd->rf_lvl_cmpstn = NULL;
	tnr_dmd->lna_thrs_tbl_air = NULL;
	tnr_dmd->lna_thrs_tbl_cable = NULL;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_diver_create(struct cxd2880_tnrdmd
					     *tnr_dmd_main,
					     struct cxd2880_io *io_main,
					     struct cxd2880_tnrdmd *tnr_dmd_sub,
					     struct cxd2880_io *io_sub,
					     struct
					     cxd2880_tnrdmd_diver_create_param
					     *create_param)
{
	if ((!tnr_dmd_main) || (!io_main) || (!tnr_dmd_sub) || (!io_sub) ||
	    (!create_param))
		return CXD2880_RESULT_ERROR_ARG;

	cxd2880_memset(tnr_dmd_main, 0, sizeof(struct cxd2880_tnrdmd));
	cxd2880_memset(tnr_dmd_sub, 0, sizeof(struct cxd2880_tnrdmd));

	tnr_dmd_main->io = io_main;
	tnr_dmd_main->diver_mode = CXD2880_TNRDMD_DIVERMODE_MAIN;
	tnr_dmd_main->diver_sub = tnr_dmd_sub;
	tnr_dmd_main->create_param.en_internal_ldo =
	    create_param->en_internal_ldo;
	tnr_dmd_main->create_param.ts_output_if = create_param->ts_output_if;
	tnr_dmd_main->create_param.xtal_share_type =
	    CXD2880_TNRDMD_XTAL_SHARE_MASTER;
	tnr_dmd_main->create_param.xosc_cap = create_param->xosc_cap_main;
	tnr_dmd_main->create_param.xosc_i = create_param->xosc_i_main;
	tnr_dmd_main->create_param.is_cxd2881gg = create_param->is_cxd2881gg;
	tnr_dmd_main->create_param.stationary_use =
	    create_param->stationary_use;

	tnr_dmd_sub->io = io_sub;
	tnr_dmd_sub->diver_mode = CXD2880_TNRDMD_DIVERMODE_SUB;
	tnr_dmd_sub->diver_sub = NULL;
	tnr_dmd_sub->create_param.en_internal_ldo =
	    create_param->en_internal_ldo;
	tnr_dmd_sub->create_param.ts_output_if = create_param->ts_output_if;
	tnr_dmd_sub->create_param.xtal_share_type =
	    CXD2880_TNRDMD_XTAL_SHARE_SLAVE;
	tnr_dmd_sub->create_param.xosc_cap = 0;
	tnr_dmd_sub->create_param.xosc_i = create_param->xosc_i_sub;
	tnr_dmd_sub->create_param.is_cxd2881gg = create_param->is_cxd2881gg;
	tnr_dmd_sub->create_param.stationary_use = create_param->stationary_use;

	tnr_dmd_main->srl_ts_clk_mod_cnts = 1;
	tnr_dmd_main->en_fef_intmtnt_base = 1;
	tnr_dmd_main->en_fef_intmtnt_lite = 1;
	tnr_dmd_main->rf_lvl_cmpstn = NULL;
	tnr_dmd_main->lna_thrs_tbl_air = NULL;
	tnr_dmd_main->lna_thrs_tbl_cable = NULL;

	tnr_dmd_sub->srl_ts_clk_mod_cnts = 1;
	tnr_dmd_sub->en_fef_intmtnt_base = 1;
	tnr_dmd_sub->en_fef_intmtnt_lite = 1;
	tnr_dmd_sub->rf_lvl_cmpstn = NULL;
	tnr_dmd_sub->lna_thrs_tbl_air = NULL;
	tnr_dmd_sub->lna_thrs_tbl_cable = NULL;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_init1(struct cxd2880_tnrdmd *tnr_dmd)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB))
		return CXD2880_RESULT_ERROR_ARG;

	tnr_dmd->chip_id = CXD2880_TNRDMD_CHIP_ID_UNKNOWN;
	tnr_dmd->state = CXD2880_TNRDMD_STATE_UNKNOWN;
	tnr_dmd->clk_mode = CXD2880_TNRDMD_CLOCKMODE_UNKNOWN;
	tnr_dmd->frequency_khz = 0;
	tnr_dmd->sys = CXD2880_DTV_SYS_UNKNOWN;
	tnr_dmd->bandwidth = CXD2880_DTV_BW_UNKNOWN;
	tnr_dmd->scan_mode = 0;
	cxd2880_atomic_set(&tnr_dmd->cancel, 0);

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		tnr_dmd->diver_sub->chip_id = CXD2880_TNRDMD_CHIP_ID_UNKNOWN;
		tnr_dmd->diver_sub->state = CXD2880_TNRDMD_STATE_UNKNOWN;
		tnr_dmd->diver_sub->clk_mode = CXD2880_TNRDMD_CLOCKMODE_UNKNOWN;
		tnr_dmd->diver_sub->frequency_khz = 0;
		tnr_dmd->diver_sub->sys = CXD2880_DTV_SYS_UNKNOWN;
		tnr_dmd->diver_sub->bandwidth = CXD2880_DTV_BW_UNKNOWN;
		tnr_dmd->diver_sub->scan_mode = 0;
		cxd2880_atomic_set(&tnr_dmd->diver_sub->cancel, 0);
	}

	ret = cxd2880_tnrdmd_chip_id(tnr_dmd, &tnr_dmd->chip_id);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (!CXD2880_TNRDMD_CHIP_ID_VALID(tnr_dmd->chip_id))
		return CXD2880_RESULT_ERROR_NOSUPPORT;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret =
		    cxd2880_tnrdmd_chip_id(tnr_dmd->diver_sub,
					   &tnr_dmd->diver_sub->chip_id);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (!CXD2880_TNRDMD_CHIP_ID_VALID(tnr_dmd->diver_sub->chip_id))
			return CXD2880_RESULT_ERROR_NOSUPPORT;
	}

	ret = p_init1(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret = p_init1(tnr_dmd->diver_sub);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	CXD2880_SLEEP(1);

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret = p_init2(tnr_dmd->diver_sub);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	ret = p_init2(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	CXD2880_SLEEP(5);

	ret = p_init3(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret = p_init3(tnr_dmd->diver_sub);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	ret = rf_init1(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret = rf_init1(tnr_dmd->diver_sub);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_init2(struct cxd2880_tnrdmd *tnr_dmd)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	{
		u8 cpu_task_completed = 0;

		ret =
		    cxd2880_tnrdmd_check_internal_cpu_status(tnr_dmd,
						     &cpu_task_completed);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (!cpu_task_completed)
			return CXD2880_RESULT_ERROR_HW_STATE;
	}

	ret = rf_init2(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret = rf_init2(tnr_dmd->diver_sub);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	ret = load_cfg_mem(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret = load_cfg_mem(tnr_dmd->diver_sub);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	tnr_dmd->state = CXD2880_TNRDMD_STATE_SLEEP;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN)
		tnr_dmd->diver_sub->state = CXD2880_TNRDMD_STATE_SLEEP;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_check_internal_cpu_status(struct cxd2880_tnrdmd
							  *tnr_dmd,
							  u8 *task_completed)
{
	u16 cpu_status = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!task_completed))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	ret = cxd2880_tnrdmd_mon_internal_cpu_status(tnr_dmd, &cpu_status);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SINGLE) {
		if (cpu_status == 0)
			*task_completed = 1;
		else
			*task_completed = 0;

		return ret;
	}
	if (cpu_status != 0) {
		*task_completed = 0;
		return ret;
	}

	ret = cxd2880_tnrdmd_mon_internal_cpu_status_sub(tnr_dmd, &cpu_status);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (cpu_status == 0)
		*task_completed = 1;
	else
		*task_completed = 0;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_common_tune_setting1(struct cxd2880_tnrdmd
						     *tnr_dmd,
						     enum cxd2880_dtv_sys sys,
						     u32 frequency_khz,
						     enum cxd2880_dtv_bandwidth
						     bandwidth, u8 one_seg_opt,
						     u8 one_seg_opt_shft_dir)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (frequency_khz < 4000)
		return CXD2880_RESULT_ERROR_RANGE;

	ret = cxd2880_tnrdmd_sleep(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	{
		u8 data = 0;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x00,
					   0x00) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x2B, &data,
					   1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		switch (sys) {
		case CXD2880_DTV_SYS_DVBT:
		case CXD2880_DTV_SYS_ISDBT:
		case CXD2880_DTV_SYS_ISDBTSB:
		case CXD2880_DTV_SYS_ISDBTMM_A:
		case CXD2880_DTV_SYS_ISDBTMM_B:
			if (data == 0x00) {
				ret = t_power_x(tnr_dmd, 1);
				if (ret != CXD2880_RESULT_OK)
					return ret;

				if (tnr_dmd->diver_mode ==
				    CXD2880_TNRDMD_DIVERMODE_MAIN) {
					ret = t_power_x(tnr_dmd->diver_sub, 1);
					if (ret != CXD2880_RESULT_OK)
						return ret;
				}
			}
			break;

		case CXD2880_DTV_SYS_DVBT2:
			if (data == 0x01) {
				ret = t_power_x(tnr_dmd, 0);
				if (ret != CXD2880_RESULT_OK)
					return ret;

				if (tnr_dmd->diver_mode ==
				    CXD2880_TNRDMD_DIVERMODE_MAIN) {
					ret = t_power_x(tnr_dmd->diver_sub, 0);
					if (ret != CXD2880_RESULT_OK)
						return ret;
				}
			}
			break;

		default:
			return CXD2880_RESULT_ERROR_ARG;
		}
	}

	{
		enum cxd2880_tnrdmd_clockmode new_clk_mode =
		    CXD2880_TNRDMD_CLOCKMODE_A;

		ret = spll_reset(tnr_dmd, new_clk_mode);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		tnr_dmd->clk_mode = new_clk_mode;

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			ret = spll_reset(tnr_dmd->diver_sub, new_clk_mode);
			if (ret != CXD2880_RESULT_OK)
				return ret;

			tnr_dmd->diver_sub->clk_mode = new_clk_mode;
		}

		ret = load_cfg_mem(tnr_dmd);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			ret = load_cfg_mem(tnr_dmd->diver_sub);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		}
	}

	{
		int shift_frequency_khz = 0;

		if (one_seg_opt) {
			if (tnr_dmd->diver_mode ==
			    CXD2880_TNRDMD_DIVERMODE_MAIN) {
				shift_frequency_khz = 350;
			} else {
				if (one_seg_opt_shft_dir)
					shift_frequency_khz = 350;
				else
					shift_frequency_khz = -350;

				if (tnr_dmd->create_param.xtal_share_type ==
				    CXD2880_TNRDMD_XTAL_SHARE_SLAVE)
					shift_frequency_khz *= -1;
			}
		} else {
			if (tnr_dmd->diver_mode ==
			    CXD2880_TNRDMD_DIVERMODE_MAIN) {
				shift_frequency_khz = 150;
			} else {
				switch (tnr_dmd->create_param.xtal_share_type) {
				case CXD2880_TNRDMD_XTAL_SHARE_NONE:
				case CXD2880_TNRDMD_XTAL_SHARE_EXTREF:
				default:
					shift_frequency_khz = 0;
					break;
				case CXD2880_TNRDMD_XTAL_SHARE_MASTER:
					shift_frequency_khz = 150;
					break;
				case CXD2880_TNRDMD_XTAL_SHARE_SLAVE:
					shift_frequency_khz = -150;
					break;
				}
			}
		}

		ret =
		    x_tune1(tnr_dmd, sys, frequency_khz, bandwidth,
			    tnr_dmd->is_cable_input, shift_frequency_khz);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			ret =
			    x_tune1(tnr_dmd->diver_sub, sys, frequency_khz,
				    bandwidth, tnr_dmd->is_cable_input,
				    -shift_frequency_khz);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		}

		CXD2880_SLEEP(10);

		{
			u8 cpu_task_completed = 0;

			ret =
			    cxd2880_tnrdmd_check_internal_cpu_status(tnr_dmd,
						     &cpu_task_completed);
			if (ret != CXD2880_RESULT_OK)
				return ret;

			if (!cpu_task_completed)
				return CXD2880_RESULT_ERROR_HW_STATE;
		}

		ret =
		    x_tune2(tnr_dmd, bandwidth, tnr_dmd->clk_mode,
			    shift_frequency_khz);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			ret =
			    x_tune2(tnr_dmd->diver_sub, bandwidth,
				    tnr_dmd->diver_sub->clk_mode,
				    -shift_frequency_khz);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		}
	}

	if (tnr_dmd->create_param.ts_output_if == CXD2880_TNRDMD_TSOUT_IF_TS) {
		ret = set_ts_clk_mode_and_freq(tnr_dmd, sys);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	} else {
		struct cxd2880_tnrdmd_pid_ftr_cfg *pid_ftr_cfg;

		if (tnr_dmd->pid_ftr_cfg_en)
			pid_ftr_cfg = &tnr_dmd->pid_ftr_cfg;
		else
			pid_ftr_cfg = NULL;

		ret = pid_ftr_setting(tnr_dmd, pid_ftr_cfg);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_common_tune_setting2(struct cxd2880_tnrdmd
						     *tnr_dmd,
						     enum cxd2880_dtv_sys sys,
						     u8 en_fef_intmtnt_ctrl)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = x_tune3(tnr_dmd, sys, en_fef_intmtnt_ctrl);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret = x_tune3(tnr_dmd->diver_sub, sys, en_fef_intmtnt_ctrl);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		ret = x_tune4(tnr_dmd);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	ret = cxd2880_tnrdmd_set_ts_output(tnr_dmd, 1);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_sleep(struct cxd2880_tnrdmd *tnr_dmd)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state == CXD2880_TNRDMD_STATE_SLEEP) {
	} else if (tnr_dmd->state == CXD2880_TNRDMD_STATE_ACTIVE) {
		ret = cxd2880_tnrdmd_set_ts_output(tnr_dmd, 0);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			ret = x_sleep1(tnr_dmd);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		}

		ret = x_sleep2(tnr_dmd);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			ret = x_sleep2(tnr_dmd->diver_sub);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		}

		switch (tnr_dmd->sys) {
		case CXD2880_DTV_SYS_DVBT:
			ret = cxd2880_tnrdmd_dvbt_sleep_setting(tnr_dmd);
			if (ret != CXD2880_RESULT_OK)
				return ret;
			break;

		case CXD2880_DTV_SYS_DVBT2:
			ret = cxd2880_tnrdmd_dvbt2_sleep_setting(tnr_dmd);
			if (ret != CXD2880_RESULT_OK)
				return ret;
			break;

		default:
			return CXD2880_RESULT_ERROR_SW_STATE;
		}

		ret = x_sleep3(tnr_dmd);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			ret = x_sleep3(tnr_dmd->diver_sub);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		}

		ret = x_sleep4(tnr_dmd);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			ret = x_sleep4(tnr_dmd->diver_sub);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		}

		tnr_dmd->state = CXD2880_TNRDMD_STATE_SLEEP;
		tnr_dmd->frequency_khz = 0;
		tnr_dmd->sys = CXD2880_DTV_SYS_UNKNOWN;
		tnr_dmd->bandwidth = CXD2880_DTV_BW_UNKNOWN;

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
			tnr_dmd->diver_sub->state = CXD2880_TNRDMD_STATE_SLEEP;
			tnr_dmd->diver_sub->frequency_khz = 0;
			tnr_dmd->diver_sub->sys = CXD2880_DTV_SYS_UNKNOWN;
			tnr_dmd->diver_sub->bandwidth = CXD2880_DTV_BW_UNKNOWN;
		}
	} else {
		return CXD2880_RESULT_ERROR_SW_STATE;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_set_cfg(struct cxd2880_tnrdmd *tnr_dmd,
					enum cxd2880_tnrdmd_cfg_id id,
					int value)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	u8 data[2] = { 0 };
	u8 need_sub_setting = 0;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	switch (id) {
	case CXD2880_TNRDMD_CFG_OUTPUT_SEL_MSB:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0xC4,
							 (u8)(value ? 0x00 :
							       0x10), 0x10);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TSVALID_ACTIVE_HI:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0xC5,
							 (u8)(value ? 0x00 :
							       0x02), 0x02);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TSSYNC_ACTIVE_HI:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0xC5,
							 (u8)(value ? 0x00 :
							       0x04), 0x04);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TSERR_ACTIVE_HI:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0xCB,
							 (u8)(value ? 0x00 :
							       0x01), 0x01);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_LATCH_ON_POSEDGE:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0xC5,
							 (u8)(value ? 0x01 :
							       0x00), 0x01);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TSCLK_CONT:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		tnr_dmd->srl_ts_clk_mod_cnts = (u8)(value ? 0x01 : 0x00);
		break;

	case CXD2880_TNRDMD_CFG_TSCLK_MASK:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		if ((value < 0) || (value > 0x1F))
			return CXD2880_RESULT_ERROR_RANGE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0xC6, (u8)value,
							 0x1F);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TSVALID_MASK:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		if ((value < 0) || (value > 0x1F))
			return CXD2880_RESULT_ERROR_RANGE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0xC8, (u8)value,
							 0x1F);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TSERR_MASK:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		if ((value < 0) || (value > 0x1F))
			return CXD2880_RESULT_ERROR_RANGE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0xC9, (u8)value,
							 0x1F);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TSERR_VALID_DIS:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0x91,
							 (u8)(value ? 0x01 :
							       0x00), 0x01);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TSPIN_CURRENT:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_SYS,
							 0x00, 0x51, (u8)value,
							 0x3F);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TSPIN_PULLUP_MANUAL:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_SYS,
							 0x00, 0x50,
							 (u8)(value ? 0x80 :
							       0x00), 0x80);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TSPIN_PULLUP:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_SYS,
							 0x00, 0x50, (u8)value,
							 0x3F);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TSCLK_FREQ:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		if ((value < 0) || (value > 1))
			return CXD2880_RESULT_ERROR_RANGE;

		tnr_dmd->srl_ts_clk_frq =
		    (enum cxd2880_tnrdmd_serial_ts_clk)value;
		break;

	case CXD2880_TNRDMD_CFG_TSBYTECLK_MANUAL:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		if ((value < 0) || (value > 0xFF))
			return CXD2880_RESULT_ERROR_RANGE;

		tnr_dmd->ts_byte_clk_manual_setting = (u8)value;

		break;

	case CXD2880_TNRDMD_CFG_TS_PACKET_GAP:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		if ((value < 0) || (value > 7))
			return CXD2880_RESULT_ERROR_RANGE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0xD6, (u8)value,
							 0x07);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		break;

	case CXD2880_TNRDMD_CFG_TS_BACKWARDS_COMPATIBLE:
		if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
			return CXD2880_RESULT_ERROR_SW_STATE;

		tnr_dmd->is_ts_backwards_compatible_mode = (u8)(value ? 1 : 0);

		break;

	case CXD2880_TNRDMD_CFG_PWM_VALUE:
		if ((value < 0) || (value > 0x1000))
			return CXD2880_RESULT_ERROR_RANGE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0x22,
							 (u8)(value ? 0x01 :
							       0x00), 0x01);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		{
			u8 data[2];

			data[0] = (u8)(((u16)value >> 8) & 0x1F);
			data[1] = (u8)((u16)value & 0xFF);

			ret =
			    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0x23,
							 data[0], 0x1F);
			if (ret != CXD2880_RESULT_OK)
				return ret;

			ret =
			    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0x24,
							 data[1], 0xFF);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		}

		break;

	case CXD2880_TNRDMD_CFG_INTERRUPT:
		data[0] = (u8)((value >> 8) & 0xFF);
		data[1] = (u8)(value & 0xFF);
		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_SYS,
							 0x00, 0x48, data[0],
							 0xFF);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_SYS,
							 0x00, 0x49, data[1],
							 0xFF);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_INTERRUPT_LOCK_SEL:
		data[0] = (u8)(value & 0x07);
		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_SYS,
							 0x00, 0x4A, data[0],
							 0x07);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_INTERRUPT_INV_LOCK_SEL:
		data[0] = (u8)((value & 0x07) << 3);
		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_SYS,
							 0x00, 0x4A, data[0],
							 0x38);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_FIXED_CLOCKMODE:
		if ((value < (int)CXD2880_TNRDMD_CLOCKMODE_UNKNOWN) ||
		    (value > (int)CXD2880_TNRDMD_CLOCKMODE_C))
			return CXD2880_RESULT_ERROR_RANGE;
		tnr_dmd->fixed_clk_mode = (enum cxd2880_tnrdmd_clockmode)value;
		break;

	case CXD2880_TNRDMD_CFG_CABLE_INPUT:
		tnr_dmd->is_cable_input = (u8)(value ? 1 : 0);
		break;

	case CXD2880_TNRDMD_CFG_DVBT2_FEF_INTERMITTENT_BASE:
		tnr_dmd->en_fef_intmtnt_base = (u8)(value ? 1 : 0);
		break;

	case CXD2880_TNRDMD_CFG_DVBT2_FEF_INTERMITTENT_LITE:
		tnr_dmd->en_fef_intmtnt_lite = (u8)(value ? 1 : 0);
		break;

	case CXD2880_TNRDMD_CFG_TS_BUF_ALMOST_EMPTY_THRS:
		data[0] = (u8)((value >> 8) & 0x07);
		data[1] = (u8)(value & 0xFF);
		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0x99, data[0],
							 0x07);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0x9A, data[1],
							 0xFF);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TS_BUF_ALMOST_FULL_THRS:
		data[0] = (u8)((value >> 8) & 0x07);
		data[1] = (u8)(value & 0xFF);
		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0x9B, data[0],
							 0x07);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0x9C, data[1],
							 0xFF);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_TS_BUF_RRDY_THRS:
		data[0] = (u8)((value >> 8) & 0x07);
		data[1] = (u8)(value & 0xFF);
		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0x9D, data[0],
							 0x07);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x00, 0x9E, data[1],
							 0xFF);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_BLINDTUNE_DVBT2_FIRST:
		tnr_dmd->blind_tune_dvbt2_first = (u8)(value ? 1 : 0);
		break;

	case CXD2880_TNRDMD_CFG_DVBT_BERN_PERIOD:
		if ((value < 0) || (value > 31))
			return CXD2880_RESULT_ERROR_RANGE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x10, 0x60,
							 (u8)(value & 0x1F),
							 0x1F);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_DVBT_VBER_PERIOD:
		if ((value < 0) || (value > 7))
			return CXD2880_RESULT_ERROR_RANGE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x10, 0x6F,
							 (u8)(value & 0x07),
							 0x07);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_DVBT2_BBER_MES:
		if ((value < 0) || (value > 15))
			return CXD2880_RESULT_ERROR_RANGE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x20, 0x72,
							 (u8)(value & 0x0F),
							 0x0F);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_DVBT2_LBER_MES:
		if ((value < 0) || (value > 15))
			return CXD2880_RESULT_ERROR_RANGE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x20, 0x6F,
							 (u8)(value & 0x0F),
							 0x0F);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_DVBT_PER_MES:
		if ((value < 0) || (value > 15))
			return CXD2880_RESULT_ERROR_RANGE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x10, 0x5C,
							 (u8)(value & 0x0F),
							 0x0F);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_DVBT2_PER_MES:
		if ((value < 0) || (value > 15))
			return CXD2880_RESULT_ERROR_RANGE;

		ret =
		    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x24, 0xDC,
							 (u8)(value & 0x0F),
							 0x0F);
		if (ret != CXD2880_RESULT_OK)
			return ret;
		break;

	case CXD2880_TNRDMD_CFG_ISDBT_BERPER_PERIOD:
		{
			u8 data[2];

			data[0] = (u8)((value & 0x00007F00) >> 8);
			data[1] = (u8)(value & 0x000000FF);

			ret =
			    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x60, 0x5B,
							 data[0], 0x7F);
			if (ret != CXD2880_RESULT_OK)
				return ret;
			ret =
			    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd,
							 CXD2880_IO_TGT_DMD,
							 0x60, 0x5C,
							 data[1], 0xFF);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		}
		break;

	default:
		return CXD2880_RESULT_ERROR_ARG;
	}

	if (need_sub_setting &&
	    (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN)) {
		ret = cxd2880_tnrdmd_set_cfg(tnr_dmd->diver_sub, id, value);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_gpio_set_cfg(struct cxd2880_tnrdmd *tnr_dmd,
					     u8 id,
					     u8 en,
					     enum cxd2880_tnrdmd_gpio_mode mode,
					     u8 open_drain, u8 invert)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (id > 2)
		return CXD2880_RESULT_ERROR_ARG;

	if (mode > CXD2880_TNRDMD_GPIO_MODE_EEW)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret =
	    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd, CXD2880_IO_TGT_SYS,
						 0x00, 0x40 + id, (u8)mode,
						 0x0F);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret =
	    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd, CXD2880_IO_TGT_SYS,
						 0x00, 0x43,
						 (u8)(open_drain ? (1 << id) :
						       0), (u8)(1 << id));
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret =
	    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd, CXD2880_IO_TGT_SYS,
						 0x00, 0x44,
						 (u8)(invert ? (1 << id) : 0),
						 (u8)(1 << id));
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret =
	    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd, CXD2880_IO_TGT_SYS,
						 0x00, 0x45,
						 (u8)(en ? 0 : (1 << id)),
						 (u8)(1 << id));
	if (ret != CXD2880_RESULT_OK)
		return ret;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_gpio_set_cfg_sub(struct cxd2880_tnrdmd *tnr_dmd,
						 u8 id,
						 u8 en,
						 enum cxd2880_tnrdmd_gpio_mode
						 mode, u8 open_drain, u8 invert)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret =
	    cxd2880_tnrdmd_gpio_set_cfg(tnr_dmd->diver_sub, id, en, mode,
					open_drain, invert);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_gpio_read(struct cxd2880_tnrdmd *tnr_dmd,
					  u8 id, u8 *value)
{
	u8 data = 0;

	if ((!tnr_dmd) || (!value))
		return CXD2880_RESULT_ERROR_ARG;

	if (id > 2)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x0A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x20, &data,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	*value = (u8)((data >> id) & 0x01);

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_gpio_read_sub(struct cxd2880_tnrdmd *tnr_dmd,
					      u8 id, u8 *value)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret = cxd2880_tnrdmd_gpio_read(tnr_dmd->diver_sub, id, value);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_gpio_write(struct cxd2880_tnrdmd *tnr_dmd,
					   u8 id, u8 value)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (id > 2)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret =
	    cxd2880_tnrdmd_set_and_save_reg_bits(tnr_dmd, CXD2880_IO_TGT_SYS,
						 0x00, 0x46,
						 (u8)(value ? (1 << id) : 0),
						 (u8)(1 << id));
	if (ret != CXD2880_RESULT_OK)
		return ret;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_gpio_write_sub(struct cxd2880_tnrdmd *tnr_dmd,
					       u8 id, u8 value)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret = cxd2880_tnrdmd_gpio_write(tnr_dmd->diver_sub, id, value);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_interrupt_read(struct cxd2880_tnrdmd *tnr_dmd,
					       u16 *value)
{
	u8 data[2] = { 0 };

	if ((!tnr_dmd) || (!value))
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x0A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x15, data,
				   2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	*value = (u16)(((u16)data[0] << 8) | (data[1]));

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_interrupt_clear(struct cxd2880_tnrdmd *tnr_dmd,
						u16 value)
{
	u8 data[2] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	data[0] = (u8)((value >> 8) & 0xFF);
	data[1] = (u8)(value & 0xFF);
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_SYS, 0x3C, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_ts_buf_clear(struct cxd2880_tnrdmd *tnr_dmd,
					     u8 clear_overflow_flag,
					     u8 clear_underflow_flag,
					     u8 clear_buf)
{
	u8 data[2] = { 0 };

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	data[0] = (u8)(clear_overflow_flag ? 0x02 : 0x00);
	data[0] |= (u8)(clear_underflow_flag ? 0x01 : 0x00);
	data[1] = (u8)(clear_buf ? 0x01 : 0x00);
	if (tnr_dmd->io->write_regs(tnr_dmd->io,
				    CXD2880_IO_TGT_DMD, 0x9F, data,
				    2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_chip_id(struct cxd2880_tnrdmd *tnr_dmd,
					enum cxd2880_tnrdmd_chip_id *chip_id)
{
	u8 data = 0;

	if ((!tnr_dmd) || (!chip_id))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0xFD, &data,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	*chip_id = (enum cxd2880_tnrdmd_chip_id)data;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_set_and_save_reg_bits(struct cxd2880_tnrdmd
						      *tnr_dmd,
						      enum cxd2880_io_tgt tgt,
						      u8 bank, u8 address,
						      u8 value, u8 bit_mask)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   tgt, 0x00, bank) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (cxd2880_io_set_reg_bits(tnr_dmd->io, tgt, address, value, bit_mask)
	    != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	ret = set_cfg_mem(tnr_dmd, tgt, bank, address, value, bit_mask);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_set_scan_mode(struct cxd2880_tnrdmd *tnr_dmd,
					      enum cxd2880_dtv_sys sys,
					      u8 scan_mode_end)
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	CXD2880_ARG_UNUSED(sys);

	tnr_dmd->scan_mode = scan_mode_end;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN) {
		enum cxd2880_ret ret = CXD2880_RESULT_OK;

		ret =
		    cxd2880_tnrdmd_set_scan_mode(tnr_dmd->diver_sub, sys,
						 scan_mode_end);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_set_pid_ftr(struct cxd2880_tnrdmd *tnr_dmd,
					    struct cxd2880_tnrdmd_pid_ftr_cfg
					    *pid_ftr_cfg)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->create_param.ts_output_if == CXD2880_TNRDMD_TSOUT_IF_TS)
		return CXD2880_RESULT_ERROR_NOSUPPORT;

	if (pid_ftr_cfg) {
		tnr_dmd->pid_ftr_cfg = *pid_ftr_cfg;
		tnr_dmd->pid_ftr_cfg_en = 1;
	} else {
		tnr_dmd->pid_ftr_cfg_en = 0;
	}

	if (tnr_dmd->state == CXD2880_TNRDMD_STATE_ACTIVE) {
		ret = pid_ftr_setting(tnr_dmd, pid_ftr_cfg);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_set_rf_lvl_cmpstn(struct cxd2880_tnrdmd
					  *tnr_dmd,
					  enum
					  cxd2880_ret(*rf_lvl_cmpstn)
					  (struct cxd2880_tnrdmd *,
					   int *))
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	tnr_dmd->rf_lvl_cmpstn = rf_lvl_cmpstn;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_set_rf_lvl_cmpstn_sub(struct cxd2880_tnrdmd
						      *tnr_dmd,
						      enum
						      cxd2880_ret
						      (*rf_lvl_cmpstn)(struct
								cxd2880_tnrdmd
								*,
								int *))
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret =
	    cxd2880_tnrdmd_set_rf_lvl_cmpstn(tnr_dmd->diver_sub, rf_lvl_cmpstn);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_set_lna_thrs(struct cxd2880_tnrdmd *tnr_dmd,
					     struct
					     cxd2880_tnrdmd_lna_thrs_tbl_air
					     *tbl_air,
					     struct
					     cxd2880_tnrdmd_lna_thrs_tbl_cable
					     *tbl_cable)
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	tnr_dmd->lna_thrs_tbl_air = tbl_air;
	tnr_dmd->lna_thrs_tbl_cable = tbl_cable;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_set_lna_thrs_sub(struct cxd2880_tnrdmd *tnr_dmd,
					 struct
					 cxd2880_tnrdmd_lna_thrs_tbl_air
					 *tbl_air,
					 struct
					 cxd2880_tnrdmd_lna_thrs_tbl_cable
					 *tbl_cable)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret =
	    cxd2880_tnrdmd_set_lna_thrs(tnr_dmd->diver_sub, tbl_air, tbl_cable);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_set_ts_pin_high_low(struct cxd2880_tnrdmd
						    *tnr_dmd, u8 en, u8 value)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->create_param.ts_output_if != CXD2880_TNRDMD_TSOUT_IF_TS)
		return CXD2880_RESULT_ERROR_NOSUPPORT;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (en) {
		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x50,
					   ((value & 0x1F) | 0x80)) !=
		    CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x52,
					   (value & 0x1F)) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	} else {
		ret = tnr_dmd->io->write_reg(tnr_dmd->io,
					     CXD2880_IO_TGT_SYS, 0x50, 0x3F);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x52,
					   0x1F) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		ret = load_cfg_mem(tnr_dmd);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_set_ts_output(struct cxd2880_tnrdmd *tnr_dmd,
					      u8 en)
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	switch (tnr_dmd->create_param.ts_output_if) {
	case CXD2880_TNRDMD_TSOUT_IF_TS:
		if (en) {
			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_SYS, 0x00,
						   0x00) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_SYS, 0x52,
						   0x00) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x00,
						   0x00) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0xC3,
						   0x00) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		} else {
			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x00,
						   0x00) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0xC3,
						   0x01) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_SYS, 0x00,
						   0x00) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_SYS, 0x52,
						   0x1F) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}
		break;

	case CXD2880_TNRDMD_TSOUT_IF_SPI:
		break;

	case CXD2880_TNRDMD_TSOUT_IF_SDIO:
		break;

	default:
		return CXD2880_RESULT_ERROR_SW_STATE;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret slvt_freeze_reg(struct cxd2880_tnrdmd *tnr_dmd)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	switch (tnr_dmd->create_param.ts_output_if) {
	case CXD2880_TNRDMD_TSOUT_IF_SPI:
	case CXD2880_TNRDMD_TSOUT_IF_SDIO:
		{
			u8 data = 0;

			if (tnr_dmd->io->read_regs(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x00,
						   &data,
						   1) != CXD2880_RESULT_OK)
				return CXD2880_RESULT_ERROR_IO;
		}
		break;
	case CXD2880_TNRDMD_TSOUT_IF_TS:
	default:
		break;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x01,
				   0x01) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	return ret;
}
