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

//============================================================
// include files
//============================================================
#include "odm_precomp.h"


VOID
odm_RSSIMonitorInit(
	IN		PVOID		pDM_VOID
	)
{
	PDM_ODM_T		pDM_Odm = (PDM_ODM_T)pDM_VOID;
	pRA_T		pRA_Table = &pDM_Odm->DM_RA_Table;
	pRA_Table->firstconnect = FALSE;
}

VOID
odm_RSSIMonitorCheck(
	IN		PVOID		pDM_VOID
	)
{
	//
	// For AP/ADSL use prtl8192cd_priv
	// For CE/NIC use PADAPTER
	//
	PDM_ODM_T pDM_Odm = (PDM_ODM_T)pDM_VOID;

	if (!(pDM_Odm->SupportAbility & ODM_BB_RSSI_MONITOR))
		return;

	//
	// 2011/09/29 MH In HW integration first stage, we provide 4 different handle to operate
	// at the same time. In the stage2/3, we need to prive universal interface and merge all
	// HW dynamic mechanism.
	//
	switch	(pDM_Odm->SupportPlatform)
	{
		case	ODM_WIN:
			odm_RSSIMonitorCheckMP(pDM_Odm);
			break;

		case	ODM_CE:
			odm_RSSIMonitorCheckCE(pDM_Odm);
			break;

		case	ODM_AP:
			odm_RSSIMonitorCheckAP(pDM_Odm);
			break;

		case	ODM_ADSL:
			//odm_DIGAP(pDM_Odm);
			break;
	}

}	// odm_RSSIMonitorCheck

VOID
odm_RSSIMonitorCheckMP(
	IN	PVOID	pDM_VOID
	)
{
}

//
//sherry move from DUSC to here 20110517
//
static VOID
FindMinimumRSSI_Dmsp(
	IN	PADAPTER	pAdapter
)
{
}

static void
FindMinimumRSSI(
IN	PADAPTER	pAdapter
	)
{
	HAL_DATA_TYPE	*pHalData = GET_HAL_DATA(pAdapter);
	struct dm_priv	*pdmpriv = &pHalData->dmpriv;
	PDM_ODM_T		pDM_Odm = &(pHalData->odmpriv);

	//1 1.Determine the minimum RSSI

	if((pDM_Odm->bLinked != _TRUE) &&
		(pdmpriv->EntryMinUndecoratedSmoothedPWDB == 0))
	{
		pdmpriv->MinUndecoratedPWDBForDM = 0;
		//ODM_RT_TRACE(pDM_Odm,COMP_BB_POWERSAVING, DBG_LOUD, ("Not connected to any \n"));
	}
	else
	{
		pdmpriv->MinUndecoratedPWDBForDM = pdmpriv->EntryMinUndecoratedSmoothedPWDB;
	}
}

