// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  linux/drivers/mmc/core/sd_ops.h
 *
 *  Copyright 2006-2007 Pierre Ossman
 */

#include <linux/slab.h>
#include <linux/types.h>
#include <linux/export.h>
#include <linux/scatterlist.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>

#include "core.h"
#include "sd_ops.h"
#include "mmc_ops.h"

/*
 * Extensive testing has shown that some specific SD cards
 * require an increased command timeout to be successfully
 * initialized.
 */
#define SD_APP_OP_COND_PERIOD_US	(10 * 1000) /* 10ms */
#define SD_APP_OP_COND_TIMEOUT_MS	2000 /* 2s */

struct sd_app_op_cond_busy_data {
	struct mmc_host *host;
	u32 ocr;
	struct mmc_command *cmd;
};

int mmc_app_cmd(struct mmc_host *host, struct mmc_card *card)
{
	int err;
	struct mmc_command cmd = {};

	if (WARN_ON(card && card->host != host))
		return -EINVAL;

	cmd.opcode = MMC_APP_CMD;

	if (card) {
		cmd.arg = card->rca << 16;
		cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;
	} else {
		cmd.arg = 0;
		cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_BCR;
	}

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err)
		return err;

	/* Check that card supported application commands */
	if (!mmc_host_is_spi(host) && !(cmd.resp[0] & R1_APP_CMD))
		return -EOPNOTSUPP;

	return 0;
}
EXPORT_SYMBOL_GPL(mmc_app_cmd);

static int mmc_wait_for_app_cmd(struct mmc_host *host, struct mmc_card *card,
				struct mmc_command *cmd)
{
	struct mmc_request mrq = {};
	int i, err = -EIO;

	/*
	 * We have to resend MMC_APP_CMD for each attempt so
	 * we cannot use the retries field in mmc_command.
	 */
	for (i = 0; i <= MMC_CMD_RETRIES; i++) {
		err = mmc_app_cmd(host, card);
		if (err) {
			/* no point in retrying; no APP commands allowed */
			if (mmc_host_is_spi(host)) {
				if (cmd->resp[0] & R1_SPI_ILLEGAL_COMMAND)
					break;
			}
			continue;
		}

		memset(&mrq, 0, sizeof(struct mmc_request));

		memset(cmd->resp, 0, sizeof(cmd->resp));
		cmd->retries = 0;

		mrq.cmd = cmd;
		cmd->data = NULL;

		mmc_wait_for_req(host, &mrq);

		err = cmd->error;
		if (!cmd->error)
			break;

		/* no point in retrying illegal APP commands */
		if (mmc_host_is_spi(host)) {
			if (cmd->resp[0] & R1_SPI_ILLEGAL_COMMAND)
				break;
		}
	}

	return err;
}

int mmc_app_set_bus_width(struct mmc_card *card, int width)
{
	struct mmc_command cmd = {};

	cmd.opcode = SD_APP_SET_BUS_WIDTH;
	cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

	switch (width) {
	case MMC_BUS_WIDTH_1:
		cmd.arg = SD_BUS_WIDTH_1;
		break;
	case MMC_BUS_WIDTH_4:
		cmd.arg = SD_BUS_WIDTH_4;
		break;
	default:
		return -EINVAL;
	}

	return mmc_wait_for_app_cmd(card->host, card, &cmd);
}

static int sd_app_op_cond_cb(void *cb_data, bool *busy)
{
	struct sd_app_op_cond_busy_data *data = cb_data;
	struct mmc_host *host = data->host;
	struct mmc_command *cmd = data->cmd;
	u32 ocr = data->ocr;
	int err;

	*busy = false;

	err = mmc_wait_for_app_cmd(host, NULL, cmd);
	if (err)
		return err;

	/* If we're just probing, do a single pass. */
	if (ocr == 0)
		return 0;

	/* Wait until reset completes. */
	if (mmc_host_is_spi(host)) {
		if (!(cmd->resp[0] & R1_SPI_IDLE))
			return 0;
	} else if (cmd->resp[0] & MMC_CARD_BUSY) {
		return 0;
	}

	*busy = true;
	return 0;
}

int mmc_send_app_op_cond(struct mmc_host *host, u32 ocr, u32 *rocr)
{
	struct mmc_command cmd = {};
	struct sd_app_op_cond_busy_data cb_data = {
		.host = host,
		.ocr = ocr,
		.cmd = &cmd
	};
	int err;

	cmd.opcode = SD_APP_OP_COND;
	if (mmc_host_is_spi(host))
		cmd.arg = ocr & (1 << 30); /* SPI only defines one bit */
	else
		cmd.arg = ocr;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R3 | MMC_CMD_BCR;

	err = __mmc_poll_for_busy(host, SD_APP_OP_COND_PERIOD_US,
				  SD_APP_OP_COND_TIMEOUT_MS, &sd_app_op_cond_cb,
				  &cb_data);
	if (err)
		return err;

	if (rocr && !mmc_host_is_spi(host))
		*rocr = cmd.resp[0];

	return 0;
}

