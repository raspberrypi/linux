// SPDX-License-Identifier: GPL-2.0
#include "sitronix_ts.h"
#include "sitronix_st7123.h"

//rawdata parameters
int rawdataType; //0 = disable, 1 = rawdata, 2 = delta
int *rawBase; //baseline
//
int sitronix_ts_get_device_status(struct sitronix_ts_data *ts_data)
{
	int ret;
	uint8_t buf[8];

	ret = sitronix_ts_reg_read(ts_data, STATUS_REG, buf, sizeof(buf));
	if (ret < 0) {
		sterr("%s: Read Status register error!(%d)\n", __func__, ret);
		return ret;
	}

	stmsg("buf = %X, %X, %X, %X, %X, %X, %X, %X\n", buf[0], buf[1], buf[2],
	      buf[3], buf[4], buf[5], buf[6], buf[7]);
	stmsg("Status register = %d.\n", (buf[0] & 0xFF));

	return (int)(buf[0] & 0x0F);
}

static int sitronix_ts_get_fw_revision(struct sitronix_ts_data *ts_data)
{
	int ret = 0;
	uint8_t buf[4];

	ret = sitronix_ts_reg_read(ts_data, FIRMWARE_VERSION, buf, 1);
	if (ret < 0) {
		sterr("%s: Read FW Version error!(%d)\n", __func__, ret);
		return ret;
	}

	ts_data->ts_dev_info.fw_version = buf[0];
	stmsg("FW Version (hex) = %x\n", buf[0]);

	ret = sitronix_ts_reg_read(ts_data, FIRMWARE_REVISION_3, buf,
				   sizeof(buf));
	if (ret < 0) {
		sterr("%s: Read FW revision error!(%d)\n", __func__, ret);
		return ret;
	}

	memcpy(&ts_data->ts_dev_info.fw_revision, buf, 4);
	stmsg("FW revision (hex) = %x %x %x %x\n", buf[0], buf[1], buf[2],
	      buf[3]);

	return 0;
}

static int sitronix_ts_get_max_touches(struct sitronix_ts_data *ts_data)
{
	int ret = 0;
	uint8_t max_touches;

	ret = sitronix_ts_reg_read(ts_data, MAX_NUM_TOUCHES, &max_touches,
				   sizeof(max_touches));
	if (ret < 0) {
		sterr("%s: Read max touches error!(%d)\n", __func__, ret);
		return ret;
	}

	ts_data->ts_dev_info.max_touches = max_touches;
	stmsg("Max touches = %d.\n", ts_data->ts_dev_info.max_touches);

	return 0;
}

static int sitronix_ts_get_chip_id(struct sitronix_ts_data *ts_data)
{
	int ret = 0;
	uint8_t chip_id;

	ret = sitronix_ts_reg_read(ts_data, CHIP_ID, &chip_id, sizeof(chip_id));
	if (ret < 0) {
		sterr("%s: Read chip ID error!(%d)\n", __func__, ret);
		return ret;
	}

	ts_data->ts_dev_info.chip_id = chip_id;
	stmsg("Chip ID = 0x%X.\n", ts_data->ts_dev_info.chip_id);

	return 0;
}

static int sitronix_ts_get_xy_chs(struct sitronix_ts_data *ts_data)
{
	int ret = 0;
	uint8_t buf[18];

	ret = TDU_FWInfoRead(2, buf, sizeof(buf));
	if (ret < 0) {
		sterr("%s: Read XY channels error!(%d)\n", __func__, ret);
		//return ret;
	}

	ts_data->ts_dev_info.x_chs = buf[2];
	ts_data->ts_dev_info.y_chs = buf[3];
	ts_data->ts_dev_info.k_chs = buf[4];
	stmsg("X_chs = %d.\n", ts_data->ts_dev_info.x_chs);
	stmsg("Y_chs = %d.\n", ts_data->ts_dev_info.y_chs);
	stmsg("K_chs = %d.\n", ts_data->ts_dev_info.k_chs);

	ts_data->ts_dev_info.n_chs = (buf[5] & 0x78) >> 3;
	//stmsg("n_chs = %d.\n", ts_data->ts_dev_info.n_chs);
	ts_data->ts_dev_info.max_swk_touch_num = buf[13];
	stmsg("swk_num = %d.\n", ts_data->ts_dev_info.max_swk_touch_num);
	return 0;
}

static int sitronix_ts_get_resolution(struct sitronix_ts_data *ts_data)
{
	int ret = 0;
	uint8_t buf[4];

	ret = sitronix_ts_reg_read(ts_data, X_RESOLUTION_HIGH, buf,
				   sizeof(buf));
	if (ret < 0) {
		sterr("%s: Read resolution error!(%d)\n", __func__, ret);
		return ret;
	}

	ts_data->ts_dev_info.x_res = (((uint16_t)buf[0] & 0x3F) << 8) | buf[1];
	ts_data->ts_dev_info.y_res = (((uint16_t)buf[2] & 0x3F) << 8) | buf[3];
	stmsg("Resolution = %u x %u\n", ts_data->ts_dev_info.x_res,
	      ts_data->ts_dev_info.y_res);

	return 0;
}

static int sitronix_ts_get_customer_info(struct sitronix_ts_data *ts_data)
{
	int ret = 0;
	uint8_t buf[4];
	uint8_t bank;

	ret = sitronix_ts_reg_read(ts_data, MISC_CONTROL, buf, 1);
	if (ret < 0) {
		sterr("%s: Read MISC_CONTROL error!(%d)\n", __func__, ret);
		return ret;
	}

	bank = buf[0];
	buf[0] = (buf[0] & 0xFC) | 1;
	ret = sitronix_ts_reg_write(ts_data, MISC_CONTROL, buf, 1);

	ret = sitronix_ts_reg_read(ts_data, FIRMWARE_REVISION_3, buf,
				   sizeof(buf));
	memcpy(&ts_data->ts_dev_info.customer_info, buf, 4);
	stmsg("Customer Info (hex) = %x %x %x %x\n", buf[0], buf[1], buf[2],
	      buf[3]);

	buf[0] = bank;
	ret = sitronix_ts_reg_write(ts_data, MISC_CONTROL, buf, 1);
	return 0;
}

static int sitronix_ts_get_misc_info(struct sitronix_ts_data *ts_data)
{
	int ret = 0;
	uint8_t misc_info;

	ret = sitronix_ts_reg_read(ts_data, MISC_INFO, &misc_info,
				   sizeof(misc_info));
	if (ret < 0) {
		sterr("%s: Read Misc. Info error!(%d)\n", __func__, ret);
		return ret;
	}

	ts_data->ts_dev_info.misc_info = misc_info;
	ts_data->is_support_coord_chksum =
		(misc_info & ST_MISC_INFO_COORD_CHKSUM_FLAG);

	stmsg("Misc. Info = 0x%X.\n", ts_data->ts_dev_info.misc_info);

	return 0;
}

