/*
 * cxd2880_spi_device.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * SPI access functions
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

#include <linux/spi/spi.h>

#include "cxd2880_spi_device.h"

static enum cxd2880_ret cxd2880_spi_device_write(struct cxd2880_spi *spi,
						const u8 *data, u32 size)
{
	struct cxd2880_spi_device *spi_device = NULL;
	struct spi_message msg;
	struct spi_transfer tx;
	int result = 0;

	if ((!spi) || (!spi->user) || (!data) || (size == 0))
		return CXD2880_RESULT_ERROR_ARG;

	spi_device = (struct cxd2880_spi_device *)(spi->user);

	memset(&tx, 0, sizeof(tx));
	tx.tx_buf = data;
	tx.len = size;

	spi_message_init(&msg);
	spi_message_add_tail(&tx, &msg);
	result = spi_sync(spi_device->spi, &msg);

	if (result < 0)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret cxd2880_spi_device_write_read(struct cxd2880_spi *spi,
							const u8 *tx_data,
							u32 tx_size,
							u8 *rx_data,
							u32 rx_size)
{
	struct cxd2880_spi_device *spi_device = NULL;
	int result = 0;

	if ((!spi) || (!spi->user) || (!tx_data) ||
		 (tx_size == 0) || (!rx_data) || (rx_size == 0))
		return CXD2880_RESULT_ERROR_ARG;

	spi_device = (struct cxd2880_spi_device *)(spi->user);

	result = spi_write_then_read(spi_device->spi, tx_data,
					tx_size, rx_data, rx_size);
	if (result < 0)
		return CXD2880_RESULT_ERROR_IO;

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret
cxd2880_spi_device_initialize(struct cxd2880_spi_device *spi_device,
				enum cxd2880_spi_mode mode,
				u32 speed_hz)
{
	int result = 0;
	struct spi_device *spi = spi_device->spi;

	switch (mode) {
	case CXD2880_SPI_MODE_0:
		spi->mode = SPI_MODE_0;
		break;
	case CXD2880_SPI_MODE_1:
		spi->mode = SPI_MODE_1;
		break;
	case CXD2880_SPI_MODE_2:
		spi->mode = SPI_MODE_2;
		break;
	case CXD2880_SPI_MODE_3:
		spi->mode = SPI_MODE_3;
		break;
	default:
		return CXD2880_RESULT_ERROR_ARG;
	}

	spi->max_speed_hz = speed_hz;
	spi->bits_per_word = 8;
	result = spi_setup(spi);
	if (result != 0) {
		pr_err("spi_setup failed %d\n", result);
		return CXD2880_RESULT_ERROR_ARG;
	}

	return CXD2880_RESULT_OK;
}

enum cxd2880_ret cxd2880_spi_device_create_spi(struct cxd2880_spi *spi,
					struct cxd2880_spi_device *spi_device)
{
	if ((!spi) || (!spi_device))
		return CXD2880_RESULT_ERROR_ARG;

	spi->read = NULL;
	spi->write = cxd2880_spi_device_write;
	spi->write_read = cxd2880_spi_device_write_read;
	spi->flags = 0;
	spi->user = spi_device;

	return CXD2880_RESULT_OK;
}
