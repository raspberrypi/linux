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
//#include "Mp_Precomp.h"
#include "odm_precomp.h"

VOID
ODM_EdcaTurboInit(
	IN	PVOID		pDM_VOID)
{
	PDM_ODM_T		pDM_Odm = (PDM_ODM_T)pDM_VOID;
	PADAPTER	Adapter = pDM_Odm->Adapter;
	pDM_Odm->DM_EDCA_Table.bCurrentTurboEDCA = FALSE;
	pDM_Odm->DM_EDCA_Table.bIsCurRDLState = FALSE;
	Adapter->recvpriv.bIsAnyNonBEPkts =FALSE;

	ODM_RT_TRACE(pDM_Odm,ODM_COMP_EDCA_TURBO,ODM_DBG_LOUD,("Orginial VO PARAM: 0x%x\n",ODM_Read4Byte(pDM_Odm,ODM_EDCA_VO_PARAM)));
	ODM_RT_TRACE(pDM_Odm,ODM_COMP_EDCA_TURBO,ODM_DBG_LOUD,("Orginial VI PARAM: 0x%x\n",ODM_Read4Byte(pDM_Odm,ODM_EDCA_VI_PARAM)));
	ODM_RT_TRACE(pDM_Odm,ODM_COMP_EDCA_TURBO,ODM_DBG_LOUD,("Orginial BE PARAM: 0x%x\n",ODM_Read4Byte(pDM_Odm,ODM_EDCA_BE_PARAM)));
	ODM_RT_TRACE(pDM_Odm,ODM_COMP_EDCA_TURBO,ODM_DBG_LOUD,("Orginial BK PARAM: 0x%x\n",ODM_Read4Byte(pDM_Odm,ODM_EDCA_BK_PARAM)));


}	// ODM_InitEdcaTurbo

VOID
odm_EdcaTurboCheck(
	IN	PVOID		pDM_VOID
	)
{
	//
	// For AP/ADSL use prtl8192cd_priv
	// For CE/NIC use PADAPTER
	//

	//
	// 2011/09/29 MH In HW integration first stage, we provide 4 different handle to operate
	// at the same time. In the stage2/3, we need to prive universal interface and merge all
	// HW dynamic mechanism.
	//
	PDM_ODM_T		pDM_Odm = (PDM_ODM_T)pDM_VOID;
	ODM_RT_TRACE(pDM_Odm,ODM_COMP_EDCA_TURBO,ODM_DBG_LOUD,("odm_EdcaTurboCheck========================>\n"));

	if(!(pDM_Odm->SupportAbility& ODM_MAC_EDCA_TURBO ))
		return;

	switch	(pDM_Odm->SupportPlatform)
	{
		case	ODM_WIN:
			break;
		case	ODM_CE:
			odm_EdcaTurboCheckCE(pDM_Odm);
			break;
		case	ODM_AP:
		case	ODM_ADSL:
			break;
	}
	ODM_RT_TRACE(pDM_Odm,ODM_COMP_EDCA_TURBO,ODM_DBG_LOUD,("<========================odm_EdcaTurboCheck\n"));

}	// odm_CheckEdcaTurbo