int sitronix_ts_get_device_info(struct sitronix_ts_data *ts_data)
{
	int ret;

	ret = sitronix_ts_get_fw_revision(ts_data);
	if (ret)
		return ret;
	ret = sitronix_ts_get_customer_info(ts_data);
	if (ret)
		return ret;
	ret = sitronix_ts_get_resolution(ts_data);
	if (ret)
		return ret;
	ret = sitronix_ts_get_max_touches(ts_data);
	if (ret)
		return ret;
	ret = sitronix_ts_get_chip_id(ts_data);
	if (ret)
		return ret;
	ret = sitronix_ts_get_xy_chs(ts_data);
	if (ret)
		return ret;
	ret = sitronix_ts_get_misc_info(ts_data);
	if (ret)
		return ret;

	return 0;
}

int sitronix_ts_powerdown(struct sitronix_ts_data *ts_data, bool powerdown)
{
	int ret = 0;
	uint8_t ctrl;

	ret = sitronix_ts_reg_read(ts_data, DEVICE_CONTROL_REG, &ctrl, 1);
	if (ret < 0) {
		sterr("%s: Read device control status error! (%d)\n", __func__,
		      ret);
		return ret;
	}

	ctrl = powerdown ? (ctrl | 0x02) : (ctrl & ~0x02);

	ret = sitronix_ts_reg_write(ts_data, DEVICE_CONTROL_REG, &ctrl, 1);
	if (ret < 0) {
		sterr("%s: Write power down error! (%d)\n", __func__, ret);
		return ret;
	}

	return 0;
}

struct CommandIoPacket {
	unsigned char CmdID;
	unsigned char ValidDataSize;
	unsigned char CmdData[30];
};

void STChecksumCalculation(unsigned short *pChecksum, unsigned char *pInData,
			   unsigned long Len)
{
	unsigned long i;
	unsigned char LowByteChecksum = 0;

	for (i = 0; i < Len; i++) {
		*pChecksum += (unsigned short)pInData[i];
		LowByteChecksum = (unsigned char)(*pChecksum & 0xFF);
		LowByteChecksum = (LowByteChecksum) >> 7 | (LowByteChecksum)
								   << 1;
		*pChecksum = (*pChecksum & 0xFF00) | LowByteChecksum;
	}
}

static bool TDU_SetH2DReady(void)
{
	int ret;
	bool bRet = false;
	unsigned char buf[2];

	buf[0] = 0xF8;
	buf[1] = 0x01;

	/* ret = st_spi_write_bytes(0xF8, buf+1, 1); */
	ret = sitronix_ts_reg_write(gts, CMDIO_CONTROL, buf + 1, 1);
	if (ret <= 0) {
		sterr("%s: write ready error.\n", __func__);
		bRet = false;
	} else {
		bRet = true;
	}
	return bRet;
}

static bool TDU_GetH2DReady(void)
{
	bool bRet = false;
	unsigned char tmp = 0xff;
	int ret, retry = 0;

	do {
		usleep_range(1000, 2000);
		/* ret = st_spi_read_bytes(0xF8, &tmp, 1); */
		ret = sitronix_ts_reg_read(gts, CMDIO_CONTROL, &tmp, 1);

		if (ret <= 0) {
			sterr("%s: retry(%d) read ready.\n", __func__, retry++);
		} else {
			if (tmp == 0x01) {
				stdbg("retry  ............\n");
				retry++;
				ret = 0;
				/* continue; */
			}
		}
		if (retry > 1000) {
			sterr("%s: time out\n", __func__);
			/* break; */
			bRet = false;
			return bRet;
		}

	} while (ret <= 0);

	if (tmp == 0x00) { /* OK */
		bRet = true;
	} else if (tmp == 0x80) {
		sterr("TDU_ReadIOCommand: Unknown Command ID.\n");
		bRet = false;
	} else if (tmp == 0x81) {
		sterr("TDU_ReadIOCommand: Host to device command checksum error.\n");
		bRet = false;
	} else {
		sterr("Unknown Error(0x%02X).\n", tmp);
		bRet = false;
	}
	return bRet;
}

static bool TDU_ReadIOCommand(struct CommandIoPacket *packet)
{
	bool bRet = false;
	int ret;
	unsigned char tmp[32];
	/* process command */
	memset(tmp, 0, 32);
	/* ret = st_spi_read_bytes(0xD0, tmp, 32); */
	ret = sitronix_ts_reg_read(gts, CMDIO_PORT, tmp, 32);
	if (ret <= 0) {
		sterr("%s: read packet error.\n", __func__);
		bRet = false;
	} else {
		memcpy(packet, tmp, 32);
		bRet = true;
	}
	return bRet;
}

static bool TDU_WriteIOCommand(struct CommandIoPacket *packet)
{
	bool bRet = false;
	int ret;
	unsigned char tmp[33];

	memset(tmp, 0x00, 33);
	memcpy(tmp + 1, packet, 32);

	tmp[0] = 0xD0;
	/* ret = st_spi_write_bytes(0xD0, tmp+1, 32); */
	ret = sitronix_ts_reg_write(gts, CMDIO_PORT, tmp + 1, 32);

	if (ret <= 0) {
		sterr("%s: write packet error.\n", __func__);
		bRet = false;
	} else {
		bRet = true;
	}

	return bRet;
}

int TDU_CmdioRead(int type, int address, unsigned char *buf, int len)
{
	int getLen = 0, offset = 0;
	struct CommandIoPacket outPacket;
	struct CommandIoPacket inPacket;
	int remain = len;
	int pktDataSize = 0;
	unsigned short chksum, vchksum;
	int retry = 0;

	do {
		pktDataSize = (remain > 24) ? 24 : remain;
		outPacket.CmdID = 0x02; /* read RAM/ROM */
		outPacket.ValidDataSize = 5;
		outPacket.CmdData[0] = type; /* RAM */
		outPacket.CmdData[3] = pktDataSize;
		chksum = 0;
		STChecksumCalculation(&chksum, (unsigned char *)&outPacket, 6);
		outPacket.CmdData[4] = (chksum & 0xFF);
		if (!TDU_WriteIOCommand(&outPacket)) {
			stmsg("%s: (E)TDU_WriteIOCommand.\n", __func__);
			goto TDU_CmdioRead_retry;
			/* return getLen; */
		}
		if (!TDU_SetH2DReady()) {
			stmsg("%s: (E)TDU_SetH2DReady.\n", __func__);
			goto TDU_CmdioRead_retry;
			/* return getLen; */
		}
		if (!TDU_GetH2DReady()) {
			stmsg("%s: (E)TDU_GetH2DReady.\n", __func__);
			goto TDU_CmdioRead_retry;
			/* return getLen; */
		}
		if (!TDU_ReadIOCommand(&inPacket)) {
			stmsg("%s: (E)TDU_ReadIOCommand.\n", __func__);
			goto TDU_CmdioRead_retry;
			/* return getLen; */
		}

		if (inPacket.CmdID == 0x82 && inPacket.CmdData[0] == type) {
			vchksum = 0;
			STChecksumCalculation(&vchksum,
					      (unsigned char *)&inPacket,
					      inPacket.ValidDataSize + 1);
			vchksum = (vchksum & 0xFF);
			if (vchksum ==
			    inPacket.CmdData[inPacket.ValidDataSize - 1]) {
				memcpy(buf + offset, &(inPacket.CmdData[2]),
				       inPacket.CmdData[1]);
				remain -= inPacket.CmdData[1]; /* data size */
				offset += inPacket.CmdData[1];
				getLen += inPacket.CmdData[1];
				retry = 0;

			} else {
				/* drop packet */
				stmsg("Invalid Cheksum Expect(0x%02x) Get(0x%02X)\n",
				      vchksum,
				      inPacket.CmdData[inPacket.ValidDataSize -
						       1]);
				goto TDU_CmdioRead_retry;
			}
		} else {
			/* drop packet */
			stmsg("Unexpect CmdID (0x%02x) or Type (0x%02X)\n",
			      inPacket.CmdID, inPacket.CmdData[0]);
TDU_CmdioRead_retry:
			retry++;
			sterr("%s_retry %d with add = %x, size = %d\n",
			      __func__, retry, (address + offset), pktDataSize);
			if (retry > 10)
				return -EIO;
		}

	} while (remain > 0);
	return getLen;
}

