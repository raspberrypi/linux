// SPDX-License-Identifier: GPL-2.0
#include "sitronix_ts.h"
#include "sitronix_st7123.h"
#include "sitronix_ts_upgrade.h"

#define st_u8 u8
#define st_u16 u16
#define st_u32 u32
#define st_char char
#define st_msleep msleep
#define st_usleep usleep
#define st_usleep_range usleep_range
#define st_int int

static bool st_icp_flash_wakeup(u16 *flash_write_block_size);
static int st_icp_flash_read(u8 *data, int off, int len);


#ifdef ST_DO_WRITE_DISPLAY_AREA
int st_check_display(const char *data)
{
	if (data[0x00] != 'D' || data[0x01] != 'I' || data[0x02] != 'S' ||
	    data[0x03] != 'P') {
		sterr("check Display fail\n");
		return -EINVAL;
	}
	return 0;
}
#endif /* ST_DO_WRITE_DISPLAY_AREA */

int st_check_cfg(const char *data, int *cfgSize)
{
	if (data[0x00] != 'C' || data[0x01] != 'F' || data[0x02] != 'T' ||
	    data[0x03] != '1') {
		sterr("check CFT1 fail , %x %x %x %x\n", data[0x00], data[0x01],
		      data[0x02], data[0x03]);
		return -EINVAL;
	}
	if (data[0x08] == 0x84)
		*cfgSize = data[0x0B] * 0x100 + data[0x0C] + 3;
	else
		*cfgSize = data[0x0A] * 0x100 + data[0x0B] + 3;
	stmsg("cfgSize = 0x%X\n", *cfgSize);

	return 0;
}

static st_u32 st_remainderCal(st_u32 poly, st_u32 dat, st_u8 polysize, st_u8 datasize)
{
	const st_u8 SHIFT_MAX = datasize - (polysize + 1);
	st_u8 ind;
	st_u32 MSBOFDATA = (u32)1 << (datasize - 1);
	st_u32 tmp;

	poly |= (1 << polysize);
	for (ind = 0; ind <= SHIFT_MAX; ind++) {
		if ((dat << ind) & MSBOFDATA) { //if MSB == 1
			tmp = poly << (SHIFT_MAX - ind);
			dat ^= tmp; //poly dosen't include the MSB of the divider.
		}
	}
	return dat; // remainder is the lowest N bits
}

static st_u32 st_crc24Cal(st_u32 poly, st_u8 *dat, st_u32 dat_len)
{
	const char POLYSIZE = 24, DATASIZE = 8;
	st_u32 crc;
	st_u32 ind;

	crc = dat[0];
	for (ind = 1; ind < dat_len; ind++) {
		crc = (crc << DATASIZE) | dat[ind];
		crc = st_remainderCal(poly, crc, POLYSIZE,
				      (u8)(POLYSIZE + DATASIZE));
	}
	for (ind = 0; ind < (POLYSIZE / DATASIZE); ind++) {
		// CRC24, the data should be shifted left 24bits.
		//shift 8bit 3 times and calculate the remainder
		crc = crc << DATASIZE; // CRC24, shift left 8bit
		crc = st_remainderCal(poly, crc, POLYSIZE,
				      (u8)(POLYSIZE + DATASIZE));
	}
	return crc;
}

static void st_calculateFwChecksum(st_u32 *pChecksum, unsigned char *pInData,
			    unsigned long Len)
{
	//CRC checksum
	*pChecksum = st_crc24Cal(0x864CFB, pInData, Len);
}

int st_check_fw(const char *data, int *fwOff, int *fwSize, int *fwInfoOff,
		int *cfgFlashOff, int *cfgDramOff)
{
	int i = 0;
	int fwCrcOff = 0;
	st_u32 checksum = 0, checksumFw = 0;
	*fwOff = 0;

	if (data[0x84] == 0) {
		//72K firmware
		*cfgFlashOff = 0x12000;
		*cfgDramOff = data[0x93] * 0x100 + data[0x94];

		*fwInfoOff = (data[0x96] << 16);
		*fwInfoOff |= (data[0x97] << 8);
		*fwInfoOff |= (data[0x98]);

		fwCrcOff |= (data[0x85] << 16);
		fwCrcOff |= (data[0x86] << 8);
		fwCrcOff |= (data[0x87]);
		*fwSize = fwCrcOff + 1;
		if (gts->ts_dev_info.chip_id == 0x83 &&
		    gts->ts_dev_info.chip_ver < 0x47) {
			sterr("ChipId(0x%02X): ChipVer(0x%02X) does NOT support 72K firmware!\n",
			      gts->ts_dev_info.chip_id,
			      gts->ts_dev_info.chip_ver);
			return -EINVAL;
		}
	} else {
		*fwSize = data[0x84] * 0x100 + data[0x85] + 1;
		if (gts->ts_dev_info.chip_id == 0x84) {
			*cfgFlashOff = 0xF000;
		} else {
			*cfgFlashOff =
				0x10000; //data[0x8E] * 0x10000 + data[0x8F] * 0x100 + data[0x90];
		}
		*cfgDramOff = data[0x91] * 0x100 + data[0x92];
		*fwInfoOff = data[0x93] * 0x100 + data[0x94];
		fwCrcOff = data[0x84] * 0x100 + data[0x85];
	}

	if (fwCrcOff > ST_DUMP_MAX_LEN) {
		sterr("fwCrcOff(0x%X) > ST_DUMP_MAX_LEN(0x%X) , error!\n",
		      fwCrcOff, ST_DUMP_MAX_LEN);
		return -EINVAL;
	}

	checksumFw = data[fwCrcOff] | // CRC 24 low byte
		     (data[fwCrcOff - 1] << 8) | // CRC 24 middle byte
		     (data[fwCrcOff - 2] << 16); // CRC 24 high byte

	st_calculateFwChecksum(&checksum, (unsigned char *)data, fwCrcOff - 2);
	if (checksumFw != checksum) {
		sterr("checksumFw(0x%X) !=  checksumHost(0x%X) with fwCrcOff = (0x%X), error!\n",
		      checksumFw, checksum, fwCrcOff);
		return -EINVAL;
	}

	i = *fwInfoOff;
	if (i > ST_DUMP_MAX_LEN) {
		sterr("fwInfoOff(0x%X) > ST_DUMP_MAX_LEN(0x%X) , error!\n", i,
		      ST_DUMP_MAX_LEN);
		return -EINVAL;
	}
	if (data[i] == 0x54 && data[i + 1] == 0x46 && data[i + 2] == 0x49 &&
	    data[i + 3] == 0x33) {
		stmsg("TOUCH_FW_INFO offset = 0x%X\n", i + 4);
		*fwInfoOff = i + 4;

		stmsg("fwOff = 0x%X\n", *fwOff);
		stmsg("fwSize = 0x%X\n", *fwSize);
		stmsg("fwInfoOff = 0x%X\n", *fwInfoOff);
		stmsg("cfgFlashOff = 0x%X\n", *cfgFlashOff);
		stmsg("cfgDramOff = 0x%X\n", *cfgDramOff);
		return 0;
	}

	stmsg("can't find TOUCH_FW_INFO offset\n");
	return -EINVAL;
}

