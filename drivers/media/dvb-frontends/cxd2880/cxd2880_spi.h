/*
 * cxd2880_spi.h
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * SPI access definitions
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

#ifndef CXD2880_SPI_H
#define CXD2880_SPI_H

#include "cxd2880_common.h"

enum cxd2880_spi_mode {
	CXD2880_SPI_MODE_0,
	CXD2880_SPI_MODE_1,
	CXD2880_SPI_MODE_2,
	CXD2880_SPI_MODE_3
};

struct cxd2880_spi {
	enum cxd2880_ret (*read)(struct cxd2880_spi *spi, u8 *data,
				  u32 size);
	enum cxd2880_ret (*write)(struct cxd2880_spi *spi, const u8 *data,
				   u32 size);
	enum cxd2880_ret (*write_read)(struct cxd2880_spi *spi,
					const u8 *tx_data, u32 tx_size,
					u8 *rx_data, u32 rx_size);
	u32 flags;
	void *user;
};

#endif
