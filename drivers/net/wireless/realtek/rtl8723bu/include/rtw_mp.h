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
#ifndef _RTW_MP_H_
#define _RTW_MP_H_

#define MAX_MP_XMITBUF_SZ	2048
#define NR_MP_XMITFRAME		8

struct mp_xmit_frame
{
	_list	list;

	struct pkt_attrib attrib;

	_pkt *pkt;

	int frame_tag;

	_adapter *padapter;

	//insert urb, irp, and irpcnt info below...
	//max frag_cnt = 8

	u8 *mem_addr;
	u32 sz[8];
	PURB pxmit_urb[8];
	u8 bpending[8];
	sint ac_tag[8];
	sint last[8];
	uint irpcnt;
	uint fragcnt;

	uint mem[(MAX_MP_XMITBUF_SZ >> 2)];
};

struct mp_wiparam
{
	u32 bcompleted;
	u32 act_type;
	u32 io_offset;
	u32 io_value;
};

typedef void(*wi_act_func)(void* padapter);

struct mp_tx
{
	u8 stop;
	u32 count, sended;
	u8 payload;
	struct pkt_attrib attrib;
	//struct tx_desc desc;
	//u8 resvdtx[7];
	u8 desc[TXDESC_SIZE];
	u8 *pallocated_buf;
	u8 *buf;
	u32 buf_size, write_size;
	_thread_hdl_ PktTxThread;
};

#define MP_MAX_LINES		1000
#define MP_MAX_LINES_BYTES	256
#define u1Byte u8
#define s1Byte s8
#define u4Byte u32
#define s4Byte s32
#define u1Byte		u8
#define pu1Byte			u8*

#define u2Byte		u16
#define pu2Byte			u16*

#define u4Byte		u32
#define pu4Byte			u32*

#define u8Byte		u64
#define pu8Byte			u64*

#define s1Byte		s8
#define ps1Byte			s8*

#define s2Byte		s16
#define ps2Byte			s16*

#define s4Byte		s32
#define ps4Byte			s32*

#define s8Byte		s64
#define ps8Byte			s64*

#define UCHAR u8
#define USHORT u16
#define UINT u32
#define ULONG u32
#define PULONG u32*