int st_get_dump_size(void)
{
	return ST_DUMP_MAX_LEN;
}

void st_bakcup_flash_dump_buf(void)
{
	st_u16 flash_write_block_size;

	if (gts->fw_request_status == 0) {
		if (!st_icp_flash_wakeup(&flash_write_block_size))
			return;
		if (st_icp_flash_read(dump_buf, 0, ST_DUMP_MAX_LEN) < 0)
			return;
		gts->fw_request_status = 1;
		sitronix_ts_reset_device(gts);
		stmsg("Dump flash success.\n");
	}
	//	else {
	//		stmsg("ignore dump flash.\n");
	//	}
}

void st_set_dump(const char *data, int off, int size)
{
	memcpy(dump_buf + off, data, size);
}

void st_get_dump(unsigned char *data, int off, int size)
{
	memcpy(data, dump_buf + off, size);
}

static int st_compare_array(unsigned char *b1, unsigned char *b2, int len)
{
	int i = 0;

	for (i = 0; i < len; i++) {
		if (b1[i] != b2[i])
			return -EINVAL;
	}
	return 0;
}

static int st_irq_off(void)
{
	//disable_irq(gts->irq);
	// sitronix_ts_irq_enable(gts, false);
	sitronix_disable_irq(gts);
	gts->upgrade_doing = true;
	return 0;
}
static int st_irq_on(void)
{
	//enable_irq(gts->irq);
	// sitronix_ts_irq_enable(gts, true);
	sitronix_enable_irq(gts);
	gts->upgrade_doing = false;
	return 0;
}

//unsigned char fw_check[ST_FLASH_PAGE_SIZE];
unsigned char fw_check[ST_FLASH_SIZE];

/* ///////////////ICP */

static int st_icp_read(st_u8 *tx_buf, int tx_len, st_u8 *rx_buf, int rx_len)
{
	int ret = 0;

	memcpy(wbuf, tx_buf, tx_len + 1);
	memset(wbuf + tx_len + 1, 0, rx_len);
	ret = sitronix_ts_addrmode_read(gts, wbuf, tx_len, rbuf, rx_len);

	if (ret < 0) {
		stmsg("%s error (%d)\n", __func__, ret);
		return ret;
	}
	memcpy(rx_buf, rbuf + ret, rx_len);
	/* st_msleep(100); */
	return rx_len;
}

static int st_icp_write(st_u8 *tx_buf, int tx_len)
{
	int ret = 0;

	if (tx_buf == NULL)
		return -EIO;

	memcpy(wbuf, tx_buf, tx_len + 1);

	ret = sitronix_ts_addrmode_write(gts, wbuf, tx_len);
	/* ret = i2c_master_send(sitronix_ts_gpts.client, txbuf, len); */
	if (ret < 0) {
		stmsg("st_icp_writeerror (%d)\n", ret);
		return ret;
	}

	/* st_msleep(100); */
	return tx_len;
}

static bool st_icp_check_WEL_set(void)
{
	st_u8 tx_buf[2];
	st_u8 rx_buf[2];
	st_u8 flash_status;
	int ret;
	int max_time = 100;

	do {
		tx_buf[1] = 0x05; /* Read Status Register-1 */
		ret = st_icp_read(tx_buf, 1, rx_buf, 1);
		if (ret < 0) {
			sterr("read flash status fail\n");
			return false;
		}

		flash_status = rx_buf[0];

		if ((flash_status & 0x02))
			break;

		st_usleep_range(1000, 2000);

		tx_buf[1] = 0x06; /* Write Enable */
		ret = st_icp_write(tx_buf, 1);
		if (ret < 0) {
			sterr("flash write Enable fail\n");
			return false;
		}

		st_usleep_range(1000, 2000);

	} while (max_time--); //WEL=bit1.

	if (max_time < 0) {
		sterr("WEL is not set, return false\n");
		return false;
	}

	return true;
}

static bool st_icp_check_flash_busy(void)
{
	st_u8 tx_buf[2];
	st_u8 rx_buf[2];
	st_u8 flash_status;
	int ret;
	int max_time = 10000;

	tx_buf[1] = 05; /* Read Status Register-1 */

	do {
		ret = st_icp_read(tx_buf, 1, rx_buf, 1);
		if (ret < 0) {
			sterr("read flash status fail\n");
			return false;
		}
		flash_status = rx_buf[0];
	} while (flash_status & 0x01 && max_time-- > 0); //WIP=bit0.

	if (max_time < 0) {
		sterr("flash busy , return false\n");
		return false;
	}

	return true;
}

