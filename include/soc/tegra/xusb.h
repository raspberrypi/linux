/*
 * Copyright (C) 2014 NVIDIA Corporation
 * Copyright (C) 2014 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#ifndef __SOC_TEGRA_XUSB_H__
#define __SOC_TEGRA_XUSB_H__

/* Command requests from the firmware */
enum tegra_xusb_mbox_cmd {
	MBOX_CMD_MSG_ENABLED = 1,
	MBOX_CMD_INC_FALC_CLOCK,
	MBOX_CMD_DEC_FALC_CLOCK,
	MBOX_CMD_INC_SSPI_CLOCK,
	MBOX_CMD_DEC_SSPI_CLOCK,
	MBOX_CMD_SET_BW, /* no ACK/NAK required */
	MBOX_CMD_SET_SS_PWR_GATING,
	MBOX_CMD_SET_SS_PWR_UNGATING,
	MBOX_CMD_SAVE_DFE_CTLE_CTX,
	MBOX_CMD_AIRPLANE_MODE_ENABLED, /* unused */
	MBOX_CMD_AIRPLANE_MODE_DISABLED, /* unused */
	MBOX_CMD_START_HSIC_IDLE,
	MBOX_CMD_STOP_HSIC_IDLE,
	MBOX_CMD_DBC_WAKE_STACK, /* unused */
	MBOX_CMD_HSIC_PRETEND_CONNECT,

	MBOX_CMD_MAX,

	/* Response message to above commands */
	MBOX_CMD_ACK = 128,
	MBOX_CMD_NAK
};

struct tegra_xusb_mbox_msg {
	u32 cmd;
	u32 data;
};

#endif /* __SOC_TEGRA_XUSB_H__ */