typedef VOID (*MPT_WORK_ITEM_HANDLER)(IN PVOID Adapter);
typedef struct _MPT_CONTEXT
{
	// Indicate if we have started Mass Production Test.
	BOOLEAN			bMassProdTest;

	// Indicate if the driver is unloading or unloaded.
	BOOLEAN			bMptDrvUnload;

	_sema			MPh2c_Sema;
	_timer			MPh2c_timeout_timer;
// Event used to sync H2c for BT control

	BOOLEAN		MptH2cRspEvent;
	BOOLEAN		MptBtC2hEvent;
	BOOLEAN		bMPh2c_timeout;

	/* 8190 PCI does not support NDIS_WORK_ITEM. */
	// Work Item for Mass Production Test.
	//NDIS_WORK_ITEM	MptWorkItem;
//	RT_WORK_ITEM		MptWorkItem;
	// Event used to sync the case unloading driver and MptWorkItem is still in progress.
//	NDIS_EVENT		MptWorkItemEvent;
	// To protect the following variables.
//	NDIS_SPIN_LOCK		MptWorkItemSpinLock;
	// Indicate a MptWorkItem is scheduled and not yet finished.
	BOOLEAN			bMptWorkItemInProgress;
	// An instance which implements function and context of MptWorkItem.
	MPT_WORK_ITEM_HANDLER	CurrMptAct;

	// 1=Start, 0=Stop from UI.
	ULONG			MptTestStart;
	// _TEST_MODE, defined in MPT_Req2.h
	ULONG			MptTestItem;
	// Variable needed in each implementation of CurrMptAct.
	ULONG			MptActType;	// Type of action performed in CurrMptAct.
	// The Offset of IO operation is depend of MptActType.
	ULONG			MptIoOffset;
	// The Value of IO operation is depend of MptActType.
	ULONG			MptIoValue;
	// The RfPath of IO operation is depend of MptActType.
	ULONG			MptRfPath;

	WIRELESS_MODE		MptWirelessModeToSw;	// Wireless mode to switch.
	u8			MptChannelToSw;		// Channel to switch.
	u8			MptInitGainToSet;	// Initial gain to set.
	//ULONG			bMptAntennaA;		// TRUE if we want to use antenna A.
	ULONG			MptBandWidth;		// bandwidth to switch.
	ULONG			MptRateIndex;		// rate index.
	// Register value kept for Single Carrier Tx test.
	u8			btMpCckTxPower;
	// Register value kept for Single Carrier Tx test.
	u8			btMpOfdmTxPower;
	// For MP Tx Power index
	u8			TxPwrLevel[2];	// rf-A, rf-B
	u32			RegTxPwrLimit;
	// Content of RCR Regsiter for Mass Production Test.
	ULONG			MptRCR;
	// TRUE if we only receive packets with specific pattern.
	BOOLEAN			bMptFilterPattern;
	// Rx OK count, statistics used in Mass Production Test.
	ULONG			MptRxOkCnt;
	// Rx CRC32 error count, statistics used in Mass Production Test.
	ULONG			MptRxCrcErrCnt;

	BOOLEAN			bCckContTx;	// TRUE if we are in CCK Continuous Tx test.
	BOOLEAN			bOfdmContTx;	// TRUE if we are in OFDM Continuous Tx test.
	BOOLEAN			bStartContTx;	// TRUE if we have start Continuous Tx test.
	// TRUE if we are in Single Carrier Tx test.
	BOOLEAN			bSingleCarrier;
	// TRUE if we are in Carrier Suppression Tx Test.
	BOOLEAN			bCarrierSuppression;
	//TRUE if we are in Single Tone Tx test.
	BOOLEAN			bSingleTone;

	// ACK counter asked by K.Y..
	BOOLEAN			bMptEnableAckCounter;
	ULONG			MptAckCounter;

	// SD3 Willis For 8192S to save 1T/2T RF table for ACUT	Only fro ACUT delete later ~~~!
	//s1Byte		BufOfLines[2][MAX_LINES_HWCONFIG_TXT][MAX_BYTES_LINE_HWCONFIG_TXT];
	//s1Byte			BufOfLines[2][MP_MAX_LINES][MP_MAX_LINES_BYTES];
	//s4Byte			RfReadLine[2];

	u8		APK_bound[2];	//for APK	path A/path B
	BOOLEAN		bMptIndexEven;

	u8		backup0xc50;
	u8		backup0xc58;
	u8		backup0xc30;
	u8		backup0x52_RF_A;
	u8		backup0x52_RF_B;

	u4Byte			backup0x58_RF_A;
	u4Byte			backup0x58_RF_B;

	u1Byte			h2cReqNum;
	u1Byte			c2hBuf[32];

    u1Byte          btInBuf[100];
	ULONG			mptOutLen;
    u1Byte          mptOutBuf[100];

}MPT_CONTEXT, *PMPT_CONTEXT;

/* E-Fuse */
#define EFUSE_MAP_SIZE		512

#define EFUSE_MAX_SIZE		512
/* end of E-Fuse */

//#define RTPRIV_IOCTL_MP					( SIOCIWFIRSTPRIV + 0x17)
enum {
	WRITE_REG = 1,
	READ_REG,
	WRITE_RF,
	READ_RF,
	MP_START,
	MP_STOP,
	MP_RATE,
	MP_CHANNEL,
	MP_BANDWIDTH,
	MP_TXPOWER,
	MP_ANT_TX,
	MP_ANT_RX,
	MP_CTX,
	MP_QUERY,
	MP_ARX,
	MP_PSD,
	MP_PWRTRK,
	MP_THER,
	MP_IOCTL,
	EFUSE_GET,
	EFUSE_SET,
	MP_RESET_STATS,
	MP_DUMP,
	MP_PHYPARA,
	MP_SetRFPathSwh,
	MP_QueryDrvStats,
	MP_SetBT,
	CTA_TEST,
	MP_DISABLE_BT_COEXIST,
	MP_PwrCtlDM,
#ifdef CONFIG_WOWLAN
	MP_WOW_ENABLE,
#endif
#ifdef CONFIG_AP_WOWLAN
	MP_AP_WOW_ENABLE,
#endif
	MP_NULL,
	MP_GET_TXPOWER_INX,
};

struct mp_priv
{
	_adapter *papdater;

	//Testing Flag
	u32 mode;//0 for normal type packet, 1 for loopback packet (16bytes TXCMD)

	u32 prev_fw_state;

	//OID cmd handler
	struct mp_wiparam workparam;
//	u8 act_in_progress;

	//Tx Section
	u8 TID;
	u32 tx_pktcount;
	u32 pktInterval;
	struct mp_tx tx;

	//Rx Section
	u32 rx_bssidpktcount;
	u32 rx_pktcount;
	u32 rx_pktcount_filter_out;
	u32 rx_crcerrpktcount;
	u32 rx_pktloss;
	BOOLEAN  rx_bindicatePkt;
	struct recv_stat rxstat;

