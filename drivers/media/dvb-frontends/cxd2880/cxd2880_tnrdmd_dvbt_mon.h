/*
 * cxd2880_tnrdmd_dvbt_mon.h
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * DVB-T monitor interface
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

#ifndef CXD2880_TNRDMD_DVBT_MON_H
#define CXD2880_TNRDMD_DVBT_MON_H

#include "cxd2880_tnrdmd.h"
#include "cxd2880_dvbt.h"

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_sync_stat(struct cxd2880_tnrdmd
						   *tnr_dmd, u8 *sync_stat,
						   u8 *ts_lock_stat,
						   u8 *unlock_detected);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_sync_stat_sub(struct cxd2880_tnrdmd
						       *tnr_dmd, u8 *sync_stat,
						       u8 *unlock_detected);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_mode_guard(struct cxd2880_tnrdmd
						    *tnr_dmd,
						    enum cxd2880_dvbt_mode
						    *mode,
						    enum cxd2880_dvbt_guard
						    *guard);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_carrier_offset(struct cxd2880_tnrdmd
							*tnr_dmd, int *offset);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_carrier_offset_sub(struct
							    cxd2880_tnrdmd
							    *tnr_dmd,
							    int *offset);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_pre_viterbiber(struct cxd2880_tnrdmd
							*tnr_dmd, u32 *ber);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_pre_rsber(struct cxd2880_tnrdmd
						   *tnr_dmd, u32 *ber);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_tps_info(struct cxd2880_tnrdmd
						  *tnr_dmd,
						  struct cxd2880_dvbt_tpsinfo
						  *info);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_packet_error_number(struct
							     cxd2880_tnrdmd
							     *tnr_dmd,
							     u32 *pen);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_spectrum_sense(struct cxd2880_tnrdmd
						*tnr_dmd,
						enum
						cxd2880_tnrdmd_spectrum_sense
						*sense);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_snr(struct cxd2880_tnrdmd *tnr_dmd,
					     int *snr);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_snr_diver(struct cxd2880_tnrdmd
						   *tnr_dmd, int *snr,
						   int *snr_main, int *snr_sub);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_sampling_offset(struct cxd2880_tnrdmd
							 *tnr_dmd, int *ppm);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_sampling_offset_sub(struct
							     cxd2880_tnrdmd
							     *tnr_dmd,
							     int *ppm);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_quality(struct cxd2880_tnrdmd *tnr_dmd,
						 u8 *quality);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_per(struct cxd2880_tnrdmd *tnr_dmd,
					     u32 *per);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_ssi(struct cxd2880_tnrdmd *tnr_dmd,
					     u8 *ssi);

enum cxd2880_ret cxd2880_tnrdmd_dvbt_mon_ssi_sub(struct cxd2880_tnrdmd *tnr_dmd,
						 u8 *ssi);

#endif
