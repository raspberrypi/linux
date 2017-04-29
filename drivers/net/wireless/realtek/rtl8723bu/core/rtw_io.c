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
/*

The purpose of rtw_io.c

a. provides the API

b. provides the protocol engine

c. provides the software interface between caller and the hardware interface


Compiler Flag Option:

2. CONFIG_USB_HCI:
   a. USE_ASYNC_IRP: Both sync/async operations are provided.

3. CONFIG_CFIO_HCI:
   b. USE_SYNC_IRP: Only sync operations are provided.


Only sync read/rtw_write_mem operations are provided.

jackson@realtek.com.tw

*/

#define _RTW_IO_C_

#include <drv_types.h>

u8 rtw_read8(_adapter *adapter, u32 addr)
{
	u8 r_val;
	//struct	io_queue	*pio_queue = (struct io_queue *)adapter->pio_queue;
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl *pintfhdl = &(pio_priv->intf);

	r_val = pintfhdl->io_ops._read8(pintfhdl, addr);
	return r_val;
}

u16 rtw_read16(_adapter *adapter, u32 addr)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl *pintfhdl = &(pio_priv->intf);

	return pintfhdl->io_ops._read16(pintfhdl, addr);
}

u32 rtw_read32(_adapter *adapter, u32 addr)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl *pintfhdl = &(pio_priv->intf);

	return pintfhdl->io_ops._read32(pintfhdl, addr);
}

int rtw_write8(_adapter *adapter, u32 addr, u8 val)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl *pintfhdl = &(pio_priv->intf);
	int ret;

	ret = pintfhdl->io_ops._write8(pintfhdl, addr, val);
	return RTW_STATUS_CODE(ret);
}

int rtw_write16(_adapter *adapter, u32 addr, u16 val)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl		*pintfhdl = &(pio_priv->intf);
	int ret;

	ret = pintfhdl->io_ops._write16(pintfhdl, addr, val);
	return RTW_STATUS_CODE(ret);
}

int rtw_write32(_adapter *adapter, u32 addr, u32 val)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl		*pintfhdl = &(pio_priv->intf);
	int ret;

	ret = pintfhdl->io_ops._write32(pintfhdl, addr, val);
	return RTW_STATUS_CODE(ret);
}

int rtw_writeN(_adapter *adapter, u32 addr ,u32 length , u8 *pdata)
{
	struct io_priv *pio_priv = &adapter->iopriv;
        struct	intf_hdl	*pintfhdl = (struct intf_hdl*)(&(pio_priv->intf));
	int ret;

	ret = pintfhdl->io_ops._writeN(pintfhdl, addr,length,pdata);
	return RTW_STATUS_CODE(ret);
}

int rtw_write8_async(_adapter *adapter, u32 addr, u8 val)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl		*pintfhdl = &(pio_priv->intf);
	int ret;

	ret = pintfhdl->io_ops._write8_async(pintfhdl, addr, val);
	return RTW_STATUS_CODE(ret);
}

int rtw_write16_async(_adapter *adapter, u32 addr, u16 val)
{
	//struct	io_queue	*pio_queue = (struct io_queue *)adapter->pio_queue;
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl		*pintfhdl = &(pio_priv->intf);
	int ret;

	ret = pintfhdl->io_ops._write16_async(pintfhdl, addr, val);
	return RTW_STATUS_CODE(ret);
}

int rtw_write32_async(_adapter *adapter, u32 addr, u32 val)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl		*pintfhdl = &(pio_priv->intf);
	int ret;

	ret = pintfhdl->io_ops._write32_async(pintfhdl, addr, val);
	return RTW_STATUS_CODE(ret);
}

void rtw_read_mem(_adapter *adapter, u32 addr, u32 cnt, u8 *pmem)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl		*pintfhdl = &(pio_priv->intf);

	if ((adapter->bDriverStopped ==_TRUE) ||
	    (adapter->bSurpriseRemoved == _TRUE)) {
		RT_TRACE(_module_rtl871x_io_c_, _drv_info_,
			 ("rtw_read_mem:bDriverStopped(%d) OR bSurpriseRemoved(%d)",
			 adapter->bDriverStopped, adapter->bSurpriseRemoved));
		return;
	}
	pintfhdl->io_ops._read_mem(pintfhdl, addr, cnt, pmem);
}

void rtw_write_mem(_adapter *adapter, u32 addr, u32 cnt, u8 *pmem)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl		*pintfhdl = &(pio_priv->intf);

	pintfhdl->io_ops._write_mem(pintfhdl, addr, cnt, pmem);
}