	//RF/BB relative
	u8 channel;
	u8 bandwidth;
	u8 prime_channel_offset;
	u8 txpoweridx;
	u8 txpoweridx_b;
	u8 rateidx;
	u32 preamble;
//	u8 modem;
	u32 CrystalCap;
//	u32 curr_crystalcap;

	u16 antenna_tx;
	u16 antenna_rx;
//	u8 curr_rfpath;

	u8 check_mp_pkt;

	u8 bSetTxPower;
//	uint ForcedDataRate;
	u8 mp_dm;
	u8 mac_filter[ETH_ALEN];
	u8 bmac_filter;

	struct wlan_network mp_network;
	NDIS_802_11_MAC_ADDRESS network_macaddr;

	u8 *pallocated_mp_xmitframe_buf;
	u8 *pmp_xmtframe_buf;
	_queue free_mp_xmitqueue;
	u32 free_mp_xmitframe_cnt;
	BOOLEAN bSetRxBssid;
	BOOLEAN bTxBufCkFail;

	MPT_CONTEXT MptCtx;

	u8		*TXradomBuffer;
};

typedef struct _IOCMD_STRUCT_ {
	u8	cmdclass;
	u16	value;
	u8	index;
}IOCMD_STRUCT;

struct rf_reg_param {
	u32 path;
	u32 offset;
	u32 value;
};

struct bb_reg_param {
	u32 offset;
	u32 value;
};

//=======================================================================

#define LOWER	_TRUE
#define RAISE	_FALSE

/* Hardware Registers */
#define BB_REG_BASE_ADDR		0x800

/* MP variables */
typedef enum _MP_MODE_ {
	MP_OFF,
	MP_ON,
	MP_ERR,
	MP_CONTINUOUS_TX,
	MP_SINGLE_CARRIER_TX,
	MP_CARRIER_SUPPRISSION_TX,
	MP_SINGLE_TONE_TX,
	MP_PACKET_TX,
	MP_PACKET_RX
} MP_MODE;


#define MAX_RF_PATH_NUMS	RF_PATH_MAX


extern u8 mpdatarate[NumRates];

/* MP set force data rate base on the definition. */
typedef enum _MPT_RATE_INDEX
{
	/* CCK rate. */
	MPT_RATE_1M =0 ,	/* 0 */
	MPT_RATE_2M,
	MPT_RATE_55M,
	MPT_RATE_11M,	/* 3 */

	/* OFDM rate. */
	MPT_RATE_6M,	/* 4 */
	MPT_RATE_9M,
	MPT_RATE_12M,
	MPT_RATE_18M,
	MPT_RATE_24M,
	MPT_RATE_36M,
	MPT_RATE_48M,
	MPT_RATE_54M,	/* 11 */

	/* HT rate. */
	MPT_RATE_MCS0,	/* 12 */
	MPT_RATE_MCS1,
	MPT_RATE_MCS2,
	MPT_RATE_MCS3,
	MPT_RATE_MCS4,
	MPT_RATE_MCS5,
	MPT_RATE_MCS6,
	MPT_RATE_MCS7,	/* 19 */
	MPT_RATE_MCS8,
	MPT_RATE_MCS9,
	MPT_RATE_MCS10,
	MPT_RATE_MCS11,
	MPT_RATE_MCS12,
	MPT_RATE_MCS13,
	MPT_RATE_MCS14,
	MPT_RATE_MCS15,	/* 27 */
	/* VHT rate. Total: 20*/
	MPT_RATE_VHT1SS_MCS0 = 100,// To reserve MCS16~MCS31, the index starts from #100.
	MPT_RATE_VHT1SS_MCS1, // #101
	MPT_RATE_VHT1SS_MCS2,
	MPT_RATE_VHT1SS_MCS3,
	MPT_RATE_VHT1SS_MCS4,
	MPT_RATE_VHT1SS_MCS5,
	MPT_RATE_VHT1SS_MCS6, // #106
	MPT_RATE_VHT1SS_MCS7,
	MPT_RATE_VHT1SS_MCS8,
	MPT_RATE_VHT1SS_MCS9,
	MPT_RATE_VHT2SS_MCS0,
	MPT_RATE_VHT2SS_MCS1, // #111
	MPT_RATE_VHT2SS_MCS2,
	MPT_RATE_VHT2SS_MCS3,
	MPT_RATE_VHT2SS_MCS4,
	MPT_RATE_VHT2SS_MCS5,
	MPT_RATE_VHT2SS_MCS6, // #116
	MPT_RATE_VHT2SS_MCS7,
	MPT_RATE_VHT2SS_MCS8,
	MPT_RATE_VHT2SS_MCS9,
	MPT_RATE_LAST
}MPT_RATE_E, *PMPT_RATE_E;