static bool st_icp_flash_wakeup(st_u16 *flash_write_block_size)
{
	st_u8 tx_buf[0x10];
	st_u8 rx_buf[0x10];
	int ret = 0;

	/* enter addr mode */
	tx_buf[1] = 0x53;
	tx_buf[2] = 0x71;
	tx_buf[3] = 0x23;
	tx_buf[4] = 0xA5;
	tx_buf[5] = 0x3C;

	ret = st_icp_write(tx_buf, 5);
	ret = st_icp_write(tx_buf, 5); /* x2 */

	if (ret < 0) {
		sterr("enter addr mode fail\n");
		return false;
	}

	/* icp unlock */
	tx_buf[1] = 0x53;
	tx_buf[2] = 0x71;
	tx_buf[3] = 0x23;
	tx_buf[4] = 0x94;
	tx_buf[5] = 0x55;

	ret = st_icp_write(tx_buf, 5);
	if (ret < 0) {
		sterr("icp unlock fail\n");
		return false;
	}

	/* AFE unlock */
	tx_buf[1] = 0x53;
	tx_buf[2] = 0x71;
	tx_buf[3] = 0x23;
	tx_buf[4] = 0x14;
	tx_buf[5] = 0x55;

	ret = st_icp_write(tx_buf, 5);
	if (ret < 0) {
		sterr("icp unlock fail\n");
		return false;
	}

	//MCU KEY
	tx_buf[1] = 0x00;
	tx_buf[2] = 0xF3;
	tx_buf[3] = 0x00;
	tx_buf[4] = 0x5A;
	tx_buf[5] = 0xA5;
	ret = st_icp_write(tx_buf, 5);
	if (ret < 0) {
		sterr("%s: Fail to set MCU KEY\n", __func__);
		return false;
	}

	//CMD_MCU_PRSTN
	tx_buf[1] = 0x00;
	tx_buf[2] = 0xF3;
	tx_buf[3] = 0x02;
	tx_buf[4] = 0x00;
	tx_buf[5] = 0x01;
	ret = st_icp_write(tx_buf, 5);
	if (ret < 0) {
		sterr("%s: Fail to set CMD_MCU_PRSTN\n", __func__);
		return false;
	}

	st_msleep(200);

	//MCU IFC1: RSETAFE
	tx_buf[1] = 0x01;
	tx_buf[2] = 0x01;
	tx_buf[3] = 0xDC;
	tx_buf[4] = 0x00;
	tx_buf[5] = 0x00;
	ret = st_icp_write(tx_buf, 5);
	if (ret < 0) {
		sterr("%s: Fail to set  RSETAFE(L)\n", __func__);
		return false;
	}
	st_usleep_range(10000, 11000);

	//MCU IFC1: RSETAFE
	tx_buf[1] = 0x01;
	tx_buf[2] = 0x01;
	tx_buf[3] = 0xDC;
	tx_buf[4] = 0xF3;
	tx_buf[5] = 0xF3;
	ret = st_icp_write(tx_buf, 5);
	if (ret < 0) {
		sterr("%s: Fail to set RSETAFE(H)\n", __func__);
		return false;
	}
	st_msleep(100);

	/* CMD_MCU_PRSTN */
	tx_buf[1] = 0x00;
	tx_buf[2] = 0xF3;
	tx_buf[3] = 0x02;
	tx_buf[4] = 0x00;
	tx_buf[5] = 0x00;

	ret = st_icp_write(tx_buf, 5);
	if (ret < 0) {
		sterr("CMD_MCU_PRSTN fail\n");
		return false;
	}

	/* spi 2 flash */
	tx_buf[1] = 0x00;
	tx_buf[2] = 0xF3;
	tx_buf[3] = 0x16;
	tx_buf[4] = 0x00;
	tx_buf[5] = 0x01;

	ret = st_icp_write(tx_buf, 5);
	if (ret < 0) {
		sterr("spi 2 flash fail\n");
		return false;
	}

	st_msleep(100);

	/* Release PD */
	tx_buf[1] = 0xAB; /* Release Power-down */

	ret = st_icp_write(tx_buf, 1);
	if (ret < 0) {
		sterr("Release PD fail\n");
		return false;
	}

	st_msleep(100);

	/* read flash ID */
	tx_buf[1] = 0x9F; /* Read JEDEC ID */
	ret = st_icp_read(tx_buf, 1, rx_buf, 4);
	if (ret < 0) {
		sterr("read flash ID fail\n");
		return false;
	}

	*flash_write_block_size = 0x100;
	if ((rx_buf[0] == 0xC2) && (rx_buf[1] == 0x25) && (rx_buf[2] == 0x11))
		*flash_write_block_size = 0x20;

	if ((rx_buf[0] == 0xC2) && (rx_buf[1] == 0x25) && (rx_buf[2] == 0x31))
		*flash_write_block_size = 0x20;

	/* st_usleep_range(10000, 11000); */

	if (st_icp_check_flash_busy() == false)
		return false;
	return true;
}

static bool st_icp_flash_lock(bool is_lock)
{
	st_u8 tx_buf[0x3];
	int ret = 0;

	tx_buf[1] = 0x06; /* Write Enable */
	ret = st_icp_write(tx_buf, 1);
	if (ret < 0) {
		sterr("flash write Enable fail\n");
		return false;
	}

	if (st_icp_check_WEL_set() == false)
		return false;

	tx_buf[1] = 0x01; /* Write Status Register */
	if (!is_lock)
		tx_buf[2] = 0x00; /* unlock */
	else
		tx_buf[2] = 0x1C; /* lock 0x8C > 0x1C */
	ret = st_icp_write(tx_buf, 2);
	if (ret < 0) {
		sterr("flash write status register fail\n");
		return false;
	}

	if (st_icp_check_flash_busy() == false)
		return false;
	return true;
}

