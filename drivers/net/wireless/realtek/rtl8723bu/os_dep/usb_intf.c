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
#define _HCI_INTF_C_

#include <drv_types.h>
#include <hal_data.h>
#include <platform_ops.h>

#ifdef CONFIG_80211N_HT
extern int rtw_ht_enable;
#endif

#ifdef CONFIG_GLOBAL_UI_PID
int ui_pid[3] = {0, 0, 0};
#endif


static int rtw_suspend(struct usb_interface *intf, pm_message_t message);
static int rtw_resume(struct usb_interface *intf);


static int rtw_drv_init(struct usb_interface *pusb_intf,const struct usb_device_id *pdid);
static void rtw_dev_remove(struct usb_interface *pusb_intf);

static void rtw_dev_shutdown(struct device *dev)
{
	struct usb_interface *usb_intf = container_of(dev, struct usb_interface, dev);
	struct dvobj_priv *dvobj = NULL;
	_adapter *adapter = NULL;
	int i;

	DBG_871X("%s\n", __func__);

	if(usb_intf)
	{
		dvobj = usb_get_intfdata(usb_intf);
		if (dvobj)
		{
			for (i = 0; i<dvobj->iface_nums; i++)
			{
				adapter = dvobj->padapters[i];
				if (adapter)
				{
					adapter->bSurpriseRemoved = _TRUE;
				}
			}

			ATOMIC_SET(&dvobj->continual_io_error, MAX_CONTINUAL_IO_ERR+1);
		}
	}
}

#if (LINUX_VERSION_CODE<=KERNEL_VERSION(2,6,23))
/* Some useful macros to use to create struct usb_device_id */
 #define USB_DEVICE_ID_MATCH_VENDOR			 0x0001
 #define USB_DEVICE_ID_MATCH_PRODUCT			 0x0002
 #define USB_DEVICE_ID_MATCH_DEV_LO			 0x0004
 #define USB_DEVICE_ID_MATCH_DEV_HI			 0x0008
 #define USB_DEVICE_ID_MATCH_DEV_CLASS			 0x0010
 #define USB_DEVICE_ID_MATCH_DEV_SUBCLASS		 0x0020
 #define USB_DEVICE_ID_MATCH_DEV_PROTOCOL		 0x0040
 #define USB_DEVICE_ID_MATCH_INT_CLASS			 0x0080
 #define USB_DEVICE_ID_MATCH_INT_SUBCLASS		 0x0100
 #define USB_DEVICE_ID_MATCH_INT_PROTOCOL		 0x0200
 #define USB_DEVICE_ID_MATCH_INT_NUMBER			 0x0400


#define USB_DEVICE_ID_MATCH_INT_INFO \
				 (USB_DEVICE_ID_MATCH_INT_CLASS | \
				 USB_DEVICE_ID_MATCH_INT_SUBCLASS | \
				 USB_DEVICE_ID_MATCH_INT_PROTOCOL)


#define USB_DEVICE_AND_INTERFACE_INFO(vend, prod, cl, sc, pr) \
		 .match_flags = USB_DEVICE_ID_MATCH_INT_INFO \
				 | USB_DEVICE_ID_MATCH_DEVICE, \
		 .idVendor = (vend), \
		 .idProduct = (prod), \
		 .bInterfaceClass = (cl), \
		 .bInterfaceSubClass = (sc), \
		 .bInterfaceProtocol = (pr)

 /**
  * USB_VENDOR_AND_INTERFACE_INFO - describe a specific usb vendor with a class of usb interfaces
  * @vend: the 16 bit USB Vendor ID
  * @cl: bInterfaceClass value
  * @sc: bInterfaceSubClass value
  * @pr: bInterfaceProtocol value
  *
  * This macro is used to create a struct usb_device_id that matches a
  * specific vendor with a specific class of interfaces.
  *
  * This is especially useful when explicitly matching devices that have
  * vendor specific bDeviceClass values, but standards-compliant interfaces.
  */
#define USB_VENDOR_AND_INTERFACE_INFO(vend, cl, sc, pr) \
		 .match_flags = USB_DEVICE_ID_MATCH_INT_INFO \
				 | USB_DEVICE_ID_MATCH_VENDOR, \
		 .idVendor = (vend), \
		 .bInterfaceClass = (cl), \
		 .bInterfaceSubClass = (sc), \
		 .bInterfaceProtocol = (pr)

/* ----------------------------------------------------------------------- */
#endif


#define USB_VENDER_ID_REALTEK		0x0BDA


/* DID_USB_v916_20130116 */
static struct usb_device_id rtw_usb_id_tbl[] ={

	//*=== Realtek demoboard ===*/
	{USB_DEVICE_AND_INTERFACE_INFO(USB_VENDER_ID_REALTEK, 0xB720,0xff,0xff,0xff),.driver_info = RTL8723B}, /* 8723BU 1*1 */
	{USB_DEVICE_AND_INTERFACE_INFO(0x7392, 0xa611, 0xff, 0xff, 0xff), .driver_info = RTL8723B}, /* 8723BU 1*1 */
	{}	/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, rtw_usb_id_tbl);

static int const rtw_usb_id_len = sizeof(rtw_usb_id_tbl) / sizeof(struct usb_device_id);

static struct specific_device_id specific_device_id_tbl[] = {
	{.idVendor=USB_VENDER_ID_REALTEK, .idProduct=0x8177, .flags=SPEC_DEV_ID_DISABLE_HT},//8188cu 1*1 dongole, (b/g mode only)
	{.idVendor=USB_VENDER_ID_REALTEK, .idProduct=0x817E, .flags=SPEC_DEV_ID_DISABLE_HT},//8188CE-VAU USB minCard (b/g mode only)
	{.idVendor=0x0b05, .idProduct=0x1791, .flags=SPEC_DEV_ID_DISABLE_HT},
	{.idVendor=0x13D3, .idProduct=0x3311, .flags=SPEC_DEV_ID_DISABLE_HT},
	{.idVendor=0x13D3, .idProduct=0x3359, .flags=SPEC_DEV_ID_DISABLE_HT},//Russian customer -Azwave (8188CE-VAU  g mode)
#ifdef RTK_DMP_PLATFORM
	{.idVendor=USB_VENDER_ID_REALTEK, .idProduct=0x8111, .flags=SPEC_DEV_ID_ASSIGN_IFNAME}, // Realtek 5G dongle for WiFi Display
	{.idVendor=0x2019, .idProduct=0xAB2D, .flags=SPEC_DEV_ID_ASSIGN_IFNAME}, // PCI-Abocom 5G dongle for WiFi Display
#endif /* RTK_DMP_PLATFORM */
	{}
};

struct rtw_usb_drv {
	struct usb_driver usbdrv;
	int drv_registered;
	u8 hw_type;
};