#define MAX_TX_PWR_INDEX_N_MODE 64	// 0x3F

typedef enum _POWER_MODE_ {
	POWER_LOW = 0,
	POWER_NORMAL
}POWER_MODE;

// The following enumeration is used to define the value of Reg0xD00[30:28] or JaguarReg0x914[18:16].
typedef enum _OFDM_TX_MODE {
	OFDM_ALL_OFF		= 0,
	OFDM_ContinuousTx	= 1,
	OFDM_SingleCarrier	= 2,
	OFDM_SingleTone		= 4,
} OFDM_TX_MODE;


#define RX_PKT_BROADCAST	1
#define RX_PKT_DEST_ADDR	2
#define RX_PKT_PHY_MATCH	3

#define Mac_OFDM_OK			0x00000000
#define Mac_OFDM_Fail			0x10000000
#define Mac_OFDM_FasleAlarm	0x20000000
#define Mac_CCK_OK				0x30000000
#define Mac_CCK_Fail			0x40000000
#define Mac_CCK_FasleAlarm		0x50000000
#define Mac_HT_OK				0x60000000
#define Mac_HT_Fail			0x70000000
#define Mac_HT_FasleAlarm		0x90000000
#define Mac_DropPacket			0xA0000000

typedef enum _ENCRY_CTRL_STATE_ {
	HW_CONTROL,		//hw encryption& decryption
	SW_CONTROL,		//sw encryption& decryption
	HW_ENCRY_SW_DECRY,	//hw encryption & sw decryption
	SW_ENCRY_HW_DECRY	//sw encryption & hw decryption
}ENCRY_CTRL_STATE;

typedef enum	_MPT_TXPWR_DEF{
	MPT_CCK,
	MPT_OFDM, // L and HT OFDM
	MPT_VHT_OFDM
}MPT_TXPWR_DEF;

#ifdef CONFIG_RF_GAIN_OFFSET

	#define		REG_RF_BB_GAIN_OFFSET	0x7f
	#define		RF_GAIN_OFFSET_MASK	0xfffff

#endif //CONFIG_RF_GAIN_OFFSET

//=======================================================================

extern s32 init_mp_priv(PADAPTER padapter);
extern void free_mp_priv(struct mp_priv *pmp_priv);
extern s32 MPT_InitializeAdapter(PADAPTER padapter, u8 Channel);
extern void MPT_DeInitAdapter(PADAPTER padapter);
extern s32 mp_start_test(PADAPTER padapter);
extern void mp_stop_test(PADAPTER padapter);

//=======================================================================
//extern void	IQCalibrateBcut(PADAPTER pAdapter);

//extern u32	bb_reg_read(PADAPTER Adapter, u16 offset);
//extern u8	bb_reg_write(PADAPTER Adapter, u16 offset, u32 value);
//extern u32	rf_reg_read(PADAPTER Adapter, u8 path, u8 offset);
//extern u8	rf_reg_write(PADAPTER Adapter, u8 path, u8 offset, u32 value);

//extern u32	get_bb_reg(PADAPTER Adapter, u16 offset, u32 bitmask);
//extern u8	set_bb_reg(PADAPTER Adapter, u16 offset, u32 bitmask, u32 value);
//extern u32	get_rf_reg(PADAPTER Adapter, u8 path, u8 offset, u32 bitmask);
//extern u8	set_rf_reg(PADAPTER Adapter, u8 path, u8 offset, u32 bitmask, u32 value);

extern u32 _read_rfreg(PADAPTER padapter, u8 rfpath, u32 addr, u32 bitmask);
extern void _write_rfreg(PADAPTER padapter, u8 rfpath, u32 addr, u32 bitmask, u32 val);

extern u32 read_macreg(_adapter *padapter, u32 addr, u32 sz);
extern void write_macreg(_adapter *padapter, u32 addr, u32 val, u32 sz);
extern u32 read_bbreg(_adapter *padapter, u32 addr, u32 bitmask);
extern void write_bbreg(_adapter *padapter, u32 addr, u32 bitmask, u32 val);
extern u32 read_rfreg(PADAPTER padapter, u8 rfpath, u32 addr);
extern void write_rfreg(PADAPTER padapter, u8 rfpath, u32 addr, u32 val);