static int st_icp_flash_read(st_u8 *data, int off, int len)
{
	int ret;
	st_u8 tx_buf[8];
	st_u32 offset;
	st_u32 readLen;
	st_u32 nowLen;

	offset = off;
	nowLen = len;
	while (nowLen > 0) {
		if (offset % ST_ICP_READ_BLOCK_SIZE != 0)
			readLen = ST_ICP_READ_BLOCK_SIZE -
				  offset % ST_ICP_READ_BLOCK_SIZE;
		else
			readLen = ST_ICP_READ_BLOCK_SIZE;

		if (offset + readLen > off + len)
			readLen = off + len - offset;

		stmsg("offset %x , readLen %x\n", offset, readLen);
		/* read */
		tx_buf[1] = 0x03;
		tx_buf[2] = (st_u8)(offset >> 16); /* A23-A16 */
		tx_buf[3] = (st_u8)(offset >> 8); /* A15-A8 */
		tx_buf[4] = (st_u8)(offset);

		ret = st_icp_read(tx_buf, 4, data + (offset - off), readLen);
		if (ret != readLen) {
			sterr("%s fail\n", __func__);
			return -EIO;
		}

		nowLen -= readLen;
		offset += readLen;
	}

	return 0;
}

static int st_icp_flash_erase(int offset)
{
	int ret;
	st_u8 tx_buf[8];

	tx_buf[1] = 0x06; /* Write Enable */
	ret = st_icp_write(tx_buf, 1);
	if (ret < 0) {
		sterr("flash write Enable fail\n");
		return -EIO;
	}

	if (st_icp_check_WEL_set() == false)
		return false;

	tx_buf[1] = 0x20; /* Sector Erase */
	tx_buf[2] = (st_u8)(offset >> 16); /* A23-A16 */
	tx_buf[3] = (st_u8)(offset >> 8); /* A15-A8 */
	tx_buf[4] = (st_u8)(offset); /* A7-A0 */

	ret = st_icp_write(tx_buf, 4);
	if (ret < 0) {
		sterr("flash sector erase fail\n");
		return -EIO;
	}

	st_msleep(50);

	if (st_icp_check_flash_busy() == false)
		return -EIO;
	return 0;
}

static int st_icp_flash_write_block(st_u8 *data, int offset,
			     st_u16 flash_write_block_size)
{
	int ret = 0;
	//st_u8 tx_buf[0x100 + 8];
	st_u8 *tx_buf;
	int tx_buf_len = 0x100 + 8;

	tx_buf = kzalloc(tx_buf_len, GFP_KERNEL);
	if (!tx_buf) {
		sterr("%s: Alloc memory for tx_buf failed!\n", __func__);
		return -ENOMEM;
	}

	stmsg("write offset %X , len %X\n", offset, flash_write_block_size);

	tx_buf[1] = 0x06; /* Write Enable */
	ret = st_icp_write(tx_buf, 1);
	if (ret < 0) {
		sterr("flash write Enable fail\n");
		ret = -EIO;
		goto err_return;
	}

	if (st_icp_check_WEL_set() == false)
		return false;

	tx_buf[1] = 0x02; /* Page Program */
	tx_buf[2] = (st_u8)(offset >> 16); /* A23-A16 */
	tx_buf[3] = (st_u8)(offset >> 8); /* A15-A8 */
	tx_buf[4] = (st_u8)(offset); /* A7-A0 */

	memcpy(tx_buf + 5, data, flash_write_block_size);

	ret = st_icp_write(tx_buf, 4 + flash_write_block_size);
	if (ret < 0) {
		sterr("flash write block fail\n");
		ret = -EIO;
		goto err_return;
	}

	if (st_icp_check_flash_busy() == false) {
		ret = -EIO;
		goto err_return;
	}

	ret = 0;

err_return:
	kfree(tx_buf);

	return ret;
}

static int st_icp_flash_write(st_u8 *data, int offset, int write_len_total,
		       st_u16 flash_write_block_size)
{
	st_u16 start_page;
	st_u16 page_offset;
	int write_byte;
	st_u16 write_len_once;
	st_u8 *temp_buf;
	int retry = 0;
	int isSuccess = 0;
	int i;

	temp_buf = kzalloc(ST_FLASH_PAGE_SIZE, GFP_KERNEL);
	stmsg("Write flash offset:0x%X , length:0x%X\n", offset,
	      write_len_total);

	write_byte = 0;
	if (write_len_total == 0)
		return write_byte;

	if ((offset + write_len_total) > ST_FLASH_SIZE)
		write_len_total = ST_FLASH_SIZE - offset;

	start_page = offset / ST_FLASH_PAGE_SIZE;
	page_offset = offset % ST_FLASH_PAGE_SIZE;
	while (write_len_total > 0) {
		if ((page_offset != 0) ||
		    (write_len_total < ST_FLASH_PAGE_SIZE)) {
			if (st_icp_flash_read(temp_buf,
					      start_page * ST_FLASH_PAGE_SIZE,
					      ST_FLASH_PAGE_SIZE) < 0)
				return -EIO;
		}

		write_len_once = ST_FLASH_PAGE_SIZE - page_offset;
		if (write_len_total < write_len_once)
			write_len_once = write_len_total;
		memcpy(&temp_buf[page_offset], data, write_len_once);

		retry = 0;
		isSuccess = 0;
		while (retry++ < 2 && isSuccess == 0) {
			if (st_icp_flash_erase(start_page *
					       ST_FLASH_PAGE_SIZE) >= 0) {
				stmsg("write page:%d\n", start_page);
				isSuccess = 1;
				for (i = 0; i < ST_FLASH_PAGE_SIZE;
				     i += flash_write_block_size) {
					if (st_icp_flash_write_block(
						    temp_buf + i,
						    start_page * ST_FLASH_PAGE_SIZE +
							    i,
						    flash_write_block_size) < 0)
						isSuccess = 0;
				}
			}

			if (isSuccess == 0)
				stmsg("%s write page %d retry: %d\n",
				      __func__, start_page, retry);
		}
		if (isSuccess == 0) {
			stmsg("%s write page %d error , break\n",
			      __func__, start_page);
			return -EIO;
		}
		start_page++;

		write_len_total -= write_len_once;
		data += write_len_once;
		write_byte += write_len_once;
		page_offset = 0;
	}
	kfree(temp_buf);
	return write_byte;
}