VOID
odm_RSSIMonitorCheckCE(
	IN		PVOID		pDM_VOID
	)
{
	PDM_ODM_T		pDM_Odm = (PDM_ODM_T)pDM_VOID;
	PADAPTER	Adapter = pDM_Odm->Adapter;
	HAL_DATA_TYPE	*pHalData = GET_HAL_DATA(Adapter);
	struct dm_priv	*pdmpriv = &pHalData->dmpriv;
	struct dvobj_priv	*pdvobjpriv = adapter_to_dvobj(Adapter);
	int	i;
	int	tmpEntryMaxPWDB=0, tmpEntryMinPWDB=0xff;
	u8	sta_cnt=0;
	u32	UL_DL_STATE = 0, STBC_TX = 0, TxBF_EN = 0;
	u32	PWDB_rssi[NUM_STA]={0};//[0~15]:MACID, [16~31]:PWDB_rssi
	BOOLEAN			FirstConnect = FALSE;
	pRA_T			pRA_Table = &pDM_Odm->DM_RA_Table;

	if(pDM_Odm->bLinked != _TRUE)
		return;

	FirstConnect = (pDM_Odm->bLinked) && (pRA_Table->firstconnect == FALSE);
	pRA_Table->firstconnect = pDM_Odm->bLinked;

	{
		struct sta_info *psta;

		for(i=0; i<ODM_ASSOCIATE_ENTRY_NUM; i++) {
			if (IS_STA_VALID(psta = pDM_Odm->pODM_StaInfo[i]))
			{
					if(IS_MCAST( psta->hwaddr))  //if(psta->mac_id ==1)
						 continue;

					if(psta->rssi_stat.UndecoratedSmoothedPWDB == (-1))
						 continue;

					if(psta->rssi_stat.UndecoratedSmoothedPWDB < tmpEntryMinPWDB)
						tmpEntryMinPWDB = psta->rssi_stat.UndecoratedSmoothedPWDB;

					if(psta->rssi_stat.UndecoratedSmoothedPWDB > tmpEntryMaxPWDB)
						tmpEntryMaxPWDB = psta->rssi_stat.UndecoratedSmoothedPWDB;

					if(psta->rssi_stat.UndecoratedSmoothedPWDB != (-1)) {

#ifdef CONFIG_80211N_HT
						if(pDM_Odm->SupportICType == ODM_RTL8192E || pDM_Odm->SupportICType == ODM_RTL8812)
						{
#ifdef CONFIG_BEAMFORMING
							BEAMFORMING_CAP Beamform_cap = beamforming_get_entry_beam_cap_by_mac_id(&Adapter->mlmepriv, psta->mac_id);

							if(Beamform_cap & (BEAMFORMER_CAP_HT_EXPLICIT |BEAMFORMER_CAP_VHT_SU))
								TxBF_EN = 1;
							else
								TxBF_EN = 0;

							if (TxBF_EN) {
								STBC_TX = 0;
							}
							else
#endif
							{
#ifdef CONFIG_80211AC_VHT
								if(IsSupportedVHT(psta->wireless_mode))
									STBC_TX = TEST_FLAG(psta->vhtpriv.stbc_cap, STBC_VHT_ENABLE_TX);
								else
#endif
									STBC_TX = TEST_FLAG(psta->htpriv.stbc_cap, STBC_HT_ENABLE_TX);
							}
						}
#endif

						if(pDM_Odm->SupportICType == ODM_RTL8192D)
							PWDB_rssi[sta_cnt++] = (psta->mac_id | (psta->rssi_stat.UndecoratedSmoothedPWDB<<16) | ((Adapter->stapriv.asoc_sta_count+1) << 8));
						else if ((pDM_Odm->SupportICType == ODM_RTL8192E)||(pDM_Odm->SupportICType == ODM_RTL8812)||(pDM_Odm->SupportICType == ODM_RTL8821))
							PWDB_rssi[sta_cnt++] = (((u8)(psta->mac_id&0xFF)) | ((psta->rssi_stat.UndecoratedSmoothedPWDB&0x7F)<<16) |(STBC_TX << 25) | (FirstConnect << 29) | (TxBF_EN << 30));
						else
							PWDB_rssi[sta_cnt++] = (psta->mac_id | (psta->rssi_stat.UndecoratedSmoothedPWDB<<16) );
					}
			}
		}
		for(i=0; i< sta_cnt; i++)
		{
			if(PWDB_rssi[i] != (0)){
				if(pHalData->fw_ractrl == _TRUE)// Report every sta's RSSI to FW
				{
					if(pDM_Odm->SupportICType == ODM_RTL8723B){
						rtl8723b_set_rssi_cmd(Adapter, (u8 *)(&PWDB_rssi[i]));
					}
				}
			}
		}
	}



	if(tmpEntryMaxPWDB != 0)	// If associated entry is found
	{
		pdmpriv->EntryMaxUndecoratedSmoothedPWDB = tmpEntryMaxPWDB;
	}
	else
	{
		pdmpriv->EntryMaxUndecoratedSmoothedPWDB = 0;
	}

	if(tmpEntryMinPWDB != 0xff) // If associated entry is found
	{
		pdmpriv->EntryMinUndecoratedSmoothedPWDB = tmpEntryMinPWDB;
	}
	else
	{
		pdmpriv->EntryMinUndecoratedSmoothedPWDB = 0;
	}

	FindMinimumRSSI(Adapter);//get pdmpriv->MinUndecoratedPWDBForDM

	pDM_Odm->RSSI_Min = pdmpriv->MinUndecoratedPWDBForDM;
}


