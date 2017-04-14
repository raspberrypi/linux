/*
 * cxd2880_dvbt.h
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * DVB-T related definitions
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

#ifndef CXD2880_DVBT_H
#define CXD2880_DVBT_H

#include "cxd2880_common.h"

enum cxd2880_dvbt_constellation {
	CXD2880_DVBT_CONSTELLATION_QPSK,
	CXD2880_DVBT_CONSTELLATION_16QAM,
	CXD2880_DVBT_CONSTELLATION_64QAM,
	CXD2880_DVBT_CONSTELLATION_RESERVED_3
};

enum cxd2880_dvbt_hierarchy {
	CXD2880_DVBT_HIERARCHY_NON,
	CXD2880_DVBT_HIERARCHY_1,
	CXD2880_DVBT_HIERARCHY_2,
	CXD2880_DVBT_HIERARCHY_4
};

enum cxd2880_dvbt_coderate {
	CXD2880_DVBT_CODERATE_1_2,
	CXD2880_DVBT_CODERATE_2_3,
	CXD2880_DVBT_CODERATE_3_4,
	CXD2880_DVBT_CODERATE_5_6,
	CXD2880_DVBT_CODERATE_7_8,
	CXD2880_DVBT_CODERATE_RESERVED_5,
	CXD2880_DVBT_CODERATE_RESERVED_6,
	CXD2880_DVBT_CODERATE_RESERVED_7
};

enum cxd2880_dvbt_guard {
	CXD2880_DVBT_GUARD_1_32,
	CXD2880_DVBT_GUARD_1_16,
	CXD2880_DVBT_GUARD_1_8,
	CXD2880_DVBT_GUARD_1_4
};

enum cxd2880_dvbt_mode {
	CXD2880_DVBT_MODE_2K,
	CXD2880_DVBT_MODE_8K,
	CXD2880_DVBT_MODE_RESERVED_2,
	CXD2880_DVBT_MODE_RESERVED_3
};

enum cxd2880_dvbt_profile {
	CXD2880_DVBT_PROFILE_HP = 0,
	CXD2880_DVBT_PROFILE_LP
};

struct cxd2880_dvbt_tpsinfo {
	enum cxd2880_dvbt_constellation constellation;
	enum cxd2880_dvbt_hierarchy hierarchy;
	enum cxd2880_dvbt_coderate rate_hp;
	enum cxd2880_dvbt_coderate rate_lp;
	enum cxd2880_dvbt_guard guard;
	enum cxd2880_dvbt_mode mode;
	u8 fnum;
	u8 length_indicator;
	u16 cell_id;
	u8 cell_id_ok;
	u8 reserved_even;
	u8 reserved_odd;
};

#endif