static int sitronix_do_upgrade_flash_icp_all(void)
{
	int rt = 0;
	int fwOff = 0;
	int cfgFlashOff = 0;
	int cfgDramOff = 0;
	int fwSize = 0;
	int cfgSize = 0;
	int displaySize = 0;
	int fwInfoOff = 0;
	int fwInfoLen = 0;
	//int flashFwInfoOff = 0;
	//int flashFwInfoLen = 0;
	st_u16 flash_write_block_size;
	/* int powerfulWrite = 0; */
	int retry;

	gts->upgrade_doing = true;

	rt = st_check_fw(dump_buf, &fwOff, &fwSize, &fwInfoOff, &cfgFlashOff,
			 &cfgDramOff);
	if (rt < 0) {
		rt = -EIO;
		goto ST_ICP_HW_RESET;
	}

	if (cfgFlashOff > sizeof(dump_buf)) {
		sterr("check failed by cfgFlashOff = 0x%x\n", cfgFlashOff);
		rt = -EIO;
		goto ST_ICP_HW_RESET;
	}

	rt = st_check_cfg(dump_buf + cfgFlashOff, &cfgSize);
	if (rt < 0) {
		rt = -EIO;
		goto ST_ICP_HW_RESET;
	}

	fwInfoLen =
		dump_buf[fwInfoOff] * 0x100 + dump_buf[fwInfoOff + 1] + 2 + 4;

	stmsg("fwInfoLen 0x%x\n", fwInfoLen);

	st_irq_off();

	if (!gts->flash_powerful_upgrade) {
#ifdef SITRONIX_FLASH_CHECK_FWVER
		cfgDramOff =
			(dump_buf[cfgFlashOff + SITRONIX_FLASH_CFG_TAB_VER_OFF]
			 << 8) +
			dump_buf[cfgFlashOff + SITRONIX_FLASH_CFG_TAB_VER_OFF +
				 1];

		if (cfgFlashOff + cfgDramOff > sizeof(dump_buf)) {
			rt = -1;
			sterr("check CFG error! IdOffset = 0x%x\n", cfgDramOff);
			goto ST_ICP_HW_RESET;
		}

		cfgDramOff = dump_buf[cfgFlashOff + cfgDramOff];
		stmsg("CustomerVersion of dumpbuf = 0x%x , of current = 0x%x\n",
		      cfgDramOff, gts->ts_dev_info.fw_version);

		if (cfgDramOff <= gts->ts_dev_info.fw_version) {
			rt = 0;
			stmsg("Don't have new CustomerVersion , don't need to upgrade\n");
			cfgSize = 0;
			fwSize = 0;
			displaySize = 0;
			goto ST_ICP_HW_RESET;
		}
#endif
	}

	if (!st_icp_flash_wakeup(&flash_write_block_size)) {
		rt = -1;
		goto ST_ICP_HW_RESET;
	}

	if (st_icp_flash_read(fw_check, 0, fwSize) < 0) {
		sterr("read flash fail @0x0 , cancel upgrade\n");
		rt = -1;
		goto ST_ICP_HW_RESET;
	}

	if (st_compare_array(fw_check, dump_buf, fwSize) == 0) {
		stmsg("fw compare :same\n");
		fwSize = 0;
	} else {
		stmsg("fw compare :different\n");
	}

	st_icp_flash_read(fw_check, cfgFlashOff, cfgSize);

	if (st_compare_array(fw_check, dump_buf + cfgFlashOff, cfgSize) == 0) {
		stmsg("cfg compare :same\n");
		cfgSize = 0;
	} else {
		stmsg("cfg compare : different\n");
	}

#ifdef ST_DO_WRITE_DISPLAY_AREA
	displaySize = ST_DISPLAY_SIZE;
	rt = st_check_display(dump_buf + ST_DISPLAY_DUMP_OFF);
	if (rt < 0) {
		displaySize = 0;
		rt = 0;
		stmsg("dump buf doesn't contain display data\n");
	} else {
		st_icp_flash_read(fw_check, ST_ICP_DISPLAY_FLASH_OFF,
				  displaySize);

		if (st_compare_array(fw_check, dump_buf + ST_DISPLAY_DUMP_OFF,
				     displaySize) == 0) {
			stmsg("display compare :same\n");
			displaySize = 0;
		} else {
			stmsg("display compare : different\n");
		}
	}
#endif /* ST_DO_WRITE_DISPLAY_AREA */

	if (!st_icp_flash_wakeup(&flash_write_block_size)) {
		rt = -1;
		sterr("st_icp_flash_wakeup fail!\n");
		goto ST_ICP_HW_RESET;
	}

	if (!st_icp_flash_lock(false)) {
		rt = -1;
		sterr("st_icp_flash_lock unlock fail!\n");
		goto ST_ICP_HW_RESET;
	}

	if (cfgSize != 0) {
		retry = 3;
		do {
			stmsg("upgrade cfg to flash...\n");
			st_icp_flash_write(dump_buf + cfgFlashOff, cfgFlashOff,
					   cfgSize, flash_write_block_size);

			st_icp_flash_read(fw_check, cfgFlashOff, cfgSize);
			if (st_compare_array(fw_check, dump_buf + cfgFlashOff,
					     cfgSize) == 0) {
				stmsg("cfg compare :same\n");
				break;
			}
			stmsg("cfg compare : different\n");
		} while (retry--);
	}

	if (fwSize != 0) {
		retry = 3;
		do {
			stmsg("upgrade fw to flash...\n");
			st_icp_flash_write(dump_buf + fwOff, fwOff, fwSize,
					   flash_write_block_size);

			st_icp_flash_read(fw_check, 0, fwSize);
			if (st_compare_array(fw_check, dump_buf + fwOff,
					     fwSize) == 0) {
				stmsg("fw compare :same\n");
				break;
			}
			stmsg("fw compare : different\n");
		} while (retry--);
	}

	if (displaySize != 0)
		st_icp_flash_write(dump_buf + ST_DISPLAY_DUMP_OFF,
				   ST_ICP_DISPLAY_FLASH_OFF, displaySize,
				   flash_write_block_size);

	if (!st_icp_flash_lock(true))
		sterr("st_icp_flash_lock lock fail!\n");

ST_ICP_HW_RESET:
	sitronix_ts_reset_device(gts);

	st_irq_on();

	gts->upgrade_doing = false;

	if (rt < 0) {
		return rt;
	} else if (cfgSize != 0 || fwSize != 0 || displaySize != 0) {
		sitronix_ts_get_device_info(gts);
		return 1;
	} else {
		return rt;
	}
}