VOID
odm_RSSIMonitorCheckAP(
	IN		PVOID		pDM_VOID
	)
{
}

VOID
odm_RateAdaptiveMaskInit(
	IN	PVOID	pDM_VOID
	)
{
	PDM_ODM_T		pDM_Odm = (PDM_ODM_T)pDM_VOID;
	PODM_RATE_ADAPTIVE	pOdmRA = &pDM_Odm->RateAdaptive;

	pOdmRA->Type = DM_Type_ByDriver;
	if (pOdmRA->Type == DM_Type_ByDriver)
		pDM_Odm->bUseRAMask = _TRUE;
	else
		pDM_Odm->bUseRAMask = _FALSE;

	pOdmRA->RATRState = DM_RATR_STA_INIT;

	pOdmRA->LdpcThres = 35;
	pOdmRA->bUseLdpc = FALSE;

	pOdmRA->HighRSSIThresh = 50;
	pOdmRA->LowRSSIThresh = 20;
}
/*-----------------------------------------------------------------------------
 * Function:	odm_RefreshRateAdaptiveMask()
 *
 * Overview:	Update rate table mask according to rssi
 *
 * Input:		NONE
 *
 * Output:		NONE
 *
 * Return:		NONE
 *
 * Revised History:
 *	When		Who		Remark
 *	05/27/2009	hpfan	Create Version 0.
 *
 *---------------------------------------------------------------------------*/
VOID
odm_RefreshRateAdaptiveMask(
	IN	PVOID	pDM_VOID
	)
{
	PDM_ODM_T		pDM_Odm = (PDM_ODM_T)pDM_VOID;
	ODM_RT_TRACE(pDM_Odm, ODM_COMP_RA_MASK, ODM_DBG_TRACE, ("odm_RefreshRateAdaptiveMask()---------->\n"));
	if (!(pDM_Odm->SupportAbility & ODM_BB_RA_MASK))
	{
		ODM_RT_TRACE(pDM_Odm, ODM_COMP_RA_MASK, ODM_DBG_TRACE, ("odm_RefreshRateAdaptiveMask(): Return cos not supported\n"));
		return;
	}
	//
	// 2011/09/29 MH In HW integration first stage, we provide 4 different handle to operate
	// at the same time. In the stage2/3, we need to prive universal interface and merge all
	// HW dynamic mechanism.
	//
	switch	(pDM_Odm->SupportPlatform)
	{
		case	ODM_WIN:
			odm_RefreshRateAdaptiveMaskMP(pDM_Odm);
			break;

		case	ODM_CE:
			odm_RefreshRateAdaptiveMaskCE(pDM_Odm);
			break;

		case	ODM_AP:
		case	ODM_ADSL:
			odm_RefreshRateAdaptiveMaskAPADSL(pDM_Odm);
			break;
	}

}

VOID
odm_RefreshRateAdaptiveMaskMP(
	IN		PVOID		pDM_VOID
	)
{
}



