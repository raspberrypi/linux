/*
 * cxd2880_io.h
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * register I/O interface definitions
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

#ifndef CXD2880_IO_H
#define CXD2880_IO_H

#include "cxd2880_common.h"

enum cxd2880_io_tgt {
	CXD2880_IO_TGT_SYS,
	CXD2880_IO_TGT_DMD
};

struct cxd2880_io {
	enum cxd2880_ret (*read_regs)(struct cxd2880_io *io,
				       enum cxd2880_io_tgt tgt, u8 sub_address,
				       u8 *data, u32 size);
	enum cxd2880_ret (*write_regs)(struct cxd2880_io *io,
					enum cxd2880_io_tgt tgt, u8 sub_address,
					const u8 *data, u32 size);
	enum cxd2880_ret (*write_reg)(struct cxd2880_io *io,
				       enum cxd2880_io_tgt tgt, u8 sub_address,
				       u8 data);
	void *if_object;
	u8 i2c_address_sys;
	u8 i2c_address_demod;
	u8 slave_select;
	void *user;
};

enum cxd2880_ret cxd2880_io_common_write_one_reg(struct cxd2880_io *io,
						 enum cxd2880_io_tgt tgt,
						 u8 sub_address, u8 data);

enum cxd2880_ret cxd2880_io_set_reg_bits(struct cxd2880_io *io,
					 enum cxd2880_io_tgt tgt,
					 u8 sub_address, u8 data, u8 mask);

#endif
