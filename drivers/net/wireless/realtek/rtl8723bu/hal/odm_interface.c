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

//
// ODM IO Relative API.
//

u1Byte
ODM_Read1Byte(
	IN	PDM_ODM_T		pDM_Odm,
	IN	u4Byte			RegAddr
	)
{
	PADAPTER		Adapter = pDM_Odm->Adapter;
	return rtw_read8(Adapter,RegAddr);
}

u2Byte
ODM_Read2Byte(
	IN	PDM_ODM_T		pDM_Odm,
	IN	u4Byte			RegAddr
	)
{
	PADAPTER		Adapter = pDM_Odm->Adapter;
	return rtw_read16(Adapter,RegAddr);
}

u4Byte
ODM_Read4Byte(
	IN	PDM_ODM_T		pDM_Odm,
	IN	u4Byte			RegAddr
	)
{
	PADAPTER		Adapter = pDM_Odm->Adapter;
	return rtw_read32(Adapter,RegAddr);
}

VOID
ODM_Write1Byte(
	IN	PDM_ODM_T		pDM_Odm,
	IN	u4Byte			RegAddr,
	IN	u1Byte			Data
	)
{
	PADAPTER		Adapter = pDM_Odm->Adapter;
	rtw_write8(Adapter,RegAddr, Data);
}

VOID
ODM_Write2Byte(
	IN	PDM_ODM_T		pDM_Odm,
	IN	u4Byte			RegAddr,
	IN	u2Byte			Data
	)
{
	PADAPTER		Adapter = pDM_Odm->Adapter;
	rtw_write16(Adapter,RegAddr, Data);
}

VOID
ODM_Write4Byte(
	IN	PDM_ODM_T		pDM_Odm,
	IN	u4Byte			RegAddr,
	IN	u4Byte			Data
	)
{
	PADAPTER		Adapter = pDM_Odm->Adapter;
	rtw_write32(Adapter,RegAddr, Data);
}

VOID
ODM_SetMACReg(
	IN	PDM_ODM_T	pDM_Odm,
	IN	u4Byte		RegAddr,
	IN	u4Byte		BitMask,
	IN	u4Byte		Data
	)
{
	PADAPTER		Adapter = pDM_Odm->Adapter;
	PHY_SetBBReg(Adapter, RegAddr, BitMask, Data);
}


u4Byte
ODM_GetMACReg(
	IN	PDM_ODM_T	pDM_Odm,
	IN	u4Byte		RegAddr,
	IN	u4Byte		BitMask
	)
{
	return PHY_QueryBBReg(pDM_Odm->Adapter, RegAddr, BitMask);
}


VOID
ODM_SetBBReg(
	IN	PDM_ODM_T	pDM_Odm,
	IN	u4Byte		RegAddr,
	IN	u4Byte		BitMask,
	IN	u4Byte		Data
	)
{
	PADAPTER		Adapter = pDM_Odm->Adapter;
	PHY_SetBBReg(Adapter, RegAddr, BitMask, Data);
}


u4Byte
ODM_GetBBReg(
	IN	PDM_ODM_T	pDM_Odm,
	IN	u4Byte		RegAddr,
	IN	u4Byte		BitMask
	)
{
	PADAPTER		Adapter = pDM_Odm->Adapter;
	return PHY_QueryBBReg(Adapter, RegAddr, BitMask);
}


VOID
ODM_SetRFReg(
	IN	PDM_ODM_T			pDM_Odm,
	IN	ODM_RF_RADIO_PATH_E	eRFPath,
	IN	u4Byte				RegAddr,
	IN	u4Byte				BitMask,
	IN	u4Byte				Data
	)
{
	PADAPTER		Adapter = pDM_Odm->Adapter;
	PHY_SetRFReg(Adapter, eRFPath, RegAddr, BitMask, Data);
}


u4Byte
ODM_GetRFReg(
	IN	PDM_ODM_T			pDM_Odm,
	IN	ODM_RF_RADIO_PATH_E	eRFPath,
	IN	u4Byte				RegAddr,
	IN	u4Byte				BitMask
	)
{
	PADAPTER		Adapter = pDM_Odm->Adapter;
	return PHY_QueryRFReg(Adapter, eRFPath, RegAddr, BitMask);
}




//
// ODM Memory relative API.
//
VOID
ODM_AllocateMemory(
	IN	PDM_ODM_T	pDM_Odm,
	OUT	PVOID		*pPtr,
	IN	u4Byte		length
	)
{
	*pPtr = rtw_zvmalloc(length);
}

// length could be ignored, used to detect memory leakage.
VOID
ODM_FreeMemory(
	IN	PDM_ODM_T	pDM_Odm,
	OUT	PVOID		pPtr,
	IN	u4Byte		length
	)
{
	rtw_vmfree(pPtr, length);
}

