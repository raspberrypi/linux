/*
 * cxd2880_tnrdmd_dvbt2_mon.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * DVB-T2 monitor functions
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
#include "cxd2880_tnrdmd_dvbt2.h"
#include "cxd2880_tnrdmd_dvbt2_mon.h"
#include "cxd2880_math.h"

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_sync_stat(struct cxd2880_tnrdmd
						    *tnr_dmd, u8 *sync_stat,
						    u8 *ts_lock_stat,
						    u8 *unlock_detected)
{
	if ((!tnr_dmd) || (!sync_stat) || (!ts_lock_stat) || (!unlock_detected))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0B) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	{
		u8 data;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x10, &data,
					   sizeof(data)) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		*sync_stat = data & 0x07;
		*ts_lock_stat = ((data & 0x20) ? 1 : 0);
		*unlock_detected = ((data & 0x10) ? 1 : 0);
	}

	if (*sync_stat == 0x07)
		return CXD2880_RESULT_ERROR_HW_STATE;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_sync_stat_sub(struct cxd2880_tnrdmd
							*tnr_dmd,
							u8 *sync_stat,
							u8 *unlock_detected)
{
	u8 ts_lock_stat = 0;

	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!sync_stat) || (!unlock_detected))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret =
	    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd->diver_sub, sync_stat,
					       &ts_lock_stat, unlock_detected);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_carrier_offset(struct cxd2880_tnrdmd
							 *tnr_dmd, int *offset)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!offset))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 data[4];
		u32 ctl_val = 0;
		u8 sync_state = 0;
		u8 ts_lock = 0;
		u8 unlock_detected = 0;

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_state,
						       &ts_lock,
						       &unlock_detected);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (sync_state != 6) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x30, data,
					   sizeof(data)) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		slvt_unfreeze_reg(tnr_dmd);

		ctl_val =
		    ((data[0] & 0x0F) << 24) | (data[1] << 16) | (data[2] << 8)
		    | (data[3]);
		*offset = cxd2880_convert2s_complement(ctl_val, 28);

		switch (tnr_dmd->bandwidth) {
		case CXD2880_DTV_BW_1_7_MHZ:
			*offset = -1 * ((*offset) / 582);
			break;
		case CXD2880_DTV_BW_5_MHZ:
		case CXD2880_DTV_BW_6_MHZ:
		case CXD2880_DTV_BW_7_MHZ:
		case CXD2880_DTV_BW_8_MHZ:
			*offset =
			    -1 * ((*offset) * (u8)tnr_dmd->bandwidth / 940);
			break;
		default:
			return CXD2880_RESULT_ERROR_SW_STATE;
		}
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_carrier_offset_sub(struct
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
	    cxd2880_tnrdmd_dvbt2_mon_carrier_offset(tnr_dmd->diver_sub, offset);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_l1_pre(struct cxd2880_tnrdmd *tnr_dmd,
						 struct cxd2880_dvbt2_l1pre
						 *l1_pre)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!l1_pre))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 data[37];
		u8 sync_state = 0;
		u8 ts_lock = 0;
		u8 unlock_detected = 0;
		u8 version = 0;
		enum cxd2880_dvbt2_profile profile;

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_state,
						       &ts_lock,
						       &unlock_detected);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return ret;
		}

		if (sync_state < 5) {
			if (tnr_dmd->diver_mode ==
			    CXD2880_TNRDMD_DIVERMODE_MAIN) {
				ret =
				    cxd2880_tnrdmd_dvbt2_mon_sync_stat_sub
				    (tnr_dmd, &sync_state, &unlock_detected);
				if (ret != CXD2880_RESULT_OK) {
					slvt_unfreeze_reg(tnr_dmd);
					return ret;
				}

				if (sync_state < 5) {
					slvt_unfreeze_reg(tnr_dmd);
					return CXD2880_RESULT_ERROR_HW_STATE;
				}
			} else {
				slvt_unfreeze_reg(tnr_dmd);
				return CXD2880_RESULT_ERROR_HW_STATE;
			}
		}

		ret = cxd2880_tnrdmd_dvbt2_mon_profile(tnr_dmd, &profile);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return ret;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x61, data,
					   sizeof(data)) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}
		slvt_unfreeze_reg(tnr_dmd);

		l1_pre->type = (enum cxd2880_dvbt2_l1pre_type)data[0];
		l1_pre->bw_ext = data[1] & 0x01;
		l1_pre->s1 = (enum cxd2880_dvbt2_s1)(data[2] & 0x07);
		l1_pre->s2 = data[3] & 0x0F;
		l1_pre->l1_rep = data[4] & 0x01;
		l1_pre->gi = (enum cxd2880_dvbt2_guard)(data[5] & 0x07);
		l1_pre->papr = (enum cxd2880_dvbt2_papr)(data[6] & 0x0F);
		l1_pre->mod =
		    (enum cxd2880_dvbt2_l1post_constell)(data[7] & 0x0F);
		l1_pre->cr = (enum cxd2880_dvbt2_l1post_cr)(data[8] & 0x03);
		l1_pre->fec =
		    (enum cxd2880_dvbt2_l1post_fec_type)(data[9] & 0x03);
		l1_pre->l1_post_size = (data[10] & 0x03) << 16;
		l1_pre->l1_post_size |= (data[11]) << 8;
		l1_pre->l1_post_size |= (data[12]);
		l1_pre->l1_post_info_size = (data[13] & 0x03) << 16;
		l1_pre->l1_post_info_size |= (data[14]) << 8;
		l1_pre->l1_post_info_size |= (data[15]);
		l1_pre->pp = (enum cxd2880_dvbt2_pp)(data[16] & 0x0F);
		l1_pre->tx_id_availability = data[17];
		l1_pre->cell_id = (data[18] << 8);
		l1_pre->cell_id |= (data[19]);
		l1_pre->network_id = (data[20] << 8);
		l1_pre->network_id |= (data[21]);
		l1_pre->sys_id = (data[22] << 8);
		l1_pre->sys_id |= (data[23]);
		l1_pre->num_frames = data[24];
		l1_pre->num_symbols = (data[25] & 0x0F) << 8;
		l1_pre->num_symbols |= data[26];
		l1_pre->regen = data[27] & 0x07;
		l1_pre->post_ext = data[28] & 0x01;
		l1_pre->num_rf_freqs = data[29] & 0x07;
		l1_pre->rf_idx = data[30] & 0x07;
		version = (data[31] & 0x03) << 2;
		version |= (data[32] & 0xC0) >> 6;
		l1_pre->t2_version = (enum cxd2880_dvbt2_version)version;
		l1_pre->l1_post_scrambled = (data[32] & 0x20) >> 5;
		l1_pre->t2_base_lite = (data[32] & 0x10) >> 4;
		l1_pre->crc32 = (data[33] << 24);
		l1_pre->crc32 |= (data[34] << 16);
		l1_pre->crc32 |= (data[35] << 8);
		l1_pre->crc32 |= data[36];

		if (profile == CXD2880_DVBT2_PROFILE_BASE) {
			switch ((l1_pre->s2 >> 1)) {
			case CXD2880_DVBT2_BASE_S2_M1K_G_ANY:
				l1_pre->fft_mode = CXD2880_DVBT2_M1K;
				break;
			case CXD2880_DVBT2_BASE_S2_M2K_G_ANY:
				l1_pre->fft_mode = CXD2880_DVBT2_M2K;
				break;
			case CXD2880_DVBT2_BASE_S2_M4K_G_ANY:
				l1_pre->fft_mode = CXD2880_DVBT2_M4K;
				break;
			case CXD2880_DVBT2_BASE_S2_M8K_G_DVBT:
			case CXD2880_DVBT2_BASE_S2_M8K_G_DVBT2:
				l1_pre->fft_mode = CXD2880_DVBT2_M8K;
				break;
			case CXD2880_DVBT2_BASE_S2_M16K_G_ANY:
				l1_pre->fft_mode = CXD2880_DVBT2_M16K;
				break;
			case CXD2880_DVBT2_BASE_S2_M32K_G_DVBT:
			case CXD2880_DVBT2_BASE_S2_M32K_G_DVBT2:
				l1_pre->fft_mode = CXD2880_DVBT2_M32K;
				break;
			default:
				return CXD2880_RESULT_ERROR_HW_STATE;
			}
		} else if (profile == CXD2880_DVBT2_PROFILE_LITE) {
			switch ((l1_pre->s2 >> 1)) {
			case CXD2880_DVBT2_LITE_S2_M2K_G_ANY:
				l1_pre->fft_mode = CXD2880_DVBT2_M2K;
				break;
			case CXD2880_DVBT2_LITE_S2_M4K_G_ANY:
				l1_pre->fft_mode = CXD2880_DVBT2_M4K;
				break;
			case CXD2880_DVBT2_LITE_S2_M8K_G_DVBT:
			case CXD2880_DVBT2_LITE_S2_M8K_G_DVBT2:
				l1_pre->fft_mode = CXD2880_DVBT2_M8K;
				break;
			case CXD2880_DVBT2_LITE_S2_M16K_G_DVBT:
			case CXD2880_DVBT2_LITE_S2_M16K_G_DVBT2:
				l1_pre->fft_mode = CXD2880_DVBT2_M16K;
				break;
			default:
				return CXD2880_RESULT_ERROR_HW_STATE;
			}
		} else {
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		l1_pre->mixed = l1_pre->s2 & 0x01;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_version(struct cxd2880_tnrdmd
						  *tnr_dmd,
						  enum cxd2880_dvbt2_version
						  *ver)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	u8 version = 0;

	if ((!tnr_dmd) || (!ver))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 data[2];
		u8 sync_state = 0;
		u8 ts_lock = 0;
		u8 unlock_detected = 0;

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_state,
						       &ts_lock,
						       &unlock_detected);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (sync_state < 5) {
			if (tnr_dmd->diver_mode ==
			    CXD2880_TNRDMD_DIVERMODE_MAIN) {
				ret =
				    cxd2880_tnrdmd_dvbt2_mon_sync_stat_sub
				    (tnr_dmd, &sync_state, &unlock_detected);
				if (ret != CXD2880_RESULT_OK) {
					slvt_unfreeze_reg(tnr_dmd);
					return ret;
				}

				if (sync_state < 5) {
					slvt_unfreeze_reg(tnr_dmd);
					return CXD2880_RESULT_ERROR_HW_STATE;
				}
			} else {
				slvt_unfreeze_reg(tnr_dmd);
				return CXD2880_RESULT_ERROR_HW_STATE;
			}
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x80, data,
					   sizeof(data)) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		slvt_unfreeze_reg(tnr_dmd);

		version = ((data[0] & 0x03) << 2);
		version |= ((data[1] & 0xC0) >> 6);
		*ver = (enum cxd2880_dvbt2_version)version;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_ofdm(struct cxd2880_tnrdmd *tnr_dmd,
					       struct cxd2880_dvbt2_ofdm *ofdm)
{
	if ((!tnr_dmd) || (!ofdm))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 data[5];
		u8 sync_state = 0;
		u8 ts_lock = 0;
		u8 unlock_detected = 0;
		enum cxd2880_ret ret = CXD2880_RESULT_OK;

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_state,
						       &ts_lock,
						       &unlock_detected);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (sync_state != 6) {
			slvt_unfreeze_reg(tnr_dmd);

			ret = CXD2880_RESULT_ERROR_HW_STATE;

			if (tnr_dmd->diver_mode ==
			    CXD2880_TNRDMD_DIVERMODE_MAIN)
				ret =
				    cxd2880_tnrdmd_dvbt2_mon_ofdm(
					tnr_dmd->diver_sub, ofdm);

			return ret;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x1D, data,
					   sizeof(data)) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		slvt_unfreeze_reg(tnr_dmd);

		ofdm->mixed = ((data[0] & 0x20) ? 1 : 0);
		ofdm->is_miso = ((data[0] & 0x10) >> 4);
		ofdm->mode = (enum cxd2880_dvbt2_mode)(data[0] & 0x07);
		ofdm->gi = (enum cxd2880_dvbt2_guard)((data[1] & 0x70) >> 4);
		ofdm->pp = (enum cxd2880_dvbt2_pp)(data[1] & 0x07);
		ofdm->bw_ext = (data[2] & 0x10) >> 4;
		ofdm->papr = (enum cxd2880_dvbt2_papr)(data[2] & 0x0F);
		ofdm->num_symbols = (data[3] << 8) | data[4];
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_data_plps(struct cxd2880_tnrdmd
						    *tnr_dmd, u8 *plp_ids,
						    u8 *num_plps)
{
	if ((!tnr_dmd) || (!num_plps))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 l1_post_ok = 0;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x86,
					   &l1_post_ok,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (!(l1_post_ok & 0x01)) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xC1, num_plps,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (*num_plps == 0) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_OTHER;
		}

		if (!plp_ids) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_OK;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xC2, plp_ids,
					   ((*num_plps >
					     62) ? 62 : *num_plps)) !=
		    CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (*num_plps > 62) {
			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x00,
						   0x0C) != CXD2880_RESULT_OK) {
				slvt_unfreeze_reg(tnr_dmd);
				return CXD2880_RESULT_ERROR_IO;
			}

			if (tnr_dmd->io->read_regs(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x10,
						   plp_ids + 62,
						   *num_plps - 62) !=
			    CXD2880_RESULT_OK) {
				slvt_unfreeze_reg(tnr_dmd);
				return CXD2880_RESULT_ERROR_IO;
			}
		}

		slvt_unfreeze_reg(tnr_dmd);
	}
	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_active_plp(struct cxd2880_tnrdmd
						     *tnr_dmd,
						     enum
						     cxd2880_dvbt2_plp_btype
						     type,
						     struct cxd2880_dvbt2_plp
						     *plp_info)
{
	if ((!tnr_dmd) || (!plp_info))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 data[20];
		u8 addr = 0;
		u8 index = 0;
		u8 l1_post_ok = 0;

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x86,
					   &l1_post_ok,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (!l1_post_ok) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		if (type == CXD2880_DVBT2_PLP_COMMON)
			addr = 0xA9;
		else
			addr = 0x96;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, addr, data,
					   sizeof(data)) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		slvt_unfreeze_reg(tnr_dmd);

		if ((type == CXD2880_DVBT2_PLP_COMMON) && (data[13] == 0))
			return CXD2880_RESULT_ERROR_HW_STATE;

		plp_info->id = data[index++];
		plp_info->type =
		    (enum cxd2880_dvbt2_plp_type)(data[index++] & 0x07);
		plp_info->payload =
		    (enum cxd2880_dvbt2_plp_payload)(data[index++] & 0x1F);
		plp_info->ff = data[index++] & 0x01;
		plp_info->first_rf_idx = data[index++] & 0x07;
		plp_info->first_frm_idx = data[index++];
		plp_info->group_id = data[index++];
		plp_info->plp_cr =
		    (enum cxd2880_dvbt2_plp_code_rate)(data[index++] & 0x07);
		plp_info->constell =
		    (enum cxd2880_dvbt2_plp_constell)(data[index++] & 0x07);
		plp_info->rot = data[index++] & 0x01;
		plp_info->fec =
		    (enum cxd2880_dvbt2_plp_fec)(data[index++] & 0x03);
		plp_info->num_blocks_max = (u16)((data[index++] & 0x03)) << 8;
		plp_info->num_blocks_max |= data[index++];
		plp_info->frm_int = data[index++];
		plp_info->til_len = data[index++];
		plp_info->til_type = data[index++] & 0x01;

		plp_info->in_band_a_flag = data[index++] & 0x01;
		plp_info->rsvd = data[index++] << 8;
		plp_info->rsvd |= data[index++];

		plp_info->in_band_b_flag =
		    (u8)((plp_info->rsvd & 0x8000) >> 15);
		plp_info->plp_mode =
		    (enum cxd2880_dvbt2_plp_mode)((plp_info->rsvd & 0x000C) >>
						  2);
		plp_info->static_flag = (u8)((plp_info->rsvd & 0x0002) >> 1);
		plp_info->static_padding_flag = (u8)(plp_info->rsvd & 0x0001);
		plp_info->rsvd = (u16)((plp_info->rsvd & 0x7FF0) >> 4);
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_data_plp_error(struct cxd2880_tnrdmd
							 *tnr_dmd,
							 u8 *plp_error)
{
	if ((!tnr_dmd) || (!plp_error))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	{
		u8 data;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x86, &data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if ((data & 0x01) == 0x00) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xC0, &data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		slvt_unfreeze_reg(tnr_dmd);

		*plp_error = data & 0x01;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_l1_change(struct cxd2880_tnrdmd
						    *tnr_dmd, u8 *l1_change)
{
	if ((!tnr_dmd) || (!l1_change))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 data;
		u8 sync_state = 0;
		u8 ts_lock = 0;
		u8 unlock_detected = 0;
		enum cxd2880_ret ret = CXD2880_RESULT_OK;

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_state,
						       &ts_lock,
						       &unlock_detected);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (sync_state < 5) {
			if (tnr_dmd->diver_mode ==
			    CXD2880_TNRDMD_DIVERMODE_MAIN) {
				ret =
				    cxd2880_tnrdmd_dvbt2_mon_sync_stat_sub
				    (tnr_dmd, &sync_state, &unlock_detected);
				if (ret != CXD2880_RESULT_OK) {
					slvt_unfreeze_reg(tnr_dmd);
					return ret;
				}

				if (sync_state < 5) {
					slvt_unfreeze_reg(tnr_dmd);
					return CXD2880_RESULT_ERROR_HW_STATE;
				}
			} else {
				slvt_unfreeze_reg(tnr_dmd);
				return CXD2880_RESULT_ERROR_HW_STATE;
			}
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x5F, &data,
					   sizeof(data)) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		*l1_change = data & 0x01;
		if (*l1_change) {
			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x00,
						   0x22) != CXD2880_RESULT_OK) {
				slvt_unfreeze_reg(tnr_dmd);
				return CXD2880_RESULT_ERROR_IO;
			}

			if (tnr_dmd->io->write_reg(tnr_dmd->io,
						   CXD2880_IO_TGT_DMD, 0x16,
						   0x01) != CXD2880_RESULT_OK) {
				slvt_unfreeze_reg(tnr_dmd);
				return CXD2880_RESULT_ERROR_IO;
			}
		}
		slvt_unfreeze_reg(tnr_dmd);
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_l1_post(struct cxd2880_tnrdmd
						  *tnr_dmd,
						  struct cxd2880_dvbt2_l1post
						  *l1_post)
{
	if ((!tnr_dmd) || (!l1_post))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 data[16];

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x86, data,
					   sizeof(data)) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (!(data[0] & 0x01))
			return CXD2880_RESULT_ERROR_HW_STATE;

		l1_post->sub_slices_per_frame = (data[1] & 0x7F) << 8;
		l1_post->sub_slices_per_frame |= data[2];
		l1_post->num_plps = data[3];
		l1_post->num_aux = data[4] & 0x0F;
		l1_post->aux_cfg_rfu = data[5];
		l1_post->rf_idx = data[6] & 0x07;
		l1_post->freq = data[7] << 24;
		l1_post->freq |= data[8] << 16;
		l1_post->freq |= data[9] << 8;
		l1_post->freq |= data[10];
		l1_post->fef_type = data[11] & 0x0F;
		l1_post->fef_length = data[12] << 16;
		l1_post->fef_length |= data[13] << 8;
		l1_post->fef_length |= data[14];
		l1_post->fef_intvl = data[15];
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_bbheader(struct cxd2880_tnrdmd
						   *tnr_dmd,
						   enum cxd2880_dvbt2_plp_btype
						   type,
						   struct cxd2880_dvbt2_bbheader
						   *bbheader)
{
	if ((!tnr_dmd) || (!bbheader))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	{
		enum cxd2880_ret ret = CXD2880_RESULT_OK;
		u8 sync_state = 0;
		u8 ts_lock = 0;
		u8 unlock_detected = 0;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_state,
						       &ts_lock,
						       &unlock_detected);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return ret;
		}

		if (!ts_lock) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0B) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (type == CXD2880_DVBT2_PLP_COMMON) {
		u8 l1_post_ok;
		u8 data;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x86,
					   &l1_post_ok,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (!(l1_post_ok & 0x01)) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xB6, &data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (data == 0) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}
	}

	{
		u8 data[14];
		u8 addr = 0;

		if (type == CXD2880_DVBT2_PLP_COMMON)
			addr = 0x51;
		else
			addr = 0x42;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, addr, data,
					   sizeof(data)) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		slvt_unfreeze_reg(tnr_dmd);

		bbheader->stream_input =
		    (enum cxd2880_dvbt2_stream)((data[0] >> 6) & 0x03);
		bbheader->is_single_input_stream = (u8)((data[0] >> 5) & 0x01);
		bbheader->is_constant_coding_modulation =
		    (u8)((data[0] >> 4) & 0x01);
		bbheader->issy_indicator = (u8)((data[0] >> 3) & 0x01);
		bbheader->null_packet_deletion = (u8)((data[0] >> 2) & 0x01);
		bbheader->ext = (u8)(data[0] & 0x03);

		bbheader->input_stream_identifier = data[1];
		bbheader->plp_mode =
		    (data[3] & 0x01) ? CXD2880_DVBT2_PLP_MODE_HEM :
		    CXD2880_DVBT2_PLP_MODE_NM;
		bbheader->data_field_length = (u16)((data[4] << 8) | data[5]);

		if (bbheader->plp_mode == CXD2880_DVBT2_PLP_MODE_NM) {
			bbheader->user_packet_length =
			    (u16)((data[6] << 8) | data[7]);
			bbheader->sync_byte = data[8];
			bbheader->issy = 0;
		} else {
			bbheader->user_packet_length = 0;
			bbheader->sync_byte = 0;
			bbheader->issy =
			    (u32)((data[11] << 16) | (data[12] << 8) |
				   data[13]);
		}
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_in_bandb_ts_rate(struct cxd2880_tnrdmd
						   *tnr_dmd,
						   enum
						   cxd2880_dvbt2_plp_btype
						   type,
						   u32 *ts_rate_bps)
{
	if ((!tnr_dmd) || (!ts_rate_bps))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	{
		enum cxd2880_ret ret = CXD2880_RESULT_OK;
		u8 sync_state = 0;
		u8 ts_lock = 0;
		u8 unlock_detected = 0;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_state,
						       &ts_lock,
						       &unlock_detected);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return ret;
		}

		if (!ts_lock) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0B) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	{
		u8 l1_post_ok = 0;
		u8 addr = 0;
		u8 data = 0;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x86,
					   &l1_post_ok,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (!(l1_post_ok & 0x01)) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		if (type == CXD2880_DVBT2_PLP_COMMON)
			addr = 0xBA;
		else
			addr = 0xA7;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, addr, &data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if ((data & 0x80) == 0x00) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}
	}

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x25) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	{
		u8 data[4];
		u8 addr = 0;

		if (type == CXD2880_DVBT2_PLP_COMMON)
			addr = 0xA6;
		else
			addr = 0xAA;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, addr, &data[0],
					   4) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		*ts_rate_bps =
		    (u32)(((data[0] & 0x07) << 24) | (data[1] << 16) |
			   (data[2] << 8) | data[3]);
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_spectrum_sense(struct cxd2880_tnrdmd
						 *tnr_dmd,
						 enum
						 cxd2880_tnrdmd_spectrum_sense
						 *sense)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	u8 sync_state = 0;
	u8 ts_lock = 0;
	u8 early_unlock = 0;

	if ((!tnr_dmd) || (!sense))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	ret =
	    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_state, &ts_lock,
					       &early_unlock);
	if (ret != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return ret;
	}

	if (sync_state != 6) {
		slvt_unfreeze_reg(tnr_dmd);

		ret = CXD2880_RESULT_ERROR_HW_STATE;

		if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_MAIN)
			ret =
			    cxd2880_tnrdmd_dvbt2_mon_spectrum_sense(
				tnr_dmd->diver_sub, sense);

		return ret;
	}

	{
		u8 data = 0;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x2F, &data,
					   sizeof(data)) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		slvt_unfreeze_reg(tnr_dmd);

		*sense =
		    (data & 0x01) ? CXD2880_TNRDMD_SPECTRUM_INV :
		    CXD2880_TNRDMD_SPECTRUM_NORMAL;
	}

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret dvbt2_read_snr_reg(struct cxd2880_tnrdmd *tnr_dmd,
					   u16 *reg_value)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!reg_value))
		return CXD2880_RESULT_ERROR_ARG;

	{
		u8 sync_state = 0;
		u8 ts_lock = 0;
		u8 unlock_detected = 0;
		u8 data[2];

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_state,
						       &ts_lock,
						       &unlock_detected);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (sync_state != 6) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x13, data,
					   sizeof(data)) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		slvt_unfreeze_reg(tnr_dmd);

		*reg_value = (data[0] << 8) | data[1];
	}

	return ret;
}

static enum cxd2880_ret dvbt2_calc_snr(struct cxd2880_tnrdmd *tnr_dmd,
				       u32 reg_value, int *snr)
{
	if ((!tnr_dmd) || (!snr))
		return CXD2880_RESULT_ERROR_ARG;

	if (reg_value == 0)
		return CXD2880_RESULT_ERROR_HW_STATE;

	if (reg_value > 10876)
		reg_value = 10876;

	*snr =
	    10 * 10 * ((int)cxd2880_math_log10(reg_value) -
		       (int)cxd2880_math_log10(12600 - reg_value));
	*snr += 32000;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_snr(struct cxd2880_tnrdmd *tnr_dmd,
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

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SINGLE) {
		ret = dvbt2_read_snr_reg(tnr_dmd, &reg_value);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		ret = dvbt2_calc_snr(tnr_dmd, reg_value, snr);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	} else {
		int snr_main = 0;
		int snr_sub = 0;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_snr_diver(tnr_dmd, snr, &snr_main,
						       &snr_sub);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_snr_diver(struct cxd2880_tnrdmd
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

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = dvbt2_read_snr_reg(tnr_dmd, &reg_value);
	if (ret == CXD2880_RESULT_OK) {
		ret = dvbt2_calc_snr(tnr_dmd, reg_value, snr_main);
		if (ret != CXD2880_RESULT_OK)
			reg_value = 0;
	} else if (ret == CXD2880_RESULT_ERROR_HW_STATE) {
		reg_value = 0;
	} else {
		return ret;
	}

	reg_value_sum += reg_value;

	ret = dvbt2_read_snr_reg(tnr_dmd->diver_sub, &reg_value);
	if (ret == CXD2880_RESULT_OK) {
		ret = dvbt2_calc_snr(tnr_dmd->diver_sub, reg_value, snr_sub);
		if (ret != CXD2880_RESULT_OK)
			reg_value = 0;
	} else if (ret == CXD2880_RESULT_ERROR_HW_STATE) {
		reg_value = 0;
	} else {
		return ret;
	}

	reg_value_sum += reg_value;

	ret = dvbt2_calc_snr(tnr_dmd, reg_value_sum, snr);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_pre_ldpcber(struct cxd2880_tnrdmd
						      *tnr_dmd, u32 *ber)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	u32 bit_error = 0;
	u32 period_exp = 0;
	u32 n_ldpc = 0;

	if ((!tnr_dmd) || (!ber))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0B) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	{
		u8 data[5];

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x3C, data,
					   sizeof(data)) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (!(data[0] & 0x01)) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		bit_error =
		    ((data[1] & 0x0F) << 24) | (data[2] << 16) | (data[3] << 8)
		    | data[4];

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xA0, data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (((enum cxd2880_dvbt2_plp_fec)(data[0] & 0x03)) ==
		    CXD2880_DVBT2_FEC_LDPC_16K)
			n_ldpc = 16200;
		else
			n_ldpc = 64800;

		slvt_unfreeze_reg(tnr_dmd);

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x20) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x6F, data,
					   1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		period_exp = data[0] & 0x0F;
	}

	if (bit_error > ((1U << period_exp) * n_ldpc))
		return CXD2880_RESULT_ERROR_HW_STATE;

	{
		u32 div = 0;
		u32 Q = 0;
		u32 R = 0;

		if (period_exp >= 4) {
			div = (1U << (period_exp - 4)) * (n_ldpc / 200);

			Q = (bit_error * 5) / div;
			R = (bit_error * 5) % div;

			R *= 625;
			Q = Q * 625 + R / div;
			R = R % div;
		} else {
			div = (1U << period_exp) * (n_ldpc / 200);

			Q = (bit_error * 10) / div;
			R = (bit_error * 10) % div;

			R *= 5000;
			Q = Q * 5000 + R / div;
			R = R % div;
		}

		if (div / 2 <= R)
			*ber = Q + 1;
		else
			*ber = Q;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_post_bchfer(struct cxd2880_tnrdmd
						      *tnr_dmd, u32 *fer)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	u32 fec_error = 0;
	u32 period = 0;

	if ((!tnr_dmd) || (!fer))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0B) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	{
		u8 data[2];

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x1B, data,
					   2) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (!(data[0] & 0x80))
			return CXD2880_RESULT_ERROR_HW_STATE;

		fec_error = ((data[0] & 0x7F) << 8) | (data[1]);

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x20) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x72, data,
					   1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		period = (1 << (data[0] & 0x0F));
	}

	if ((period == 0) || (fec_error > period))
		return CXD2880_RESULT_ERROR_HW_STATE;

	{
		u32 div = 0;
		u32 Q = 0;
		u32 R = 0;

		div = period;

		Q = (fec_error * 1000) / div;
		R = (fec_error * 1000) % div;

		R *= 1000;
		Q = Q * 1000 + R / div;
		R = R % div;

		if ((div != 1) && (div / 2 <= R))
			*fer = Q + 1;
		else
			*fer = Q;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_pre_bchber(struct cxd2880_tnrdmd
						     *tnr_dmd, u32 *ber)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	u32 bit_error = 0;
	u32 period_exp = 0;
	u32 n_bch = 0;

	if ((!tnr_dmd) || (!ber))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 data[3];
		enum cxd2880_dvbt2_plp_fec plp_fec_type =
		    CXD2880_DVBT2_FEC_LDPC_16K;
		enum cxd2880_dvbt2_plp_code_rate plp_cr = CXD2880_DVBT2_R1_2;

		static const u16 n_bch_bits_lookup[2][8] = {
			{7200, 9720, 10800, 11880, 12600, 13320, 5400, 6480},
			{32400, 38880, 43200, 48600, 51840, 54000, 21600, 25920}
		};

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x15, data,
					   3) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (!(data[0] & 0x40)) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		bit_error = ((data[0] & 0x3F) << 16) | (data[1] << 8) | data[2];

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x9D, data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		plp_cr = (enum cxd2880_dvbt2_plp_code_rate)(data[0] & 0x07);

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xA0, data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		plp_fec_type = (enum cxd2880_dvbt2_plp_fec)(data[0] & 0x03);

		slvt_unfreeze_reg(tnr_dmd);

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x20) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x72, data,
					   1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		period_exp = data[0] & 0x0F;

		if ((plp_fec_type > CXD2880_DVBT2_FEC_LDPC_64K) ||
		    (plp_cr > CXD2880_DVBT2_R2_5))
			return CXD2880_RESULT_ERROR_HW_STATE;

		n_bch = n_bch_bits_lookup[plp_fec_type][plp_cr];
	}

	if (bit_error > ((1U << period_exp) * n_bch))
		return CXD2880_RESULT_ERROR_HW_STATE;

	{
		u32 div = 0;
		u32 Q = 0;
		u32 R = 0;

		if (period_exp >= 6) {
			div = (1U << (period_exp - 6)) * (n_bch / 40);

			Q = (bit_error * 625) / div;
			R = (bit_error * 625) % div;

			R *= 625;
			Q = Q * 625 + R / div;
			R = R % div;
		} else {
			div = (1U << period_exp) * (n_bch / 40);

			Q = (bit_error * 1000) / div;
			R = (bit_error * 1000) % div;

			R *= 25000;
			Q = Q * 25000 + R / div;
			R = R % div;
		}

		if (div / 2 <= R)
			*ber = Q + 1;
		else
			*ber = Q;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_packet_error_number(struct
							      cxd2880_tnrdmd
							      *tnr_dmd,
							      u32 *pen)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	u8 data[3];

	if ((!tnr_dmd) || (!pen))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0B) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x39, data,
				   sizeof(data)) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (!(data[0] & 0x01))
		return CXD2880_RESULT_ERROR_HW_STATE;

	*pen = ((data[1] << 8) | data[2]);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_sampling_offset(struct cxd2880_tnrdmd
							  *tnr_dmd, int *ppm)
{
	if ((!tnr_dmd) || (!ppm))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 ctl_val_reg[5];
		u8 nominal_rate_reg[5];
		u32 trl_ctl_val = 0;
		u32 trcg_nominal_rate = 0;
		int num;
		int den;
		enum cxd2880_ret ret = CXD2880_RESULT_OK;
		u8 sync_state = 0;
		u8 ts_lock = 0;
		u8 unlock_detected = 0;
		s8 diff_upper = 0;

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_state,
						       &ts_lock,
						       &unlock_detected);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (sync_state != 6) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x34,
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
					   CXD2880_IO_TGT_DMD, 0x10,
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

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_sampling_offset_sub(struct
							      cxd2880_tnrdmd
							      *tnr_dmd,
							      int *ppm)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!ppm))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	ret = cxd2880_tnrdmd_dvbt2_mon_sampling_offset(tnr_dmd->diver_sub, ppm);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_quality(struct cxd2880_tnrdmd
						  *tnr_dmd, u8 *quality)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	int snr = 0;
	int snr_rel = 0;
	u32 ber = 0;
	u32 ber_sqi = 0;
	enum cxd2880_dvbt2_plp_constell qam;
	enum cxd2880_dvbt2_plp_code_rate code_rate;

	static const int snr_nordig_p1_db_1000[4][8] = {
		{3500, 4700, 5600, 6600, 7200, 7700, 1300, 2200},
		{8700, 10100, 11400, 12500, 13300, 13800, 6000, 7200},
		{13000, 14800, 16200, 17700, 18700, 19400, 9800, 11100},
		{17000, 19400, 20800, 22900, 24300, 25100, 13200, 14800},
	};

	if ((!tnr_dmd) || (!quality))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = cxd2880_tnrdmd_dvbt2_mon_pre_bchber(tnr_dmd, &ber);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = cxd2880_tnrdmd_dvbt2_mon_snr(tnr_dmd, &snr);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret =
	    cxd2880_tnrdmd_dvbt2_mon_qam(tnr_dmd, CXD2880_DVBT2_PLP_DATA, &qam);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret =
	    cxd2880_tnrdmd_dvbt2_mon_code_rate(tnr_dmd, CXD2880_DVBT2_PLP_DATA,
					       &code_rate);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if ((code_rate > CXD2880_DVBT2_R2_5) || (qam > CXD2880_DVBT2_QAM256))
		return CXD2880_RESULT_ERROR_OTHER;

	if (ber > 100000)
		ber_sqi = 0;
	else if (ber >= 100)
		ber_sqi = 6667;
	else
		ber_sqi = 16667;

	snr_rel = snr - snr_nordig_p1_db_1000[qam][code_rate];

	if (snr_rel < -3000) {
		*quality = 0;
	} else if (snr_rel <= 3000) {
		u32 temp_sqi =
		    (((snr_rel + 3000) * ber_sqi) + 500000) / 1000000;
		*quality = (temp_sqi > 100) ? 100 : (u8)temp_sqi;
	} else {
		*quality = 100;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_ts_rate(struct cxd2880_tnrdmd
						  *tnr_dmd, u32 *ts_rate_kbps)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	u32 rd_smooth_dp = 0;
	u32 ep_ck_nume = 0;
	u32 ep_ck_deno = 0;
	u8 issy_on_data = 0;

	if ((!tnr_dmd) || (!ts_rate_kbps))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 data[12];
		u8 sync_state = 0;
		u8 ts_lock = 0;
		u8 unlock_detected = 0;

		if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_sync_stat(tnr_dmd, &sync_state,
						       &ts_lock,
						       &unlock_detected);
		if (ret != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (!ts_lock) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x23, data,
					   12) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		rd_smooth_dp = (u32)((data[0] & 0x1F) << 24);
		rd_smooth_dp |= (u32)(data[1] << 16);
		rd_smooth_dp |= (u32)(data[2] << 8);
		rd_smooth_dp |= (u32)data[3];

		if (rd_smooth_dp < 214958) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		ep_ck_nume = (u32)((data[4] & 0x3F) << 24);
		ep_ck_nume |= (u32)(data[5] << 16);
		ep_ck_nume |= (u32)(data[6] << 8);
		ep_ck_nume |= (u32)data[7];

		ep_ck_deno = (u32)((data[8] & 0x3F) << 24);
		ep_ck_deno |= (u32)(data[9] << 16);
		ep_ck_deno |= (u32)(data[10] << 8);
		ep_ck_deno |= (u32)data[11];

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x41, data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		issy_on_data = data[0] & 0x01;

		slvt_unfreeze_reg(tnr_dmd);
	}

	if (issy_on_data) {
		if ((ep_ck_deno == 0) || (ep_ck_nume == 0) ||
		    (ep_ck_deno >= ep_ck_nume))
			return CXD2880_RESULT_ERROR_HW_STATE;
	}

	{
		u32 ick_x100;
		u32 div = 0;
		u32 Q = 0;
		u32 R = 0;

		switch (tnr_dmd->clk_mode) {
		case CXD2880_TNRDMD_CLOCKMODE_A:
			ick_x100 = 8228;
			break;
		case CXD2880_TNRDMD_CLOCKMODE_B:
			ick_x100 = 9330;
			break;
		case CXD2880_TNRDMD_CLOCKMODE_C:
			ick_x100 = 9600;
			break;
		default:
			return CXD2880_RESULT_ERROR_SW_STATE;
		}

		div = rd_smooth_dp;

		Q = ick_x100 * 262144U / div;
		R = ick_x100 * 262144U % div;

		R *= 5U;
		Q = Q * 5 + R / div;
		R = R % div;

		R *= 2U;
		Q = Q * 2 + R / div;
		R = R % div;

		if (div / 2 <= R)
			*ts_rate_kbps = Q + 1;
		else
			*ts_rate_kbps = Q;
	}

	if (issy_on_data) {
		u32 diff = ep_ck_nume - ep_ck_deno;

		while (diff > 0x7FFF) {
			diff >>= 1;
			ep_ck_nume >>= 1;
		}

		*ts_rate_kbps -=
		    (*ts_rate_kbps * diff + ep_ck_nume / 2) / ep_ck_nume;
	}

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_per(struct cxd2880_tnrdmd *tnr_dmd,
					      u32 *per)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	u32 packet_error = 0;
	u32 period = 0;

	if (!tnr_dmd || !per)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;
	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 rdata[3];

		if (tnr_dmd->io->write_reg(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x00,
					   0x0B) != CXD2880_RESULT_OK)
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
					   0x24) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xDC, rdata,
					   1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		period = 1U << (rdata[0] & 0x0F);
	}

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

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_qam(struct cxd2880_tnrdmd *tnr_dmd,
					      enum cxd2880_dvbt2_plp_btype type,
					      enum cxd2880_dvbt2_plp_constell
					      *qam)
{
	u8 data;
	u8 l1_post_ok = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!qam))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0B) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x86, &l1_post_ok,
				   1) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (!(l1_post_ok & 0x01)) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_HW_STATE;
	}

	if (type == CXD2880_DVBT2_PLP_COMMON) {
		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xB6, &data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (data == 0) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xB1, &data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}
	} else {
		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x9E, &data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}
	}

	slvt_unfreeze_reg(tnr_dmd);

	*qam = (enum cxd2880_dvbt2_plp_constell)(data & 0x07);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_code_rate(struct cxd2880_tnrdmd
						    *tnr_dmd,
						    enum cxd2880_dvbt2_plp_btype
						    type,
						    enum
						    cxd2880_dvbt2_plp_code_rate
						    *code_rate)
{
	u8 data;
	u8 l1_post_ok = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!code_rate))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnr_dmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0B) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnr_dmd->io->read_regs(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x86, &l1_post_ok,
				   1) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (!(l1_post_ok & 0x01)) {
		slvt_unfreeze_reg(tnr_dmd);
		return CXD2880_RESULT_ERROR_HW_STATE;
	}

	if (type == CXD2880_DVBT2_PLP_COMMON) {
		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xB6, &data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (data == 0) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0xB0, &data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}
	} else {
		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x9D, &data,
					   1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnr_dmd);
			return CXD2880_RESULT_ERROR_IO;
		}
	}

	slvt_unfreeze_reg(tnr_dmd);

	*code_rate = (enum cxd2880_dvbt2_plp_code_rate)(data & 0x07);

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_profile(struct cxd2880_tnrdmd
						  *tnr_dmd,
						  enum cxd2880_dvbt2_profile
						  *profile)
{
	if ((!tnr_dmd) || (!profile))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->io->write_reg(tnr_dmd->io,
				   CXD2880_IO_TGT_DMD, 0x00,
				   0x0B) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	{
		u8 data;

		if (tnr_dmd->io->read_regs(tnr_dmd->io,
					   CXD2880_IO_TGT_DMD, 0x22, &data,
					   sizeof(data)) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (data & 0x02) {
			if (data & 0x01)
				*profile = CXD2880_DVBT2_PROFILE_LITE;
			else
				*profile = CXD2880_DVBT2_PROFILE_BASE;
		} else {
			enum cxd2880_ret ret = CXD2880_RESULT_ERROR_HW_STATE;

			if (tnr_dmd->diver_mode ==
			    CXD2880_TNRDMD_DIVERMODE_MAIN)
				ret =
				    cxd2880_tnrdmd_dvbt2_mon_profile(
					tnr_dmd->diver_sub, profile);

			return ret;
		}
	}

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret dvbt2_calc_sdi(struct cxd2880_tnrdmd *tnr_dmd,
				       int rf_lvl, u8 *ssi)
{
	enum cxd2880_dvbt2_plp_constell qam;
	enum cxd2880_dvbt2_plp_code_rate code_rate;
	int prel;
	int temp_ssi = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	static const int ref_dbm_1000[4][8] = {
		{-96000, -95000, -94000, -93000, -92000, -92000, -98000,
		 -97000},
		{-91000, -89000, -88000, -87000, -86000, -86000, -93000,
		 -92000},
		{-86000, -85000, -83000, -82000, -81000, -80000, -89000,
		 -88000},
		{-82000, -80000, -78000, -76000, -75000, -74000, -86000,
		 -84000},
	};

	if ((!tnr_dmd) || (!ssi))
		return CXD2880_RESULT_ERROR_ARG;

	ret =
	    cxd2880_tnrdmd_dvbt2_mon_qam(tnr_dmd, CXD2880_DVBT2_PLP_DATA, &qam);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret =
	    cxd2880_tnrdmd_dvbt2_mon_code_rate(tnr_dmd, CXD2880_DVBT2_PLP_DATA,
					       &code_rate);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if ((code_rate > CXD2880_DVBT2_R2_5) || (qam > CXD2880_DVBT2_QAM256))
		return CXD2880_RESULT_ERROR_OTHER;

	prel = rf_lvl - ref_dbm_1000[qam][code_rate];

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

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_ssi(struct cxd2880_tnrdmd *tnr_dmd,
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

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = cxd2880_tnrdmd_mon_rf_lvl(tnr_dmd, &rf_lvl);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = dvbt2_calc_sdi(tnr_dmd, rf_lvl, ssi);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	return ret;
}

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_ssi_sub(struct cxd2880_tnrdmd
						  *tnr_dmd, u8 *ssi)
{
	int rf_lvl = 0;
	enum cxd2880_ret ret = CXD2880_RESULT_OK;

	if ((!tnr_dmd) || (!ssi))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode != CXD2880_TNRDMD_DIVERMODE_MAIN)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnr_dmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = cxd2880_tnrdmd_mon_rf_lvl(tnr_dmd->diver_sub, &rf_lvl);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = dvbt2_calc_sdi(tnr_dmd, rf_lvl, ssi);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	return ret;
}
