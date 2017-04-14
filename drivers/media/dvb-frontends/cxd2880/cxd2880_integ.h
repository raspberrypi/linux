/*
 * cxd2880_integ.h
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * integration layer common interface
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

#ifndef CXD2880_INTEG_H
#define CXD2880_INTEG_H

#include "cxd2880_tnrdmd.h"

#define CXD2880_TNRDMD_WAIT_INIT_TIMEOUT	500
#define CXD2880_TNRDMD_WAIT_INIT_INTVL	10

#define CXD2880_TNRDMD_WAIT_AGC_STABLE		100

enum cxd2880_ret cxd2880_integ_init(struct cxd2880_tnrdmd *tnr_dmd);

enum cxd2880_ret cxd2880_integ_cancel(struct cxd2880_tnrdmd *tnr_dmd);

enum cxd2880_ret cxd2880_integ_check_cancellation(struct cxd2880_tnrdmd
						  *tnr_dmd);

#endif