static struct rtw_usb_drv usb_drv = {
	.usbdrv.name =(char*)DRV_NAME,
	.usbdrv.probe = rtw_drv_init,
	.usbdrv.disconnect = rtw_dev_remove,
	.usbdrv.id_table = rtw_usb_id_tbl,
	.usbdrv.suspend =  rtw_suspend,
	.usbdrv.resume = rtw_resume,
	#if (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 22))
	.usbdrv.reset_resume   = rtw_resume,
	#endif
	#ifdef CONFIG_AUTOSUSPEND
	.usbdrv.supports_autosuspend = 1,
	#endif

	#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 19))
	.usbdrv.drvwrap.driver.shutdown = rtw_dev_shutdown,
	#else
	.usbdrv.driver.shutdown = rtw_dev_shutdown,
	#endif
};

static inline int RT_usb_endpoint_dir_in(const struct usb_endpoint_descriptor *epd)
{
	return ((epd->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN);
}

static inline int RT_usb_endpoint_dir_out(const struct usb_endpoint_descriptor *epd)
{
	return ((epd->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT);
}

static inline int RT_usb_endpoint_xfer_int(const struct usb_endpoint_descriptor *epd)
{
	return ((epd->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT);
}

static inline int RT_usb_endpoint_xfer_bulk(const struct usb_endpoint_descriptor *epd)
{
	return ((epd->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK);
}

static inline int RT_usb_endpoint_is_bulk_in(const struct usb_endpoint_descriptor *epd)
{
	return (RT_usb_endpoint_xfer_bulk(epd) && RT_usb_endpoint_dir_in(epd));
}

static inline int RT_usb_endpoint_is_bulk_out(const struct usb_endpoint_descriptor *epd)
{
	return (RT_usb_endpoint_xfer_bulk(epd) && RT_usb_endpoint_dir_out(epd));
}

static inline int RT_usb_endpoint_is_int_in(const struct usb_endpoint_descriptor *epd)
{
	return (RT_usb_endpoint_xfer_int(epd) && RT_usb_endpoint_dir_in(epd));
}

static inline int RT_usb_endpoint_num(const struct usb_endpoint_descriptor *epd)
{
	return epd->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
}

static u8 rtw_init_intf_priv(struct dvobj_priv *dvobj)
{
	u8 rst = _SUCCESS;

	#ifdef CONFIG_USB_VENDOR_REQ_MUTEX
	_rtw_mutex_init(&dvobj->usb_vendor_req_mutex);
	#endif


	#ifdef CONFIG_USB_VENDOR_REQ_BUFFER_PREALLOC
	dvobj->usb_alloc_vendor_req_buf = rtw_zmalloc(MAX_USB_IO_CTL_SIZE);
	if (dvobj->usb_alloc_vendor_req_buf == NULL) {
		DBG_871X("alloc usb_vendor_req_buf failed... /n");
		rst = _FAIL;
		goto exit;
	}
	dvobj->usb_vendor_req_buf  =
		(u8 *)N_BYTE_ALIGMENT((SIZE_PTR)(dvobj->usb_alloc_vendor_req_buf ), ALIGNMENT_UNIT);
exit:
	#endif

	return rst;

}

static u8 rtw_deinit_intf_priv(struct dvobj_priv *dvobj)
{
	u8 rst = _SUCCESS;

	#ifdef CONFIG_USB_VENDOR_REQ_BUFFER_PREALLOC
	if(dvobj->usb_vendor_req_buf)
		rtw_mfree(dvobj->usb_alloc_vendor_req_buf, MAX_USB_IO_CTL_SIZE);
	#endif

	#ifdef CONFIG_USB_VENDOR_REQ_MUTEX
	_rtw_mutex_free(&dvobj->usb_vendor_req_mutex);
	#endif

	return rst;
}

static struct dvobj_priv *usb_dvobj_init(struct usb_interface *usb_intf)
{
	int	i;
	u8	val8;
	int	status = _FAIL;
	struct dvobj_priv *pdvobjpriv;
	struct usb_device_descriptor	*pdev_desc;
	struct usb_host_config			*phost_conf;
	struct usb_config_descriptor		*pconf_desc;
	struct usb_host_interface		*phost_iface;
	struct usb_interface_descriptor	*piface_desc;
	struct usb_host_endpoint		*phost_endp;
	struct usb_endpoint_descriptor	*pendp_desc;
	struct usb_device				*pusbd;




	if((pdvobjpriv = devobj_init()) == NULL) {
		goto exit;
	}


	pdvobjpriv->pusbintf = usb_intf ;
	pusbd = pdvobjpriv->pusbdev = interface_to_usbdev(usb_intf);
	usb_set_intfdata(usb_intf, pdvobjpriv);

	pdvobjpriv->RtNumInPipes = 0;
	pdvobjpriv->RtNumOutPipes = 0;

	pdev_desc = &pusbd->descriptor;

	phost_conf = pusbd->actconfig;
	pconf_desc = &phost_conf->desc;

	phost_iface = &usb_intf->altsetting[0];
	piface_desc = &phost_iface->desc;

	pdvobjpriv->NumInterfaces = pconf_desc->bNumInterfaces;
	pdvobjpriv->InterfaceNumber = piface_desc->bInterfaceNumber;
	pdvobjpriv->nr_endpoint = piface_desc->bNumEndpoints;

	for (i = 0; i < pdvobjpriv->nr_endpoint; i++)
	{
		phost_endp = phost_iface->endpoint + i;
		if (phost_endp)
		{
			pendp_desc = &phost_endp->desc;

			DBG_871X("\nusb_endpoint_descriptor(%d):\n", i);
			DBG_871X("bLength=%x\n",pendp_desc->bLength);
			DBG_871X("bDescriptorType=%x\n",pendp_desc->bDescriptorType);
			DBG_871X("bEndpointAddress=%x\n",pendp_desc->bEndpointAddress);
			DBG_871X("wMaxPacketSize=%d\n",le16_to_cpu(pendp_desc->wMaxPacketSize));
			DBG_871X("bInterval=%x\n",pendp_desc->bInterval);

			if (RT_usb_endpoint_is_bulk_in(pendp_desc))
			{
				DBG_871X("RT_usb_endpoint_is_bulk_in = %x\n", RT_usb_endpoint_num(pendp_desc));
				pdvobjpriv->RtInPipe[pdvobjpriv->RtNumInPipes] = RT_usb_endpoint_num(pendp_desc);
				pdvobjpriv->RtNumInPipes++;
			}
			else if (RT_usb_endpoint_is_int_in(pendp_desc))
			{
				DBG_871X("RT_usb_endpoint_is_int_in = %x, Interval = %x\n", RT_usb_endpoint_num(pendp_desc),pendp_desc->bInterval);
				pdvobjpriv->RtInPipe[pdvobjpriv->RtNumInPipes] = RT_usb_endpoint_num(pendp_desc);
				pdvobjpriv->RtNumInPipes++;
			}
			else if (RT_usb_endpoint_is_bulk_out(pendp_desc))
			{
				DBG_871X("RT_usb_endpoint_is_bulk_out = %x\n", RT_usb_endpoint_num(pendp_desc));
				pdvobjpriv->RtOutPipe[pdvobjpriv->RtNumOutPipes] = RT_usb_endpoint_num(pendp_desc);
				pdvobjpriv->RtNumOutPipes++;
			}
			pdvobjpriv->ep_num[i] = RT_usb_endpoint_num(pendp_desc);
		}
	}

	DBG_871X("nr_endpoint=%d, in_num=%d, out_num=%d\n\n", pdvobjpriv->nr_endpoint, pdvobjpriv->RtNumInPipes, pdvobjpriv->RtNumOutPipes);

	switch(pusbd->speed) {
		case USB_SPEED_LOW:
			DBG_871X("USB_SPEED_LOW\n");
			pdvobjpriv->usb_speed = RTW_USB_SPEED_1_1;
			break;
		case USB_SPEED_FULL:
			DBG_871X("USB_SPEED_FULL\n");
			pdvobjpriv->usb_speed = RTW_USB_SPEED_1_1;
			break;
		case USB_SPEED_HIGH:
			DBG_871X("USB_SPEED_HIGH\n");
			pdvobjpriv->usb_speed = RTW_USB_SPEED_2;
			break;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,31))
		case USB_SPEED_SUPER:
			DBG_871X("USB_SPEED_SUPER\n");
			pdvobjpriv->usb_speed = RTW_USB_SPEED_3;
			break;
#endif
		default:
			DBG_871X("USB_SPEED_UNKNOWN(%x)\n",pusbd->speed);
			pdvobjpriv->usb_speed = RTW_USB_SPEED_UNKNOWN;
			break;
	}

	if (pdvobjpriv->usb_speed == RTW_USB_SPEED_UNKNOWN) {
		DBG_871X("UNKNOWN USB SPEED MODE, ERROR !!!\n");
		goto free_dvobj;
	}

	if (rtw_init_intf_priv(pdvobjpriv) == _FAIL) {
		RT_TRACE(_module_os_intfs_c_,_drv_err_,("\n Can't INIT rtw_init_intf_priv\n"));
		goto free_dvobj;
	}

	//.3 misc
	_rtw_init_sema(&(pdvobjpriv->usb_suspend_sema), 0);
	rtw_reset_continual_io_error(pdvobjpriv);

	usb_get_dev(pusbd);

	status = _SUCCESS;

free_dvobj:
	if (status != _SUCCESS && pdvobjpriv) {
		usb_set_intfdata(usb_intf, NULL);

		devobj_deinit(pdvobjpriv);

		pdvobjpriv = NULL;
	}
exit:

	return pdvobjpriv;
}

static void usb_dvobj_deinit(struct usb_interface *usb_intf)
{
	struct dvobj_priv *dvobj = usb_get_intfdata(usb_intf);



	usb_set_intfdata(usb_intf, NULL);
	if (dvobj) {
		//Modify condition for 92DU DMDP 2010.11.18, by Thomas
		if ((dvobj->NumInterfaces != 2 && dvobj->NumInterfaces != 3)
			|| (dvobj->InterfaceNumber == 1)) {
			if (interface_to_usbdev(usb_intf)->state != USB_STATE_NOTATTACHED) {
				//If we didn't unplug usb dongle and remove/insert modlue, driver fails on sitesurvey for the first time when device is up .
				//Reset usb port for sitesurvey fail issue. 2009.8.13, by Thomas
				DBG_871X("usb attached..., try to reset usb device\n");
				usb_reset_device(interface_to_usbdev(usb_intf));
			}
		}

		rtw_deinit_intf_priv(dvobj);

		devobj_deinit(dvobj);
	}

	//DBG_871X("%s %d\n", __func__, ATOMIC_READ(&usb_intf->dev.kobj.kref.refcount));
	usb_put_dev(interface_to_usbdev(usb_intf));


}

static void rtw_decide_chip_type_by_usb_info(_adapter *padapter, const struct usb_device_id *pdid)
{
	padapter->chip_type = pdid->driver_info;

	if(padapter->chip_type == RTL8723B)
		rtl8723bu_set_hw_type(padapter);
}

void rtw_set_hal_ops(_adapter *padapter)
{
	//alloc memory for HAL DATA
	rtw_hal_data_init(padapter);

	if(padapter->chip_type == RTL8723B)
		rtl8723bu_set_hal_ops(padapter);
}

static void usb_set_intf_ops(_adapter *padapter,struct _io_ops *pops)
{
	if(padapter->chip_type == RTL8723B)
		rtl8723bu_set_intf_ops(pops);
}


static void usb_intf_start(_adapter *padapter)
{

	RT_TRACE(_module_hci_intfs_c_,_drv_err_,("+usb_intf_start\n"));

	rtw_hal_inirp_init(padapter);

	RT_TRACE(_module_hci_intfs_c_,_drv_err_,("-usb_intf_start\n"));

}

static void usb_intf_stop(_adapter *padapter)
{

	RT_TRACE(_module_hci_intfs_c_,_drv_err_,("+usb_intf_stop\n"));

	//disabel_hw_interrupt
	if(padapter->bSurpriseRemoved == _FALSE)
	{
		//device still exists, so driver can do i/o operation
		//TODO:
		RT_TRACE(_module_hci_intfs_c_,_drv_err_,("SurpriseRemoved==_FALSE\n"));
	}

	//cancel in irp
	rtw_hal_inirp_deinit(padapter);

	//cancel out irp
	rtw_write_port_cancel(padapter);

	//todo:cancel other irps

	RT_TRACE(_module_hci_intfs_c_,_drv_err_,("-usb_intf_stop\n"));

}

static void process_spec_devid(const struct usb_device_id *pdid)
{
	u16 vid, pid;
	u32 flags;
	int i;
	int num = sizeof(specific_device_id_tbl)/sizeof(struct specific_device_id);

	for(i=0; i<num; i++)
	{
		vid = specific_device_id_tbl[i].idVendor;
		pid = specific_device_id_tbl[i].idProduct;
		flags = specific_device_id_tbl[i].flags;

#ifdef CONFIG_80211N_HT
		if((pdid->idVendor==vid) && (pdid->idProduct==pid) && (flags&SPEC_DEV_ID_DISABLE_HT))
		{
			 rtw_ht_enable = 0;
			 rtw_bw_mode = 0;
			 rtw_ampdu_enable = 0;
		}
#endif

#ifdef RTK_DMP_PLATFORM
		// Change the ifname to wlan10 when PC side WFD dongle plugin on DMP platform.
		// It is used to distinguish between normal and PC-side wifi dongle/module.
		if((pdid->idVendor==vid) && (pdid->idProduct==pid) && (flags&SPEC_DEV_ID_ASSIGN_IFNAME))
		{
			extern char* ifname;
			strncpy(ifname, "wlan10", 6);
			//DBG_871X("%s()-%d: ifname=%s, vid=%04X, pid=%04X\n", __FUNCTION__, __LINE__, ifname, vid, pid);
		}
#endif /* RTK_DMP_PLATFORM */

	}
}

#ifdef SUPPORT_HW_RFOFF_DETECTED
int rtw_hw_suspend(_adapter *padapter )
{
	struct pwrctrl_priv *pwrpriv;
	struct usb_interface *pusb_intf;
	struct net_device *pnetdev;


	if(NULL==padapter)
		goto error_exit;

	if((_FALSE==padapter->bup) || (_TRUE == padapter->bDriverStopped)||(_TRUE==padapter->bSurpriseRemoved))
	{
		DBG_871X("padapter->bup=%d bDriverStopped=%d bSurpriseRemoved = %d\n",
			padapter->bup, padapter->bDriverStopped,padapter->bSurpriseRemoved);
		goto error_exit;
	}

	pwrpriv = adapter_to_pwrctl(padapter);
	pusb_intf = adapter_to_dvobj(padapter)->pusbintf;
	pnetdev = padapter->pnetdev;

	LeaveAllPowerSaveMode(padapter);

	DBG_871X("==> rtw_hw_suspend\n");
	_enter_pwrlock(&pwrpriv->lock);
	pwrpriv->bips_processing = _TRUE;
	//padapter->net_closed = _TRUE;
	//s1.
	if(pnetdev)
	{
		netif_carrier_off(pnetdev);
		rtw_netif_stop_queue(pnetdev);
	}

	//s2.
	rtw_disassoc_cmd(padapter, 500, _FALSE);

	//s2-2.  indicate disconnect to os
	//rtw_indicate_disconnect(padapter);
	{
		struct	mlme_priv *pmlmepriv = &padapter->mlmepriv;
		if(check_fwstate(pmlmepriv, _FW_LINKED))
		{
			_clr_fwstate_(pmlmepriv, _FW_LINKED);
			rtw_led_control(padapter, LED_CTL_NO_LINK);

			rtw_os_indicate_disconnect(padapter);

			#ifdef CONFIG_LPS
			//donnot enqueue cmd
			rtw_lps_ctrl_wk_cmd(padapter, LPS_CTRL_DISCONNECT, 0);
			#endif
		}
	}
	//s2-3.
	rtw_free_assoc_resources(padapter, 1);

	//s2-4.
	rtw_free_network_queue(padapter,_TRUE);
	#ifdef CONFIG_IPS
	rtw_ips_dev_unload(padapter);
	#endif
	pwrpriv->rf_pwrstate = rf_off;
	pwrpriv->bips_processing = _FALSE;
	_exit_pwrlock(&pwrpriv->lock);


	return 0;

error_exit:
	DBG_871X("%s, failed \n",__FUNCTION__);
	return (-1);

}

int rtw_hw_resume(_adapter *padapter)
{
	struct pwrctrl_priv *pwrpriv = adapter_to_pwrctl(padapter);
	struct usb_interface *pusb_intf = adapter_to_dvobj(padapter)->pusbintf;
	struct net_device *pnetdev = padapter->pnetdev;


	DBG_871X("==> rtw_hw_resume\n");
	_enter_pwrlock(&pwrpriv->lock);
	pwrpriv->bips_processing = _TRUE;
	rtw_reset_drv_sw(padapter);

	if(pm_netdev_open(pnetdev,_FALSE) != 0)
	{
		_exit_pwrlock(&pwrpriv->lock);
		goto error_exit;
	}

	netif_device_attach(pnetdev);
	netif_carrier_on(pnetdev);

	if(!rtw_netif_queue_stopped(pnetdev))
		rtw_netif_start_queue(pnetdev);
	else
		rtw_netif_wake_queue(pnetdev);

	pwrpriv->bkeepfwalive = _FALSE;
	pwrpriv->brfoffbyhw = _FALSE;

	pwrpriv->rf_pwrstate = rf_on;
	pwrpriv->bips_processing = _FALSE;
	_exit_pwrlock(&pwrpriv->lock);



	return 0;
error_exit:
	DBG_871X("%s, Open net dev failed \n",__FUNCTION__);
	return (-1);
}
#endif

static int rtw_suspend(struct usb_interface *pusb_intf, pm_message_t message)
{
	struct dvobj_priv *dvobj;
	struct pwrctrl_priv *pwrpriv;
	struct debug_priv *pdbgpriv;
	PADAPTER padapter;
	int ret = 0;


	dvobj = usb_get_intfdata(pusb_intf);
	pwrpriv = dvobj_to_pwrctl(dvobj);
	pdbgpriv = &dvobj->drv_dbg;
	padapter = dvobj->if1;

	if (pwrpriv->bInSuspend == _TRUE) {
		DBG_871X("%s bInSuspend = %d\n", __FUNCTION__, pwrpriv->bInSuspend);
		pdbgpriv->dbg_suspend_error_cnt++;
		goto exit;
	}

	if((padapter->bup) || (padapter->bDriverStopped == _FALSE)||(padapter->bSurpriseRemoved == _FALSE))
	{
#ifdef CONFIG_AUTOSUSPEND
		if(pwrpriv->bInternalAutoSuspend ){

			#ifdef SUPPORT_HW_RFOFF_DETECTED
			// The FW command register update must after MAC and FW init ready.
			if((padapter->bFWReady) && (pwrpriv->bHWPwrPindetect ) && (padapter->registrypriv.usbss_enable ))
			{
				u8 bOpen = _TRUE;
				rtw_interface_ps_func(padapter,HAL_USB_SELECT_SUSPEND,&bOpen);
				//rtl8192c_set_FwSelectSuspend_cmd(padapter,_TRUE ,500);//note fw to support hw power down ping detect
			}
			#endif//SUPPORT_HW_RFOFF_DETECTED
		}
#endif//CONFIG_AUTOSUSPEND
	}

	ret =  rtw_suspend_common(padapter);

exit:
	return ret;
}

int rtw_resume_process(_adapter *padapter)
{
	int ret,pm_cnt = 0;
	struct pwrctrl_priv *pwrpriv = adapter_to_pwrctl(padapter);
	struct dvobj_priv *pdvobj = padapter->dvobj;
	struct debug_priv *pdbgpriv = &pdvobj->drv_dbg;


	if (pwrpriv->bInSuspend == _FALSE)
	{
		pdbgpriv->dbg_resume_error_cnt++;
		DBG_871X("%s bInSuspend = %d\n", __FUNCTION__, pwrpriv->bInSuspend);
		return -1;
	}

#if defined(CONFIG_BT_COEXIST) && defined(CONFIG_AUTOSUSPEND) //add by amy for 8723as-vau
#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,32))
	DBG_871X("%s...pm_usage_cnt(%d)  pwrpriv->bAutoResume=%x.  ....\n",__func__,atomic_read(&(adapter_to_dvobj(padapter)->pusbintf->pm_usage_cnt)),pwrpriv->bAutoResume);
	pm_cnt=atomic_read(&(adapter_to_dvobj(padapter)->pusbintf->pm_usage_cnt));
#else // kernel < 2.6.32
	DBG_871X("...pm_usage_cnt(%d).....\n", adapter_to_dvobj(padapter)->pusbintf->pm_usage_cnt);
	pm_cnt = adapter_to_dvobj(padapter)->pusbintf->pm_usage_cnt;
#endif // kernel < 2.6.32

	DBG_871X("pwrpriv->bAutoResume (%x)\n",pwrpriv->bAutoResume );
	if( _TRUE == pwrpriv->bAutoResume ){
		pwrpriv->bInternalAutoSuspend = _FALSE;
		pwrpriv->bAutoResume=_FALSE;
		DBG_871X("pwrpriv->bAutoResume (%x)  pwrpriv->bInternalAutoSuspend(%x)\n",pwrpriv->bAutoResume,pwrpriv->bInternalAutoSuspend );

	}
#endif //#ifdef CONFIG_BT_COEXIST &CONFIG_AUTOSUSPEND&

#if defined (CONFIG_WOWLAN) || defined (CONFIG_AP_WOWLAN)
	/*
	 * Due to usb wow suspend flow will cancel read/write port via intf_stop and
	 * bReadPortCancel and bWritePortCancel are set _TRUE in intf_stop.
	 * But they will not be clear in intf_start during wow resume flow.
	 * It should move to os_intf in the feature.
	 */
	RTW_ENABLE_FUNC(padapter, DF_RX_BIT);
	RTW_ENABLE_FUNC(padapter, DF_TX_BIT);
#endif

	ret =  rtw_resume_common(padapter);

	#ifdef CONFIG_AUTOSUSPEND
	if(pwrpriv->bInternalAutoSuspend )
	{
		#ifdef SUPPORT_HW_RFOFF_DETECTED
			// The FW command register update must after MAC and FW init ready.
		if((padapter->bFWReady) && (pwrpriv->bHWPwrPindetect) && (padapter->registrypriv.usbss_enable ))
		{
			//rtl8192c_set_FwSelectSuspend_cmd(padapter,_FALSE ,500);//note fw to support hw power down ping detect
			u8 bOpen = _FALSE;
			rtw_interface_ps_func(padapter,HAL_USB_SELECT_SUSPEND,&bOpen);
		}
		#endif
		#ifdef CONFIG_BT_COEXIST // for 8723as-vau
		DBG_871X("pwrpriv->bAutoResume (%x)\n",pwrpriv->bAutoResume );
		if( _TRUE == pwrpriv->bAutoResume ){
		pwrpriv->bInternalAutoSuspend = _FALSE;
			pwrpriv->bAutoResume=_FALSE;
			DBG_871X("pwrpriv->bAutoResume (%x)  pwrpriv->bInternalAutoSuspend(%x)\n",pwrpriv->bAutoResume,pwrpriv->bInternalAutoSuspend );
		}

		#else	//#ifdef CONFIG_BT_COEXIST
		pwrpriv->bInternalAutoSuspend = _FALSE;
		#endif	//#ifdef CONFIG_BT_COEXIST
		pwrpriv->brfoffbyhw = _FALSE;
	}
	#endif//CONFIG_AUTOSUSPEND


	return ret;
}

static int rtw_resume(struct usb_interface *pusb_intf)
{
	struct dvobj_priv *dvobj;
	struct pwrctrl_priv *pwrpriv;
	struct debug_priv *pdbgpriv;
	PADAPTER padapter;
	struct mlme_ext_priv *pmlmeext;
	int ret = 0;


	dvobj = usb_get_intfdata(pusb_intf);
	pwrpriv = dvobj_to_pwrctl(dvobj);
	pdbgpriv = &dvobj->drv_dbg;
	padapter = dvobj->if1;
	pmlmeext = &padapter->mlmeextpriv;

	DBG_871X("==> %s (%s:%d)\n", __FUNCTION__, current->comm, current->pid);
	pdbgpriv->dbg_resume_cnt++;

	if(pwrpriv->bInternalAutoSuspend)
	{
		ret = rtw_resume_process(padapter);
	}
	else
	{
		if(pwrpriv->wowlan_mode || pwrpriv->wowlan_ap_mode)
		{
			rtw_resume_lock_suspend();
			ret = rtw_resume_process(padapter);
			rtw_resume_unlock_suspend();
		}
		else
		{
#ifdef CONFIG_RESUME_IN_WORKQUEUE
			rtw_resume_in_workqueue(pwrpriv);
#else
			if (rtw_is_earlysuspend_registered(pwrpriv))
			{
				/* jeff: bypass resume here, do in late_resume */
				rtw_set_do_late_resume(pwrpriv, _TRUE);
			}
			else
			{
				rtw_resume_lock_suspend();
				ret = rtw_resume_process(padapter);
				rtw_resume_unlock_suspend();
			}
#endif
		}
	}

	pmlmeext->last_scan_time = rtw_get_current_time();
	DBG_871X("<========  %s return %d\n", __FUNCTION__, ret);

	return ret;
}



#ifdef CONFIG_AUTOSUSPEND
void autosuspend_enter(_adapter* padapter)
{
	struct dvobj_priv *dvobj = adapter_to_dvobj(padapter);
	struct pwrctrl_priv *pwrpriv = dvobj_to_pwrctl(dvobj);

	DBG_871X("==>autosuspend_enter...........\n");

	pwrpriv->bInternalAutoSuspend = _TRUE;
	pwrpriv->bips_processing = _TRUE;

	if(rf_off == pwrpriv->change_rfpwrstate )
	{
#ifndef	CONFIG_BT_COEXIST
		#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,35))
		usb_enable_autosuspend(dvobj->pusbdev);
		#else
		dvobj->pusbdev->autosuspend_disabled = 0;//autosuspend disabled by the user
		#endif

		#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,33))
			usb_autopm_put_interface(dvobj->pusbintf);
		#elif (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,20))
			usb_autopm_enable(dvobj->pusbintf);
		#else
			usb_autosuspend_device(dvobj->pusbdev, 1);
		#endif
#else	//#ifndef	CONFIG_BT_COEXIST
		if(1==pwrpriv->autopm_cnt){
		#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,35))
		usb_enable_autosuspend(dvobj->pusbdev);
		#else
		dvobj->pusbdev->autosuspend_disabled = 0;//autosuspend disabled by the user
		#endif

		#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,33))
			usb_autopm_put_interface(dvobj->pusbintf);
		#elif (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,20))
			usb_autopm_enable(dvobj->pusbintf);
		#else
			usb_autosuspend_device(dvobj->pusbdev, 1);
		#endif
			pwrpriv->autopm_cnt --;
		}
		else
		DBG_871X("0!=pwrpriv->autopm_cnt[%d]   didn't usb_autopm_put_interface\n", pwrpriv->autopm_cnt);

#endif	//#ifndef	CONFIG_BT_COEXIST
	}
	#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,32))
	DBG_871X("...pm_usage_cnt(%d).....\n", atomic_read(&(dvobj->pusbintf->pm_usage_cnt)));
	#else
	DBG_871X("...pm_usage_cnt(%d).....\n", dvobj->pusbintf->pm_usage_cnt);
	#endif

}

int autoresume_enter(_adapter* padapter)
{
	int result = _SUCCESS;
	struct security_priv* psecuritypriv=&(padapter->securitypriv);
	struct mlme_ext_priv	*pmlmeext = &padapter->mlmeextpriv;
	struct mlme_ext_info	*pmlmeinfo = &(pmlmeext->mlmext_info);
	struct dvobj_priv *dvobj = adapter_to_dvobj(padapter);
	struct pwrctrl_priv *pwrpriv = dvobj_to_pwrctl(dvobj);

	DBG_871X("====> autoresume_enter \n");

	if(rf_off == pwrpriv->rf_pwrstate )
	{
		pwrpriv->ps_flag = _FALSE;
#ifndef	CONFIG_BT_COEXIST
		#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,33))
			if (usb_autopm_get_interface(dvobj->pusbintf) < 0)
			{
				DBG_871X( "can't get autopm: %d\n", result);
				result = _FAIL;
				goto error_exit;
			}
		#elif (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,20))
			usb_autopm_disable(dvobj->pusbintf);
		#else
			usb_autoresume_device(dvobj->pusbdev, 1);
		#endif

		#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,32))
		DBG_871X("...pm_usage_cnt(%d).....\n", atomic_read(&(dvobj->pusbintf->pm_usage_cnt)));
		#else
		DBG_871X("...pm_usage_cnt(%d).....\n", dvobj->pusbintf->pm_usage_cnt);
		#endif
#else	//#ifndef	CONFIG_BT_COEXIST
		pwrpriv->bAutoResume=_TRUE;
		if(0==pwrpriv->autopm_cnt){
		#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,33))
			if (usb_autopm_get_interface(dvobj->pusbintf) < 0)
			{
				DBG_871X( "can't get autopm: %d\n", result);
				result = _FAIL;
				goto error_exit;
			}
		#elif (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,20))
			usb_autopm_disable(dvobj->pusbintf);
		#else
			usb_autoresume_device(dvobj->pusbdev, 1);
		#endif
		#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,32))
			DBG_871X("...pm_usage_cnt(%d).....\n", atomic_read(&(dvobj->pusbintf->pm_usage_cnt)));
		#else
			DBG_871X("...pm_usage_cnt(%d).....\n", dvobj->pusbintf->pm_usage_cnt);
		#endif
			pwrpriv->autopm_cnt++;
		}
		else
			DBG_871X("0!=pwrpriv->autopm_cnt[%d]   didn't usb_autopm_get_interface\n",pwrpriv->autopm_cnt);
#endif //#ifndef	CONFIG_BT_COEXIST
	}
	DBG_871X("<==== autoresume_enter \n");
error_exit:

	return result;
}
#endif

#ifdef CONFIG_PLATFORM_RTD2880B
extern void rtd2885_wlan_netlink_sendMsg(char *action_string, char *name);
#endif

/*
 * drv_init() - a device potentially for us
 *
 * notes: drv_init() is called when the bus driver has located a card for us to support.
 *        We accept the new device by returning 0.
*/

static _adapter  *rtw_sw_export = NULL;

static _adapter *rtw_usb_if1_init(struct dvobj_priv *dvobj,
	struct usb_interface *pusb_intf, const struct usb_device_id *pdid)
{
	_adapter *padapter = NULL;
	struct net_device *pnetdev = NULL;
	int status = _FAIL;

	if ((padapter = (_adapter *)rtw_zvmalloc(sizeof(*padapter))) == NULL) {
		goto exit;
	}
	padapter->dvobj = dvobj;
	dvobj->if1 = padapter;

	padapter->bDriverStopped=_TRUE;

	dvobj->padapters[dvobj->iface_nums++] = padapter;
	padapter->iface_id = IFACE_ID0;

#if defined(CONFIG_CONCURRENT_MODE)
	//set adapter_type/iface type for primary padapter
	padapter->isprimary = _TRUE;
	padapter->adapter_type = PRIMARY_ADAPTER;
	#ifndef CONFIG_HWPORT_SWAP
	padapter->iface_type = IFACE_PORT0;
	#else
	padapter->iface_type = IFACE_PORT1;
	#endif
#endif

	//step 1-1., decide the chip_type via driver_info
	padapter->interface_type = RTW_USB;
	rtw_decide_chip_type_by_usb_info(padapter, pdid);

	if (rtw_handle_dualmac(padapter, 1) != _SUCCESS)
		goto free_adapter;

	if((pnetdev = rtw_init_netdev(padapter)) == NULL) {
		goto handle_dualmac;
	}
	SET_NETDEV_DEV(pnetdev, dvobj_to_dev(dvobj));
	padapter = rtw_netdev_priv(pnetdev);

#ifdef CONFIG_IOCTL_CFG80211
	if(rtw_wdev_alloc(padapter, dvobj_to_dev(dvobj)) != 0) {
		goto handle_dualmac;
	}
#endif

	//step 2. hook HalFunc, allocate HalData
	//hal_set_hal_ops(padapter);
	rtw_set_hal_ops(padapter);

	padapter->intf_start=&usb_intf_start;
	padapter->intf_stop=&usb_intf_stop;

	//step init_io_priv
	rtw_init_io_priv(padapter,usb_set_intf_ops);

	//step read_chip_version
	rtw_hal_read_chip_version(padapter);

	//step usb endpoint mapping
	rtw_hal_chip_configure(padapter);

#ifdef CONFIG_BT_COEXIST
	rtw_btcoex_Initialize(padapter);
#endif // CONFIG_BT_COEXIST

	//step read efuse/eeprom data and get mac_addr
	rtw_hal_read_chip_info(padapter);

	//step 5.
	if(rtw_init_drv_sw(padapter) ==_FAIL) {
		RT_TRACE(_module_hci_intfs_c_,_drv_err_,("Initialize driver software resource Failed!\n"));
		goto free_hal_data;
	}

#ifdef CONFIG_PM
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,6,18))
	if(dvobj_to_pwrctl(dvobj)->bSupportRemoteWakeup)
	{
		dvobj->pusbdev->do_remote_wakeup=1;
		pusb_intf->needs_remote_wakeup = 1;
		device_init_wakeup(&pusb_intf->dev, 1);
		DBG_871X("pwrctrlpriv.bSupportRemoteWakeup~~~~~~\n");
		DBG_871X("pwrctrlpriv.bSupportRemoteWakeup~~~[%d]~~~\n",device_may_wakeup(&pusb_intf->dev));
	}
#endif
#endif

#ifdef CONFIG_AUTOSUSPEND
	if( padapter->registrypriv.power_mgnt != PS_MODE_ACTIVE )
	{
		if(padapter->registrypriv.usbss_enable ){	/* autosuspend (2s delay) */
			#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,38))
			dvobj->pusbdev->dev.power.autosuspend_delay = 0 * HZ;//15 * HZ; idle-delay time
			#else
			dvobj->pusbdev->autosuspend_delay = 0 * HZ;//15 * HZ; idle-delay time
			#endif

			#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,35))
			usb_enable_autosuspend(dvobj->pusbdev);
			#elif  (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,22) && LINUX_VERSION_CODE<=KERNEL_VERSION(2,6,34))
			padapter->bDisableAutosuspend = dvobj->pusbdev->autosuspend_disabled ;
			dvobj->pusbdev->autosuspend_disabled = 0;//autosuspend disabled by the user
			#endif

			//usb_autopm_get_interface(adapter_to_dvobj(padapter)->pusbintf );//init pm_usage_cnt ,let it start from 1

			#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,32))
			DBG_871X("%s...pm_usage_cnt(%d).....\n",__FUNCTION__,atomic_read(&(dvobj->pusbintf ->pm_usage_cnt)));
			#else
			DBG_871X("%s...pm_usage_cnt(%d).....\n",__FUNCTION__,dvobj->pusbintf ->pm_usage_cnt);
			#endif
		}
	}
#endif
	//2012-07-11 Move here to prevent the 8723AS-VAU BT auto suspend influence
	#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,33))
	if (usb_autopm_get_interface(pusb_intf) < 0)
		DBG_871X( "can't get autopm: \n");
	#endif
#ifdef	CONFIG_BT_COEXIST
	dvobj_to_pwrctl(dvobj)->autopm_cnt=1;
#endif

	// set mac addr
	rtw_macaddr_cfg(padapter->eeprompriv.mac_addr);
#ifdef CONFIG_P2P
	rtw_init_wifidirect_addrs(padapter, padapter->eeprompriv.mac_addr, padapter->eeprompriv.mac_addr);
#endif // CONFIG_P2P
	DBG_871X("bDriverStopped:%d, bSurpriseRemoved:%d, bup:%d, hw_init_completed:%d\n"
		, padapter->bDriverStopped
		, padapter->bSurpriseRemoved
		, padapter->bup
		, padapter->hw_init_completed
	);

	status = _SUCCESS;

free_hal_data:
	if(status != _SUCCESS && padapter->HalData)
		kfree(padapter->HalData);
free_wdev:
	if(status != _SUCCESS) {
		#ifdef CONFIG_IOCTL_CFG80211
		rtw_wdev_unregister(padapter->rtw_wdev);
		rtw_wdev_free(padapter->rtw_wdev);
		#endif
	}
handle_dualmac:
	if (status != _SUCCESS)
		rtw_handle_dualmac(padapter, 0);
free_adapter:
	if (status != _SUCCESS) {
		if (pnetdev)
			rtw_free_netdev(pnetdev);
		else
			rtw_vmfree((u8*)padapter, sizeof(*padapter));
		padapter = NULL;
	}
exit:
	return padapter;
}