int TDU_CmdioWrite(int type, int address, unsigned char *buf, int len)
{
	int setLen = 0, offset = 0;
	struct CommandIoPacket outPacket;
	/* CommandIoPacket inPacket; */
	int remain = len;
	int pktDataSize = 0;
	unsigned short chksum; /* , vchksum; */
	int retry = 0;

	do {
		pktDataSize = (remain > 24) ? 24 : remain;
		outPacket.CmdID = 0x01; /* write RAM/ROM */
		outPacket.ValidDataSize = pktDataSize + 5;
		outPacket.CmdData[0] = type;
		outPacket.CmdData[3] = pktDataSize;
		memcpy(&outPacket.CmdData[4], buf + offset, pktDataSize);
		chksum = 0;
		STChecksumCalculation(&chksum, (unsigned char *)&outPacket,
				      outPacket.ValidDataSize + 1);
		outPacket.CmdData[outPacket.ValidDataSize - 1] =
			(chksum & 0xFF);
		if (!TDU_WriteIOCommand(&outPacket)) {
			sterr("TDU_SetDeviceRam: (E)TDU_WriteIOCommand.\n");
			goto TDU_CmdioWrite_retry;
			/* return setLen; */
		}
		if (!TDU_SetH2DReady()) {
			sterr("TDU_SetDeviceRam: (E)TDU_SetH2DReady.\n");
			goto TDU_CmdioWrite_retry;
			/* return setLen; */
		}
		/* check processing result */
		if (!TDU_GetH2DReady()) {
			sterr("TDU_SetDeviceRam: (E)TDU_GetH2DReady.\n");
			/* return setLen; */
TDU_CmdioWrite_retry:
			retry++;
			sterr("TDU_CmdioWrite_retry %d with add = %x, size = %d\n",
			      retry, (address + offset), pktDataSize);
			if (retry > 10)
				return -EIO;
		} else {
			/* processing write command OK */
			remain -= pktDataSize;
			offset += pktDataSize;
			setLen += pktDataSize;
			retry = 0;
		}

	} while (remain > 0);

	return setLen;
}

static int TDU_Cmdio_ReadFwControl(int ctrlID, unsigned char *inBuf, int inLen,
				    unsigned char *getBuf)
{
	int getLen = 0;
	struct CommandIoPacket outPacket;
	struct CommandIoPacket inPacket;
	unsigned short chksum, vchksum;

	if (inLen > 28)
		inLen = 28;

	outPacket.CmdID = 0x05; //FW control command
	outPacket.ValidDataSize = inLen + 2; //1 for control id, 1 for checksum
	outPacket.CmdData[0] = 0x80 | (ctrlID & 0xFF); //read

	memcpy(&outPacket.CmdData[1], inBuf, inLen); //control data
	chksum = 0;
	STChecksumCalculation(&chksum, (unsigned char *)&outPacket,
			      outPacket.ValidDataSize + 1);

	outPacket.CmdData[outPacket.ValidDataSize - 1] = (chksum & 0xFF);

	if (!TDU_WriteIOCommand(&outPacket)) {
		sterr("%s(NewA): (E)WriteIOCommand.\n", __func__);
		return getLen;
	}
	if (!TDU_SetH2DReady()) {
		sterr("%s(NewA): (E)SetH2DReady.\n", __func__);
		return getLen;
	}
	if (!TDU_GetH2DReady()) {
		sterr("%s(NewA): (E)GetH2DReady.\n", __func__);
		return getLen;
	}
	if (!TDU_ReadIOCommand(&inPacket)) {
		sterr("%s(NewA): (E)ReadIOCommand.\n", __func__);
		return getLen;
	}

	if ((inPacket.CmdID == 0x85) &&
	    (inPacket.CmdData[0] == (0x80 | (ctrlID & 0xFF)))) {
		vchksum = 0;
		STChecksumCalculation(&vchksum, (unsigned char *)&inPacket,
				      inPacket.ValidDataSize + 1);
		vchksum = (vchksum & 0xFF);
		if (vchksum == inPacket.CmdData[inPacket.ValidDataSize - 1]) {
			memcpy(getBuf, &(inPacket.CmdData[1]),
			       inPacket.ValidDataSize - 2);
			getLen += (inPacket.ValidDataSize - 2);
		} else {
			//drop packet
			sterr("%s(NewA): Invalid Cheksum Expect(0x%02x) Get(0x%02X)\n",
			      __func__, vchksum,
			      inPacket.CmdData[inPacket.ValidDataSize - 1]);
			return -1;
		}

	} else {
		sterr("%s(NewA): Unknown Commad(0x%02X) or control info(0x%02X)\n",
		      __func__, inPacket.CmdID, inPacket.CmdData[0]);
		return -1;
	}
	return getLen;
}

static int TDU_Cmdio_WriteFwControl(int ctrlID, unsigned char *buf, int len)
{
	int setLen = 0;
	struct CommandIoPacket outPacket;
	unsigned short chksum;

	if (len > 28)
		len = 28;

	outPacket.CmdID = 0x05; //FW control command
	outPacket.ValidDataSize = len + 2; //1 for CtrlID, 1 for checksum
	outPacket.CmdData[0] = (ctrlID & 0xFF);
	memcpy(&outPacket.CmdData[1], buf, len);

	chksum = 0;
	STChecksumCalculation(&chksum, (unsigned char *)&outPacket,
			      outPacket.ValidDataSize + 1);
	outPacket.CmdData[outPacket.ValidDataSize - 1] = (chksum & 0xFF);

	if (!TDU_WriteIOCommand(&outPacket)) {
		sterr("%s(NewA): (E)WriteIOCommand.\n", __func__);
		return setLen;
	}
	if (!TDU_SetH2DReady()) {
		sterr("%s(NewA): (E)SetH2DReady.\n", __func__);
		return setLen;
	}
	//check processing result
	if (!TDU_GetH2DReady()) {
		sterr("%s(NewA): (E)GetH2DReady.\n", __func__);
		return setLen;
	}
	//processing write command OK
	setLen += len;

	return setLen;
}

