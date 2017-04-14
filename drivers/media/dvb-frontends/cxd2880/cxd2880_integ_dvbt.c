/*
 * cxd2880_integ_dvbt.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * integration layer functions for DVB-T
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
#include "cxd2880_integ_dvbt.h"

static enum cxd2880_ret dvbt_wait_demod_lock(struct cxd2880_tnrdmd *tnr_dmd);

enum cxd2880_ret cxd2880_integ_dvbt_tune(struct cxd2880_tnrdmd *tnr_dmd,
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

	cxd2880_atomic_set(&tnr_dmd->cancel, 0);

	if ((tune_param->bandwidth != CXD2880_DTV_BW_5_MHZ) &&
	    (tune_param->bandwidth != CXD2880_DTV_BW_6_MHZ) &&
	    (tune_param->bandwidth != CXD2880_DTV_BW_7_MHZ) &&
	    (tune_param->bandwidth != CXD2880_DTV_BW_8_MHZ)) {
		return CXD2880_RESULT_ERROR_NOSUPPORT;
	}

	ret = cxd2880_tnrdmd_dvbt_tune1(tnr_dmd, tune_param);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	CXD2880_SLEEP(CXD2880_TNRDMD_WAIT_AGC_STABLE);

	ret = cxd2880_tnrdmd_dvbt_tune2(tnr_dmd, tune_param);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = dvbt_wait_demod_lock(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	return ret;
}

enum cxd2880_ret cxd2880_integ_dvbt_wait_ts_lock(struct cxd2880_tnrdmd *tnr_dmd)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	enum cxd2880_tnrdmd_lock_result lock =
	    CXD2880_TNRDMD_LOCK_RESULT_NOTDETECT;
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

	for (;;) {
		ret = cxd2880_stopwatch_elapsed(&timer, &elapsed);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (elapsed >= CXD2880_DVBT_WAIT_TS_LOCK)
			continue_wait = 0;

		ret = cxd2880_tnrdmd_dvbt_check_ts_lock(tnr_dmd, &lock);
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
					    CXD2880_DVBT_WAIT_LOCK_INTVL);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		} else {
			ret = CXD2880_RESULT_ERROR_TIMEOUT;
			break;
		}
	}

	return ret;
}

static enum cxd2880_ret dvbt_wait_demod_lock(struct cxd2880_tnrdmd *tnr_dmd)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	enum cxd2880_tnrdmd_lock_result lock =
	    CXD2880_TNRDMD_LOCK_RESULT_NOTDETECT;
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

	for (;;) {
		ret = cxd2880_stopwatch_elapsed(&timer, &elapsed);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (elapsed >= CXD2880_DVBT_WAIT_DMD_LOCK)
			continue_wait = 0;

		ret = cxd2880_tnrdmd_dvbt_check_demod_lock(tnr_dmd, &lock);
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
					    CXD2880_DVBT_WAIT_LOCK_INTVL);
			if (ret != CXD2880_RESULT_OK)
				return ret;
		} else {
			ret = CXD2880_RESULT_ERROR_TIMEOUT;
			break;
		}
	}

	return ret;
}
