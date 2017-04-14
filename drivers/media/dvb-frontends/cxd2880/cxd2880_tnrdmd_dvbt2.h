/*
 * cxd2880_tnrdmd_dvbt2.h
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * control interface for DVB-T2
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

#ifndef CXD2880_TNRDMD_DVBT2_H
#define CXD2880_TNRDMD_DVBT2_H

#include "cxd2880_common.h"
#include "cxd2880_tnrdmd.h"

enum cxd2880_tnrdmd_dvbt2_tune_info {
	CXD2880_TNRDMD_DVBT2_TUNE_INFO_OK,
	CXD2880_TNRDMD_DVBT2_TUNE_INFO_INVALID_PLP_ID
};

struct cxd2880_dvbt2_tune_param {
	u32 center_freq_khz;
	enum cxd2880_dtv_bandwidth bandwidth;
	u16 data_plp_id;
	enum cxd2880_dvbt2_profile profile;
	enum cxd2880_tnrdmd_dvbt2_tune_info tune_info;
};

#define CXD2880_DVBT2_TUNE_PARAM_PLPID_AUTO  0xFFFF

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_tune1(struct cxd2880_tnrdmd *tnr_dmd,
					    struct cxd2880_dvbt2_tune_param
					    *tune_param);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_tune2(struct cxd2880_tnrdmd *tnr_dmd,
					    struct cxd2880_dvbt2_tune_param
					    *tune_param);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_sleep_setting(struct cxd2880_tnrdmd
						    *tnr_dmd);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_check_demod_lock(struct cxd2880_tnrdmd
					       *tnr_dmd,
					       enum
					       cxd2880_tnrdmd_lock_result
					       *lock);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_check_ts_lock(struct cxd2880_tnrdmd
						    *tnr_dmd,
						    enum
						    cxd2880_tnrdmd_lock_result
						    *lock);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_set_plp_cfg(struct cxd2880_tnrdmd
						  *tnr_dmd, u8 auto_plp,
						  u8 plp_id);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_diver_fef_setting(struct cxd2880_tnrdmd
							*tnr_dmd);

enum cxd2880_ret cxd2880_tnrdmd_dvbt2_check_l1post_valid(struct cxd2880_tnrdmd
							 *tnr_dmd,
							 u8 *l1_post_valid);

#endif