// mode=0: SWK mode.
// mode=1: Gesture mode.
static int TDU_Cmdio_ReadSWKConfig(int mode, unsigned char *buf, int buf_len)
{
	int get_len = 0;
	struct CommandIoPacket outPacket;
	struct CommandIoPacket inPacket;
	unsigned short chksum, vchksum;

	memset(&outPacket, 0, sizeof(outPacket));
	memset(&inPacket, 0, sizeof(inPacket));

	outPacket.CmdID = 0x07; //SWK config command
	outPacket.ValidDataSize =
		buf_len + 2; //1 for Control Info, 1 for checksum
	outPacket.CmdData[0] = 0x80 | (mode << 6); //read

	chksum = 0;
	STChecksumCalculation(&chksum, (unsigned char *)&outPacket,
			      outPacket.ValidDataSize + 1);

	outPacket.CmdData[outPacket.ValidDataSize - 1] = (chksum & 0xFF);

	if (!TDU_WriteIOCommand(&outPacket)) {
		sterr("%s: (E)WriteIOCommand.\n", __func__);
		return get_len;
	}
	if (!TDU_SetH2DReady()) {
		sterr("%s: (E)SetH2DReady.\n", __func__);
		return get_len;
	}
	if (!TDU_GetH2DReady()) {
		sterr("%s: (E)GetH2DReady.\n", __func__);
		return get_len;
	}
	if (!TDU_ReadIOCommand(&inPacket)) {
		sterr("%s: (E)ReadIOCommand.\n", __func__);
		return get_len;
	}

	if ((inPacket.CmdID == 0x87) && (inPacket.CmdData[0] & 0x80)) {
		vchksum = 0;
		STChecksumCalculation(&vchksum, (unsigned char *)&inPacket,
				      inPacket.ValidDataSize + 1);
		vchksum = (vchksum & 0xFF);
		if (vchksum == inPacket.CmdData[inPacket.ValidDataSize - 1]) {
			//memcpy(buf, &(inPacket.CmdData[1]), inPacket.ValidDataSize - 2);
			memcpy(buf, &(inPacket.CmdData[1]), buf_len);
			get_len += (inPacket.ValidDataSize - 2);
			stmsg("%s: buf_len=%u, get_len=%u.\n", __func__,
			      buf_len, get_len);
		} else {
			//drop packet
			sterr("%s: Invalid Cheksum Expect(0x%02x) Get(0x%02X)\n",
			      __func__, vchksum,
			      inPacket.CmdData[inPacket.ValidDataSize - 1]);
			return -1;
		}
	} else {
		sterr("%s: Unknown Commad(0x%02X) or control info(0x%02X)\n",
		      __func__, inPacket.CmdID, inPacket.CmdData[0]);
		return -1;
	}

	return get_len;
}

// mode=0: SWK mode.
// mode=1: Gesture mode.
static int TDU_Cmdio_WriteSWKConfig(int mode, unsigned char *buf, int len)
{
	int set_len = 0;
	struct CommandIoPacket outPacket;
	unsigned short chksum;

	memset(&outPacket, 0, sizeof(outPacket));

	if (len > 28)
		len = 28;

	outPacket.CmdID = 0x07; //SWK config command
	outPacket.ValidDataSize = len + 2; //1 for Control Info, 1 for checksum
	outPacket.CmdData[0] = (mode << 6); //write config
	memcpy(&outPacket.CmdData[1], buf, len);

	chksum = 0;
	STChecksumCalculation(&chksum, (unsigned char *)&outPacket,
			      outPacket.ValidDataSize + 1);
	outPacket.CmdData[outPacket.ValidDataSize - 1] = (chksum & 0xFF);

	if (!TDU_WriteIOCommand(&outPacket)) {
		sterr("%s: (E)WriteIOCommand.\n", __func__);
		return set_len;
	}
	if (!TDU_SetH2DReady()) {
		sterr("%s: (E)SetH2DReady.\n", __func__);
		return set_len;
	}
	//check processing result
	if (!TDU_GetH2DReady()) {
		sterr("%s: (E)GetH2DReady.\n", __func__);
		return set_len;
	}
	//processing write command OK
	set_len += len;

	return set_len;
}

int TDU_FWInfoRead(int type, unsigned char *buf, int len)
{
	int getLen = 0, offset = 0;
	struct CommandIoPacket outPacket;
	struct CommandIoPacket inPacket;
	int remain = len;
	int pktDataSize = 0;
	unsigned short chksum, vchksum;
	int retry = 0;

	do {
		pktDataSize = (remain > 24) ? 24 : remain;
		outPacket.CmdID = 0x04; /* FW information read */
		outPacket.ValidDataSize = 2;
		outPacket.CmdData[0] = type; /* RAM */
		chksum = 0;
		STChecksumCalculation(&chksum, (unsigned char *)&outPacket, 3);
		outPacket.CmdData[1] = (chksum & 0xFF);

		if (!TDU_WriteIOCommand(&outPacket)) {
			stmsg("%s: (E)TDU_WriteIOCommand.\n", __func__);
			goto TDU_FWInfoRead_retry;
			/* return getLen; */
		}
		if (!TDU_SetH2DReady()) {
			stmsg("%s: (E)TDU_SetH2DReady.\n", __func__);
			goto TDU_FWInfoRead_retry;
			/* return getLen; */
		}
		if (!TDU_GetH2DReady()) {
			stmsg("%s: (E)TDU_GetH2DReady.\n", __func__);
			goto TDU_FWInfoRead_retry;
			/* return getLen; */
		}
		if (!TDU_ReadIOCommand(&inPacket)) {
			stmsg("%s: (E)TDU_ReadIOCommand.\n", __func__);
			goto TDU_FWInfoRead_retry;
			/* return getLen; */
		}

		if (inPacket.CmdID == 0x84 && inPacket.CmdData[0] == type) {
			vchksum = 0;
			STChecksumCalculation(&vchksum,
					      (unsigned char *)&inPacket,
					      inPacket.ValidDataSize + 1);
			vchksum = (vchksum & 0xFF);
			if (vchksum ==
			    inPacket.CmdData[inPacket.ValidDataSize - 1]) {
				memcpy(buf + offset, &(inPacket.CmdData[0]),
				       len);
				remain = 0;
				getLen = len;
				retry = 0;
			} else {
				/* drop packet */
				stmsg("Invalid Cheksum Expect(0x%02x) Get(0x%02X)\n",
				      vchksum,
				      inPacket.CmdData[inPacket.ValidDataSize -
						       1]);
				goto TDU_FWInfoRead_retry;
			}
		} else {
			/* drop packet */
			stmsg("Unexpect CmdID (0x%02x) or Type (0x%02X)\n",
			      inPacket.CmdID, inPacket.CmdData[0]);
TDU_FWInfoRead_retry:
			retry++;
			sterr("%s_retry %d with add = %x, size = %d\n",
			      __func__, retry, offset, pktDataSize);
			if (retry > 10)
				return -EIO;
		}

	} while (remain > 0);
	return getLen;
}

static int TDU_FWControlRead(int modeID, unsigned char *buf, int len)
{
	//buf[0] = 0x80;
	unsigned char rbuf[30];
	unsigned char wbuf[30];
	unsigned char rLen = 0;
	unsigned char wLen = 0;

	memset(rbuf, 0, sizeof(rbuf));
	memset(wbuf, 0, sizeof(wbuf));

	wbuf[wLen++] = modeID & 0xff; //mode id

	rLen = TDU_Cmdio_ReadFwControl(0x01, wbuf, wLen, rbuf);
	memcpy(buf, rbuf + 1, len);
	stmsg("Read Mode[%02X] = %02X\n", rbuf[0], buf[0]);
	return 0;
}