int sitronix_spi_pram_rw(bool isread, st_u32 addr, st_u8 *txbuf, st_u8 *rxbuf,
			 int len)
{
	int nowlen = len;
	st_u32 nowoff = 0;
	st_u32 ramoff = addr;
	int ret = 0;
	int max_trans_len;

	if (isread)
		max_trans_len = ST_PLATFORM_READ_LEN_MAX;
	else
		max_trans_len = ST_PLATFORM_WRITE_LEN_MAX;

	while (nowlen > 0) {
		if (nowlen > max_trans_len) {
			ramoff = addr + (nowoff);

			wbuf[1] = (ramoff >> 16) & 0xFF;
			wbuf[2] = (ramoff >> 8) & 0xFF;
			wbuf[3] = (ramoff) & 0xFF;

			if (isread)
				wbuf[1] |= 0x80;

			if (isread) {
				memset(wbuf + 4, 0, max_trans_len + 1);
				ret = sitronix_ts_addrmode_read(
					gts, wbuf, 3, rbuf, max_trans_len + 1);

				if (ret < 0)
					return ret;
				memcpy(rxbuf + nowoff, rbuf + ret + 1,
				       max_trans_len);
			} else {
				memcpy(wbuf + 4, txbuf + nowoff, max_trans_len);
				ret = sitronix_ts_addrmode_write(
					gts, wbuf, max_trans_len + 3);
				if (ret < 0)
					return ret;
			}

			nowoff += max_trans_len;
			nowlen -= max_trans_len;
		} else {
			ramoff = addr + (nowoff);
			wbuf[1] = (ramoff >> 16) & 0xFF;
			wbuf[2] = (ramoff >> 8) & 0xFF;
			wbuf[3] = (ramoff) & 0xFF;

			if (isread)
				wbuf[1] |= 0x80;

			if (isread) {
				memset(wbuf + 4, 0, nowlen + 1);
				ret = sitronix_ts_addrmode_read(
					gts, wbuf, 3, rbuf, nowlen + 1);

				if (ret < 0)
					return ret;
				memcpy(rxbuf + nowoff, rbuf + ret + 1,
				       nowlen);
			} else {
				memcpy(wbuf + 4, txbuf + nowoff, nowlen);
				ret = sitronix_ts_addrmode_write(gts, wbuf,
								 nowlen + 3);
				if (ret < 0)
					return ret;
			}
			nowlen = 0;
		}
	}
	return 0;
}

