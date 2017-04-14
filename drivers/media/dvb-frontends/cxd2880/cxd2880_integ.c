/*
 * cxd2880_integ.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * integration layer common functions
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

#include "cxd2880_tnrdmd.h"
#include "cxd2880_tnrdmd_mon.h"
#include "cxd2880_integ.h"

enum cxd2880_ret cxd2880_integ_init(struct cxd2880_tnrdmd *tnr_dmd)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	struct cxd2880_stopwatch timer;
	u32 elapsed_time = 0;
	u8 cpu_task_completed = 0;

	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	ret = cxd2880_tnrdmd_init1(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	ret = cxd2880_stopwatch_start(&timer);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	while (1) {
		ret = cxd2880_stopwatch_elapsed(&timer, &elapsed_time);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		ret =
		    cxd2880_tnrdmd_check_internal_cpu_status(tnr_dmd,
						     &cpu_task_completed);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		if (cpu_task_completed)
			break;

		if (elapsed_time > CXD2880_TNRDMD_WAIT_INIT_TIMEOUT)
			return CXD2880_RESULT_ERROR_TIMEOUT;
		ret =
		    cxd2880_stopwatch_sleep(&timer,
					    CXD2880_TNRDMD_WAIT_INIT_INTVL);
		if (ret != CXD2880_RESULT_OK)
			return ret;
	}

	ret = cxd2880_tnrdmd_init2(tnr_dmd);
	if (ret != CXD2880_RESULT_OK)
		return ret;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_integ_cancel(struct cxd2880_tnrdmd *tnr_dmd)
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	cxd2880_atomic_set(&tnr_dmd->cancel, 1);

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_integ_check_cancellation(struct cxd2880_tnrdmd
						  *tnr_dmd)
{
	if (!tnr_dmd)
		return CXD2880_RESULT_ERROR_ARG;

	if (cxd2880_atomic_read(&tnr_dmd->cancel) != 0)
		return CXD2880_RESULT_ERROR_CANCEL;

	return CXD2880_RESULT_OK;
}
