/*
 * cxd2880_tnrdmd_mon.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * common monitor functions
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
#include "cxd2880_tnrdmd_mon.h"

enum cxd2880_ret cxd2880_tnrdmd_mon_rf_lvl(struct cxd2880_tnrdmd *tnr_dmd,
					   int *rf_lvl_db)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!rf_lvl_db))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

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

	{
		u8 data[2] = { 0x80, 0x00 };

		if (tnr_dmd->io->write_regs(tnr_dmd->io,
					    CXD2880_IO_TGT_SYS, 0x5B, data,
					    2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;
	}

	CXD2880_SLEEP_IN_MON(2, tnr_dmd);

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x1A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	{
		u8 data[2];

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x15, data,
					   2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if ((data[0] != 0) || (data[1] != 0))
			return CXD2880_RESULT_ERROR_OTHER;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_SYS, 0x11, data,
					   2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		*rf_lvl_db =
		    cxd2880_convert2s_complement((data[0] << 3) |
						 ((data[1] & 0xE0) >> 5), 11);
	}

	*rf_lvl_db *= 125;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x10,
				   0x00) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->rf_lvl_cmpstn) {
		ret = tnr_dmd->rf_lvl_cmpstn(tnr_dmd, rf_lvl_db);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_mon_rf_lvl_sub(struct cxd2880_tnrdmd *tnr_dmd,
					       int *rf_lvl_db)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!rf_lvl_db))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret = cxd2880_tnrdmd_mon_rf_lvl(tnr_dmd->diver_sub, rf_lvl_db);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_mon_internal_cpu_status(struct cxd2880_tnrdmd
							*tnr_dmd, u16 *status)
{
	u8 data[2] = { 0 };

	if ((!tnr_dmd) || (!status))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x00,
				   0x1A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_SYS, 0x15, data,
				   2) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	*status = (u16)(((u16)data[0] << 8) | data[1]);

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_mon_internal_cpu_status_sub(struct
							    cxd2880_tnrdmd
							    *tnr_dmd,
							    u16 *status)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!status))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret =
	    cxd2880_tnrdmd_mon_internal_cpu_status(tnr_dmd->diver_sub, status);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_mon_ts_buf_info(struct cxd2880_tnrdmd *tnr_dmd,
						struct
						cxd2880_tnrdmd_ts_buf_info
						*info)
{
	u8 data[3] = { 0 };
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!info))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if ((tnr_dmd->state != CXD2880_TNRDMD_STATE_SLEEP) &&
	    (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE))
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0A) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;
	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x50, data,
				   3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	info->read_ready = (u8)((data[0] & 0x10) ? 0x01 : 0x00);
	info->almost_full = (u8)((data[0] & 0x08) ? 0x01 : 0x00);
	info->almost_empty = (u8)((data[0] & 0x04) ? 0x01 : 0x00);
	info->overflow = (u8)((data[0] & 0x02) ? 0x01 : 0x00);
	info->underflow = (u8)((data[0] & 0x01) ? 0x01 : 0x00);

	info->packet_num = (u16)(((u32)(data[1] & 0x07) << 8) | data[2]);

	return ret;
}