void rtw_read_port(_adapter *adapter, u32 addr, u32 cnt, u8 *pmem)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl		*pintfhdl = &(pio_priv->intf);

	if ((adapter->bDriverStopped ==_TRUE) ||
	    (adapter->bSurpriseRemoved == _TRUE)) {
		RT_TRACE(_module_rtl871x_io_c_, _drv_info_,
			 ("rtw_read_port:bDriverStopped(%d) OR bSurpriseRemoved(%d)",
			 adapter->bDriverStopped, adapter->bSurpriseRemoved));
		return;
	}

	pintfhdl->io_ops._read_port(pintfhdl, addr, cnt, pmem);
}

void rtw_read_port_cancel(_adapter *adapter)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct intf_hdl *pintfhdl = &(pio_priv->intf);

	RTW_DISABLE_FUNC(adapter, DF_RX_BIT);

	if(pintfhdl->io_ops._read_port_cancel)
		pintfhdl->io_ops._read_port_cancel(pintfhdl);
}

u32 rtw_write_port(_adapter *adapter, u32 addr, u32 cnt, u8 *pmem)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct	intf_hdl		*pintfhdl = &(pio_priv->intf);

	return pintfhdl->io_ops._write_port(pintfhdl, addr, cnt, pmem);
}

u32 rtw_write_port_and_wait(_adapter *adapter, u32 addr, u32 cnt, u8 *pmem, int timeout_ms)
{
	int ret = _SUCCESS;
	struct xmit_buf *pxmitbuf = (struct xmit_buf *)pmem;
	struct submit_ctx sctx;

	rtw_sctx_init(&sctx, timeout_ms);
	pxmitbuf->sctx = &sctx;

	ret = rtw_write_port(adapter, addr, cnt, pmem);

	if (ret == _SUCCESS)
		ret = rtw_sctx_wait(&sctx, __func__);

	return ret;
}

void rtw_write_port_cancel(_adapter *adapter)
{
	struct io_priv *pio_priv = &adapter->iopriv;
	struct intf_hdl *pintfhdl = &(pio_priv->intf);

	RTW_DISABLE_FUNC(adapter, DF_TX_BIT);

	if(pintfhdl->io_ops._write_port_cancel)
		pintfhdl->io_ops._write_port_cancel(pintfhdl);
}

int rtw_init_io_priv(_adapter *padapter, void (*set_intf_ops)(_adapter *padapter,struct _io_ops *pops))
{
	struct io_priv	*piopriv = &padapter->iopriv;
	struct intf_hdl *pintf = &piopriv->intf;

	if (set_intf_ops == NULL)
		return _FAIL;

	piopriv->padapter = padapter;
	pintf->padapter = padapter;
	pintf->pintf_dev = adapter_to_dvobj(padapter);

	set_intf_ops(padapter,&pintf->io_ops);

	return _SUCCESS;
}

/*
* Increase and check if the continual_io_error of this @param dvobjprive is larger than MAX_CONTINUAL_IO_ERR
* @return _TRUE:
* @return _FALSE:
*/
int rtw_inc_and_chk_continual_io_error(struct dvobj_priv *dvobj)
{
	int ret = _FALSE;
	int value;

	if( (value=ATOMIC_INC_RETURN(&dvobj->continual_io_error)) > MAX_CONTINUAL_IO_ERR) {
		DBG_871X("[dvobj:%p][ERROR] continual_io_error:%d > %d\n", dvobj, value, MAX_CONTINUAL_IO_ERR);
		ret = _TRUE;
	}
	return ret;
}

/*
* Set the continual_io_error of this @param dvobjprive to 0
*/
void rtw_reset_continual_io_error(struct dvobj_priv *dvobj)
{
	ATOMIC_SET(&dvobj->continual_io_error, 0);
}

#ifdef DBG_IO

u16 read_sniff_ranges[][2] = {
	//{0x520, 0x523},
};

u16 write_sniff_ranges[][2] = {
	//{0x520, 0x523},
	//{0x4c, 0x4c},
};

int read_sniff_num = sizeof(read_sniff_ranges)/sizeof(u16)/2;
int write_sniff_num = sizeof(write_sniff_ranges)/sizeof(u16)/2;

bool match_read_sniff_ranges(u16 addr, u16 len)
{
	int i;
	for (i = 0; i<read_sniff_num; i++) {
		if (addr + len > read_sniff_ranges[i][0] && addr <= read_sniff_ranges[i][1])
			return _TRUE;
	}

	return _FALSE;
}

bool match_write_sniff_ranges(u16 addr, u16 len)
{
	int i;
	for (i = 0; i<write_sniff_num; i++) {
		if (addr + len > write_sniff_ranges[i][0] && addr <= write_sniff_ranges[i][1])
			return _TRUE;
	}

	return _FALSE;
}

#endif
