/*
 * cxd2880_devio_spi.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * I/O interface via SPI
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

#include "cxd2880_devio_spi.h"
#include "cxd2880_stdlib.h"

#define BURST_WRITE_MAX 128

static enum cxd2880_ret cxd2880_io_spi_read_reg(struct cxd2880_io *io,
						enum cxd2880_io_tgt tgt,
						u8 sub_address, u8 *data,
						u32 size)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	struct cxd2880_spi *spi = NULL;
	u8 send_data[6];
	u8 *read_data_top = data;

	if ((!io) || (!io->if_object) || (!data))
		return CXD2880_RESULT_ERROR_ARG;

	if (sub_address + size > 0x100)
		return CXD2880_RESULT_ERROR_RANGE;

	spi = (struct cxd2880_spi *)(io->if_object);

	if (tgt == CXD2880_IO_TGT_SYS)
		send_data[0] = 0x0B;
	else
		send_data[0] = 0x0A;

	send_data[3] = 0;
	send_data[4] = 0;
	send_data[5] = 0;

	while (size > 0) {
		send_data[1] = sub_address;
		if (size > 255)
			send_data[2] = 255;
		else
			send_data[2] = (u8)size;

		ret =
		    spi->write_read(spi, send_data, sizeof(send_data),
				    read_data_top, send_data[2]);
		if (ret != CXD2880_RESULT_OK)
			return ret;

		sub_address += send_data[2];
		read_data_top += send_data[2];
		size -= send_data[2];
	}

	return ret;
}

static enum cxd2880_ret cxd2880_io_spi_write_reg(struct cxd2880_io *io,
						 enum cxd2880_io_tgt tgt,
						 u8 sub_address,
						 const u8 *data, u32 size)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	struct cxd2880_spi *spi = NULL;
	u8 send_data[BURST_WRITE_MAX + 4];
	const u8 *write_data_top = data;

	if ((!io) || (!io->if_object) || (!data))
		return CXD2880_RESULT_ERROR_ARG;

	if (size > BURST_WRITE_MAX)
		return CXD2880_RESULT_ERROR_OVERFLOW;

	if (sub_address + size > 0x100)
		return CXD2880_RESULT_ERROR_RANGE;

	spi = (struct cxd2880_spi *)(io->if_object);

	if (tgt == CXD2880_IO_TGT_SYS)
		send_data[0] = 0x0F;
	else
		send_data[0] = 0x0E;

	while (size > 0) {
		send_data[1] = sub_address;
		if (size > 255)
			send_data[2] = 255;
		else
			send_data[2] = (u8)size;

		cxd2880_memcpy(&send_data[3], write_data_top, send_data[2]);

		if (tgt == CXD2880_IO_TGT_SYS) {
			send_data[3 + send_data[2]] = 0x00;
			ret = spi->write(spi, send_data, send_data[2] + 4);
		} else {
			ret = spi->write(spi, send_data, send_data[2] + 3);
		}
		if (ret != CXD2880_RESULT_OK)
			return ret;

		sub_address += send_data[2];
		write_data_top += send_data[2];
		size -= send_data[2];
	}

	return ret;
}

enum cxd2880_ret cxd2880_io_spi_create(struct cxd2880_io *io,
				       struct cxd2880_spi *spi, u8 slave_select)
{
	if ((!io) || (!spi))
		return CXD2880_RESULT_ERROR_ARG;

	io->read_regs = cxd2880_io_spi_read_reg;
	io->write_regs = cxd2880_io_spi_write_reg;
	io->write_reg = cxd2880_io_common_write_one_reg;
	io->if_object = spi;
	io->i2c_address_sys = 0;
	io->i2c_address_demod = 0;
	io->slave_select = slave_select;

	return CXD2880_RESULT_OK;
}