static void rtw_usb_if1_deinit(_adapter *if1)
{
	struct pwrctrl_priv *pwrctl = adapter_to_pwrctl(if1);
	struct net_device *pnetdev = if1->pnetdev;
	struct mlme_priv *pmlmepriv= &if1->mlmepriv;

	if(check_fwstate(pmlmepriv, _FW_LINKED))
		rtw_disassoc_cmd(if1, 0, _FALSE);


#ifdef CONFIG_AP_MODE
	free_mlme_ap_info(if1);
	#ifdef CONFIG_HOSTAPD_MLME
	hostapd_mode_unload(if1);
	#endif
#endif

	rtw_cancel_all_timer(if1);

#ifdef CONFIG_WOWLAN
	pwrctl->wowlan_mode=_FALSE;
#endif //CONFIG_WOWLAN

	rtw_dev_unload(if1);

	DBG_871X("+r871xu_dev_remove, hw_init_completed=%d\n", if1->hw_init_completed);

	rtw_handle_dualmac(if1, 0);

#ifdef CONFIG_IOCTL_CFG80211
	if(if1->rtw_wdev) {
		rtw_wdev_free(if1->rtw_wdev);
	}
#endif

#ifdef CONFIG_BT_COEXIST
	if(1 == pwrctl->autopm_cnt){
		#if (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,33))
		usb_autopm_put_interface(adapter_to_dvobj(if1)->pusbintf);
		#elif (LINUX_VERSION_CODE>=KERNEL_VERSION(2,6,20))
		usb_autopm_enable(adapter_to_dvobj(if1)->pusbintf);
		#else
		usb_autosuspend_device(adapter_to_dvobj(if1)->pusbdev, 1);
		#endif
		pwrctl->autopm_cnt --;
	}
#endif

	rtw_free_drv_sw(if1);

	if(pnetdev)
		rtw_free_netdev(pnetdev);

#ifdef CONFIG_PLATFORM_RTD2880B
	DBG_871X("wlan link down\n");
	rtd2885_wlan_netlink_sendMsg("linkdown", "8712");
#endif

}