static int sitronix_spi_hdl_fw(unsigned char *buf)
{
	int ret = 0;
	int retry = 0;
	int isSuccess = -1;
	int fwOff = 0;
	int cfgFlashOff = 0;
	int cfgDramhOff = 0;
	int fwSize = 0;
	int cfgSize = 0;
	int displaySize = 0;
	int fwInfoOff = 0;
	int i;
	uint8_t hdl_rbuf[4];
	st_u8 cmd[6];

	//hdl_rbuf = kmalloc(ST_FW_LEN, GFP_KERNEL);

	ret = st_check_fw(buf, &fwOff, &fwSize, &fwInfoOff, &cfgFlashOff,
			  &cfgDramhOff);
	if (ret < 0) {
		strscpy(gts->upgrade_msg, "st_check_fw fail",
			sizeof(gts->upgrade_msg));
		sterr("st_check_fw fail\n");
		goto sitronix_spi_hdl_fw_finish;
	}

	ret = st_check_cfg(buf + cfgFlashOff, &cfgSize);

	if (ret < 0) {
		strscpy(gts->upgrade_msg, "st_check_cfg fail",
			sizeof(gts->upgrade_msg));
		sterr("st_check_cfg fail\n");
		goto sitronix_spi_hdl_fw_finish;
	}
#ifdef ST_DO_WRITE_DISPLAY_AREA
	displaySize = ST_DISPLAY_SIZE;
	ret = st_check_display(buf + ST_DISPLAY_DUMP_OFF);

	if (ret < 0) {
		displaySize = 0;
		stmsg("dump buf doesn't contain display data\n");
	}
#endif /* ST_DO_WRITE_DISPLAY_AREA */

	while (isSuccess == -1 && retry++ < ST_RAM_MODE_RETRY_MAX) {
		sitronix_ts_reset_device(gts);

		/* addr mode */
		cmd[1] = 0x53;
		cmd[2] = 0x71;
		cmd[3] = 0x23;
		cmd[4] = 0xA5;
		cmd[5] = 0x3C;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0) {
			sterr("HDL set address mode fail\n");
			goto sitronix_spi_hdl_fw_retry;
		}

		/* PRAM unlock */
		cmd[1] = 0x53;
		cmd[2] = 0x71;
		cmd[3] = 0x23;
		cmd[4] = 0x05;
		cmd[5] = 0x55;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0) {
			sterr("HDL PRAM unlock fail\n");
			goto sitronix_spi_hdl_fw_retry;
		}

		/* URAM unlock */
		cmd[1] = 0x53;
		cmd[2] = 0x71;
		cmd[3] = 0x23;
		cmd[4] = 0x55;
		cmd[5] = 0x55;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0) {
			sterr("HDL URAM unlock fail\n");
			goto sitronix_spi_hdl_fw_retry;
		}

		/* AFE unlock */
		cmd[1] = 0x53;
		cmd[2] = 0x71;
		cmd[3] = 0x23;
		cmd[4] = 0x14;
		cmd[5] = 0x55;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0) {
			sterr("HDL PRAM unlock fail\n");
			goto sitronix_spi_hdl_fw_retry;
		}

		/* MCU KEY */
		cmd[1] = 0x00;
		cmd[2] = 0xF3;
		cmd[3] = 0x00;
		cmd[4] = 0x5A;
		cmd[5] = 0xA5;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0) {
			sterr("HDL PRAM unlock fail\n");
			goto sitronix_spi_hdl_fw_retry;
		}

		/* MCU PRSTN */
		cmd[1] = 0x00;
		cmd[2] = 0xF3;
		cmd[3] = 0x02;
		cmd[4] = 0x00;
		cmd[5] = 0x01;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0) {
			sterr("HDL MCU PRSTN fail\n");
			goto sitronix_spi_hdl_fw_retry;
		}

		/* MCU PRSTN */
		cmd[1] = 0x00;
		cmd[2] = 0xF3;
		cmd[3] = 0x02;
		cmd[4] = 0x00;
		cmd[5] = 0x00;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0) {
			sterr("HDL MCU PRSTN fail\n");
			goto sitronix_spi_hdl_fw_retry;
		}

		/* MCU PRSTN */
		cmd[1] = 0x00;
		cmd[2] = 0xF3;
		cmd[3] = 0x02;
		cmd[4] = 0x00;
		cmd[5] = 0x01;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0) {
			sterr("HDL MCU PRSTN fail\n");
			goto sitronix_spi_hdl_fw_retry;
		}

		//sitronix_spi_pram_rw( true, ST_HDL_RAM_ADDR + 0x1000 -4  , NULL, hdl_rbuf, 4);
		//sitronix_spi_pram_rw( true, ST_HDL_RAM_ADDR + 0x1000 -4  , NULL, hdl_rbuf, 4);
		//sitronix_spi_pram_rw( true, ST_HDL_RAM_ADDR + 0x1000 -4  , NULL, hdl_rbuf, 4);
		//ret = sitronix_spi_pram_rw( true, 0xF000 , NULL, hdl_rbuf, 0x30);
		//ret = sitronix_spi_pram_rw( true, 0xF000 , NULL, hdl_rbuf, 0x30);
		//ret = sitronix_spi_pram_rw( true, 0xF000 , NULL, hdl_rbuf, 0x30);
		//ret = sitronix_spi_pram_rw( true, 0xF000 , NULL, hdl_rbuf, 0x30);
		//ret = sitronix_spi_pram_rw( true, 0xF000 , NULL, hdl_rbuf, 0x30);
		//ret = sitronix_spi_pram_rw( true, 0xF000 , NULL, hdl_rbuf, 0x30);
		//ret = sitronix_spi_pram_rw( true, 0xF000 , NULL, hdl_rbuf, 0x30);
		//ret = sitronix_spi_pram_rw( true, 0xF000 , NULL, hdl_rbuf, 0x30);

		//ret = sitronix_spi_pram_rw(false, ST_HDL_RAM_ADDR, buf, NULL, 0x30);
		//ret = sitronix_spi_pram_rw( true, ST_HDL_RAM_ADDR , NULL, hdl_rbuf, 0x30);
		//ret = sitronix_spi_pram_rw( true, ST_HDL_RAM_ADDR , NULL, hdl_rbuf, 0x30);
		//ret = sitronix_spi_pram_rw( true, ST_HDL_RAM_ADDR , NULL, hdl_rbuf, 0x30);

		//ret = -1;
		//if (ret == -1)
		//	return -1;
		ret = sitronix_spi_pram_rw(false, ST_HDL_PRAM_ADDR, buf, NULL,
					   ST_FW_LEN);
		/* stmsg("write_wbuf %x %x %x %x\n",buf[0],buf[1],buf[2],buf[3]); */
		if (ret < 0) {
			sterr("HDL program PRAM fail\n");
			goto sitronix_spi_hdl_fw_retry;
		}

		//Download part2 if necessary
		if (gts->ts_dev_info.chip_id != 0x83 ||
		    gts->ts_dev_info.chip_ver >= 0x47) {
			if (buf[0x84] == 0) { //large firmware
				ret = sitronix_spi_pram_rw(
					false, ST_HDL_PRAM_ADDR_PART2,
					buf + ST_FW_LEN, NULL, ST_FW_LEN_PART2);
				if (ret < 0) {
					sterr("HDL program PRAM PART2 fail\n");
					goto sitronix_spi_hdl_fw_retry;
				}
			}
		}

		for (i = 0x1000; i < ST_FW_LEN; i += 0x1000) {
#ifdef SITRONIX_HDL_VALIDATE_BY_RANDOM
			ret = (get_random_int() % (0x800 - 2)) * 2 + i;
			sitronix_spi_pram_rw(true, ST_HDL_PRAM_ADDR + ret, NULL,
					     hdl_rbuf, 4);
			ret = st_compare_array(buf + (ret), hdl_rbuf, 4);
#else
			ret = sitronix_spi_pram_rw(true,
						   ST_HDL_PRAM_ADDR + i - 4,
						   NULL, hdl_rbuf, 4);
			ret = st_compare_array(buf + (i - 4), hdl_rbuf, 4);
#endif /* SITRONIX_HDL_VALIDATE_BY_RANDOM */

			if (ret < 0) {
				sterr("valid FW (compare) fail , retry: %d\n",
				      retry);
				isSuccess = -1;
				i = ST_FW_LEN;
				goto sitronix_spi_hdl_fw_retry;
			}
			isSuccess = 0;
		}

		ret = sitronix_spi_pram_rw(false,
					   ST_HDL_URAM_ADDR + cfgDramhOff,
					   buf + cfgFlashOff, NULL, cfgSize);
		ret = sitronix_spi_pram_rw(
			true, ST_HDL_URAM_ADDR + cfgDramhOff + cfgSize - 4,
			NULL, hdl_rbuf, 4);

		ret = st_compare_array(buf + (cfgFlashOff + cfgSize - 4),
				       hdl_rbuf, 4);
		if (ret < 0) {
			sterr("valid CFG (compare) fail , retry: %d\n", retry);
			isSuccess = -1;
			goto sitronix_spi_hdl_fw_retry;
		}
		isSuccess = 0;

		if (displaySize != 0) {
			ret = sitronix_spi_pram_rw(false,
						   ST_HDL_DISPLAY_RAM_ADDR,
						   buf + ST_DISPLAY_DUMP_OFF,
						   NULL, ST_DISPLAY_SIZE);
			if (ret < 0) {
				sterr("HDL program Display Ram fail\n");
				isSuccess = -1;
				goto sitronix_spi_hdl_fw_retry;
			}
		}

		/* MCU PRSTN + MCU_CRSTN */
		cmd[1] = 0x00;
		cmd[2] = 0xF3;
		cmd[3] = 0x02;
		cmd[4] = 0x00;
		cmd[5] = 0x03;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0) {
			sterr("HDL MCU PRSTN and CRSTN fail\n");
			isSuccess = -1;
			goto sitronix_spi_hdl_fw_retry;
		}

		if (ret < 0)
			sterr("write FW (PRAM) fail with error: %d , retry: %d\n",
			      ret, retry);