static int __mmc_send_if_cond(struct mmc_host *host, u32 ocr, u8 pcie_bits,
			      u32 *resp)
{
	struct mmc_command cmd = {};
	int err;
	static const u8 test_pattern = 0xAA;
	u8 result_pattern;

	/*
	 * To support SD 2.0 cards, we must always invoke SD_SEND_IF_COND
	 * before SD_APP_OP_COND. This command will harmlessly fail for
	 * SD 1.0 cards.
	 */
	cmd.opcode = SD_SEND_IF_COND;
	cmd.arg = ((ocr & 0xFF8000) != 0) << 8 | pcie_bits << 8 | test_pattern;
	cmd.flags = MMC_RSP_SPI_R7 | MMC_RSP_R7 | MMC_CMD_BCR;

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err)
		return err;

	if (mmc_host_is_spi(host))
		result_pattern = cmd.resp[1] & 0xFF;
	else
		result_pattern = cmd.resp[0] & 0xFF;

	if (result_pattern != test_pattern)
		return -EIO;

	if (resp)
		*resp = cmd.resp[0];

	return 0;
}

int mmc_send_if_cond(struct mmc_host *host, u32 ocr)
{
	return __mmc_send_if_cond(host, ocr, 0, NULL);
}

int mmc_send_if_cond_pcie(struct mmc_host *host, u32 ocr)
{
	u32 resp = 0;
	u8 pcie_bits = 0;
	int ret;

	if (host->caps2 & MMC_CAP2_SD_EXP) {
		/* Probe card for SD express support via PCIe. */
		pcie_bits = 0x10;
		if (host->caps2 & MMC_CAP2_SD_EXP_1_2V)
			/* Probe also for 1.2V support. */
			pcie_bits = 0x30;
	}

	ret = __mmc_send_if_cond(host, ocr, pcie_bits, &resp);
	if (ret)
		return 0;

	/* Continue with the SD express init, if the card supports it. */
	resp &= 0x3000;
	if (pcie_bits && resp) {
		if (resp == 0x3000)
			host->ios.timing = MMC_TIMING_SD_EXP_1_2V;
		else
			host->ios.timing = MMC_TIMING_SD_EXP;

		/*
		 * According to the spec the clock shall also be gated, but
		 * let's leave this to the host driver for more flexibility.
		 */
		return host->ops->init_sd_express(host, &host->ios);
	}

	return 0;
}

int mmc_send_relative_addr(struct mmc_host *host, unsigned int *rca)
{
	int err;
	struct mmc_command cmd = {};

	cmd.opcode = SD_SEND_RELATIVE_ADDR;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_R6 | MMC_CMD_BCR;

	err = mmc_wait_for_cmd(host, &cmd, MMC_CMD_RETRIES);
	if (err)
		return err;

	*rca = cmd.resp[0] >> 16;

	return 0;
}

int mmc_app_send_scr(struct mmc_card *card)
{
	int err;
	struct mmc_request mrq = {};
	struct mmc_command cmd = {};
	struct mmc_data data = {};
	struct scatterlist sg;
	__be32 *scr;

	/* NOTE: caller guarantees scr is heap-allocated */

	err = mmc_app_cmd(card->host, card);
	if (err)
		return err;

	/* dma onto stack is unsafe/nonportable, but callers to this
	 * routine normally provide temporary on-stack buffers ...
	 */
	scr = kmalloc(sizeof(card->raw_scr), GFP_KERNEL);
	if (!scr)
		return -ENOMEM;

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = SD_APP_SEND_SCR;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;

	data.blksz = 8;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;

	sg_init_one(&sg, scr, 8);

	mmc_set_data_timeout(&data, card);

	mmc_wait_for_req(card->host, &mrq);

	card->raw_scr[0] = be32_to_cpu(scr[0]);
	card->raw_scr[1] = be32_to_cpu(scr[1]);

	kfree(scr);

	if (cmd.error)
		return cmd.error;
	if (data.error)
		return data.error;

	return 0;
}

int mmc_sd_switch(struct mmc_card *card, bool mode, int group,
	u8 value, u8 *resp)
{
	u32 cmd_args;

	/* NOTE: caller guarantees resp is heap-allocated */

	value &= 0xF;
	cmd_args = mode << 31 | 0x00FFFFFF;
	cmd_args &= ~(0xF << (group * 4));
	cmd_args |= value << (group * 4);

	return mmc_send_adtc_data(card, card->host, SD_SWITCH, cmd_args, resp,
				  64);
}
EXPORT_SYMBOL_GPL(mmc_sd_switch);

