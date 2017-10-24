/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 *
 ******************************************************************************/

#ifndef	__ODMEDCATURBOCHECK_H__
#define    __ODMEDCATURBOCHECK_H__

typedef struct _EDCA_TURBO_
{
	BOOLEAN bCurrentTurboEDCA;
	BOOLEAN bIsCurRDLState;

	u4Byte	prv_traffic_idx; // edca turbo

}EDCA_T,*pEDCA_T;

static u4Byte edca_setting_UL[HT_IOT_PEER_MAX] =
// UNKNOWN		REALTEK_90	REALTEK_92SE	BROADCOM		RALINK		ATHEROS		CISCO		MERU        MARVELL	92U_AP		SELF_AP(DownLink/Tx)
{ 0x5e4322,		0xa44f,			0x5e4322,		0x5ea32b,		0x5ea422,	0x5ea322,	0x3ea430,	0x5ea42b, 0x5ea44f,	0x5e4322,	0x5e4322};


static u4Byte edca_setting_DL[HT_IOT_PEER_MAX] =
// UNKNOWN		REALTEK_90	REALTEK_92SE	BROADCOM		RALINK		ATHEROS		CISCO		MERU,       MARVELL	92U_AP		SELF_AP(UpLink/Rx)
{ 0xa44f,		0x5ea44f,	0x5e4322,		0x5ea42b,		0xa44f,			0xa630,			0x5ea630,	0x5ea42b, 0xa44f,		0xa42b,		0xa42b};

static u4Byte edca_setting_DL_GMode[HT_IOT_PEER_MAX] =
// UNKNOWN		REALTEK_90	REALTEK_92SE	BROADCOM		RALINK		ATHEROS		CISCO		MERU,       MARVELL	92U_AP		SELF_AP
{ 0x4322,		0xa44f,			0x5e4322,		0xa42b,				0x5e4322,	0x4322,			0xa42b,		0x5ea42b, 0xa44f,		0x5e4322,	0x5ea42b};


VOID
odm_EdcaTurboCheck(
	IN	PVOID		pDM_VOID
	);
VOID
ODM_EdcaTurboInit(
	IN	PVOID		pDM_VOID
);

VOID
odm_EdcaTurboCheckCE(
	IN	PVOID		pDM_VOID
	);

#endif