static int rtw_drv_init(struct usb_interface *pusb_intf, const struct usb_device_id *pdid)
{
	_adapter *if1 = NULL, *if2 = NULL;
	int status = _FAIL;
	struct dvobj_priv *dvobj;
#ifdef CONFIG_MULTI_VIR_IFACES
	int i;
#endif //CONFIG_MULTI_VIR_IFACES

	RT_TRACE(_module_hci_intfs_c_, _drv_err_, ("+rtw_drv_init\n"));
	//DBG_871X("+rtw_drv_init\n");

	//step 0.
	process_spec_devid(pdid);

	/* Initialize dvobj_priv */
	if ((dvobj = usb_dvobj_init(pusb_intf)) == NULL) {
		RT_TRACE(_module_hci_intfs_c_, _drv_err_, ("initialize device object priv Failed!\n"));
		goto exit;
	}

	if ((if1 = rtw_usb_if1_init(dvobj, pusb_intf, pdid)) == NULL) {
		DBG_871X("rtw_usb_if1_init Failed!\n");
		goto free_dvobj;
	}

#ifdef CONFIG_CONCURRENT_MODE
	if((if2 = rtw_drv_if2_init(if1, usb_set_intf_ops)) == NULL) {
		goto free_if1;
	}
#ifdef CONFIG_MULTI_VIR_IFACES
	for(i=0; i<if1->registrypriv.ext_iface_num;i++)
	{
		if(rtw_drv_add_vir_if(if1, usb_set_intf_ops) == NULL)
		{
			DBG_871X("rtw_drv_add_iface failed! (%d)\n", i);
			goto free_if2;
		}
	}
#endif //CONFIG_MULTI_VIR_IFACES
#endif

#ifdef CONFIG_INTEL_PROXIM
	rtw_sw_export=if1;
#endif

#ifdef CONFIG_GLOBAL_UI_PID
	if(ui_pid[1]!=0) {
		DBG_871X("ui_pid[1]:%d\n",ui_pid[1]);
		rtw_signal_process(ui_pid[1], SIGUSR2);
	}
#endif

	//dev_alloc_name && register_netdev
	if((status = rtw_drv_register_netdev(if1)) != _SUCCESS) {
		goto free_if2;
	}

#ifdef CONFIG_HOSTAPD_MLME
	hostapd_mode_init(if1);
#endif

#ifdef CONFIG_PLATFORM_RTD2880B
	DBG_871X("wlan link up\n");
	rtd2885_wlan_netlink_sendMsg("linkup", "8712");
#endif

	RT_TRACE(_module_hci_intfs_c_,_drv_err_,("-871x_drv - drv_init, success!\n"));

	status = _SUCCESS;

free_if2:
	if(status != _SUCCESS && if2) {
		#ifdef CONFIG_CONCURRENT_MODE
		rtw_drv_if2_stop(if2);
		rtw_drv_if2_free(if2);
		#endif
	}
free_if1:
	if (status != _SUCCESS && if1) {
		rtw_usb_if1_deinit(if1);
	}
free_dvobj:
	if (status != _SUCCESS)
		usb_dvobj_deinit(pusb_intf);
exit:
	return status == _SUCCESS?0:-ENODEV;
}

