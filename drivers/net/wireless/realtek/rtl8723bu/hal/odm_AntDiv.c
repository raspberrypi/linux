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

#if (defined(CONFIG_HW_ANTENNA_DIVERSITY))
VOID
odm_AntDiv_on_off( IN PDM_ODM_T	pDM_Odm ,IN u1Byte swch)
{
	if(pDM_Odm->AntDivType==S0S1_SW_ANTDIV || pDM_Odm->AntDivType==CGCS_RX_SW_ANTDIV)
		return;

	if(pDM_Odm->SupportICType & ODM_N_ANTDIV_SUPPORT)
	{
		ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("(( Turn %s )) N-Series AntDiv Function\n",(swch==ANTDIV_ON)?"ON" : "OFF"));
		ODM_SetBBReg(pDM_Odm, 0xc50 , BIT7, swch); //OFDM AntDiv function block enable
		ODM_SetBBReg(pDM_Odm, 0xa00 , BIT15, swch); //CCK AntDiv function block enable
	}
	else if(pDM_Odm->SupportICType & ODM_AC_ANTDIV_SUPPORT)
	{
		ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("(( Turn %s )) AC-Series AntDiv Function\n",(swch==ANTDIV_ON)?"ON" : "OFF"));
		if(pDM_Odm->SupportICType == ODM_RTL8812)
		{
			ODM_SetBBReg(pDM_Odm, 0xc50 , BIT7, swch); //OFDM AntDiv function block enable
			ODM_SetBBReg(pDM_Odm, 0xa00 , BIT15, swch); //CCK AntDiv function block enable
		}
		else
		{
		ODM_SetBBReg(pDM_Odm, 0x8D4 , BIT24, swch); //OFDM AntDiv function block enable
		ODM_SetBBReg(pDM_Odm, 0x800 , BIT25, swch); //CCK AntDiv function block enable
	        }
         }
}

VOID
ODM_UpdateRxIdleAnt(IN PDM_ODM_T pDM_Odm, IN u1Byte Ant)
{
	pFAT_T	pDM_FatTable = &pDM_Odm->DM_FatTable;
	u4Byte	DefaultAnt, OptionalAnt,value32;
	PADAPTER		pAdapter = pDM_Odm->Adapter;
	HAL_DATA_TYPE	*pHalData = GET_HAL_DATA(pAdapter);

	if(pDM_FatTable->RxIdleAnt != Ant)
	{
		ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("[ Update Rx-Idle-Ant ] RxIdleAnt =%s\n",(Ant==MAIN_ANT)?"MAIN_ANT":"AUX_ANT"));
		pDM_FatTable->RxIdleAnt = Ant;

		if(Ant == MAIN_ANT)
		{
			DefaultAnt   =  ANT1_2G;
			OptionalAnt =  ANT2_2G;
		}
		else
		{
			DefaultAnt  =   ANT2_2G;
			OptionalAnt =  ANT1_2G;
		}

		if(pDM_Odm->SupportICType & ODM_N_ANTDIV_SUPPORT)
		{
			if(pDM_Odm->SupportICType==ODM_RTL8192E)
			{
				ODM_SetBBReg(pDM_Odm, 0xB38 , BIT5|BIT4|BIT3, DefaultAnt); //Default RX
				ODM_SetBBReg(pDM_Odm, 0xB38 , BIT8|BIT7|BIT6, OptionalAnt);//Optional RX
			}
			else
			{
				ODM_SetBBReg(pDM_Odm, 0x864 , BIT5|BIT4|BIT3, DefaultAnt);	//Default RX
				ODM_SetBBReg(pDM_Odm, 0x864 , BIT8|BIT7|BIT6, OptionalAnt);	//Optional RX

				if(pDM_Odm->SupportICType == ODM_RTL8723B)
				{
					value32 = ODM_GetBBReg(pDM_Odm, 0x948, 0xFFF);

					if (value32 !=0x280)
						ODM_SetBBReg(pDM_Odm, 0x948 , BIT9, DefaultAnt);

					rtw_hal_set_tx_power_level(pAdapter, pHalData->CurrentChannel);
				}

			}
			ODM_SetBBReg(pDM_Odm, 0x860, BIT14|BIT13|BIT12, DefaultAnt);	        //Default TX
		}
		else if(pDM_Odm->SupportICType & ODM_AC_ANTDIV_SUPPORT)
		{
			ODM_SetBBReg(pDM_Odm, 0xC08 , BIT21|BIT20|BIT19, DefaultAnt);	 //Default RX
			ODM_SetBBReg(pDM_Odm, 0xC08 , BIT24|BIT23|BIT22, OptionalAnt);//Optional RX
			ODM_SetBBReg(pDM_Odm, 0xC08 , BIT27|BIT26|BIT25, DefaultAnt);	 //Default TX
		}
		ODM_SetMACReg(pDM_Odm, 0x6D8 , BIT10|BIT9|BIT8, DefaultAnt);	//PathA Resp Tx
	}
	else// pDM_FatTable->RxIdleAnt == Ant
	{
		ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("[ Stay in Ori-Ant ]  RxIdleAnt =%s\n",(Ant==MAIN_ANT)?"MAIN_ANT":"AUX_ANT"));
		pDM_FatTable->RxIdleAnt = Ant;
	}
}


VOID
odm_UpdateTxAnt(IN PDM_ODM_T pDM_Odm, IN u1Byte Ant, IN u4Byte MacId)
{
	pFAT_T	pDM_FatTable = &pDM_Odm->DM_FatTable;
	u1Byte	TxAnt;

	if(Ant == MAIN_ANT)
		TxAnt = ANT1_2G;
	else
		TxAnt = ANT2_2G;

	pDM_FatTable->antsel_a[MacId] = TxAnt&BIT0;
	pDM_FatTable->antsel_b[MacId] = (TxAnt&BIT1)>>1;
	pDM_FatTable->antsel_c[MacId] = (TxAnt&BIT2)>>2;
}