static int TDU_FWControlWrite(int modeID, unsigned char *buf, int len)
{
	unsigned char wbuf[30];
	int wLen = 0, ret = 0;

	memset(wbuf, 0, sizeof(wbuf));
	wbuf[wLen++] = modeID & 0xFF; //mode id
	memcpy(wbuf + wLen, buf, len); //flag
	wLen = wLen + len;
	ret = TDU_Cmdio_WriteFwControl(0x01, wbuf, wLen);
	stmsg("Write Mode[%02X] = %02X (wLen = %d)\n", wbuf[0], wbuf[1], wLen);
	return ret;
}

int sitronix_mode_switch_value(int modeID, bool flag, unsigned char value)
{
	int ret = 0;
	unsigned char buf[2];

	stmsg("modeID: %d , flag: %d , value: %x\n", modeID, flag, value);
	if (modeID > ST_MODE_SIZE) {
		sterr("%s modeID range error : %x\n", __func__, modeID);
		return -EINVAL;
	}

	buf[0] = flag ? ST_MODE_SWITCH_ON : ST_MODE_SWITCH_OFF;
	buf[0] |= value;

	ret = TDU_FWControlWrite(modeID, buf, 1);
	if (ret < 0) {
		sterr("TDU_FWControlWrite fail with error : %d\n", ret);
		return ret;
	}
	usleep_range(10000, 11000);
	ret = TDU_FWControlRead(modeID, buf + 1, 1);
	if (ret < 0) {
		sterr("TDU_FWControlRead fail with error : %d\n", ret);
		return ret;
	}

	if (buf[1] != buf[0]) {
		sterr("Mode switch fail with compare different\n");
		return -EINVAL;
	}
	gts->mode_flag[modeID] = flag;
	gts->mode_value[modeID] = value;

	stmsg("%s success with modeID: %x , flag : %d , value : %x\n", __func__,
	      modeID, flag, value);
	return 0;
}

int sitronix_mode_switch(int modeID, bool flag)
{
	return sitronix_mode_switch_value(modeID, flag, 0);
}

void sitronix_mode_backup(void)
{
	int i;
	u8 buf[1];

	for (i = ST_MODE_RESTORE_START; i < ST_MODE_SIZE; i++) {
		TDU_FWControlRead(i, buf, 1);

		gts->mode_flag[i] = (buf[0] & ST_MODE_SWITCH_ON) ? true : false;
		gts->mode_value[i] = (buf[0] & ~ST_MODE_SWITCH_ON);
	}
}

void sitronix_mode_restore(void)
{
	int i = 0;
	int ret = 0;
	unsigned char buf[2];

	for (i = ST_MODE_RESTORE_START; i < ST_MODE_SIZE; i++) {
		buf[0] = gts->mode_flag[i] ? ST_MODE_SWITCH_ON :
					     ST_MODE_SWITCH_OFF;
		buf[0] |= gts->mode_value[i];

		TDU_FWControlRead(i, buf + 1, 1);

		if (buf[1] != buf[0])
			ret = TDU_FWControlWrite(i, buf, 1);
		//msleep(30);
	}

	if (gts->exdiff_flag)
		sitronix_ts_exdiff_enable(gts, true);

	if (ret < 0)
		stmsg("%s fail\n", __func__);
	else
		stmsg("%s success\n", __func__);
}

void sitronix_swk_config_backup(void)
{
	int mode = 0; //0: SWK mode.
	int ret = 0;
	int i;

	if (gts) {
		ret = TDU_Cmdio_ReadSWKConfig(mode, gts->swk_config,
					      sizeof(gts->swk_config));
		if (ret < 0) {
			sterr("%s: TDU_Cmdio_ReadSWKConfig() failed!\n",
			      __func__);
			gts->swk_config_def[0] = 0x01;
			gts->swk_config_def[1] = 0x8F;
			gts->swk_config_def[2] = 0xFF;
			gts->swk_config_def[3] = 0x0F;
		} else {
			memcpy(gts->swk_config_def, gts->swk_config,
			       sizeof(gts->swk_config));
		}

		for (i = 0; i < ARRAY_SIZE(gts->swk_config); i++) {
			stmsg("Read swk_config[%02X] = 0x%02X\n", i,
			      gts->swk_config[i]);
		}
	}
}

void sitronix_swk_config_restore(void)
{
	int mode = 0; //0: SWK mode.
	int ret = 0;

	if (gts) {
		ret = TDU_Cmdio_WriteSWKConfig(mode, gts->swk_config,
					       sizeof(gts->swk_config));
		if (ret < 0) {
			sterr("%s: TDU_Cmdio_WriteSWKConfig() failed!\n",
			      __func__);
		}
	}
}
int sitronix_get_ic_position(unsigned char *buf)
{
	uint8_t cmd[10] = { 0 };
	int ret = 0;
	int off = 0;

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0xA5;
	cmd[5] = 0x3C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x14;
	cmd[5] = 0x55;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFC;
	cmd[4] = 0x5A;
	cmd[5] = 0x9D;
	cmd[6] = 0xBD;
	cmd[7] = 0x00;
	cmd[8] = 0x00;
	cmd[9] = 0x00;

	ret = sitronix_ts_addrmode_write(gts, cmd, 9);
	if (ret < 0) {
		sterr("read ic position step 1 fail - 1\n");
		return -1;
	}

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = 0x00;
	cmd[5] = 0xA5;

	ret = sitronix_ts_addrmode_write(gts, cmd, 5);
	if (ret < 0) {
		sterr("read ic position step 2 fail - 1\n");
		return -1;
	}

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFC;
	cmd[4] = 0x5A;
	cmd[5] = 0x9E;
	cmd[6] = 0x6C;
	cmd[7] = 0x00;

	ret = sitronix_ts_addrmode_write(gts, cmd, 7);
	if (ret < 0) {
		sterr("read ic position step 3 fail - 1\n");
		return -1;
	}

	wbuf[1] = 0x83;
	wbuf[2] = 0x07;
	wbuf[3] = 0x04;

	off = sitronix_ts_addrmode_split_read(gts, wbuf, 3, rbuf, 6);

	if (off < 0) {
		sterr("read ic position step 4 fail - 2\n");
		ret = -1;
	}

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = 0x00;
	cmd[5] = 0xA5;

	sitronix_ts_addrmode_write(gts, cmd, 5);

	if (ret < 0) {
		sterr("read ic position step 5 fail - 3\n");
		return -1;
	}

	stmsg("ic_position buf = 0x%X 0x%X\n", rbuf[off + 4], rbuf[off + 5]);
	memcpy(buf, rbuf + off + 4, 2);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x41;
	cmd[5] = 0x4C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x5A;
	cmd[5] = 0xC3;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);
	return ret;
}

int sitronix_get_chip_id(void)
{
	uint8_t cmd[8] = { 0 };
	int ret, off;

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0xA5;
	cmd[5] = 0x3C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x14;
	cmd[5] = 0x55;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x55;
	cmd[5] = 0x55;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	//AFE ADDR = (MCU SFR - 0x80) * 2 + 0x010100
	wbuf[1] = 0x81;
	wbuf[2] = 0x01;
	wbuf[3] = 0x1E;

	off = sitronix_ts_addrmode_read(gts, wbuf, 3, rbuf, 3);
	if (off < 0) {
		sterr("read Chip ID fail\n");
		ret = -1;
	} else {
		ret = rbuf[off + 1];
	}

	return ret;
}