/*
 * dev_remove() - our device is being removed
*/
//rmmod module & unplug(SurpriseRemoved) will call r871xu_dev_remove() => how to recognize both
static void rtw_dev_remove(struct usb_interface *pusb_intf)
{
	struct dvobj_priv *dvobj = usb_get_intfdata(pusb_intf);
	struct pwrctrl_priv *pwrctl = dvobj_to_pwrctl(dvobj);
	_adapter *padapter = dvobj->if1;
	struct net_device *pnetdev = padapter->pnetdev;
	struct mlme_priv *pmlmepriv= &padapter->mlmepriv;



	DBG_871X("+rtw_dev_remove\n");
	RT_TRACE(_module_hci_intfs_c_,_drv_err_,("+dev_remove()\n"));

	dvobj->processing_dev_remove = _TRUE;

	rtw_unregister_netdevs(dvobj);

	if(usb_drv.drv_registered == _TRUE)
	{
		//DBG_871X("r871xu_dev_remove():padapter->bSurpriseRemoved == _TRUE\n");
		padapter->bSurpriseRemoved = _TRUE;
	}
	/*else
	{
		//DBG_871X("r871xu_dev_remove():module removed\n");
		padapter->hw_init_completed = _FALSE;
	}*/


#if defined(CONFIG_HAS_EARLYSUSPEND) || defined(CONFIG_ANDROID_POWER)
	rtw_unregister_early_suspend(pwrctl);
#endif

	rtw_pm_set_ips(padapter, IPS_NONE);
	rtw_pm_set_lps(padapter, PS_MODE_ACTIVE);

	LeaveAllPowerSaveMode(padapter);

#ifdef CONFIG_CONCURRENT_MODE
#ifdef CONFIG_MULTI_VIR_IFACES
	rtw_drv_stop_vir_ifaces(dvobj);
#endif //CONFIG_MULTI_VIR_IFACES
	rtw_drv_if2_stop(dvobj->if2);
#endif //CONFIG_CONCURRENT_MODE

	#ifdef CONFIG_BT_COEXIST
	rtw_btcoex_HaltNotify(padapter);
	#endif

	rtw_usb_if1_deinit(padapter);

#ifdef CONFIG_CONCURRENT_MODE
#ifdef CONFIG_MULTI_VIR_IFACES
	rtw_drv_free_vir_ifaces(dvobj);
#endif //CONFIG_MULTI_VIR_IFACES
	rtw_drv_if2_free(dvobj->if2);
#endif //CONFIG_CONCURRENT_MODE

	usb_dvobj_deinit(pusb_intf);

	RT_TRACE(_module_hci_intfs_c_,_drv_err_,("-dev_remove()\n"));
	DBG_871X("-r871xu_dev_remove, done\n");


#ifdef CONFIG_INTEL_PROXIM
	rtw_sw_export=NULL;
#endif



	return;

}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24))
extern int console_suspend_enabled;
#endif