VOID
odm_RefreshRateAdaptiveMaskCE(
	IN	PVOID	pDM_VOID
	)
{
	PDM_ODM_T		pDM_Odm = (PDM_ODM_T)pDM_VOID;
	u1Byte	i;
	PADAPTER	pAdapter	 =  pDM_Odm->Adapter;
	PODM_RATE_ADAPTIVE		pRA = &pDM_Odm->RateAdaptive;

	if(pAdapter->bDriverStopped)
	{
		ODM_RT_TRACE(pDM_Odm, ODM_COMP_RA_MASK, ODM_DBG_TRACE, ("<---- odm_RefreshRateAdaptiveMask(): driver is going to unload\n"));
		return;
	}

	if(!pDM_Odm->bUseRAMask)
	{
		ODM_RT_TRACE(pDM_Odm, ODM_COMP_RA_MASK, ODM_DBG_LOUD, ("<---- odm_RefreshRateAdaptiveMask(): driver does not control rate adaptive mask\n"));
		return;
	}

	for(i=0; i<ODM_ASSOCIATE_ENTRY_NUM; i++){
		PSTA_INFO_T pstat = pDM_Odm->pODM_StaInfo[i];
		if(IS_STA_VALID(pstat) ) {
			if(IS_MCAST( pstat->hwaddr))  //if(psta->mac_id ==1)
				 continue;
			if(IS_MCAST( pstat->hwaddr))
				continue;

			if( TRUE == ODM_RAStateCheck(pDM_Odm, pstat->rssi_stat.UndecoratedSmoothedPWDB, FALSE , &pstat->rssi_level) )
			{
				ODM_RT_TRACE(pDM_Odm, ODM_COMP_RA_MASK, ODM_DBG_LOUD, ("RSSI:%d, RSSI_LEVEL:%d\n", pstat->rssi_stat.UndecoratedSmoothedPWDB, pstat->rssi_level));
				rtw_hal_update_ra_mask(pstat, pstat->rssi_level);
			}
		}
	}
}

VOID
odm_RefreshRateAdaptiveMaskAPADSL(
	IN	PVOID	pDM_VOID
	)
{
}

// Return Value: BOOLEAN
// - TRUE: RATRState is changed.
BOOLEAN
ODM_RAStateCheck(
	IN		PVOID			pDM_VOID,
	IN		s4Byte			RSSI,
	IN		BOOLEAN			bForceUpdate,
	OUT		pu1Byte			pRATRState
	)
{
	PDM_ODM_T		pDM_Odm = (PDM_ODM_T)pDM_VOID;
	PODM_RATE_ADAPTIVE pRA = &pDM_Odm->RateAdaptive;
	const u1Byte GoUpGap = 5;
	u1Byte HighRSSIThreshForRA = pRA->HighRSSIThresh;
	u1Byte LowRSSIThreshForRA = pRA->LowRSSIThresh;
	u1Byte RATRState;

	// Threshold Adjustment:
	// when RSSI state trends to go up one or two levels, make sure RSSI is high enough.
	// Here GoUpGap is added to solve the boundary's level alternation issue.

	switch (*pRATRState)
	{
		case DM_RATR_STA_INIT:
		case DM_RATR_STA_HIGH:
			break;
		case DM_RATR_STA_MIDDLE:
			HighRSSIThreshForRA += GoUpGap;
			break;
		case DM_RATR_STA_LOW:
			HighRSSIThreshForRA += GoUpGap;
			LowRSSIThreshForRA += GoUpGap;
			break;
		default:
			ODM_RT_ASSERT(pDM_Odm, FALSE, ("wrong rssi level setting %d !", *pRATRState) );
			break;
	}

	// Decide RATRState by RSSI.
	if(RSSI > HighRSSIThreshForRA)
		RATRState = DM_RATR_STA_HIGH;
	else if(RSSI > LowRSSIThreshForRA)
		RATRState = DM_RATR_STA_MIDDLE;

	else
		RATRState = DM_RATR_STA_LOW;

	if( *pRATRState!=RATRState || bForceUpdate) {
		ODM_RT_TRACE( pDM_Odm, ODM_COMP_RA_MASK, ODM_DBG_LOUD, ("RSSI Level %d -> %d\n", *pRATRState, RATRState) );
		*pRATRState = RATRState;
		return TRUE;
	}

	return FALSE;
}

VOID
odm_RefreshBasicRateMask(
	IN	PVOID	pDM_VOID
	)
{
}