extern void	SetChannel(PADAPTER pAdapter);
extern void	SetBandwidth(PADAPTER pAdapter);
extern int SetTxPower(PADAPTER pAdapter);
extern void	SetAntennaPathPower(PADAPTER pAdapter);
//extern void	SetTxAGCOffset(PADAPTER pAdapter, u32 ulTxAGCOffset);
extern void	SetDataRate(PADAPTER pAdapter);

extern void	SetAntenna(PADAPTER pAdapter);

//extern void	SetCrystalCap(PADAPTER pAdapter);

extern s32	SetThermalMeter(PADAPTER pAdapter, u8 target_ther);
extern void	GetThermalMeter(PADAPTER pAdapter, u8 *value);

extern void	SetContinuousTx(PADAPTER pAdapter, u8 bStart);
extern void	SetSingleCarrierTx(PADAPTER pAdapter, u8 bStart);
extern void	SetSingleToneTx(PADAPTER pAdapter, u8 bStart);
extern void	SetCarrierSuppressionTx(PADAPTER pAdapter, u8 bStart);
extern void PhySetTxPowerLevel(PADAPTER pAdapter);

extern void	fill_txdesc_for_mp(PADAPTER padapter, u8 *ptxdesc);
extern void	SetPacketTx(PADAPTER padapter);
extern void	SetPacketRx(PADAPTER pAdapter, u8 bStartRx);

extern void	ResetPhyRxPktCount(PADAPTER pAdapter);
extern u32	GetPhyRxPktReceived(PADAPTER pAdapter);
extern u32	GetPhyRxPktCRC32Error(PADAPTER pAdapter);

extern s32	SetPowerTracking(PADAPTER padapter, u8 enable);
extern void	GetPowerTracking(PADAPTER padapter, u8 *enable);

extern u32	mp_query_psd(PADAPTER pAdapter, u8 *data);


extern void Hal_SetAntenna(PADAPTER pAdapter);
extern void Hal_SetBandwidth(PADAPTER pAdapter);

extern void Hal_SetTxPower(PADAPTER pAdapter);
extern void Hal_SetCarrierSuppressionTx(PADAPTER pAdapter, u8 bStart);
extern void Hal_SetSingleToneTx ( PADAPTER pAdapter , u8 bStart );
extern void Hal_SetSingleCarrierTx (PADAPTER pAdapter, u8 bStart);
extern void Hal_SetContinuousTx (PADAPTER pAdapter, u8 bStart);
extern void Hal_SetBandwidth(PADAPTER pAdapter);

extern void Hal_SetDataRate(PADAPTER pAdapter);
extern void Hal_SetChannel(PADAPTER pAdapter);
extern void Hal_SetAntennaPathPower(PADAPTER pAdapter);
extern s32 Hal_SetThermalMeter(PADAPTER pAdapter, u8 target_ther);
extern s32 Hal_SetPowerTracking(PADAPTER padapter, u8 enable);
extern void Hal_GetPowerTracking(PADAPTER padapter, u8 * enable);
extern void Hal_GetThermalMeter(PADAPTER pAdapter, u8 *value);
extern void Hal_mpt_SwitchRfSetting(PADAPTER pAdapter);
extern void Hal_MPT_CCKTxPowerAdjust(PADAPTER Adapter, BOOLEAN bInCH14);
extern void Hal_MPT_CCKTxPowerAdjustbyIndex(PADAPTER pAdapter, BOOLEAN beven);
extern void Hal_SetCCKTxPower(PADAPTER pAdapter, u8 * TxPower);
extern void Hal_SetOFDMTxPower(PADAPTER pAdapter, u8 * TxPower);
extern u8 Hal_ReadRFThermalMeter(PADAPTER pAdapter);
extern void Hal_SetCCKContinuousTx(PADAPTER pAdapter, u8 bStart);
extern void Hal_SetOFDMContinuousTx(PADAPTER pAdapter, u8 bStart);
extern void Hal_ProSetCrystalCap (PADAPTER pAdapter , u32 CrystalCapVal);
//extern void _rtw_mp_xmit_priv(struct xmit_priv *pxmitpriv);
extern void MP_PHY_SetRFPathSwitch(PADAPTER pAdapter ,BOOLEAN bMain);
extern ULONG mpt_ProQueryCalTxPower(PADAPTER	pAdapter,u8 RfPath);
extern void MPT_PwrCtlDM(PADAPTER padapter, u32 bstart);
extern u8 MptToMgntRate(u32	MptRateIdx);
extern void indicate_wx_scan_complete_event(_adapter *padapter);
extern void rtw_indicate_wx_assoc_event(_adapter *padapter);
extern void rtw_indicate_wx_disassoc_event(_adapter *padapter);


#endif //_RTW_MP_H_
