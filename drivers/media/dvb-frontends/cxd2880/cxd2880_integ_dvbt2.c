/*
 * cxd2880_integ_dvbt2.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * integration layer functions for DVB-T2
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
#include "cxd2880_integ_dvbt2.h"

static enum cxd2880_ret dvbt2_wait_demod_lock(struct cxd2880_tnrdmd *tnr_dmd,
					      enum cxd2880_dvbt2_profile
					      profile);

static enum cxd2880_ret dvbt2_wait_l1_post_lock(struct cxd2880_tnrdmd *tnr_dmd);

enum cxd2880_ret cxd2880_integ_dvbt2_tune(struct cxd2880_tnrdmd *tnr_dmd,
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

	cxd2880_atomic_set(&tnr_dmd->cancel, 0);

	if ((tune_param->bandwidth != CXD2880_DTV_BW_1_7_MHZ) &&
	    (tune_param->bandwidth != CXD2880_DTV_BW_5_MHZ) &&
	    (tune_param->bandwidth != CXD2880_DTV_BW_6_MHZ) &&
	    (tune_param->bandwidth != CXD2880_DTV_BW_7_MHZ) &&
	    (tune_param->bandwidth != CXD2880_DTV_BW_8_MHZ)) {
		return CXD2880_RESULT_ERROR_NOSUPPORT;
	}

	if ((tune_param->profile != CXD2880_DVBT2_PROFILE_BASE) &&
	    (tune_param->profile != CXD2880_DVBT2_PROFILE_LITE))
		return CXD2880_RESULT_ERROR_ARG;

	ret = cxd2880_tnrdmd_dvbt2_tune1(tnr_dmd, tune_param);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	CXD2880_SLEEP(CXD2880_TNRDMD_WAIT_AGC_STABLE);

	ret = cxd2880_tnrdmd_dvbt2_tune2(tnr_dmd, tune_param);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = dvbt2_wait_demod_lock(tnr_dmd, tune_param->profile);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = cxd2880_tnrdmd_dvbt2_diver_fef_setting(tnr_dmd);
	if (ret == CXD2880_RESULT_ERROR_HW_STATE)
		return CXD2880_RESULT_ERROR_UNLOCK;
	else if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = dvbt2_wait_l1_post_lock(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	{
		u8 plp_not_found;

		ret =
		    cxd2880_tnrdmd_dvbt2_mon_data_plp_error(tnr_dmd,
							    &plp_not_found);
		if (ret == CXD2880_RESULT_ERROR_HW_STATE)
			return CXD2880_RESULT_ERROR_UNLOCK;
		else if (ret != CXD2880_RESULT_OK)
			return ret;

		if (plp_not_found) {
			ret = CXD2880_RESULT_OK_CONFIRM;
			tune_param->tune_info =
			    CXD2880_TNRDMD_DVBT2_TUNE_INFO_INVALID_PLP_ID;
		} else {
			tune_param->tune_info =
			    CXD2880_TNRDMD_DVBT2_TUNE_INFO_OK;
		}
	}

	return ret;
}

enum cxd2880_ret cxd2880_integ_dvbt2_wait_ts_lock(struct cxd2880_tnrdmd
						  *tnr_dmd,
						  enum cxd2880_dvbt2_profile
						  profile)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	enum cxd2880_tnrdmd_lock_result lock =
	    CXD2880_TNRDMD_LOCK_RESULT_NOTDETECT;
	u16 timeout = 0;
	struct cxd2880_stopwatch timer;
	u8 continue_wait = 1;
	u32 elapsed = 0;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = cxd2880_stopwatch_start(&timer);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (profile == CXD2880_DVBT2_PROFILE_BASE)
		timeout = CXD2880_DVBT2_BASE_WAIT_TS_LOCK;
	else if (profile == CXD2880_DVBT2_PROFILE_LITE)
		timeout = CXD2880_DVBT2_LITE_WAIT_TS_LOCK;
	else
		return CXD2880_RESULT_ERROR_ARG;

	for (;;) {
		ret = cxd2880_stopwatch_elapsed(&timer, &elapsed);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (elapsed >= timeout)
			continue_wait = 0;

		ret = cxd2880_tnrdmd_dvbt2_check_ts_lock(tnr_dmd, &lock);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		switch (lock) {
		case CXD2880_TNRDMD_LOCK_RESULT_LOCKED:
			return CXD2880_RESULT_OK;

		case CXD2880_TNRDMD_LOCK_RESULT_UNLOCKED:
			return CXD2880_RESULT_ERROR_UNLOCK;

		default:
			break;
		}

		ret = cxd2880_integ_check_cancellation(tnr_dmd);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (continue_wait) {
			ret =
			    cxd2880_stopwatch_sleep(&timer,
					    CXD2880_DVBT2_WAIT_LOCK_INTVL);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		} else {
			ret = CXD2880_RESULT_ERROR_TIMEOUT;
			break;
		}
	}

	return ret;
}

static enum cxd2880_ret dvbt2_wait_demod_lock(struct cxd2880_tnrdmd *tnr_dmd,
					      enum cxd2880_dvbt2_profile
					      profile)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	enum cxd2880_tnrdmd_lock_result lock =
	    CXD2880_TNRDMD_LOCK_RESULT_NOTDETECT;
	u16 timeout = 0;
	struct cxd2880_stopwatch timer;
	u8 continue_wait = 1;
	u32 elapsed = 0;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnr_dmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	ret = cxd2880_stopwatch_start(&timer);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	if (profile == CXD2880_DVBT2_PROFILE_BASE)
		timeout = CXD2880_DVBT2_BASE_WAIT_DMD_LOCK;
	else if ((profile == CXD2880_DVBT2_PROFILE_LITE) ||
		 (profile == CXD2880_DVBT2_PROFILE_ANY))
		timeout = CXD2880_DVBT2_LITE_WAIT_DMD_LOCK;
	else
		return CXD2880_RESULT_ERROR_SW_STATE;

	for (;;) {
		ret = cxd2880_stopwatch_elapsed(&timer, &elapsed);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (elapsed >= timeout)
			continue_wait = 0;

		ret = cxd2880_tnrdmd_dvbt2_check_demod_lock(tnr_dmd, &lock);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		switch (lock) {
		case CXD2880_TNRDMD_LOCK_RESULT_LOCKED:
			return CXD2880_RESULT_OK;

		case CXD2880_TNRDMD_LOCK_RESULT_UNLOCKED:
			return CXD2880_RESULT_ERROR_UNLOCK;

		default:
			break;
		}

		ret = cxd2880_integ_check_cancellation(tnr_dmd);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (continue_wait) {
			ret =
			    cxd2880_stopwatch_sleep(&timer,
					    CXD2880_DVBT2_WAIT_LOCK_INTVL);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		} else {
			ret = CXD2880_RESULT_ERROR_TIMEOUT;
			break;
		}
	}

	return ret;
}

static enum cxd2880_ret dvbt2_wait_l1_post_lock(struct cxd2880_tnrdmd *tnr_dmd)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	struct cxd2880_stopwatch timer;
	u8 continue_wait = 1;
	u32 elapsed = 0;
	u8 l1_post_valid;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	ret = cxd2880_stopwatch_start(&timer);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	for (;;) {
		ret = cxd2880_stopwatch_elapsed(&timer, &elapsed);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (elapsed >= CXD2880_DVBT2_L1POST_TIMEOUT)
			continue_wait = 0;

		ret =
		    cxd2880_tnrdmd_dvbt2_check_l1post_valid(tnr_dmd,
							    &l1_post_valid);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (l1_post_valid)
			return CXD2880_RESULT_OK;

		ret = cxd2880_integ_check_cancellation(tnr_dmd);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (continue_wait) {
			ret =
			    cxd2880_stopwatch_sleep(&timer,
					    CXD2880_DVBT2_WAIT_LOCK_INTVL);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		} else {
			ret = CXD2880_RESULT_ERROR_TIMEOUT;
			break;
		}
	}

	return ret;
}