int sitronix_get_ic_sfrver(void)
{
	uint8_t cmd[8] = { 0 };
	int ret, off;

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0xA5;
	cmd[5] = 0x3C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x14;
	cmd[5] = 0x55;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x55;
	cmd[5] = 0x55;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	//AFE ADDR = (MCU SFR - 0x80) * 2 + 0x010100
	wbuf[1] = 0x81;
	wbuf[2] = 0x01;
	wbuf[3] = 0xC2;

	off = sitronix_ts_addrmode_read(gts, wbuf, 3, rbuf, 3);
	if (off < 0) {
		sterr("read IC VER fail\n");
		ret = -1;
	} else {
		ret = rbuf[off + 1];
	}

	return ret;
}

int sitronix_write_driver_cmd(unsigned char dc, unsigned char *buf, int len)
{
	//uint8_t cmd[128] = {0};
	uint8_t *cmd;
	int cmd_len = 128;
	int ret = 0;

	if (len > (cmd_len - 6)) {
		sterr("The max len of driver cmd is %d , now is %d\n",
		      cmd_len - 6, len);
		return -1;
	}

	cmd = kzalloc(cmd_len, GFP_KERNEL);
	if (!cmd) {
		sterr("%s: Alloc memory for cmd failed!\n", __func__);
		return -ENOMEM;
	}

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0xA5;
	cmd[5] = 0x3C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x14;
	cmd[5] = 0x55;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFC;
	cmd[4] = 0x5A;
	cmd[5] = 0x9D;

	ret = sitronix_ts_addrmode_write(gts, cmd, 5);
	if (ret < 0) {
		sterr("write_driver_cmd step 1 fail - 1\n");
		ret = -1;
		goto err_return;
	}

	if (len % 2 == 1)
		len = len + 1;

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = dc;
	cmd[5] = 0x00;
	memcpy(cmd + 6, buf, len);

	ret = sitronix_ts_addrmode_write(gts, cmd, 5 + len);
	if (ret < 0) {
		sterr("write_driver_cmd step 2 fail - 1\n");
		ret = -1;
		goto err_return;
	}

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = 0x00;
	cmd[5] = 0xA5;

	sitronix_ts_addrmode_write(gts, cmd, 5);

	if (ret < 0) {
		sterr("write_driver_cmd step 3 fail - 3\n");
		ret = -1;
		goto err_return;
	}

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x41;
	cmd[5] = 0x4C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x5A;
	cmd[5] = 0xC3;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

err_return:
	kfree(cmd);

	return ret;
}

int sitronix_read_driver_cmd(unsigned char dc, unsigned char *buf, int len)
{
	uint8_t cmd[32] = { 0 };
	int ret = 0, off;

	if (len > sizeof(cmd) - 6) {
		sterr("The max len of driver cmd is %ld , now is %d\n",
		      (sizeof(cmd) - 6), len);
		return -1;
	}

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0xA5;
	cmd[5] = 0x3C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x14;
	cmd[5] = 0x55;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	//BD 00
	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFC;
	cmd[4] = 0x5A;
	cmd[5] = 0x9D;
	cmd[6] = 0xBD;
	cmd[7] = 0x00;
	cmd[8] = 0x00;
	cmd[9] = 0x00;
	ret = sitronix_ts_addrmode_write(gts, cmd, 9);

	//Transition Complete
	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = 0x00;
	cmd[5] = 0xA5;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFC;
	cmd[4] = 0x5A;
	cmd[5] = 0x9E;
	cmd[6] = dc;
	cmd[7] = 0x00;

	ret = sitronix_ts_addrmode_write(gts, cmd, 7);

	cmd[1] = 0x83;
	cmd[2] = 0x07;
	cmd[3] = 0x00;

	off = sitronix_ts_addrmode_split_read(gts, cmd, 3, wbuf, len);

	if (off < 0)
		sterr("read display CMD1 fail - 2\n");
	memcpy(buf, wbuf + off, len);

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = 0x00;
	cmd[5] = 0xA5;

	sitronix_ts_addrmode_write(gts, cmd, 5);

	//BD 03
	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFC;
	cmd[4] = 0x5A;
	cmd[5] = 0x9D;
	cmd[6] = 0xBD;
	cmd[7] = 0x00;
	cmd[8] = 0x03;
	cmd[9] = 0x00;
	ret = sitronix_ts_addrmode_write(gts, cmd, 9);

	//Transition Complete
	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = 0x00;
	cmd[5] = 0xA5;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x41;
	cmd[5] = 0x4C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x5A;
	cmd[5] = 0xC3;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);
	return ret;
}

void sitronix_swite_driver_deep_standby(void)
{
	int ret = 0;
	uint8_t cmd[16] = { 0 };

	stmsg("run %s\n", __func__);
	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0xA5;
	cmd[5] = 0x3C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x14;
	cmd[5] = 0x55;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	//mipi.write 0x29 0x60 0x71 0x23 0xa2
	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFC;
	cmd[4] = 0x5A;
	cmd[5] = 0x9D;
	cmd[6] = 0x60;
	cmd[7] = 0x00;
	cmd[8] = 0x71;
	cmd[9] = 0x23;
	cmd[10] = 0xA2;
	cmd[11] = 0x00;
	ret = sitronix_ts_addrmode_write(gts, cmd, 11);

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = 0x00;
	cmd[5] = 0xA5;
	sitronix_ts_addrmode_write(gts, cmd, 5);
	usleep_range(1000, 2000);

	//mipi.write 0x29 0x60 0x71 0x23 0xa3
	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFC;
	cmd[4] = 0x5A;
	cmd[5] = 0x9D;
	cmd[6] = 0x60;
	cmd[7] = 0x00;
	cmd[8] = 0x71;
	cmd[9] = 0x23;
	cmd[10] = 0xA3;
	cmd[11] = 0x00;
	ret = sitronix_ts_addrmode_write(gts, cmd, 8);

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = 0x00;
	cmd[5] = 0xA5;
	sitronix_ts_addrmode_write(gts, cmd, 5);
	usleep_range(1000, 2000);

	//mipi.write 0x29 0x60 0x71 0x23 0xa4
	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFC;
	cmd[4] = 0x5A;
	cmd[5] = 0x9D;
	cmd[6] = 0x60;
	cmd[7] = 0x00;
	cmd[8] = 0x71;
	cmd[9] = 0x23;
	cmd[10] = 0xA4;
	cmd[11] = 0x00;
	ret = sitronix_ts_addrmode_write(gts, cmd, 8);

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = 0x00;
	cmd[5] = 0xA5;
	sitronix_ts_addrmode_write(gts, cmd, 5);
	usleep_range(1000, 2000);

	//mipi.write 0x29 0x4F 0x01
	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFC;
	cmd[4] = 0x5A;
	cmd[5] = 0x9D;
	cmd[6] = 0x4F;
	cmd[7] = 0x00;
	cmd[8] = 0x01;
	cmd[9] = 0x00;
	ret = sitronix_ts_addrmode_write(gts, cmd, 9);

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = 0x00;
	cmd[5] = 0xA5;
	sitronix_ts_addrmode_write(gts, cmd, 5);
	usleep_range(1000, 2000);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x41;
	cmd[5] = 0x4C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x5A;
	cmd[5] = 0xC3;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);
}