int mmc_app_sd_status(struct mmc_card *card, void *ssr)
{
	int err;
	struct mmc_request mrq = {};
	struct mmc_command cmd = {};
	struct mmc_data data = {};
	struct scatterlist sg;

	/* NOTE: caller guarantees ssr is heap-allocated */

	err = mmc_app_cmd(card->host, card);
	if (err)
		return err;

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = SD_APP_SD_STATUS;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_SPI_R2 | MMC_RSP_R1 | MMC_CMD_ADTC;

	data.blksz = 64;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;

	sg_init_one(&sg, ssr, 64);

	mmc_set_data_timeout(&data, card);

	mmc_wait_for_req(card->host, &mrq);

	if (cmd.error)
		return cmd.error;
	if (data.error)
		return data.error;

	return 0;
}


int mmc_sd_write_ext_reg(struct mmc_card *card, u8 fno, u8 page, u16 offset,
		     u8 reg_data)
{
	struct mmc_host *host = card->host;
	struct mmc_request mrq = {};
	struct mmc_command cmd = {};
	struct mmc_data data = {};
	struct scatterlist sg;
	u8 *reg_buf;

	reg_buf = card->ext_reg_buf;
	memset(reg_buf, 0, 512);

	mrq.cmd = &cmd;
	mrq.data = &data;

	/*
	 * Arguments of CMD49:
	 * [31:31] MIO (0 = memory).
	 * [30:27] FNO (function number).
	 * [26:26] MW - mask write mode (0 = disable).
	 * [25:18] page number.
	 * [17:9] offset address.
	 * [8:0] length (0 = 1 byte).
	 */
	cmd.arg = fno << 27 | page << 18 | offset << 9;

	/* The first byte in the buffer is the data to be written. */
	reg_buf[0] = reg_data;

	data.flags = MMC_DATA_WRITE;
	data.blksz = 512;
	data.blocks = 1;
	data.sg = &sg;
	data.sg_len = 1;
	sg_init_one(&sg, reg_buf, 512);

	cmd.opcode = SD_WRITE_EXTR_SINGLE;
	cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;

	mmc_set_data_timeout(&data, card);
	mmc_wait_for_req(host, &mrq);

	/*
	 * Note that, the SD card is allowed to signal busy on DAT0 up to 1s
	 * after the CMD49. Although, let's leave this to be managed by the
	 * caller.
	 */

	if (cmd.error)
		return cmd.error;
	if (data.error)
		return data.error;

	return 0;
}

int mmc_sd_read_ext_reg(struct mmc_card *card, u8 fno, u8 page,
			u16 offset, u16 len, u8 *reg_buf)
{
	u32 cmd_args;

	/*
	 * Command arguments of CMD48:
	 * [31:31] MIO (0 = memory).
	 * [30:27] FNO (function number).
	 * [26:26] reserved (0).
	 * [25:18] page number.
	 * [17:9] offset address.
	 * [8:0] length (0 = 1 byte, 1ff = 512 bytes).
	 */
	cmd_args = fno << 27 | page << 18 | offset << 9 | (len - 1);

	return mmc_send_adtc_data(card, card->host, SD_READ_EXTR_SINGLE,
				  cmd_args, reg_buf, 512);
}

static int mmc_sd_cmdq_switch(struct mmc_card *card, bool enable)
{
	int err;
	u8 reg = 0;
	/*
	 * SD offers two command queueing modes - sequential (in-order) and
	 * voluntary (out-of-order). Apps Class A2 performance is only
	 * guaranteed for voluntary CQ (bit 1 = 0), so use that in preference
	 * to sequential.
	 */
	if (enable)
		reg = BIT(0);

	/* Performance enhancement register byte 262 controls command queueing */
	err = mmc_sd_write_ext_reg(card, card->ext_perf.fno, card->ext_perf.page,
				   card->ext_perf.offset + 262, reg);
	if (!err)
		card->ext_csd.cmdq_en = enable;

	return err;
}

int mmc_sd_cmdq_enable(struct mmc_card *card)
{
	return mmc_sd_cmdq_switch(card, true);
}
EXPORT_SYMBOL_GPL(mmc_sd_cmdq_enable);

int mmc_sd_cmdq_disable(struct mmc_card *card)
{
	return mmc_sd_cmdq_switch(card, false);
}
EXPORT_SYMBOL_GPL(mmc_sd_cmdq_disable);
