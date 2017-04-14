/*
 * cxd2880_common.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * common functions
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

#define MASKUPPER(n) (((n) == 0) ? 0 : (0xFFFFFFFFU << (32 - (n))))
#define MASKLOWER(n) (((n) == 0) ? 0 : (0xFFFFFFFFU >> (32 - (n))))

int cxd2880_convert2s_complement(u32 value, u32 bitlen)
{
	if ((bitlen == 0) || (bitlen >= 32))
		return (int)value;

	if (value & (u32)(1 << (bitlen - 1)))
		return (int)(MASKUPPER(32 - bitlen) | value);
	else
		return (int)(MASKLOWER(bitlen) & value);
}

u32 cxd2880_bit_split_from_byte_array(u8 *array, u32 start_bit, u32 bit_num)
{
	u32 value = 0;
	u8 *array_read;
	u8 bit_read;
	u32 len_remain;

	if (!array)
		return 0;
	if ((bit_num == 0) || (bit_num > 32))
		return 0;

	array_read = array + (start_bit / 8);
	bit_read = (u8)(start_bit % 8);
	len_remain = bit_num;

	if (bit_read != 0) {
		if (((int)len_remain) <= 8 - bit_read) {
			value = (*array_read) >> ((8 - bit_read) - len_remain);
			len_remain = 0;
		} else {
			value = *array_read++;
			len_remain -= 8 - bit_read;
		}
	}

	while (len_remain > 0) {
		if (len_remain < 8) {
			value <<= len_remain;
			value |= (*array_read++ >> (8 - len_remain));
			len_remain = 0;
		} else {
			value <<= 8;
			value |= (u32)(*array_read++);
			len_remain -= 8;
		}
	}

	value &= MASKLOWER(bit_num);

	return value;
}