static int __init rtw_drv_entry(void)
{
	int ret = 0;

	DBG_871X_LEVEL(_drv_always_, "module init start\n");
	dump_drv_version(RTW_DBGDUMP);
#ifdef BTCOEXVERSION
	DBG_871X_LEVEL(_drv_always_, DRV_NAME" BT-Coex version = %s\n", BTCOEXVERSION);
#endif // BTCOEXVERSION

	ret = platform_wifi_power_on();
	if(ret != 0)
	{
		DBG_871X("%s: power on failed!!(%d)\n", __FUNCTION__, ret);
		ret = -1;
		goto exit;
	}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24))
	//console_suspend_enabled=0;
#endif

	usb_drv.drv_registered = _TRUE;
	rtw_suspend_lock_init();
	rtw_drv_proc_init();
	rtw_ndev_notifier_register();

	ret = usb_register(&usb_drv.usbdrv);

	if (ret != 0) {
		usb_drv.drv_registered = _FALSE;
		rtw_suspend_lock_uninit();
		rtw_drv_proc_deinit();
		rtw_ndev_notifier_unregister();
		goto exit;
	}

exit:
	DBG_871X_LEVEL(_drv_always_, "module init ret=%d\n", ret);
	return ret;
}

static void __exit rtw_drv_halt(void)
{
	DBG_871X_LEVEL(_drv_always_, "module exit start\n");

	usb_drv.drv_registered = _FALSE;

	usb_deregister(&usb_drv.usbdrv);

	platform_wifi_power_off();

	rtw_suspend_lock_uninit();
	rtw_drv_proc_deinit();
	rtw_ndev_notifier_unregister();

	DBG_871X_LEVEL(_drv_always_, "module exit success\n");

	rtw_mstat_dump(RTW_DBGDUMP);
}


module_init(rtw_drv_entry);
module_exit(rtw_drv_halt);

#ifdef CONFIG_INTEL_PROXIM
_adapter  *rtw_usb_get_sw_pointer(void)
{
	return rtw_sw_export;
}
EXPORT_SYMBOL(rtw_usb_get_sw_pointer);
#endif	//CONFIG_INTEL_PROXIM