u4Byte
ODM_Get_Rate_Bitmap(
	IN	PVOID		pDM_VOID,
	IN	u4Byte		macid,
	IN	u4Byte		ra_mask,
	IN	u1Byte		rssi_level
	)
{
	PDM_ODM_T		pDM_Odm = (PDM_ODM_T)pDM_VOID;
	PSTA_INFO_T	pEntry;
	u4Byte	rate_bitmap = 0;
	u1Byte	WirelessMode;


	pEntry = pDM_Odm->pODM_StaInfo[macid];
	if(!IS_STA_VALID(pEntry))
		return ra_mask;

	WirelessMode = pEntry->wireless_mode;

	switch(WirelessMode)
	{
		case ODM_WM_B:
			if(ra_mask & 0x0000000c)		//11M or 5.5M enable
				rate_bitmap = 0x0000000d;
			else
				rate_bitmap = 0x0000000f;
			break;

		case (ODM_WM_G):
		case (ODM_WM_A):
			if(rssi_level == DM_RATR_STA_HIGH)
				rate_bitmap = 0x00000f00;
			else
				rate_bitmap = 0x00000ff0;
			break;

		case (ODM_WM_B|ODM_WM_G):
			if(rssi_level == DM_RATR_STA_HIGH)
				rate_bitmap = 0x00000f00;
			else if(rssi_level == DM_RATR_STA_MIDDLE)
				rate_bitmap = 0x00000ff0;
			else
				rate_bitmap = 0x00000ff5;
			break;

		case (ODM_WM_B|ODM_WM_G|ODM_WM_N24G)	:
		case (ODM_WM_B|ODM_WM_N24G)	:
		case (ODM_WM_G|ODM_WM_N24G)	:
		case (ODM_WM_A|ODM_WM_N5G)	:
			{
				if (	pDM_Odm->RFType == ODM_1T2R ||pDM_Odm->RFType == ODM_1T1R)
				{
					if(rssi_level == DM_RATR_STA_HIGH)
					{
						rate_bitmap = 0x000f0000;
					}
					else if(rssi_level == DM_RATR_STA_MIDDLE)
					{
						rate_bitmap = 0x000ff000;
					}
					else{
						if (*(pDM_Odm->pBandWidth) == ODM_BW40M)
							rate_bitmap = 0x000ff015;
						else
							rate_bitmap = 0x000ff005;
					}
				}
				else
				{
					if(rssi_level == DM_RATR_STA_HIGH)
					{
						rate_bitmap = 0x0f8f0000;
					}
					else if(rssi_level == DM_RATR_STA_MIDDLE)
					{
						rate_bitmap = 0x0f8ff000;
					}
					else
					{
						if (*(pDM_Odm->pBandWidth) == ODM_BW40M)
							rate_bitmap = 0x0f8ff015;
						else
							rate_bitmap = 0x0f8ff005;
					}
				}
			}
			break;

		case (ODM_WM_AC|ODM_WM_G):
			if(rssi_level == 1)
				rate_bitmap = 0xfc3f0000;
			else if(rssi_level == 2)
				rate_bitmap = 0xfffff000;
			else
				rate_bitmap = 0xffffffff;
			break;

		case (ODM_WM_AC|ODM_WM_A):

			if (pDM_Odm->RFType == RF_1T1R)
			{
				if(rssi_level == 1)				// add by Gary for ac-series
					rate_bitmap = 0x003f8000;
				else if (rssi_level == 2)
					rate_bitmap = 0x003ff000;
				else
					rate_bitmap = 0x003ff010;
			}
			else
			{
				if(rssi_level == 1)				// add by Gary for ac-series
					rate_bitmap = 0xfe3f8000;       // VHT 2SS MCS3~9
				else if (rssi_level == 2)
					rate_bitmap = 0xfffff000;       // VHT 2SS MCS0~9
				else
					rate_bitmap = 0xfffff010;       // All
			}
			break;

		default:
			if(pDM_Odm->RFType == RF_1T2R)
				rate_bitmap = 0x000fffff;
			else
				rate_bitmap = 0x0fffffff;
			break;

	}

	ODM_RT_TRACE(pDM_Odm, ODM_COMP_RA_MASK, ODM_DBG_LOUD, (" ==> rssi_level:0x%02x, WirelessMode:0x%02x, rate_bitmap:0x%08x \n",rssi_level,WirelessMode,rate_bitmap));

	return (ra_mask&rate_bitmap);
}