VOID
ODM_MoveMemory(
	IN	PDM_ODM_T	pDM_Odm,
	OUT PVOID		pDest,
	IN  PVOID		pSrc,
	IN  u4Byte		Length
	)
{
	_rtw_memcpy(pDest, pSrc, Length);
}

void ODM_Memory_Set
	(IN	PDM_ODM_T	pDM_Odm,
		IN  PVOID	pbuf,
		IN  s1Byte	value,
		IN  u4Byte	length)
{
	_rtw_memset(pbuf,value, length);
}
s4Byte ODM_CompareMemory(
	IN	PDM_ODM_T		pDM_Odm,
	IN	PVOID           pBuf1,
	IN	PVOID           pBuf2,
	IN	u4Byte          length
       )
{
	return _rtw_memcmp(pBuf1,pBuf2,length);
}

//
// ODM MISC relative API.
//
VOID
ODM_AcquireSpinLock(
	IN	PDM_ODM_T			pDM_Odm,
	IN	RT_SPINLOCK_TYPE	type
	)
{
}
VOID
ODM_ReleaseSpinLock(
	IN	PDM_ODM_T			pDM_Odm,
	IN	RT_SPINLOCK_TYPE	type
	)
{
}

//
// Work item relative API. FOr MP driver only~!
//
VOID
ODM_InitializeWorkItem(
	IN	PDM_ODM_T					pDM_Odm,
	IN	PRT_WORK_ITEM				pRtWorkItem,
	IN	RT_WORKITEM_CALL_BACK		RtWorkItemCallback,
	IN	PVOID						pContext,
	IN	const char*					szID
	)
{
}

VOID
ODM_StartWorkItem(
	IN	PRT_WORK_ITEM	pRtWorkItem
	)
{
}

VOID
ODM_StopWorkItem(
	IN	PRT_WORK_ITEM	pRtWorkItem
	)
{
}

VOID
ODM_FreeWorkItem(
	IN	PRT_WORK_ITEM	pRtWorkItem
	)
{
}

VOID
ODM_ScheduleWorkItem(
	IN	PRT_WORK_ITEM	pRtWorkItem
	)
{
}

VOID
ODM_IsWorkItemScheduled(
	IN	PRT_WORK_ITEM	pRtWorkItem
	)
{
}



//
// ODM Timer relative API.
//
VOID
ODM_StallExecution(
	IN	u4Byte	usDelay
	)
{
	rtw_udelay_os(usDelay);
}

VOID
ODM_delay_ms(IN u4Byte	ms)
{
	rtw_mdelay_os(ms);
}

VOID
ODM_delay_us(IN u4Byte	us)
{
	rtw_udelay_os(us);
}

VOID
ODM_sleep_ms(IN u4Byte	ms)
{
	rtw_msleep_os(ms);
}

VOID
ODM_sleep_us(IN u4Byte	us)
{
	rtw_usleep_os(us);
}

VOID
ODM_SetTimer(
	IN	PDM_ODM_T		pDM_Odm,
	IN	PRT_TIMER		pTimer,
	IN	u4Byte			msDelay
	)
{
	_set_timer(pTimer,msDelay ); //ms
}

VOID
ODM_InitializeTimer(
	IN	PDM_ODM_T			pDM_Odm,
	IN	PRT_TIMER			pTimer,
	IN	RT_TIMER_CALL_BACK	CallBackFunc,
	IN	PVOID				pContext,
	IN	const char*			szID
	)
{
	PADAPTER Adapter = pDM_Odm->Adapter;
	_init_timer(pTimer,Adapter->pnetdev,CallBackFunc,pDM_Odm);
}

VOID
ODM_CancelTimer(
	IN	PDM_ODM_T		pDM_Odm,
	IN	PRT_TIMER		pTimer
	)
{
	_cancel_timer_ex(pTimer);
}

VOID
ODM_ReleaseTimer(
	IN	PDM_ODM_T		pDM_Odm,
	IN	PRT_TIMER		pTimer
	)
{
}


//
// ODM FW relative API.
//
u4Byte
ODM_FillH2CCmd(
	IN	pu1Byte		pH2CBuffer,
	IN	u4Byte		H2CBufferLen,
	IN	u4Byte		CmdNum,
	IN	pu4Byte		pElementID,
	IN	pu4Byte		pCmdLen,
	IN	pu1Byte*		pCmbBuffer,
	IN	pu1Byte		CmdStartSeq
	)
{
	return	TRUE;
}

u4Byte
ODM_GetCurrentTime(
	IN	PDM_ODM_T		pDM_Odm
	)
{
	return rtw_get_current_time();
}

s4Byte
ODM_GetProgressingTime(
	IN	PDM_ODM_T		pDM_Odm,
	IN	u4Byte			Start_Time
	)
{
	return rtw_get_passing_time_ms(Start_Time);
}
