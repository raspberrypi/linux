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

#ifndef	__ODMRAINFO_H__
#define    __ODMRAINFO_H__

#define AP_InitRateAdaptiveState	ODM_RateAdaptiveStateApInit

#define		DM_RATR_STA_INIT			0
#define		DM_RATR_STA_HIGH			1
#define			DM_RATR_STA_MIDDLE		2
#define			DM_RATR_STA_LOW			3

typedef struct _Rate_Adaptive_Table_{
	u1Byte		firstconnect;
}RA_T, *pRA_T;


typedef struct _ODM_RATE_ADAPTIVE
{
	u1Byte				Type;				// DM_Type_ByFW/DM_Type_ByDriver
	u1Byte				HighRSSIThresh;		// if RSSI > HighRSSIThresh	=> RATRState is DM_RATR_STA_HIGH
	u1Byte				LowRSSIThresh;		// if RSSI <= LowRSSIThresh	=> RATRState is DM_RATR_STA_LOW
	u1Byte				RATRState;			// Current RSSI level, DM_RATR_STA_HIGH/DM_RATR_STA_MIDDLE/DM_RATR_STA_LOW

	u1Byte				LdpcThres;			// if RSSI > LdpcThres => switch from LPDC to BCC
	BOOLEAN				bLowerRtsRate;

	BOOLEAN				bUseLdpc;

} ODM_RATE_ADAPTIVE, *PODM_RATE_ADAPTIVE;

VOID
odm_RSSIMonitorInit(
	IN		PVOID		pDM_VOID
	);

VOID
odm_RSSIMonitorCheck(
	IN		PVOID		 pDM_VOID
	);

VOID
odm_RSSIMonitorCheckMP(
	IN		PVOID		pDM_VOID
	);

VOID
odm_RSSIMonitorCheckCE(
	IN		PVOID		pDM_VOID
	);

VOID
odm_RSSIMonitorCheckAP(
	IN		PVOID		 pDM_VOID
	);


VOID
odm_RateAdaptiveMaskInit(
	IN	PVOID	pDM_VOID
	);

VOID
odm_RefreshRateAdaptiveMask(
	IN	PVOID	pDM_VOID
	);

VOID
odm_RefreshRateAdaptiveMaskMP(
	IN	PVOID	pDM_VOID
	);

VOID
odm_RefreshRateAdaptiveMaskCE(
	IN	PVOID	pDM_VOID
	);

VOID
odm_RefreshRateAdaptiveMaskAPADSL(
	IN	PVOID	pDM_VOID
	);

BOOLEAN
ODM_RAStateCheck(
	IN		PVOID			pDM_VOID,
	IN		s4Byte			RSSI,
	IN		BOOLEAN			bForceUpdate,
	OUT		pu1Byte			pRATRState
	);

VOID
odm_RefreshBasicRateMask(
	IN	PVOID	pDM_VOID
	);

u4Byte
ODM_Get_Rate_Bitmap(
	IN	PVOID		pDM_VOID,
	IN	u4Byte		macid,
	IN	u4Byte		ra_mask,
	IN	u1Byte		rssi_level
	);

#endif //#ifndef	__ODMRAINFO_H__