VOID
odm_TRX_HWAntDiv_Init_8723B(
	IN		PDM_ODM_T		pDM_Odm
)
{
	ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("***8723B AntDiv_Init =>  AntDivType=[CG_TRX_HW_ANTDIV(DPDT)]\n"));

	//Mapping Table
	ODM_SetBBReg(pDM_Odm, 0x914 , bMaskByte0, 0);
	ODM_SetBBReg(pDM_Odm, 0x914 , bMaskByte1, 1);

	//OFDM HW AntDiv Parameters
	ODM_SetBBReg(pDM_Odm, 0xCA4 , 0x7FF, 0xa0); //thershold
	ODM_SetBBReg(pDM_Odm, 0xCA4 , 0x7FF000, 0x00); //bias

	//CCK HW AntDiv Parameters
	ODM_SetBBReg(pDM_Odm, 0xA74 , BIT7, 1); //patch for clk from 88M to 80M
	ODM_SetBBReg(pDM_Odm, 0xA0C , BIT4, 1); //do 64 samples

	//BT Coexistence
	ODM_SetBBReg(pDM_Odm, 0x864, BIT12, 0); //keep antsel_map when GNT_BT = 1
	ODM_SetBBReg(pDM_Odm, 0x874 , BIT23, 0); //Disable hw antsw & fast_train.antsw when GNT_BT=1

        //Output Pin Settings
	ODM_SetBBReg(pDM_Odm, 0x870 , BIT8, 0); //

	ODM_SetBBReg(pDM_Odm, 0x948 , BIT6, 0); //WL_BB_SEL_BTG_TRXG_anta,  (1: HW CTRL  0: SW CTRL)
	ODM_SetBBReg(pDM_Odm, 0x948 , BIT7, 0);

	ODM_SetMACReg(pDM_Odm, 0x40 , BIT3, 1);
	ODM_SetMACReg(pDM_Odm, 0x38 , BIT11, 1);
	ODM_SetMACReg(pDM_Odm, 0x4C ,  BIT24|BIT23, 2); //select DPDT_P and DPDT_N as output pin

	ODM_SetBBReg(pDM_Odm, 0x944 , BIT0|BIT1, 3); //in/out
	ODM_SetBBReg(pDM_Odm, 0x944 , BIT31, 0); //

	ODM_SetBBReg(pDM_Odm, 0x92C , BIT1, 0); //DPDT_P non-inverse
	ODM_SetBBReg(pDM_Odm, 0x92C , BIT0, 1); //DPDT_N inverse

	ODM_SetBBReg(pDM_Odm, 0x930 , 0xF0, 8); // DPDT_P = ANTSEL[0]
	ODM_SetBBReg(pDM_Odm, 0x930 , 0xF, 8); // DPDT_N = ANTSEL[0]

	//Timming issue
	ODM_SetBBReg(pDM_Odm, 0xE20 , BIT23|BIT22|BIT21|BIT20, 8); //keep antidx after tx for ACK ( unit x 32 mu sec)

	//2 [--For HW Bug Setting]
	if(pDM_Odm->AntType == ODM_AUTO_ANT)
		ODM_SetBBReg(pDM_Odm, 0xA00 , BIT15, 0); //CCK AntDiv function block enable
}

VOID
odm_S0S1_SWAntDiv_Init_8723B(
	IN		PDM_ODM_T		pDM_Odm
)
{
	pSWAT_T		pDM_SWAT_Table = &pDM_Odm->DM_SWAT_Table;
	pFAT_T		pDM_FatTable = &pDM_Odm->DM_FatTable;

	ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("***8723B AntDiv_Init => AntDivType=[ S0S1_SW_AntDiv] \n"));

	//Mapping Table
	ODM_SetBBReg(pDM_Odm, 0x914 , bMaskByte0, 0);
	ODM_SetBBReg(pDM_Odm, 0x914 , bMaskByte1, 1);

	//Output Pin Settings
	//ODM_SetBBReg(pDM_Odm, 0x948 , BIT6, 0x1);
	ODM_SetBBReg(pDM_Odm, 0x870 , BIT9|BIT8, 0);

	pDM_FatTable->bBecomeLinked  =FALSE;
	pDM_SWAT_Table->try_flag = 0xff;
	pDM_SWAT_Table->Double_chk_flag = 0;
	pDM_SWAT_Table->TrafficLoad = TRAFFIC_LOW;

	//Timming issue
	ODM_SetBBReg(pDM_Odm, 0xE20 , BIT23|BIT22|BIT21|BIT20, 8); //keep antidx after tx for ACK ( unit x 32 mu sec)

	//2 [--For HW Bug Setting]
	ODM_SetBBReg(pDM_Odm, 0x80C , BIT21, 0); //TX Ant  by Reg

}