VOID
odm_EdcaTurboCheckCE(
	IN	PVOID		pDM_VOID
	)
{
	PDM_ODM_T		pDM_Odm = (PDM_ODM_T)pDM_VOID;
	PADAPTER		       Adapter = pDM_Odm->Adapter;
	u32	EDCA_BE_UL = 0x5ea42b;//Parameter suggested by Scott  //edca_setting_UL[pMgntInfo->IOTPeer];
	u32	EDCA_BE_DL = 0x5ea42b;//Parameter suggested by Scott  //edca_setting_DL[pMgntInfo->IOTPeer];
	u32	ICType=pDM_Odm->SupportICType;
	u32	IOTPeer=0;
	u8	WirelessMode=0xFF;                   //invalid value
	u32	trafficIndex;
	u32	edca_param;
	u64	cur_tx_bytes = 0;
	u64	cur_rx_bytes = 0;
	u8	bbtchange = _FALSE;
	u8	bBiasOnRx = _FALSE;
	HAL_DATA_TYPE		*pHalData = GET_HAL_DATA(Adapter);
	struct dvobj_priv		*pdvobjpriv = adapter_to_dvobj(Adapter);
	struct xmit_priv		*pxmitpriv = &(Adapter->xmitpriv);
	struct recv_priv		*precvpriv = &(Adapter->recvpriv);
	struct registry_priv	*pregpriv = &Adapter->registrypriv;
	struct mlme_ext_priv	*pmlmeext = &(Adapter->mlmeextpriv);
	struct mlme_ext_info	*pmlmeinfo = &(pmlmeext->mlmext_info);

	if(pDM_Odm->bLinked != _TRUE)
	{
		precvpriv->bIsAnyNonBEPkts = _FALSE;
		return;
	}

	if ((pregpriv->wifi_spec == 1) )//|| (pmlmeinfo->HT_enable == 0))
	{
		precvpriv->bIsAnyNonBEPkts = _FALSE;
		return;
	}

	if(pDM_Odm->pWirelessMode!=NULL)
		WirelessMode=*(pDM_Odm->pWirelessMode);

	IOTPeer = pmlmeinfo->assoc_AP_vendor;

	if (IOTPeer >=  HT_IOT_PEER_MAX)
	{
		precvpriv->bIsAnyNonBEPkts = _FALSE;
		return;
	}

	if(	(pDM_Odm->SupportICType == ODM_RTL8192C) ||
		(pDM_Odm->SupportICType == ODM_RTL8723A) ||
		(pDM_Odm->SupportICType == ODM_RTL8188E))
	{
		if((IOTPeer == HT_IOT_PEER_RALINK)||(IOTPeer == HT_IOT_PEER_ATHEROS))
			bBiasOnRx = _TRUE;
	}

	// Check if the status needs to be changed.
	if((bbtchange) || (!precvpriv->bIsAnyNonBEPkts) )
	{
		cur_tx_bytes = pdvobjpriv->traffic_stat.cur_tx_bytes;
		cur_rx_bytes = pdvobjpriv->traffic_stat.cur_rx_bytes;

		//traffic, TX or RX
		if(bBiasOnRx)
		{
			if (cur_tx_bytes > (cur_rx_bytes << 2))
			{ // Uplink TP is present.
				trafficIndex = UP_LINK;
			}
			else
			{ // Balance TP is present.
				trafficIndex = DOWN_LINK;
			}
		}
		else
		{
			if (cur_rx_bytes > (cur_tx_bytes << 2))
			{ // Downlink TP is present.
				trafficIndex = DOWN_LINK;
			}
			else
			{ // Balance TP is present.
				trafficIndex = UP_LINK;
			}
		}

		//if ((pDM_Odm->DM_EDCA_Table.prv_traffic_idx != trafficIndex) || (!pDM_Odm->DM_EDCA_Table.bCurrentTurboEDCA))
		{
			if(ICType==ODM_RTL8192D)
			{
				// Single PHY
				if(pDM_Odm->RFType==ODM_2T2R)
				{
					EDCA_BE_UL = 0x60a42b;    //0x5ea42b;
					EDCA_BE_DL = 0x60a42b;    //0x5ea42b;
				}
				else
				{
					EDCA_BE_UL = 0x6ea42b;
					EDCA_BE_DL = 0x6ea42b;
				}
			}
			else
			{
				if(pDM_Odm->SupportInterface==ODM_ITRF_PCIE) {
					if((ICType==ODM_RTL8192C)&&(pDM_Odm->RFType==ODM_2T2R)) {
						EDCA_BE_UL = 0x60a42b;
						EDCA_BE_DL = 0x60a42b;
					}
					else
					{
						EDCA_BE_UL = 0x6ea42b;
						EDCA_BE_DL = 0x6ea42b;
					}
				}
			}

			//92D txop can't be set to 0x3e for cisco1250
			if((ICType!=ODM_RTL8192D) && (IOTPeer== HT_IOT_PEER_CISCO) &&(WirelessMode==ODM_WM_N24G))
			{
				EDCA_BE_DL = edca_setting_DL[IOTPeer];
				EDCA_BE_UL = edca_setting_UL[IOTPeer];
			}
			//merge from 92s_92c_merge temp brunch v2445    20120215
			else if((IOTPeer == HT_IOT_PEER_CISCO) &&((WirelessMode==ODM_WM_G)||(WirelessMode==(ODM_WM_B|ODM_WM_G))||(WirelessMode==ODM_WM_A)||(WirelessMode==ODM_WM_B)))
			{
				EDCA_BE_DL = edca_setting_DL_GMode[IOTPeer];
			}
			else if((IOTPeer== HT_IOT_PEER_AIRGO )&& ((WirelessMode==ODM_WM_G)||(WirelessMode==ODM_WM_A)))
			{
				EDCA_BE_DL = 0xa630;
			}
			else if(IOTPeer == HT_IOT_PEER_MARVELL)
			{
				EDCA_BE_DL = edca_setting_DL[IOTPeer];
				EDCA_BE_UL = edca_setting_UL[IOTPeer];
			}
			else if(IOTPeer == HT_IOT_PEER_ATHEROS)
			{
				// Set DL EDCA for Atheros peer to 0x3ea42b. Suggested by SD3 Wilson for ASUS TP issue.
				EDCA_BE_DL = edca_setting_DL[IOTPeer];
			}

			if((ICType==ODM_RTL8812)||(ICType==ODM_RTL8821)||(ICType==ODM_RTL8192E))           //add 8812AU/8812AE
			{
				EDCA_BE_UL = 0x5ea42b;
				EDCA_BE_DL = 0x5ea42b;

				ODM_RT_TRACE(pDM_Odm,ODM_COMP_EDCA_TURBO,ODM_DBG_LOUD,("8812A: EDCA_BE_UL=0x%x EDCA_BE_DL =0x%x",EDCA_BE_UL,EDCA_BE_DL));
			}

			if (trafficIndex == DOWN_LINK)
				edca_param = EDCA_BE_DL;
			else
				edca_param = EDCA_BE_UL;

			rtw_write32(Adapter, REG_EDCA_BE_PARAM, edca_param);

			pDM_Odm->DM_EDCA_Table.prv_traffic_idx = trafficIndex;
		}

		pDM_Odm->DM_EDCA_Table.bCurrentTurboEDCA = _TRUE;
	}
	else
	{
		//
		// Turn Off EDCA turbo here.
		// Restore original EDCA according to the declaration of AP.
		//
		 if(pDM_Odm->DM_EDCA_Table.bCurrentTurboEDCA)
		{
			rtw_write32(Adapter, REG_EDCA_BE_PARAM, pHalData->AcParam_BE);
			pDM_Odm->DM_EDCA_Table.bCurrentTurboEDCA = _FALSE;
		}
	}

}
