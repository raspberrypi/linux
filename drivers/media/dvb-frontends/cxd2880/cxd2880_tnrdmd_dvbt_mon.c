/*
 * cxd2880_tnrdmd_dvbt_mon.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * DVB-T monitor functions
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

#include "cxd2880_tnrdmd_mon.h"
#include "cxd2880_tnrdmd_dvbt.h"
#include "cxd2880_tnrdmd_dvbt_mon.h"
#include "cxd2880_math.h"

static enum cxd2880_ret is_tps_locked(struct cxd2880_tnrdmd *tnr_dmd);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_sync_stat(struct cxd2880_tnrdmd
						   *tnr_dmd, u8 *sync_stat,
						   u8 *ts_lock_stat,
						   u8 *unlock_detected)
{
	u8 rdata = 0x00;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!sync_stat) || (!ts_lock_stat) || (!unlock_detected))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;
	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0D) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x10, &rdata,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	*unlock_detected = (u8)((rdata & 0x10) ? 1 : 0);
	*sync_stat = (u8)(rdata & 0x07);
	*ts_lock_stat = (u8)((rdata & 0x20) ? 1 : 0);

	if (*sync_stat == 0x07)
		return CXD2880_RESULT_ERROR_HW_STATE;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_sync_stat_sub(struct cxd2880_tnrdmd
						       *tnr_dmd, u8 *sync_stat,
						       u8 *unlock_detected)
{
	u8 ts_lock_stat = 0;

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!sync_stat) || (!unlock_detected))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret =
	    cxd2880_tnrdmd_dvbt_mon_sync_stat(tnr_dmd->diver_sub, sync_stat,
					      &ts_lock_stat, unlock_detected);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_mode_guard(struct cxd2880_tnrdmd
						    *tnr_dmd,
						    enum cxd2880_dvbt_mode
						    *mode,
						    enum cxd2880_dvbt_guard
						    *guard)
{
	u8 rdata = 0x00;

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!mode) || (!guard))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	ret = is_tps_locked(tnr_dmd);
	if (ret != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN)
			ret =
			    cxd2880_tnrdmd_dvbt_mon_mode_guard(
					tnr_dmd->diver_sub, mode, guard);

		return ret;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0D) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x1B, &rdata,
				   1) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	slvt_unfreeze_reg(tnr_dmd);

	*mode = (enum cxd2880_dvbt_mode)((rdata >> 2) & 0x03);
	*guard = (enum cxd2880_dvbt_guard)(rdata & 0x03);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_carrier_offset(struct cxd2880_tnrdmd
							*tnr_dmd, int *offset)
{
	u8 rdata[4];
	u32 ctl_val = 0;

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!offset))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	ret = is_tps_locked(tnr_dmd);
	if (ret != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return ret;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0D) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x1D, rdata,
				   4) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	slvt_unfreeze_reg(tnr_dmd);

	ctl_val =
	    ((rdata[0] & 0x1F) << 24) | (rdata[1] << 16) | (rdata[2] << 8) |
	    (rdata[3]);
	*offset = cxd2880_convert2s_complement(ctl_val, 29);
	*offset = -1 * ((*offset) * (u8)tnr_dmd->bandwidth / 235);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_carrier_offset_sub(struct
							    cxd2880_tnrdmd
							    *tnr_dmd,
							    int *offset)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!offset))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret =
	    cxd2880_tnrdmd_dvbt_mon_carrier_offset(tnr_dmd->diver_sub, offset);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_pre_viterbiber(struct cxd2880_tnrdmd
							*tnr_dmd, u32 *ber)
{
	u8 rdata[2];
	u32 bit_error = 0;
	u32 period = 0;

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!ber))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x10) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x39, rdata,
				   1) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if ((rdata[0] & 0x01) == 0) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_HW_STATE;
	}

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x22, rdata,
				   2) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	bit_error = (rdata[0] << 8) | rdata[1];

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x6F, rdata,
				   1) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	slvt_unfreeze_reg(tnr_dmd);

	period = ((rdata[0] & 0x07) == 0) ? 256 : (0x1000 << (rdata[0] & 0x07));

	if ((period == 0) || (bit_error > period))
		return CXD2880_RESULT_ERROR_HW_STATE;

	{
		u32 div = 0;
		u32 Q = 0;
		u32 R = 0;

		div = period / 128;

		Q = (bit_error * 3125) / div;
		R = (bit_error * 3125) % div;

		R *= 25;
		Q = Q * 25 + R / div;
		R = R % div;

		if (div / 2 <= R)
			*ber = Q + 1;
		else
			*ber = Q;
	}
	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_pre_rsber(struct cxd2880_tnrdmd
						   *tnr_dmd, u32 *ber)
{
	u8 rdata[3];
	u32 bit_error = 0;
	u32 period_exp = 0;

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!ber))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0D) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x15, rdata,
				   3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if ((rdata[0] & 0x40) == 0)
		return CXD2880_RESULT_ERROR_HW_STATE;

	bit_error = ((rdata[0] & 0x3F) << 16) | (rdata[1] << 8) | rdata[2];

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x60, rdata,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	period_exp = (rdata[0] & 0x1F);

	if ((period_exp <= 11) && (bit_error > (1U << period_exp) * 204 * 8))
		return CXD2880_RESULT_ERROR_HW_STATE;

	{
		u32 div = 0;
		u32 Q = 0;
		u32 R = 0;

		if (period_exp <= 8)
			div = (1U << period_exp) * 51;
		else
			div = (1U << 8) * 51;

		Q = (bit_error * 250) / div;
		R = (bit_error * 250) % div;

		R *= 1250;
		Q = Q * 1250 + R / div;
		R = R % div;

		if (period_exp > 8) {
			*ber =
			    (Q + (1 << (period_exp - 9))) >> (period_exp - 8);
		} else {
			if (div / 2 <= R)
				*ber = Q + 1;
			else
				*ber = Q;
		}
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_tps_info(struct cxd2880_tnrdmd
						  *tnr_dmd,
						  struct cxd2880_dvbt_tpsinfo
						  *info)
{
	u8 rdata[7];
	u8 cell_id_ok = 0;

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!info))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	ret = is_tps_locked(tnr_dmd);
	if (ret != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN)
			ret =
			    cxd2880_tnrdmd_dvbt_mon_tps_info(tnr_dmd->diver_sub,
							     info);

		return ret;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0D) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x29, rdata,
				   7) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x11) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0xD5, &cell_id_ok,
				   1) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	slvt_unfreeze_reg(tnr_dmd);

	info->constellation =
	    (enum cxd2880_dvbt_constellation)((rdata[0] >> 6) & 0x03);
	info->hierarchy = (enum cxd2880_dvbt_hierarchy)((rdata[0] >> 3) & 0x07);
	info->rate_hp = (enum cxd2880_dvbt_coderate)(rdata[0] & 0x07);
	info->rate_lp = (enum cxd2880_dvbt_coderate)((rdata[1] >> 5) & 0x07);
	info->guard = (enum cxd2880_dvbt_guard)((rdata[1] >> 3) & 0x03);
	info->mode = (enum cxd2880_dvbt_mode)((rdata[1] >> 1) & 0x03);
	info->fnum = (rdata[2] >> 6) & 0x03;
	info->length_indicator = rdata[2] & 0x3F;
	info->cell_id = (u16)((rdata[3] << 8) | rdata[4]);
	info->reserved_even = rdata[5] & 0x3F;
	info->reserved_odd = rdata[6] & 0x3F;

	info->cell_id_ok = cell_id_ok & 0x01;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_packet_error_number(struct
							     cxd2880_tnrdmd
							     *tnr_dmd,
							     u32 *pen)
{
	u8 rdata[3];

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!pen))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0D) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x26, rdata,
				   3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (!(rdata[0] & 0x01))
		return CXD2880_RESULT_ERROR_HW_STATE;

	*pen = (rdata[1] << 8) | rdata[2];

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_spectrum_sense(struct cxd2880_tnrdmd
						*tnr_dmd,
						enum
						cxd2880_tnrdmd_spectrum_sense
						*sense)
{
	u8 data = 0;

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!sense))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	ret = is_tps_locked(tnr_dmd);
	if (ret != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN)
			ret =
				cxd2880_tnrdmd_dvbt_mon_spectrum_sense(
					tnr_dmd->diver_sub, sense);

		return ret;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0D) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x1C, &data,
				   sizeof(data)) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	slvt_unfreeze_reg(tnr_dmd);

	*sense =
	    (data & 0x01) ? CXD2880_TNRDMD_SPECTRUM_INV :
	    CXD2880_TNRDMD_SPECTRUM_NORMAL;

	return ret;
}

static enum cxd2880_ret dvbt_read_snr_reg(struct cxd2880_tnrdmd *tnr_dmd,
					  u16 *reg_value)
{
	u8 rdata[2];

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!reg_value))
		return CXD2880_RESULT_ERROR_ARG;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	ret = is_tps_locked(tnr_dmd);
	if (ret != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return ret;
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0D) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x13, rdata,
				   2) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	slvt_unfreeze_reg(tnr_dmd);

	*reg_value = (rdata[0] << 8) | rdata[1];

	return ret;
}

static enum cxd2880_ret dvbt_calc_snr(struct cxd2880_tnrdmd *tnr_dmd,
				      u32 reg_value, int *snr)
{
	if ((!tnr_dmd) || (!snr))
		return CXD2880_RESULT_ERROR_ARG;

	if (reg_value == 0)
		return CXD2880_RESULT_ERROR_HW_STATE;

	if (reg_value > 4996)
		reg_value = 4996;

	*snr =
	    10 * 10 * ((int)cxd2880_math_log10(reg_value) -
		       (int)cxd2880_math_log10(5350 - reg_value));
	*snr += 28500;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_snr(struct cxd2880_tnrdmd *tnr_dmd,
					     int *snr)
{
	u16 reg_value = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!snr))
		return CXD2880_RESULT_ERROR_ARG;

	*snr = -1000 * 1000;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SINGLE) {
		ret = dvbt_read_snr_reg(tnr_dmd, &reg_value);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		ret = dvbt_calc_snr(tnr_dmd, reg_value, snr);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	} else {
		int snr_main = 0;
		int snr_sub = 0;

		ret =
		    cxd2880_tnrdmd_dvbt_mon_snr_diver(tnr_dmd, snr, &snr_main,
						      &snr_sub);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_snr_diver(struct cxd2880_tnrdmd
						   *tnr_dmd, int *snr,
						   int *snr_main, int *snr_sub)
{
	u16 reg_value = 0;
	u32 reg_value_sum = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!snr) || (!snr_main) || (!snr_sub))
		return CXD2880_RESULT_ERROR_ARG;

	*snr = -1000 * 1000;
	*snr_main = -1000 * 1000;
	*snr_sub = -1000 * 1000;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = dvbt_read_snr_reg(tnr_dmd, &reg_value);
	if (ret == CXD2880_RESULT_OK) {
		ret = dvbt_calc_snr(tnr_dmd, reg_value, snr_main);
		if (ret != CXD2880_RESULT_OK)
			reg_value = 0;
	} else if (ret == CXD2880_RESULT_ERROR_HW_STATE) {
		reg_value = 0;
	} else {
		return ret;
	}

	reg_value_sum += reg_value;

	ret = dvbt_read_snr_reg(tnr_dmd->diver_sub, &reg_value);
	if (ret == CXD2880_RESULT_OK) {
		ret = dvbt_calc_snr(tnr_dmd->diver_sub, reg_value, snr_sub);
		if (ret != CXD2880_RESULT_OK)
			reg_value = 0;
	} else if (ret == CXD2880_RESULT_ERROR_HW_STATE) {
		reg_value = 0;
	} else {
		return ret;
	}

	reg_value_sum += reg_value;

	ret = dvbt_calc_snr(tnr_dmd, reg_value_sum, snr);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_sampling_offset(struct cxd2880_tnrdmd
							 *tnr_dmd, int *ppm)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!ppm))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 ctl_val_reg[5];
		u8 nominal_rate_reg[5];
		u32 trl_ctl_val = 0;
		u32 trcg_nominal_rate = 0;
		int num;
		int den;
		s8 diff_upper = 0;

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		ret = is_tps_locked(tnr_dmd);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return ret;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0D) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x21,
					   ctl_val_reg,
					   sizeof(ctl_val_reg)) !=
		    CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x04) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x60,
					   nominal_rate_reg,
					   sizeof(nominal_rate_reg)) !=
		    CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		slvt_unfreeze_reg(tnr_dmd);

		diff_upper =
		    (ctl_val_reg[0] & 0x7F) - (nominal_rate_reg[0] & 0x7F);

		if ((diff_upper < -1) || (diff_upper > 1))
			return CXD2880_RESULT_ERROR_HW_STATE;

		trl_ctl_val = ctl_val_reg[1] << 24;
		trl_ctl_val |= ctl_val_reg[2] << 16;
		trl_ctl_val |= ctl_val_reg[3] << 8;
		trl_ctl_val |= ctl_val_reg[4];

		trcg_nominal_rate = nominal_rate_reg[1] << 24;
		trcg_nominal_rate |= nominal_rate_reg[2] << 16;
		trcg_nominal_rate |= nominal_rate_reg[3] << 8;
		trcg_nominal_rate |= nominal_rate_reg[4];

		trl_ctl_val >>= 1;
		trcg_nominal_rate >>= 1;

		if (diff_upper == 1)
			num =
			    (int)((trl_ctl_val + 0x80000000u) -
				  trcg_nominal_rate);
		else if (diff_upper == -1)
			num =
			    -(int)((trcg_nominal_rate + 0x80000000u) -
				   trl_ctl_val);
		else
			num = (int)(trl_ctl_val - trcg_nominal_rate);

		den = (nominal_rate_reg[0] & 0x7F) << 24;
		den |= nominal_rate_reg[1] << 16;
		den |= nominal_rate_reg[2] << 8;
		den |= nominal_rate_reg[3];
		den = (den + (390625 / 2)) / 390625;

		den >>= 1;

		if (num >= 0)
			*ppm = (num + (den / 2)) / den;
		else
			*ppm = (num - (den / 2)) / den;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_sampling_offset_sub(struct
							     cxd2880_tnrdmd
							     *tnr_dmd, int *ppm)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!ppm))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret = cxd2880_tnrdmd_dvbt_mon_sampling_offset(tnr_dmd->diver_sub, ppm);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_quality(struct cxd2880_tnrdmd *tnr_dmd,
						 u8 *quality)
{
	struct cxd2880_dvbt_tpsinfo tps;
	enum cxd2880_dvbt_profile profile = CXD2880_DVBT_PROFILE_HP;
	u32 ber = 0;
	int sn = 0;
	int sn_rel = 0;
	int ber_sqi = 0;

	static const int nordig_non_hdvbt_db_1000[3][5] = {
		{5100, 6900, 7900, 8900, 9700},
		{10800, 13100, 14600, 15600, 16000},
		{16500, 18700, 20200, 21600, 22500}
	};

	static const int nordig_hier_hp_dvbt_db_1000[3][2][5] = {
		{
		 {9100, 12000, 13600, 15000, 16600},
		 {10900, 14100, 15700, 19400, 20600}
		 },
		{
		 {6800, 9100, 10400, 11900, 12700},
		 {8500, 11000, 12800, 15000, 16000}
		 },
		{
		 {5800, 7900, 9100, 10300, 12100},
		 {8000, 9300, 11600, 13000, 12900}
		}
	};

	static const int nordig_hier_lp_dvbt_db_1000[3][2][5] = {
		{
		 {12500, 14300, 15300, 16300, 16900},
		 {16700, 19100, 20900, 22500, 23700}
		 },
		{
		 {15000, 17200, 18400, 19100, 20100},
		 {18500, 21200, 23600, 24700, 25900}
		 },
		{
		 {19500, 21400, 22500, 23700, 24700},
		 {21900, 24200, 25600, 26900, 27800}
		}
	};

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!quality))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = cxd2880_tnrdmd_dvbt_mon_tps_info(tnr_dmd, &tps);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (tps.hierarchy != CXD2880_DVBT_HIERARCHY_NON) {
		u8 data = 0;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x10) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x67, &data,
					   1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		profile =
		    ((data & 0x01) ==
		     0x01) ? CXD2880_DVBT_PROFILE_LP : CXD2880_DVBT_PROFILE_HP;
	}

	ret = cxd2880_tnrdmd_dvbt_mon_pre_rsber(tnr_dmd, &ber);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = cxd2880_tnrdmd_dvbt_mon_snr(tnr_dmd, &sn);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if ((tps.constellation >= CXD2880_DVBT_CONSTELLATION_RESERVED_3) ||
	    (tps.rate_hp >= CXD2880_DVBT_CODERATE_RESERVED_5) ||
	    (tps.rate_lp >= CXD2880_DVBT_CODERATE_RESERVED_5) ||
	    (tps.hierarchy > CXD2880_DVBT_HIERARCHY_4)) {
		return CXD2880_RESULT_ERROR_OTHER;
	}

	if ((tps.hierarchy != CXD2880_DVBT_HIERARCHY_NON) &&
	    (tps.constellation == CXD2880_DVBT_CONSTELLATION_QPSK))
		return CXD2880_RESULT_ERROR_OTHER;

	if (tps.hierarchy == CXD2880_DVBT_HIERARCHY_NON)
		sn_rel =
		    sn -
		    nordig_non_hdvbt_db_1000[tps.constellation][tps.rate_hp];
	else if (profile == CXD2880_DVBT_PROFILE_LP)
		sn_rel =
		    sn - nordig_hier_lp_dvbt_db_1000[(int)tps.hierarchy -
						     1][(int)tps.constellation -
							1][tps.rate_lp];
	else
		sn_rel =
		    sn - nordig_hier_hp_dvbt_db_1000[(int)tps.hierarchy -
						     1][(int)tps.constellation -
							1][tps.rate_hp];

	if (ber > 10000) {
		ber_sqi = 0;
	} else if (ber > 1) {
		ber_sqi = (int)(10 * cxd2880_math_log10(ber));
		ber_sqi = 20 * (7 * 1000 - (ber_sqi)) - 40 * 1000;
	} else {
		ber_sqi = 100 * 1000;
	}

	if (sn_rel < -7 * 1000) {
		*quality = 0;
	} else if (sn_rel < 3 * 1000) {
		int tmp_sqi = (((sn_rel - (3 * 1000)) / 10) + 1000);
		*quality =
		    (u8)(((tmp_sqi * ber_sqi) +
			   (1000000 / 2)) / (1000000)) & 0xFF;
	} else {
		*quality = (u8)((ber_sqi + 500) / 1000);
	}

	if (*quality > 100)
		*quality = 100;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_per(struct cxd2880_tnrdmd *tnr_dmd,
					     u32 *per)
{
	u32 packet_error = 0;
	u32 period = 0;
	u8 rdata[3];

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!per))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0D) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x18, rdata,
				   3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if ((rdata[0] & 0x01) == 0)
		return CXD2880_RESULT_ERROR_HW_STATE;

	packet_error = (rdata[1] << 8) | rdata[2];

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x5C, rdata,
				   1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	period = 1U << (rdata[0] & 0x0F);

	if ((period == 0) || (packet_error > period))
		return CXD2880_RESULT_ERROR_HW_STATE;

	{
		u32 div = 0;
		u32 Q = 0;
		u32 R = 0;

		div = period;

		Q = (packet_error * 1000) / div;
		R = (packet_error * 1000) % div;

		R *= 1000;
		Q = Q * 1000 + R / div;
		R = R % div;

		if ((div != 1) && (div / 2 <= R))
			*per = Q + 1;
		else
			*per = Q;
	}

	return ret;
}

static enum cxd2880_ret dvbt_calc_ssi(struct cxd2880_tnrdmd *tnr_dmd,
				      int rf_lvl, u8 *ssi)
{
	struct cxd2880_dvbt_tpsinfo tps;
	int prel;
	int temp_ssi = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	static const int ref_dbm_1000[3][5] = {
		{-93000, -91000, -90000, -89000, -88000},
		{-87000, -85000, -84000, -83000, -82000},
		{-82000, -80000, -78000, -77000, -76000},
	};

	if ((!tnr_dmd) || (!ssi))
		return CXD2880_RESULT_ERROR_ARG;

	ret = cxd2880_tnrdmd_dvbt_mon_tps_info(tnr_dmd, &tps);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if ((tps.constellation >= CXD2880_DVBT_CONSTELLATION_RESERVED_3) ||
	    (tps.rate_hp >= CXD2880_DVBT_CODERATE_RESERVED_5))
		return CXD2880_RESULT_ERROR_OTHER;

	prel = rf_lvl - ref_dbm_1000[tps.constellation][tps.rate_hp];

	if (prel < -15000)
		temp_ssi = 0;
	else if (prel < 0)
		temp_ssi = ((2 * (prel + 15000)) + 1500) / 3000;
	else if (prel < 20000)
		temp_ssi = (((4 * prel) + 500) / 1000) + 10;
	else if (prel < 35000)
		temp_ssi = (((2 * (prel - 20000)) + 1500) / 3000) + 90;
	else
		temp_ssi = 100;

	*ssi = (temp_ssi > 100) ? 100 : (u8)temp_ssi;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_ssi(struct cxd2880_tnrdmd *tnr_dmd,
					     u8 *ssi)
{
	int rf_lvl = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!ssi))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = cxd2880_tnrdmd_mon_rf_lvl(tnr_dmd, &rf_lvl);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = dvbt_calc_ssi(tnr_dmd, rf_lvl, ssi);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_ssi_sub(struct cxd2880_tnrdmd *tnr_dmd,
						 u8 *ssi)
{
	int rf_lvl = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!ssi))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = cxd2880_tnrdmd_mon_rf_lvl(tnr_dmd->diver_sub, &rf_lvl);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = dvbt_calc_ssi(tnr_dmd, rf_lvl, ssi);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	return ret;
}

static enum cxd2880_ret is_tps_locked(struct cxd2880_tnrdmd *tnr_dmd)
{
	u8 sync = 0;
	u8 tslock = 0;
	u8 early_unlock = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	ret =
	    cxd2880_tnrdmd_dvbt_mon_sync_stat(tnr_dmd, &sync, &tslock,
					      &early_unlock);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (sync != 6)
		return CXD2880_RESULT_ERROR_HW_STATE;

	return CXD2880_RESULT_OK;
}