VOID
odm_HW_AntDiv(
	IN		PDM_ODM_T		pDM_Odm
)
{
	u4Byte	i,MinMaxRSSI=0xFF, AntDivMaxRSSI=0, MaxRSSI=0, LocalMaxRSSI;
	u4Byte	Main_RSSI, Aux_RSSI, pkt_ratio_m=0, pkt_ratio_a=0,pkt_threshold=10;
	u1Byte	RxIdleAnt=0, TargetAnt=7;
	pFAT_T	pDM_FatTable = &pDM_Odm->DM_FatTable;
	pDIG_T	pDM_DigTable = &pDM_Odm->DM_DigTable;
	PSTA_INFO_T	pEntry;

	if(!pDM_Odm->bLinked) //bLinked==False
	{
		ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("[No Link!!!]\n"));

		if(pDM_FatTable->bBecomeLinked == TRUE)
		{
			odm_AntDiv_on_off(pDM_Odm, ANTDIV_OFF);
			ODM_UpdateRxIdleAnt(pDM_Odm, MAIN_ANT);

			pDM_FatTable->bBecomeLinked = pDM_Odm->bLinked;
		}
		return;
	}
	else
	{
		if(pDM_FatTable->bBecomeLinked ==FALSE)
		{
			ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("[Linked !!!]\n"));
			odm_AntDiv_on_off(pDM_Odm, ANTDIV_ON);
			if(pDM_Odm->SupportICType == ODM_RTL8821 )
				ODM_SetBBReg(pDM_Odm, 0x800 , BIT25, 0); //CCK AntDiv function disable

			else if(pDM_Odm->SupportICType == ODM_RTL8723B ||pDM_Odm->SupportICType == ODM_RTL8812)
				ODM_SetBBReg(pDM_Odm, 0xA00 , BIT15, 0); //CCK AntDiv function disable

			pDM_FatTable->bBecomeLinked = pDM_Odm->bLinked;

			if(pDM_Odm->SupportICType==ODM_RTL8723B && pDM_Odm->AntDivType == CG_TRX_HW_ANTDIV)
			{
				ODM_SetBBReg(pDM_Odm, 0x930 , 0xF0, 8); // DPDT_P = ANTSEL[0]   // for 8723B AntDiv function patch.  BB  Dino  130412
				ODM_SetBBReg(pDM_Odm, 0x930 , 0xF, 8); // DPDT_N = ANTSEL[0]
			}
		}
	}

	ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("\n[HW AntDiv] Start =>\n"));

	for (i=0; i<ODM_ASSOCIATE_ENTRY_NUM; i++)
	{
		pEntry = pDM_Odm->pODM_StaInfo[i];
		if(IS_STA_VALID(pEntry))
		{
			//2 Caculate RSSI per Antenna
			Main_RSSI = (pDM_FatTable->MainAnt_Cnt[i]!=0)?(pDM_FatTable->MainAnt_Sum[i]/pDM_FatTable->MainAnt_Cnt[i]):0;
			Aux_RSSI = (pDM_FatTable->AuxAnt_Cnt[i]!=0)?(pDM_FatTable->AuxAnt_Sum[i]/pDM_FatTable->AuxAnt_Cnt[i]):0;
			TargetAnt = (Main_RSSI==Aux_RSSI)?pDM_FatTable->RxIdleAnt:((Main_RSSI>=Aux_RSSI)?MAIN_ANT:AUX_ANT);
			/*
			if( pDM_FatTable->MainAnt_Cnt[i]!=0 && pDM_FatTable->AuxAnt_Cnt[i]!=0 )
			{
			pkt_ratio_m=( pDM_FatTable->MainAnt_Cnt[i] / pDM_FatTable->AuxAnt_Cnt[i] );
			pkt_ratio_a=( pDM_FatTable->AuxAnt_Cnt[i] / pDM_FatTable->MainAnt_Cnt[i] );

				if (pkt_ratio_m >= pkt_threshold)
					TargetAnt=MAIN_ANT;

				else if(pkt_ratio_a >= pkt_threshold)
					TargetAnt=AUX_ANT;
			}
			*/
			ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV,ODM_DBG_LOUD,("*** SupportICType=[%u] \n",pDM_Odm->SupportICType));
			ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("***  Main_Cnt = (( %u ))  , Main_RSSI= ((  %u )) \n", pDM_FatTable->MainAnt_Cnt[i], Main_RSSI));
			ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("***  Aux_Cnt   = (( %u ))  , Aux_RSSI = ((  %u )) \n", pDM_FatTable->AuxAnt_Cnt[i]  , Aux_RSSI ));
			ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("*** MAC ID:[ %u ] , TargetAnt = (( %s )) \n", i ,( TargetAnt ==MAIN_ANT)?"MAIN_ANT":"AUX_ANT"));

			ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV,ODM_DBG_LOUD,("*** Phy_AntSel_A=[ %d, %d, %d] \n",((pDM_Odm->DM_FatTable.antsel_rx_keep_0)&BIT2)>>2,
				                                                                              ((pDM_Odm->DM_FatTable.antsel_rx_keep_0)&BIT1) >>1, ((pDM_Odm->DM_FatTable.antsel_rx_keep_0)&BIT0)));

			LocalMaxRSSI = (Main_RSSI>Aux_RSSI)?Main_RSSI:Aux_RSSI;
			//2 Select MaxRSSI for DIG
			if((LocalMaxRSSI > AntDivMaxRSSI) && (LocalMaxRSSI < 40))
				AntDivMaxRSSI = LocalMaxRSSI;
			if(LocalMaxRSSI > MaxRSSI)
				MaxRSSI = LocalMaxRSSI;

			//2 Select RX Idle Antenna
			if ( (LocalMaxRSSI != 0) &&  (LocalMaxRSSI < MinMaxRSSI) )
			{
				RxIdleAnt = TargetAnt;
				MinMaxRSSI = LocalMaxRSSI;
			}
			//2 Select TX Antenna

			#if TX_BY_REG

			#else
				if(pDM_Odm->AntDivType != CGCS_RX_HW_ANTDIV)
					odm_UpdateTxAnt(pDM_Odm, TargetAnt, i);
			#endif

		}
		pDM_FatTable->MainAnt_Sum[i] = 0;
		pDM_FatTable->AuxAnt_Sum[i] = 0;
		pDM_FatTable->MainAnt_Cnt[i] = 0;
		pDM_FatTable->AuxAnt_Cnt[i] = 0;
	}

	//2 Set RX Idle Antenna
	ODM_UpdateRxIdleAnt(pDM_Odm, RxIdleAnt);

	pDM_DigTable->AntDiv_RSSI_max = AntDivMaxRSSI;
	pDM_DigTable->RSSI_max = MaxRSSI;
}