sitronix_spi_hdl_fw_retry:
		if (isSuccess == -1)
			sterr("%s fail , retry %d\n", __func__, retry);
	}

sitronix_spi_hdl_fw_finish:
	//kfree(hdl_rbuf);
	return isSuccess;
	/* return 0; */
}

static int sitronix_do_upgrade_hostdownload(void)
{
	int ret = 0;
	st_u8 cmd[6];

	gts->upgrade_doing = true;
	st_irq_off();

	ret = sitronix_spi_hdl_fw(dump_buf);
	if (ret < 0) {
		sterr("%s fail!\n", __func__);
		strscpy(gts->upgrade_msg, "sitronix_spi_hdl_fw fail",
			sizeof(gts->upgrade_msg));
	} else {
		stmsg("%s success\n", __func__);
	}
	if (ret >= 0) {
		/* CMD_MCU_KEY */
		cmd[1] = 0x00;
		cmd[2] = 0xF3;
		cmd[3] = 0x00;
		cmd[4] = 0x00;
		cmd[5] = 0x00;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0)
			sterr("HDL HOST PRAM Lock fail\n");

		/* HOST PROG DONE */
		cmd[1] = 0x00;
		cmd[2] = 0xF0;
		cmd[3] = 0x06;
		cmd[4] = 0x00;
		cmd[5] = 0x01;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0)
			sterr("HDL HOST PROG DONE fail\n");
		msleep(20);

		/* PRAM Lock */
		cmd[1] = 0x53;
		cmd[2] = 0x71;
		cmd[3] = 0x23;
		cmd[4] = 0x50;
		cmd[5] = 0x4C;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0)
			sterr("HDL HOST PRAM Lock fail\n");

		/* URAM Lock */
		cmd[1] = 0x53;
		cmd[2] = 0x71;
		cmd[3] = 0x23;
		cmd[4] = 0x55;
		cmd[5] = 0x4C;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0)
			sterr("HDL HOST  URAM Lock fail\n");

		/* AFE Lock */
		cmd[1] = 0x53;
		cmd[2] = 0x71;
		cmd[3] = 0x23;
		cmd[4] = 0x41;
		cmd[5] = 0x4C;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0)
			sterr("HDL HOST AFE Lock fail\n");

		/* CMD Mode */
		cmd[1] = 0x53;
		cmd[2] = 0x71;
		cmd[3] = 0x23;
		cmd[4] = 0x5A;
		cmd[5] = 0xC3;
		ret = sitronix_ts_addrmode_write(gts, cmd, 5);
		if (ret < 0)
			sterr("HDL HOST CMD Mode fail\n");
	}

	msleep(20);

	st_irq_on();
	gts->upgrade_doing = false;
	return ret;
}

int sitronix_do_upgrade(void)
{
	int ret = 0;

#ifdef ST_UPGRADE_USE_REQUESTFW_BUF
	int retry = 0;
	const struct firmware *fw = NULL;

	if (gts->fw_request_status == 0) {
		dump_buf_size = 0;
		while (retry < 3) {
			//ret = request_firmware(&fw, ST_REQUESTFW_DF_PATH, &gts->pdev->dev);
			//stmsg("request_firmware PATH = %s\n", fw_path);
			ret = request_firmware(&fw, fw_path, &gts->pdev->dev);
			if (ret) {
				sterr("request_firmware fail - %d\n", retry);
				retry++;
			} else {
				retry = 3;
			}
		}
		if (ret)
			return -ENOENT;

		//stmsg("request_firmware OK (%d)!\n");
		//dump_buf = (unsigned char *) fw->data;
		dump_buf_size = fw->size;
		if (dump_buf_size > ST_DUMP_MAX_LEN)
			dump_buf_size = ST_DUMP_MAX_LEN;
		memcpy(&dump_buf[0], fw->data, dump_buf_size);
		gts->fw_request_status = 1;
		stmsg("request_firmware %s OK (%ld)\n", fw_path,
		      dump_buf_size);
	}
#endif
	/* if (gts->host_if->bus_type == BUS_SPI && !gts->host_if->is_use_flash) */
	if (!gts->host_if->is_use_flash) {
		ret = sitronix_do_upgrade_hostdownload();
#ifdef ST_SKIP_HDL_IN_PROBE
		sitronix_ts_get_device_info(gts);
#endif //ST_SKIP_HDL_IN_PROBE
	} else {
		ret = sitronix_do_upgrade_flash_icp_all();
	}

#ifdef ST_UPGRADE_USE_REQUESTFW_BUF
	release_firmware(fw);
	dump_buf_size = 0;
#endif
	return ret;
}

void sitronix_replace_dump_buf(unsigned char *id)
{
	//set as default request firmware path
	memset(fw_path, 0, sizeof(fw_path));
	strscpy(fw_path, ST_REQUESTFW_DF_PATH);
}
