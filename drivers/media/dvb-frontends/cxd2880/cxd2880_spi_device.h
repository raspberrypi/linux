/*
 * cxd2880_spi_device.h
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * SPI access interface
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

#ifndef CXD2880_SPI_DEVICE_H
#define CXD2880_SPI_DEVICE_H

#include "cxd2880_spi.h"

struct cxd2880_spi_device {
	struct spi_device *spi;
};

enum cxd2880_ret
cxd2880_spi_device_initialize(struct cxd2880_spi_device *spi_device,
				enum cxd2880_spi_mode mode,
				u32 speedHz);

enum cxd2880_ret
cxd2880_spi_device_create_spi(struct cxd2880_spi *spi,
				struct cxd2880_spi_device *spi_device);

#endif /* CXD2880_SPI_DEVICE_H */