VOID
odm_S0S1_SwAntDiv(
	IN		PDM_ODM_T		pDM_Odm,
	IN		u1Byte			Step
	)
{
	u4Byte			i,MinMaxRSSI=0xFF, LocalMaxRSSI,LocalMinRSSI;
	u4Byte			Main_RSSI, Aux_RSSI;
	u1Byte			reset_period=10, SWAntDiv_threshold=35;
	u1Byte			HighTraffic_TrainTime_U=0x32,HighTraffic_TrainTime_L,Train_time_temp;
	u1Byte			LowTraffic_TrainTime_U=200,LowTraffic_TrainTime_L;
	u1Byte			RxIdleAnt, TargetAnt, nextAnt;
	pSWAT_T			pDM_SWAT_Table = &pDM_Odm->DM_SWAT_Table;
	pFAT_T			pDM_FatTable = &pDM_Odm->DM_FatTable;
	PSTA_INFO_T		pEntry=NULL;
	//static u1Byte		reset_idx;
	u4Byte			value32;
	PADAPTER		Adapter	 =  pDM_Odm->Adapter;
	u8Byte			curTxOkCnt=0, curRxOkCnt=0,TxCntOffset, RxCntOffset;

	if(!pDM_Odm->bLinked) //bLinked==False
	{
		ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("[No Link!!!]\n"));
		if(pDM_FatTable->bBecomeLinked == TRUE)
		{
			ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("Set REG 948[9:6]=0x0 \n"));
			if(pDM_Odm->SupportICType == ODM_RTL8723B)
				ODM_SetBBReg(pDM_Odm, 0x948 , BIT9|BIT8|BIT7|BIT6, 0x0);

			pDM_FatTable->bBecomeLinked = pDM_Odm->bLinked;
		}
		return;
	}
	else
	{
		if(pDM_FatTable->bBecomeLinked ==FALSE)
		{
			ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("[Linked !!!]\n"));

			if(pDM_Odm->SupportICType == ODM_RTL8723B)
			{
				value32 = ODM_GetBBReg(pDM_Odm, 0x864, BIT5|BIT4|BIT3);

				if (value32==0x0)
					ODM_UpdateRxIdleAnt(pDM_Odm, MAIN_ANT);
				else if (value32==0x1)
					ODM_UpdateRxIdleAnt(pDM_Odm, AUX_ANT);

				ODM_SetBBReg(pDM_Odm, 0x948 , BIT6, 0x1);
				ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("Set REG 948[6]=0x1 , Set REG 864[5:3]=0x%x \n",value32 ));
			}

			pDM_SWAT_Table->lastTxOkCnt = 0;
			pDM_SWAT_Table->lastRxOkCnt =0;
			TxCntOffset = Adapter->TxStats.NumTxBytesUnicast;
			RxCntOffset = Adapter->RxStats.NumRxBytesUnicast;

			pDM_FatTable->bBecomeLinked = pDM_Odm->bLinked;
		}
		else
		{
			TxCntOffset = 0;
			RxCntOffset = 0;
		}
	}

	ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("[%d] { try_flag=(( %d )), Step=(( %d )), Double_chk_flag = (( %d )) }\n",
		__LINE__,pDM_SWAT_Table->try_flag,Step,pDM_SWAT_Table->Double_chk_flag));

	// Handling step mismatch condition.
	// Peak step is not finished at last time. Recover the variable and check again.
	if(	Step != pDM_SWAT_Table->try_flag	)
	{
		ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("[Step != try_flag]    Need to Reset After Link\n"));
		ODM_SwAntDivRestAfterLink(pDM_Odm);
	}

	if(pDM_SWAT_Table->try_flag == 0xff)
	{
		pDM_SWAT_Table->try_flag = 0;
		pDM_SWAT_Table->Train_time_flag=0;
		ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,("[set try_flag = 0]  Prepare for peak!\n\n"));
		return;
	}
	else//if( try_flag != 0xff )
	{
		//1 Normal State (Begin Trying)
		if(pDM_SWAT_Table->try_flag == 0)
		{

			//---trafic decision---
			curTxOkCnt = Adapter->TxStats.NumTxBytesUnicast - pDM_SWAT_Table->lastTxOkCnt - TxCntOffset;
			curRxOkCnt =Adapter->RxStats.NumRxBytesUnicast - pDM_SWAT_Table->lastRxOkCnt - RxCntOffset;
			pDM_SWAT_Table->lastTxOkCnt = Adapter->TxStats.NumTxBytesUnicast;
			pDM_SWAT_Table->lastRxOkCnt = Adapter->RxStats.NumRxBytesUnicast;

			if (curTxOkCnt > 1875000 || curRxOkCnt > 1875000)//if(PlatformDivision64(curTxOkCnt+curRxOkCnt, 2) > 1875000)  ( 1.875M * 8bit ) / 2= 7.5M bits /sec )
			{
				pDM_SWAT_Table->TrafficLoad = TRAFFIC_HIGH;
				Train_time_temp=pDM_SWAT_Table->Train_time ;

				if(pDM_SWAT_Table->Train_time_flag==3)
				{
					HighTraffic_TrainTime_L=0xa;

					if(Train_time_temp<=16)
						Train_time_temp=HighTraffic_TrainTime_L;
					else
						Train_time_temp-=16;

				}
				else if(pDM_SWAT_Table->Train_time_flag==2)
				{
					Train_time_temp-=8;
					HighTraffic_TrainTime_L=0xf;
				}
				else if(pDM_SWAT_Table->Train_time_flag==1)
				{
					Train_time_temp-=4;
					HighTraffic_TrainTime_L=0x1e;
				}
				else if(pDM_SWAT_Table->Train_time_flag==0)
				{
					Train_time_temp+=8;
					HighTraffic_TrainTime_L=0x28;
				}


				//ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("*** Train_time_temp = ((%d))\n",Train_time_temp));

				//--
				if(Train_time_temp > HighTraffic_TrainTime_U)
					Train_time_temp=HighTraffic_TrainTime_U;

				else if(Train_time_temp < HighTraffic_TrainTime_L)
					Train_time_temp=HighTraffic_TrainTime_L;

				pDM_SWAT_Table->Train_time = Train_time_temp; //50ms~10ms

				ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,("  Train_time_flag=((%d)) , Train_time=((%d)) \n",pDM_SWAT_Table->Train_time_flag, pDM_SWAT_Table->Train_time));
				ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("  [HIGH Traffic]  \n" ));
			}
			else if (curTxOkCnt > 125000 || curRxOkCnt > 125000) // ( 0.125M * 8bit ) / 2 =  0.5M bits /sec )
			{
				pDM_SWAT_Table->TrafficLoad = TRAFFIC_LOW;
				Train_time_temp=pDM_SWAT_Table->Train_time ;

				if(pDM_SWAT_Table->Train_time_flag==3)
				{
					LowTraffic_TrainTime_L=10;
					if(Train_time_temp<50)
						Train_time_temp=LowTraffic_TrainTime_L;
					else
						Train_time_temp-=50;
				}
				else if(pDM_SWAT_Table->Train_time_flag==2)
				{
					Train_time_temp-=30;
					LowTraffic_TrainTime_L=36;
				}
				else if(pDM_SWAT_Table->Train_time_flag==1)
				{
					Train_time_temp-=10;
					LowTraffic_TrainTime_L=40;
				}
				else
					Train_time_temp+=10;

				//--
				if(Train_time_temp >= LowTraffic_TrainTime_U)
					Train_time_temp=LowTraffic_TrainTime_U;

				else if(Train_time_temp <= LowTraffic_TrainTime_L)
					Train_time_temp=LowTraffic_TrainTime_L;

				pDM_SWAT_Table->Train_time = Train_time_temp; //50ms~20ms

				ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,("  Train_time_flag=((%d)) , Train_time=((%d)) \n",pDM_SWAT_Table->Train_time_flag, pDM_SWAT_Table->Train_time));
				ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("  [Low Traffic]  \n" ));
			}
			else
			{
				pDM_SWAT_Table->TrafficLoad = TRAFFIC_UltraLOW;
				pDM_SWAT_Table->Train_time = 0xc8; //200ms
				ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("  [Ultra-Low Traffic]  \n" ));
			}
			ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("TxOkCnt=(( %llu )), RxOkCnt=(( %llu )) \n",
				curTxOkCnt ,curRxOkCnt ));

			//-----------------

			ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,(" Current MinMaxRSSI is ((%d)) \n",pDM_FatTable->MinMaxRSSI));

                        //---reset index---
			if(pDM_SWAT_Table->reset_idx>=reset_period)
			{
				pDM_FatTable->MinMaxRSSI=0; //
				pDM_SWAT_Table->reset_idx=0;
			}
			ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("reset_idx = (( %d )) \n",pDM_SWAT_Table->reset_idx ));
			//ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,("reset_idx=%d\n",pDM_SWAT_Table->reset_idx));
			pDM_SWAT_Table->reset_idx++;

			//---double check flag---
			if(pDM_FatTable->MinMaxRSSI > SWAntDiv_threshold && pDM_SWAT_Table->Double_chk_flag== 0)
			{
				ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,(" MinMaxRSSI is ((%d)), and > %d \n",
					pDM_FatTable->MinMaxRSSI,SWAntDiv_threshold));

				pDM_SWAT_Table->Double_chk_flag =1;
				pDM_SWAT_Table->try_flag = 1;
			        pDM_SWAT_Table->RSSI_Trying = 0;

				ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD, (" Test the current Ant for (( %d )) ms again \n", pDM_SWAT_Table->Train_time));
				ODM_UpdateRxIdleAnt(pDM_Odm, pDM_FatTable->RxIdleAnt);
				ODM_SetTimer(pDM_Odm,&pDM_SWAT_Table->SwAntennaSwitchTimer_8723B, pDM_SWAT_Table->Train_time ); //ms
				return;
			}

			nextAnt = (pDM_FatTable->RxIdleAnt == MAIN_ANT)? AUX_ANT : MAIN_ANT;

			pDM_SWAT_Table->try_flag = 1;

			if(pDM_SWAT_Table->reset_idx<=1)
				pDM_SWAT_Table->RSSI_Trying = 2;
			else
				pDM_SWAT_Table->RSSI_Trying = 1;

			ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,("[set try_flag=1]  Normal State:  Begin Trying!! \n"));

		}

		else if(pDM_SWAT_Table->try_flag == 1 && pDM_SWAT_Table->Double_chk_flag== 0)
		{
			nextAnt = (pDM_FatTable->RxIdleAnt  == MAIN_ANT)? AUX_ANT : MAIN_ANT;
			pDM_SWAT_Table->RSSI_Trying--;
		}

		//1 Decision State
		if((pDM_SWAT_Table->try_flag == 1)&&(pDM_SWAT_Table->RSSI_Trying == 0) )
		{

			for (i=0; i<ODM_ASSOCIATE_ENTRY_NUM; i++)
			{
				pEntry = pDM_Odm->pODM_StaInfo[i];
				if(IS_STA_VALID(pEntry))
				{
					//2 Caculate RSSI per Antenna
					Main_RSSI = (pDM_FatTable->MainAnt_Cnt[i]!=0)?(pDM_FatTable->MainAnt_Sum[i]/pDM_FatTable->MainAnt_Cnt[i]):0;
					Aux_RSSI = (pDM_FatTable->AuxAnt_Cnt[i]!=0)?(pDM_FatTable->AuxAnt_Sum[i]/pDM_FatTable->AuxAnt_Cnt[i]):0;

					if(pDM_FatTable->MainAnt_Cnt[i]<=1 && pDM_FatTable->CCK_counter_main>=1)
						Main_RSSI=0;

					if(pDM_FatTable->AuxAnt_Cnt[i]<=1 && pDM_FatTable->CCK_counter_aux>=1)
						Aux_RSSI=0;

					TargetAnt = (Main_RSSI==Aux_RSSI)?pDM_SWAT_Table->PreAntenna:((Main_RSSI>=Aux_RSSI)?MAIN_ANT:AUX_ANT);
					LocalMaxRSSI = (Main_RSSI>=Aux_RSSI) ? Main_RSSI : Aux_RSSI;
					LocalMinRSSI = (Main_RSSI>=Aux_RSSI) ? Aux_RSSI : Main_RSSI;

					ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("***  CCK_counter_main = (( %d ))  , CCK_counter_aux= ((  %d )) \n", pDM_FatTable->CCK_counter_main, pDM_FatTable->CCK_counter_aux));
					ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("***  OFDM_counter_main = (( %d ))  , OFDM_counter_aux= ((  %d )) \n", pDM_FatTable->OFDM_counter_main, pDM_FatTable->OFDM_counter_aux));
					ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("***  Main_Cnt = (( %d ))  , Main_RSSI= ((  %d )) \n", pDM_FatTable->MainAnt_Cnt[i], Main_RSSI));
					ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("***  Aux_Cnt   = (( %d ))  , Aux_RSSI = ((  %d )) \n", pDM_FatTable->AuxAnt_Cnt[i]  , Aux_RSSI ));
					ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("*** MAC ID:[ %d ] , TargetAnt = (( %s )) \n", i ,( TargetAnt ==MAIN_ANT)?"MAIN_ANT":"AUX_ANT"));

					//2 Select RX Idle Antenna

					if (LocalMaxRSSI != 0 && LocalMaxRSSI < MinMaxRSSI)
					{
							RxIdleAnt = TargetAnt;
							MinMaxRSSI = LocalMaxRSSI;
							ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("*** LocalMaxRSSI-LocalMinRSSI = ((%d))\n",(LocalMaxRSSI-LocalMinRSSI)));

							if((LocalMaxRSSI-LocalMinRSSI)>8)
							{
								if(LocalMinRSSI != 0)
									pDM_SWAT_Table->Train_time_flag=3;
								else
								{
									if(MinMaxRSSI > SWAntDiv_threshold)
										pDM_SWAT_Table->Train_time_flag=0;
									else
										pDM_SWAT_Table->Train_time_flag=3;
								}
							}
							else if((LocalMaxRSSI-LocalMinRSSI)>5)
								pDM_SWAT_Table->Train_time_flag=2;
							else if((LocalMaxRSSI-LocalMinRSSI)>2)
								pDM_SWAT_Table->Train_time_flag=1;
							else
								pDM_SWAT_Table->Train_time_flag=0;

					}

					//2 Select TX Antenna
					if(TargetAnt == MAIN_ANT)
						pDM_FatTable->antsel_a[i] = ANT1_2G;
					else
						pDM_FatTable->antsel_a[i] = ANT2_2G;

				}
					pDM_FatTable->MainAnt_Sum[i] = 0;
					pDM_FatTable->AuxAnt_Sum[i] = 0;
					pDM_FatTable->MainAnt_Cnt[i] = 0;
					pDM_FatTable->AuxAnt_Cnt[i] = 0;
					pDM_FatTable->CCK_counter_main=0;
					pDM_FatTable->CCK_counter_aux=0;
					pDM_FatTable->OFDM_counter_main=0;
					pDM_FatTable->OFDM_counter_aux=0;

			}


			pDM_FatTable->MinMaxRSSI=MinMaxRSSI;
			pDM_SWAT_Table->try_flag = 0;

			if( pDM_SWAT_Table->Double_chk_flag==1)
			{
				pDM_SWAT_Table->Double_chk_flag=0;
				if(pDM_FatTable->MinMaxRSSI > SWAntDiv_threshold)
				{
					ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,(" [Double check] MinMaxRSSI ((%d)) > %d again!! \n",
						pDM_FatTable->MinMaxRSSI,SWAntDiv_threshold));

					ODM_UpdateRxIdleAnt(pDM_Odm, RxIdleAnt);

					ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,("[reset try_flag = 0] Training accomplished !!!] \n\n\n"));
					return;
				}
				else
				{
					ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,(" [Double check] MinMaxRSSI ((%d)) <= %d !! \n",
						pDM_FatTable->MinMaxRSSI,SWAntDiv_threshold));

					nextAnt = (pDM_FatTable->RxIdleAnt  == MAIN_ANT)? AUX_ANT : MAIN_ANT;
					pDM_SWAT_Table->try_flag = 0;
					pDM_SWAT_Table->reset_idx=reset_period;
					ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,("[set try_flag=0]  Normal State:  Need to tryg again!! \n\n\n"));
					return;
				}
			}
			else
			{
				pDM_SWAT_Table->PreAntenna =RxIdleAnt;
				ODM_UpdateRxIdleAnt(pDM_Odm, RxIdleAnt );
				ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD,("[reset try_flag = 0] Training accomplished !!!] \n\n\n"));
			        return;
			}

		}

	}

	//1 4.Change TRX antenna

	ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("RSSI_Trying = (( %d )),    Ant: (( %s )) >>> (( %s )) \n",
		pDM_SWAT_Table->RSSI_Trying, (pDM_FatTable->RxIdleAnt  == MAIN_ANT?"MAIN":"AUX"),(nextAnt == MAIN_ANT?"MAIN":"AUX")));

	ODM_UpdateRxIdleAnt(pDM_Odm, nextAnt);

	//1 5.Reset Statistics

	pDM_FatTable->RxIdleAnt  = nextAnt;

	//1 6.Set next timer   (Trying State)

	ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV, ODM_DBG_LOUD, (" Test ((%s)) Ant for (( %d )) ms \n", (nextAnt == MAIN_ANT?"MAIN":"AUX"), pDM_SWAT_Table->Train_time));
	ODM_SetTimer(pDM_Odm,&pDM_SWAT_Table->SwAntennaSwitchTimer_8723B, pDM_SWAT_Table->Train_time ); //ms
}