static int sitronix_ts_read_raw_aa(struct sitronix_ts_data *ts_data, int *rawbuf)
{
	int ret = 0;
	int retry = 0, retrymax = 0, xCnt = 0, ix, iy, datasize, i;
	//unsigned char pkt[80];
	unsigned char *pkt;
	int pkt_len = 80;

	pkt = kzalloc(pkt_len, GFP_KERNEL);
	if (!pkt) {
		sterr("%s: Alloc memory for pkt failed!\n", __func__);
		return -ENOMEM;
	}

	retry = 0;
	retrymax = (gts->ts_dev_info.x_chs + gts->ts_dev_info.n_chs) * 2;

	//wait for header
	while (retry < retrymax) {
		ret = sitronix_ts_reg_read(ts_data, DATA_OUTPUT_BUFFER, pkt,
					   pkt_len);
		if (ret < 0)
			goto END_READ_RAW_AA;
		//stmsg("pkt type (H)= 0x%02x\n", pkt[0]);
		if (pkt[0] == 0x10) { //header
			break;
		}
		retry++;
		usleep_range(1000, 2000);
		if (retry > retrymax) {
			ret = -1;
			sterr("fail to read rawdata header.\n");
			goto END_READ_RAW_AA;
		}
	} //end of wait for header
	retry = 0;
	retrymax = gts->ts_dev_info.x_chs * 2;
	while (xCnt < gts->ts_dev_info.x_chs) {
		ret = sitronix_ts_reg_read(ts_data, DATA_OUTPUT_BUFFER, pkt,
					   pkt_len);
		if (ret < 0)
			goto END_READ_RAW_AA;
		//stmsg("pkt type (AA) = 0x%02x\n", pkt[0]);
		if (pkt[0] == 0x13) { //Rawdata AA
			ix = pkt[2];
			iy = pkt[3];
			datasize = (pkt[1] - 3) / 2;
			//stmsg("datasize = %d\n", datasize);
			for (i = 0; i < datasize; i++) {
				rawbuf[ix * gts->ts_dev_info.y_chs + iy + i] =
					(signed short)((pkt[2 + i * 2 + 2]
							<< 8) |
						       (pkt[2 + i * 2 + 3]));
			}
			//stmsg("get x_cnt = %d, x = %d, y = %d\n", xCnt, ix, iy);
			xCnt++;
		} else {
			//ignore packets or unknown packets
			retry++;
			usleep_range(1000, 2000);
			if (retry > retrymax) {
				ret = -2;
				sterr("fail to read rawdata AA.\n");
				goto END_READ_RAW_AA;
			}
		}
	}
	ret = 0;

END_READ_RAW_AA:
	kfree(pkt);

	return ret;
}

int sitronix_ts_enable_raw(struct sitronix_ts_data *ts_data, int type)
{
	int ret = 0, rawFrameCnt = 0, baselineCnt = 5, i, j, retry;
	unsigned char ctrl, tmp;
	int *rawbuf = NULL;

	if (type == rawdataType)
		return ret;
	if (type > 2)
		return -EINVAL;

	if (type == 0) {
		//exit rawdata mode
		ret = sitronix_ts_reg_read(ts_data, MISC_CONTROL, &ctrl, 1);
		ctrl = ctrl & 0xA3;
		ctrl = ctrl | 0x04;
		retry = 0;
		while (true) {
			sitronix_ts_reg_write(ts_data, MISC_CONTROL, &ctrl, 1);
			usleep_range(10000, 11000);
			ret = sitronix_ts_reg_read(ts_data, MISC_CONTROL, &tmp,
						   1);
			if (tmp == ctrl)
				break;
			retry++;
			if (retry > 5) {
				sterr("Fail to enable Normal mode.\n");
				return -EIO;
			}
		}
		//free baseline buffer
		kfree(rawBase);
		rawBase = NULL;
		rawdataType = type;
		stmsg("exit rawdata mode\n");
	} else {
		//enter rawdata mode
		if (rawdataType != 1 && rawdataType != 2) {
			//stmsg("do enter rawdata mode\n");
			ret = sitronix_ts_reg_read(ts_data, MISC_CONTROL, &ctrl,
						   1);
			ctrl = ctrl & 0xA3;
			ctrl = ctrl | 0x58;
			retry = 0;
			while (true) {
				sitronix_ts_reg_write(ts_data, MISC_CONTROL,
						      &ctrl, 1);
				usleep_range(10000, 11000);
				ret = sitronix_ts_reg_read(
					ts_data, MISC_CONTROL, &tmp, 1);
				if (tmp == ctrl)
					break;
				retry++;
				if (retry > 5) {
					sterr("Fail to enable RAWDATA mode.\n");
					return -EIO;
				}
			}
			//allocate baseline buffer
			rawFrameCnt = ts_data->ts_dev_info.x_chs *
				      ts_data->ts_dev_info.y_chs;
			//stmsg("raw frame node cnt = %d\n", rawFrameCnt);
			rawBase = kmalloc_array(rawFrameCnt, sizeof(int),
						GFP_KERNEL);
			memset(rawBase, 0, rawFrameCnt * sizeof(int));
			//create baseline
			rawbuf = kmalloc_array(rawFrameCnt, sizeof(int),
					       GFP_KERNEL);
			for (i = 0; i < baselineCnt; i++) {
				memset(rawbuf, 0, rawFrameCnt * sizeof(int));
				ret = sitronix_ts_read_raw_aa(ts_data, rawbuf);
				if (ret < 0) {
					sterr("Fail to create baseline\n");
					goto exit_enable_raw;
				}
				for (j = 0; j < rawFrameCnt; j++)
					rawBase[j] += rawbuf[j];
			}
			for (j = 0; j < rawFrameCnt; j++)
				rawBase[j] = (int)(rawBase[j] / baselineCnt);
		}
		rawdataType = type;
		stmsg("enter rawdata mode=%d\n", rawdataType);
	}

exit_enable_raw:
	kfree(rawbuf);
	rawbuf = NULL;
	return ret;
}

int sitronix_ts_get_rawdata(struct sitronix_ts_data *ts_data, int *rbuf)
{
	int ret = 0, rawFrameCnt, j;

	if (rawdataType == 0) {
		sterr("rawdata type was not defined.\n");
		return -EPROTO;
	}
	ret = sitronix_ts_read_raw_aa(ts_data, rbuf);
	if (ret >= 0) {
		if (rawdataType ==
		    2) { //calculate delta, delta = baseline - raw
			rawFrameCnt = ts_data->ts_dev_info.x_chs *
				      ts_data->ts_dev_info.y_chs;
			for (j = 0; j < rawFrameCnt; j++)
				rbuf[j] = rawBase[j] - rbuf[j];
		}
		ret = rawdataType;
	}
	return ret;
}

