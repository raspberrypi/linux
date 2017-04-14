/*
 * cxd2880_tnrdmd_dvbt2_mon.h
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * DVB-T2 monitor interface
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

#ifndef CXD2880_TNRDMD_DVBT2_MON_H
#define CXD2880_TNRDMD_DVBT2_MON_H

#include "cxd2880_tnrdmd.h"
#include "cxd2880_dvbt2.h"

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_sync_stat(struct cxd2880_tnrdmd
						    *tnr_dmd, u8 *sync_stat,
						    u8 *ts_lock_stat,
						    u8 *unlock_detected);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_sync_stat_sub(struct cxd2880_tnrdmd
							*tnr_dmd,
							u8 *sync_stat,
							u8 *unlock_detected);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_carrier_offset(struct cxd2880_tnrdmd
							 *tnr_dmd, int *offset);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_carrier_offset_sub(struct
							     cxd2880_tnrdmd
							     *tnr_dmd,
							     int *offset);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_l1_pre(struct cxd2880_tnrdmd *tnr_dmd,
						 struct cxd2880_dvbt2_l1pre
						 *l1_pre);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_version(struct cxd2880_tnrdmd
						  *tnr_dmd,
						  enum cxd2880_dvbt2_version
						  *ver);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_ofdm(struct cxd2880_tnrdmd *tnr_dmd,
					       struct cxd2880_dvbt2_ofdm *ofdm);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_data_plps(struct cxd2880_tnrdmd
						    *tnr_dmd, u8 *plp_ids,
						    u8 *num_plps);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_active_plp(struct cxd2880_tnrdmd
						     *tnr_dmd,
						     enum
						     cxd2880_dvbt2_plp_btype
						     type,
						     struct cxd2880_dvbt2_plp
						     *plp_info);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_data_plp_error(struct cxd2880_tnrdmd
							 *tnr_dmd,
							 u8 *plp_error);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_l1_change(struct cxd2880_tnrdmd
						    *tnr_dmd, u8 *l1_change);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_l1_post(struct cxd2880_tnrdmd
						  *tnr_dmd,
						  struct cxd2880_dvbt2_l1post
						  *l1_post);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_bbheader(struct cxd2880_tnrdmd
						   *tnr_dmd,
						   enum cxd2880_dvbt2_plp_btype
						   type,
						   struct cxd2880_dvbt2_bbheader
						   *bbheader);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_in_bandb_ts_rate(struct cxd2880_tnrdmd
						   *tnr_dmd,
						   enum
						   cxd2880_dvbt2_plp_btype
						   type,
						   u32 *ts_rate_bps);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_spectrum_sense(struct cxd2880_tnrdmd
						 *tnr_dmd,
						 enum
						 cxd2880_tnrdmd_spectrum_sense
						 *sense);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_snr(struct cxd2880_tnrdmd *tnr_dmd,
					      int *snr);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_snr_diver(struct cxd2880_tnrdmd
						    *tnr_dmd, int *snr,
						    int *snr_main,
						    int *snr_sub);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_pre_ldpcber(struct cxd2880_tnrdmd
						      *tnr_dmd, u32 *ber);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_post_bchfer(struct cxd2880_tnrdmd
						      *tnr_dmd, u32 *fer);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_pre_bchber(struct cxd2880_tnrdmd
						     *tnr_dmd, u32 *ber);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_packet_error_number(struct
							      cxd2880_tnrdmd
							      *tnr_dmd,
							      u32 *pen);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_sampling_offset(struct cxd2880_tnrdmd
							  *tnr_dmd, int *ppm);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_sampling_offset_sub(struct
							      cxd2880_tnrdmd
							      *tnr_dmd,
							      int *ppm);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_ts_rate(struct cxd2880_tnrdmd
						  *tnr_dmd, u32 *ts_rate_kbps);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_quality(struct cxd2880_tnrdmd
						  *tnr_dmd, u8 *quality);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_per(struct cxd2880_tnrdmd *tnr_dmd,
					      u32 *per);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_qam(struct cxd2880_tnrdmd *tnr_dmd,
					      enum cxd2880_dvbt2_plp_btype type,
					      enum cxd2880_dvbt2_plp_constell
					      *qam);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_code_rate(struct cxd2880_tnrdmd
						    *tnr_dmd,
						    enum cxd2880_dvbt2_plp_btype
						    type,
						    enum
						    cxd2880_dvbt2_plp_code_rate
						    *code_rate);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_profile(struct cxd2880_tnrdmd
						  *tnr_dmd,
						  enum cxd2880_dvbt2_profile
						  *profile);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_ssi(struct cxd2880_tnrdmd *tnr_dmd,
					      u8 *ssi);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_mon_ssi_sub(struct cxd2880_tnrdmd
						  *tnr_dmd, u8 *ssi);

#endif
