/* SPDX-License-Identifier: GPL-2.0 */
/*
 * u_uac2.h
 *
 * Utility definitions for UAC2 function
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Author: Andrzej Pietrasiewicz <andrzejtp2010@gmail.com>
 */

#ifndef U_UAC2_H
#define U_UAC2_H

#include <linux/usb/composite.h>
#include "uac_common.h"

#define UAC2_DEF_PCHMASK 0x3
#define UAC2_DEF_PSRATE 48000
#define UAC2_DEF_PSSIZE 2
#define UAC2_DEF_PHSBINT 0
#define UAC2_DEF_CCHMASK 0x3
#define UAC2_DEF_CSRATE 64000
#define UAC2_DEF_CSSIZE 2
#define UAC2_DEF_CHSBINT 0
#define UAC2_DEF_CSYNC		USB_ENDPOINT_SYNC_ASYNC

#define UAC2_DEF_MUTE_PRESENT	1
#define UAC2_DEF_VOLUME_PRESENT 1
#define UAC2_DEF_MIN_DB		(-100*256)	/* -100 dB */
#define UAC2_DEF_MAX_DB		0		/* 0 dB */
#define UAC2_DEF_RES_DB		(1*256)		/* 1 dB */

#define UAC2_DEF_REQ_NUM 2
#define UAC2_DEF_INT_REQ_NUM	10

struct f_uac2_opts {
	struct usb_function_instance	func_inst;
	int				p_chmask;
	int				p_srates[UAC_MAX_RATES];
	int				p_ssize;
	u8				p_hs_bint;
	int				c_chmask;
	int				c_srates[UAC_MAX_RATES];
	int				c_ssize;
	int				c_sync;
	u8				c_hs_bint;

	bool			p_mute_present;
	bool			p_volume_present;
	s16				p_volume_min;
	s16				p_volume_max;
	s16				p_volume_res;

	bool			c_mute_present;
	bool			c_volume_present;
	s16				c_volume_min;
	s16				c_volume_max;
	s16				c_volume_res;

	int				req_number;
	int				fb_max;
	bool			bound;

	char			function_name[USB_MAX_STRING_LEN];
	char			if_ctrl_string[USB_MAX_STRING_LEN];
	char			clksrc_in_string[USB_MAX_STRING_LEN];
	char			clksrc_out_string[USB_MAX_STRING_LEN];
	char			usb_it_string[USB_MAX_STRING_LEN];
	char			io_it_string[USB_MAX_STRING_LEN];
	char			usb_ot_string[USB_MAX_STRING_LEN];
	char			io_ot_string[USB_MAX_STRING_LEN];
	char			fu_in_string[USB_MAX_STRING_LEN];
	char			fu_out_string[USB_MAX_STRING_LEN];
	char			as_out_alt0_string[USB_MAX_STRING_LEN];
	char			as_out_alt1_string[USB_MAX_STRING_LEN];
	char			as_in_alt0_string[USB_MAX_STRING_LEN];
	char			as_in_alt1_string[USB_MAX_STRING_LEN];

	struct mutex			lock;
	int				refcnt;
};

#endif