int sitronix_ts_set_rawdata_area(struct sitronix_ts_data *ts_data, int aaXs,
				 int aaXe, int aaYs, int aaYe, int idleXs,
				 int idleXe, int noiseXs, int noiseXe)
{
	int ret = 0;
	struct CommandIoPacket outPacket;
	unsigned short chksum;
	int retry = 5;

	outPacket.CmdID = 0x06; /* Set Read Rawdata area command */
	outPacket.ValidDataSize = 10;
	outPacket.CmdData[0] = 0x00; //write control
	outPacket.CmdData[1] = (aaXs & 0xFF); //AA X start
	outPacket.CmdData[2] = (aaXe & 0xFF); //AA X end
	outPacket.CmdData[3] = (idleXs & 0xFF); //idle X start
	outPacket.CmdData[4] = (idleXe & 0xFF); //idle X end
	outPacket.CmdData[5] = (aaYs & 0xFF); //Y start
	outPacket.CmdData[6] = (aaYe & 0xFF); //Y end
	outPacket.CmdData[7] = (noiseXs & 0xFF); //noise X start
	outPacket.CmdData[8] = (noiseXe & 0xFF); //noise X end
	chksum = 0;
	STChecksumCalculation(&chksum, (unsigned char *)&outPacket,
			      outPacket.ValidDataSize + 1);
	outPacket.CmdData[outPacket.ValidDataSize - 1] = (chksum & 0xFF);

	if (!TDU_WriteIOCommand(&outPacket)) {
		sterr("%s: (E)TDU_WriteIOCommand.\n", __func__);
		ret = -EIO;
		return ret;
	}
	if (!TDU_SetH2DReady()) {
		sterr("%s: (E)TDU_SetH2DReady.\n", __func__);
		ret = -EIO;
		return ret;
	}
	while (retry > 0) {
		msleep(20);
		/* check processing result */
		if (!TDU_GetH2DReady()) {
			retry--;
			if (retry <= 0) {
				sterr("%s: (E)TDU_GetH2DReady retry timeout.\n",
				      __func__);
				ret = -EIO;
				return ret;
			}
		} else {
			break;
		}
	}

	memset(&outPacket, 0, sizeof(outPacket));
	outPacket.CmdID = 0x06; /* Set Read Rawdata area command */
	outPacket.ValidDataSize = 2;
	outPacket.CmdData[0] = 0x80; //read control
	chksum = 0;
	STChecksumCalculation(&chksum, (unsigned char *)&outPacket,
			      outPacket.ValidDataSize + 1);
	outPacket.CmdData[outPacket.ValidDataSize - 1] = (chksum & 0xFF);
	if (!TDU_WriteIOCommand(&outPacket)) {
		sterr("%s %d: (E)TDU_WriteIOCommand.\n", __func__, __LINE__);
		ret = -EIO;
		return ret;
	}
	if (!TDU_SetH2DReady()) {
		sterr("%s %d: (E)TDU_SetH2DReady.\n", __func__, __LINE__);
		ret = -EIO;
		return ret;
	}
	while (retry > 0) {
		msleep(20);
		/* check processing result */
		if (!TDU_GetH2DReady()) {
			retry--;
			if (retry <= 0) {
				sterr("%s %d: (E)TDU_GetH2DReady retry timeout.\n",
				      __func__, __LINE__);
				ret = -EIO;
				return ret;
			}
		} else {
			break;
		}
	}
	return ret;
}

int sitronix_ts_exdiff_enable(struct sitronix_ts_data *ts_data, bool exdiff_en)
{
	unsigned char buf[4];
	int ret = 0;

	ret = sitronix_ts_reg_read(ts_data, EX_DIFF_EN, buf, 1);
	if (ret < 0) {
		sterr("%s: Read EX_DIFF_EN error! (%d)\n", __func__, ret);
		return ret;
	}

	if (exdiff_en)
		buf[0] |= 0x01; //set bit0
	else
		buf[0] &= 0xFE; //clear bit1

	ret = sitronix_ts_reg_write(ts_data, EX_DIFF_EN, buf, 1);
	if (ret < 0) {
		sterr("%s: Write EX_DIFF_EN error! (%d)\n", __func__, ret);
		return ret;
	}

	ret = sitronix_ts_reg_read(ts_data, EX_DIFF_EN, buf, 4);
	if (ret < 0) {
		sterr("%s: Check EX_DIFF_EN error! (%d)\n", __func__, ret);
		return ret;
	}

	if (exdiff_en) {
		if ((buf[0] & 0x01) && buf[1] > 0) {
			ts_data->exdiff_flag = true;
			ts_data->ex_diff_pac_num = buf[1] & 0xFF;
			ts_data->ex_diff_reg_map = ((buf[2] & 0xFF) << 8) |
						   (buf[3] & 0xFF);
		} else {
			ts_data->exdiff_flag = false;
			ts_data->ex_diff_pac_num = 0;
			ts_data->ex_diff_reg_map = 0;
			sterr("%s:EX_DIFF was not supported!\n", __func__);
			return -EINVAL;
		}
	} else {
		if ((buf[0] & 0x01) == 0x00) {
			ts_data->exdiff_flag = false;
			ts_data->ex_diff_pac_num = 0;
			ts_data->ex_diff_reg_map = 0;
		}
	}
	stmsg("set exdiff_flag = %s\n",
	      ts_data->exdiff_flag ? "true" : "false");
	if (gts->exdiff_flag) {
		stmsg("ex_diff_pac_num = %d, ex_diff_map_off = 0x%04X\n",
		      ts_data->ex_diff_pac_num, ts_data->ex_diff_reg_map);
	}

	return 0;
}

#define DEBUG_PRINT_EX_DIFF_DATA
int sitronix_ts_get_exdiff(struct sitronix_ts_data *ts_data,
			   unsigned char *diff)
{
	int ret = 0;
	u8 id = 0, x_pos = 0, y_pos = 0;
#ifdef DEBUG_PRINT_EX_DIFF_DATA
	int xi, yi, value;
#endif
	unsigned short chksum = 0;

	ret = sitronix_ts_reg_read(ts_data, ts_data->ex_diff_reg_map, diff,
				   ST_DIFF_DATA_TABLE_LEN);
	if (ret < 0) {
		sterr("%s: fail to read ex diff output data!(%d)\n", __func__,
		      ret);
		return ret;
	}
	STChecksumCalculation(&chksum, diff, ST_DIFF_DATA_TABLE_LEN - 1);
	if ((chksum & 0xFF) != diff[ST_DIFF_DATA_TABLE_LEN - 1]) {
		sterr("ExDiffChecksum error :  Expect (%02X) != Received (%02X)\n",
		      (chksum & 0xFF), diff[ST_DIFF_DATA_TABLE_LEN - 1]);
		return -EIO;
	}

	id = (diff[1] & 0xF0) >> 4; //ID
	y_pos = (diff[0] & 0x3F); //Rx
	x_pos = ((diff[1] & 0x0F) << 2) | ((diff[0] & 0xC0) >> 6); //Tx

	stmsg("Diff Data ID = %d at (%d, %d)\n", id, x_pos, y_pos);
#ifdef DEBUG_PRINT_EX_DIFF_DATA
	stmsg("diff[0] = 0x%02X, diff[1]=0x%02X\n", diff[0], diff[1]);
	for (yi = 0; yi < 7; yi++) {
		for (xi = 0; xi < 7; xi++) {
			value = (signed short)((diff[(xi * 2) + (yi * 2 * 7) + 2]
						<< 8) |
					       (diff[(xi * 2) + (yi * 2 * 7) +
						     3]));
			pr_debug("%4d,", value);
		}
		pr_debug("\n");
	}
#endif
	return ret;
}