VOID
ODM_SW_AntDiv_Callback(void *FunctionContext)
{
	PDM_ODM_T	pDM_Odm= (PDM_ODM_T)FunctionContext;
	PADAPTER	padapter = pDM_Odm->Adapter;
	if(padapter->net_closed == _TRUE)
	    return;
	//odm_S0S1_SwAntDiv(pDM_Odm, SWAW_STEP_DETERMINE);
}

VOID
ODM_AntDivInit(
	IN PDM_ODM_T	pDM_Odm
	)
{
	pFAT_T			pDM_FatTable = &pDM_Odm->DM_FatTable;
	pSWAT_T			pDM_SWAT_Table = &pDM_Odm->DM_SWAT_Table;


	if(!(pDM_Odm->SupportAbility & ODM_BB_ANT_DIV))
	{
		ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV,ODM_DBG_LOUD,("[Return!!!]   Not Support Antenna Diversity Function\n"));
		return;
	}
        //---

	//2 [--General---]
	pDM_Odm->antdiv_period=0;
	pDM_Odm->antdiv_select=0;
	pDM_SWAT_Table->Ant5G = MAIN_ANT;
	pDM_SWAT_Table->Ant2G = MAIN_ANT;
	pDM_FatTable->CCK_counter_main=0;
	pDM_FatTable->CCK_counter_aux=0;
	pDM_FatTable->OFDM_counter_main=0;
	pDM_FatTable->OFDM_counter_aux=0;

	//3 [Set MAIN_ANT as default antenna if Auto-Ant enable]
	if (pDM_Odm->antdiv_select==1)
		pDM_Odm->AntType = ODM_FIX_MAIN_ANT;
	else if (pDM_Odm->antdiv_select==2)
		pDM_Odm->AntType = ODM_FIX_AUX_ANT;
	else if(pDM_Odm->antdiv_select==0)
		pDM_Odm->AntType = ODM_AUTO_ANT;

	if(pDM_Odm->AntType == ODM_AUTO_ANT)
	{
		odm_AntDiv_on_off(pDM_Odm, ANTDIV_OFF);
		ODM_UpdateRxIdleAnt(pDM_Odm, MAIN_ANT);
	}
	else
	{
		odm_AntDiv_on_off(pDM_Odm, ANTDIV_OFF);

		if(pDM_Odm->AntType == ODM_FIX_MAIN_ANT)
		{
			ODM_UpdateRxIdleAnt(pDM_Odm, MAIN_ANT);
			return;
		}
		else if(pDM_Odm->AntType == ODM_FIX_AUX_ANT)
		{
			ODM_UpdateRxIdleAnt(pDM_Odm, AUX_ANT);
			return;
		}
	}
	//---
	if(pDM_Odm->AntDivType != CGCS_RX_HW_ANTDIV)
	{
		if(pDM_Odm->SupportICType & ODM_N_ANTDIV_SUPPORT)
		{
			#if TX_BY_REG
			ODM_SetBBReg(pDM_Odm, 0x80c , BIT21, 0); //Reg80c[21]=1'b0		//from Reg
			#else
			ODM_SetBBReg(pDM_Odm, 0x80c , BIT21, 1);
			#endif
		}
		else if(pDM_Odm->SupportICType & ODM_AC_ANTDIV_SUPPORT)
		{
			#if TX_BY_REG
			ODM_SetBBReg(pDM_Odm, 0x900 , BIT18, 0);
			#else
			ODM_SetBBReg(pDM_Odm, 0x900 , BIT18, 1);
			#endif
		}
	}

	//2 [--8723B---]
	else if(pDM_Odm->SupportICType == ODM_RTL8723B)
	{
		//pDM_Odm->AntDivType = S0S1_SW_ANTDIV;
		//pDM_Odm->AntDivType = CG_TRX_HW_ANTDIV;

		if(pDM_Odm->AntDivType != S0S1_SW_ANTDIV && pDM_Odm->AntDivType != CG_TRX_HW_ANTDIV)
		{
			ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV,ODM_DBG_LOUD,("[Return!!!] 8723B  Not Supprrt This AntDiv Type\n"));
			pDM_Odm->SupportAbility &= ~(ODM_BB_ANT_DIV);
			return;
		}

		if( pDM_Odm->AntDivType==S0S1_SW_ANTDIV)
			odm_S0S1_SWAntDiv_Init_8723B(pDM_Odm);
		else if(pDM_Odm->AntDivType==CG_TRX_HW_ANTDIV)
			odm_TRX_HWAntDiv_Init_8723B(pDM_Odm);
	}
}

