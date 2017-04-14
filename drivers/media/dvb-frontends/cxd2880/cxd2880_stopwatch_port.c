/*
 * cxd2880_stopwatch_port.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * time measurement functions
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

#include <linux/ktime.h>
#include <linux/time.h>
#include <linux/timekeeping.h>

static u32 get_time_count(void)
{
	struct timespec tp;

	getnstimeofday(&tp);

	return (u32)((tp.tv_sec * 1000) + (tp.tv_nsec / 1000000));
}

enum cxd2880_ret cxd2880_stopwatch_start(struct cxd2880_stopwatch *stopwatch)
{
	if (!stopwatch)
		return CXD2880_RESULT_ERROR_ARG;

	stopwatch->start_time = get_time_count();

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_stopwatch_sleep(struct cxd2880_stopwatch *stopwatch,
					 u32 ms)
{
	if (!stopwatch)
		return CXD2880_RESULT_ERROR_ARG;
	CXD2880_ARG_UNUSED(*stopwatch);
	CXD2880_SLEEP(ms);

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_stopwatch_elapsed(struct cxd2880_stopwatch *stopwatch,
					   u32 *elapsed)
{
	if (!stopwatch || !elapsed)
		return CXD2880_RESULT_ERROR_ARG;
	*elapsed = get_time_count() - stopwatch->start_time;

	return CXD2880_RESULT_OK;
}