VOID
ODM_AntDiv(
	IN		PDM_ODM_T		pDM_Odm
)
{
	PADAPTER		pAdapter	= pDM_Odm->Adapter;
	pFAT_T			pDM_FatTable = &pDM_Odm->DM_FatTable;

	if(*pDM_Odm->pBandType == ODM_BAND_5G )
	{
		if(pDM_FatTable->idx_AntDiv_counter_5G <  pDM_Odm->antdiv_period )
		{
			pDM_FatTable->idx_AntDiv_counter_5G++;
			return;
		}
		else
			pDM_FatTable->idx_AntDiv_counter_5G=0;
	}
	else	if(*pDM_Odm->pBandType == ODM_BAND_2_4G )
	{
		if(pDM_FatTable->idx_AntDiv_counter_2G <  pDM_Odm->antdiv_period )
		{
			pDM_FatTable->idx_AntDiv_counter_2G++;
			return;
		}
		else
			pDM_FatTable->idx_AntDiv_counter_2G=0;
	}
	//----------
	if(!(pDM_Odm->SupportAbility & ODM_BB_ANT_DIV))
	{
		ODM_RT_TRACE(pDM_Odm, ODM_COMP_ANT_DIV,ODM_DBG_LOUD,("[Return!!!]   Not Support Antenna Diversity Function\n"));
		return;
	}

	//----------

	if (pDM_Odm->antdiv_select==1)
		pDM_Odm->AntType = ODM_FIX_MAIN_ANT;
	else if (pDM_Odm->antdiv_select==2)
		pDM_Odm->AntType = ODM_FIX_AUX_ANT;
	else  if (pDM_Odm->antdiv_select==0)
		pDM_Odm->AntType = ODM_AUTO_ANT;

	if(pDM_Odm->AntType != ODM_AUTO_ANT)
	{
			ODM_RT_TRACE(pDM_Odm,ODM_COMP_ANT_DIV, ODM_DBG_LOUD, ("Fix Antenna at (( %s ))\n",(pDM_Odm->AntType == ODM_FIX_MAIN_ANT)?"MAIN":"AUX"));

		if(pDM_Odm->AntType != pDM_Odm->pre_AntType)
		{
			odm_AntDiv_on_off(pDM_Odm, ANTDIV_OFF);

			if(pDM_Odm->SupportICType & ODM_N_ANTDIV_SUPPORT)
				ODM_SetBBReg(pDM_Odm, 0x80c , BIT21, 0);
			else if(pDM_Odm->SupportICType & ODM_AC_ANTDIV_SUPPORT)
				ODM_SetBBReg(pDM_Odm, 0x900 , BIT18, 0);

			if(pDM_Odm->AntType == ODM_FIX_MAIN_ANT)
				ODM_UpdateRxIdleAnt(pDM_Odm, MAIN_ANT);
			else if(pDM_Odm->AntType == ODM_FIX_AUX_ANT)
				ODM_UpdateRxIdleAnt(pDM_Odm, AUX_ANT);
		}
		pDM_Odm->pre_AntType=pDM_Odm->AntType;
		return;
	}
	else
	{
		if(pDM_Odm->AntType != pDM_Odm->pre_AntType)
		{
			odm_AntDiv_on_off(pDM_Odm, ANTDIV_ON);
			 if(pDM_Odm->SupportICType & ODM_N_ANTDIV_SUPPORT)
				ODM_SetBBReg(pDM_Odm, 0x80c , BIT21, 1);
			else if(pDM_Odm->SupportICType & ODM_AC_ANTDIV_SUPPORT)
				ODM_SetBBReg(pDM_Odm, 0x900 , BIT18, 1);
		}
		pDM_Odm->pre_AntType=pDM_Odm->AntType;
	}


	//3 -----------------------------------------------------------------------------------------------------------
	//2 [--88E---]
	if(pDM_Odm->SupportICType == ODM_RTL8188E)
	{
	}

	//2 [--8723B---]
	else if(pDM_Odm->SupportICType == ODM_RTL8723B)
	{
		if (pDM_Odm->AntDivType==S0S1_SW_ANTDIV)
			odm_S0S1_SwAntDiv(pDM_Odm, SWAW_STEP_PEAK);
		else if (pDM_Odm->AntDivType==CG_TRX_HW_ANTDIV)
			odm_HW_AntDiv(pDM_Odm);
	}
}


VOID
odm_AntselStatistics(
	IN		PDM_ODM_T		pDM_Odm,
	IN		u1Byte			antsel_tr_mux,
	IN		u4Byte			MacId,
	IN		u4Byte			RxPWDBAll
)
{
	pFAT_T	pDM_FatTable = &pDM_Odm->DM_FatTable;

	if(antsel_tr_mux == ANT1_2G)
	{
		pDM_FatTable->MainAnt_Sum[MacId]+=RxPWDBAll;
		pDM_FatTable->MainAnt_Cnt[MacId]++;
	}
	else
	{
		pDM_FatTable->AuxAnt_Sum[MacId]+=RxPWDBAll;
		pDM_FatTable->AuxAnt_Cnt[MacId]++;
	}
}


VOID
ODM_Process_RSSIForAntDiv(
	IN OUT	PDM_ODM_T					pDM_Odm,
	IN		PODM_PHY_INFO_T				pPhyInfo,
	IN		PODM_PACKET_INFO_T			pPktinfo
	)
{
u1Byte			isCCKrate=0,CCKMaxRate=DESC_RATE11M;
pFAT_T			pDM_FatTable = &pDM_Odm->DM_FatTable;

	u1Byte			RxPower_Ant0, RxPower_Ant1;

	if(pDM_Odm->SupportICType & ODM_N_ANTDIV_SUPPORT)
		CCKMaxRate=DESC_RATE11M;
	else if(pDM_Odm->SupportICType & ODM_AC_ANTDIV_SUPPORT)
		CCKMaxRate=DESC_RATE11M;
	isCCKrate = (pPktinfo->DataRate <= CCKMaxRate)?TRUE:FALSE;


	if(  (pDM_Odm->SupportICType == ODM_RTL8192E||pDM_Odm->SupportICType == ODM_RTL8812)   && (pPktinfo->DataRate > CCKMaxRate) )
	{
		RxPower_Ant0 = pPhyInfo->RxMIMOSignalStrength[0];
		RxPower_Ant1= pPhyInfo->RxMIMOSignalStrength[1];
	}
	else
		RxPower_Ant0=pPhyInfo->RxPWDBAll;

	if(pDM_Odm->AntDivType == CG_TRX_SMART_ANTDIV)
	{
		if( (pDM_Odm->SupportICType & ODM_SMART_ANT_SUPPORT) &&  pPktinfo->bPacketToSelf   && pDM_FatTable->FAT_State == FAT_TRAINING_STATE )//(pPktinfo->bPacketMatchBSSID && (!pPktinfo->bPacketBeacon))
		{
			u1Byte	antsel_tr_mux;
			antsel_tr_mux = (pDM_FatTable->antsel_rx_keep_2<<2) |(pDM_FatTable->antsel_rx_keep_1 <<1) |pDM_FatTable->antsel_rx_keep_0;
			pDM_FatTable->antSumRSSI[antsel_tr_mux] += RxPower_Ant0;
			pDM_FatTable->antRSSIcnt[antsel_tr_mux]++;
		}
	}
	else //AntDivType != CG_TRX_SMART_ANTDIV
	{
		if(  ( pDM_Odm->SupportICType & ODM_ANTDIV_SUPPORT ) &&  (pPktinfo->bPacketToSelf || pPktinfo->bPacketMatchBSSID)  )
		{
			 if(pDM_Odm->SupportICType == ODM_RTL8188E || pDM_Odm->SupportICType == ODM_RTL8192E)
				odm_AntselStatistics(pDM_Odm, pDM_FatTable->antsel_rx_keep_0, pPktinfo->StationID,RxPower_Ant0);
			else// SupportICType == ODM_RTL8821 and ODM_RTL8723B and ODM_RTL8812)
			{
				if(isCCKrate && (pDM_Odm->AntDivType == S0S1_SW_ANTDIV))
				{
					pDM_FatTable->antsel_rx_keep_0 = (pDM_FatTable->RxIdleAnt == MAIN_ANT) ? ANT1_2G : ANT2_2G;


						if(pDM_FatTable->antsel_rx_keep_0==ANT1_2G)
							pDM_FatTable->CCK_counter_main++;
						else// if(pDM_FatTable->antsel_rx_keep_0==ANT2_2G)
							pDM_FatTable->CCK_counter_aux++;

					odm_AntselStatistics(pDM_Odm, pDM_FatTable->antsel_rx_keep_0, pPktinfo->StationID, RxPower_Ant0);
				}
				else
				{

					if(pDM_FatTable->antsel_rx_keep_0==ANT1_2G)
						pDM_FatTable->OFDM_counter_main++;
					else// if(pDM_FatTable->antsel_rx_keep_0==ANT2_2G)
						pDM_FatTable->OFDM_counter_aux++;
					odm_AntselStatistics(pDM_Odm, pDM_FatTable->antsel_rx_keep_0, pPktinfo->StationID, RxPower_Ant0);
			}
		}
	}
	}
}

VOID
ODM_SetTxAntByTxInfo(
	IN		PDM_ODM_T		pDM_Odm,
	IN		pu1Byte			pDesc,
	IN		u1Byte			macId
)
{
	pFAT_T	pDM_FatTable = &pDM_Odm->DM_FatTable;

	if(!(pDM_Odm->SupportAbility & ODM_BB_ANT_DIV))
		return;

	if(pDM_Odm->AntDivType==CGCS_RX_HW_ANTDIV)
		return;


	if(pDM_Odm->SupportICType == ODM_RTL8723B)
	{
		SET_TX_DESC_ANTSEL_A_8723B(pDesc, pDM_FatTable->antsel_a[macId]);
	}
	else if(pDM_Odm->SupportICType == ODM_RTL8821)
	{
	}
	else if(pDM_Odm->SupportICType == ODM_RTL8188E)
	{
	}
	else if(pDM_Odm->SupportICType == ODM_RTL8192E)
	{


	}
}

#else

VOID ODM_AntDivInit(	IN PDM_ODM_T	pDM_Odm ){}
VOID ODM_AntDiv(	IN PDM_ODM_T		pDM_Odm){}

#endif //#if (defined(CONFIG_HW_ANTENNA_DIVERSITY))
