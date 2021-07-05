/*
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net) at
 * Carlstedt Research & Technology.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* Modified by Synopsys, Inc, 12/12/2007 */


#ifndef _USB_H_
#define _USB_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The USB records contain some unaligned little-endian word
 * components.  The U[SG]ETW macros take care of both the alignment
 * and endian problem and should always be used to access non-byte
 * values.
 */
typedef u_int8_t uByte;
typedef u_int8_t uWord[2];
typedef u_int8_t uDWord[4];

#define UGETW(w) ((w)[0] | ((w)[1] << 8))
#define USETW(w,v) ((w)[0] = (u_int8_t)(v), (w)[1] = (u_int8_t)((v) >> 8))
#define UGETDW(w) ((w)[0] | ((w)[1] << 8) | ((w)[2] << 16) | ((w)[3] << 24))
#define USETDW(w,v) ((w)[0] = (u_int8_t)(v), \
		     (w)[1] = (u_int8_t)((v) >> 8), \
		     (w)[2] = (u_int8_t)((v) >> 16), \
		     (w)[3] = (u_int8_t)((v) >> 24))

#define UPACKED __attribute__((__packed__))

typedef struct {
	uByte		bmRequestType;
	uByte		bRequest;
	uWord		wValue;
	uWord		wIndex;
	uWord		wLength;
} UPACKED usb_device_request_t;

#define UT_GET_DIR(a) ((a) & 0x80)
#define UT_WRITE		0x00
#define UT_READ			0x80

#define UT_GET_TYPE(a) ((a) & 0x60)
#define UT_STANDARD		0x00
#define UT_CLASS		0x20
#define UT_VENDOR		0x40

#define UT_GET_RECIPIENT(a) ((a) & 0x1f)
#define UT_DEVICE		0x00
#define UT_INTERFACE		0x01
#define UT_ENDPOINT		0x02
#define UT_OTHER		0x03

/* Requests */
#define UR_GET_STATUS		0x00
#define  USTAT_STANDARD_STATUS  0x00
#define  WUSTAT_WUSB_FEATURE    0x01
#define  WUSTAT_CHANNEL_INFO    0x02
#define  WUSTAT_RECEIVED_DATA   0x03
#define  WUSTAT_MAS_AVAILABILITY 0x04
#define  WUSTAT_CURRENT_TRANSMIT_POWER 0x05
#define UR_CLEAR_FEATURE	0x01
#define UR_SET_FEATURE		0x03
#define UR_SET_AND_TEST_FEATURE 0x0c
#define UR_SET_ADDRESS		0x05
#define UR_GET_DESCRIPTOR	0x06
#define  UDESC_DEVICE		0x01
#define  UDESC_CONFIG		0x02
#define  UDESC_STRING		0x03
#define  UDESC_INTERFACE	0x04
#define  UDESC_ENDPOINT		0x05
#define  UDESC_SS_USB_COMPANION	0x30
#define  UDESC_DEVICE_QUALIFIER	0x06
#define  UDESC_OTHER_SPEED_CONFIGURATION 0x07
#define  UDESC_INTERFACE_POWER	0x08
#define  UDESC_OTG		0x09
#define  WUDESC_SECURITY	0x0c
#define  WUDESC_KEY		0x0d
#define   WUD_GET_KEY_INDEX(_wValue_) ((_wValue_) & 0xf)
#define   WUD_GET_KEY_TYPE(_wValue_) (((_wValue_) & 0x30) >> 4)
#define    WUD_KEY_TYPE_ASSOC    0x01
#define    WUD_KEY_TYPE_GTK      0x02
#define   WUD_GET_KEY_ORIGIN(_wValue_) (((_wValue_) & 0x40) >> 6)
#define    WUD_KEY_ORIGIN_HOST   0x00
#define    WUD_KEY_ORIGIN_DEVICE 0x01
#define  WUDESC_ENCRYPTION_TYPE	0x0e
#define  WUDESC_BOS		0x0f
#define  WUDESC_DEVICE_CAPABILITY 0x10
#define  WUDESC_WIRELESS_ENDPOINT_COMPANION 0x11
#define  UDESC_BOS		0x0f
#define  UDESC_DEVICE_CAPABILITY 0x10
#define  UDESC_CS_DEVICE	0x21	/* class specific */
#define  UDESC_CS_CONFIG	0x22
#define  UDESC_CS_STRING	0x23
#define  UDESC_CS_INTERFACE	0x24
#define  UDESC_CS_ENDPOINT	0x25
#define  UDESC_HUB		0x29
#define UR_SET_DESCRIPTOR	0x07
#define UR_GET_CONFIG		0x08
#define UR_SET_CONFIG		0x09
#define UR_GET_INTERFACE	0x0a
#define UR_SET_INTERFACE	0x0b
#define UR_SYNCH_FRAME		0x0c
#define WUR_SET_ENCRYPTION      0x0d
#define WUR_GET_ENCRYPTION	0x0e
#define WUR_SET_HANDSHAKE	0x0f
#define WUR_GET_HANDSHAKE	0x10
#define WUR_SET_CONNECTION	0x11
#define WUR_SET_SECURITY_DATA	0x12
#define WUR_GET_SECURITY_DATA	0x13
#define WUR_SET_WUSB_DATA	0x14
#define  WUDATA_DRPIE_INFO	0x01
#define  WUDATA_TRANSMIT_DATA	0x02
#define  WUDATA_TRANSMIT_PARAMS	0x03
#define  WUDATA_RECEIVE_PARAMS	0x04
#define  WUDATA_TRANSMIT_POWER	0x05
#define WUR_LOOPBACK_DATA_WRITE	0x15
#define WUR_LOOPBACK_DATA_READ	0x16
#define WUR_SET_INTERFACE_DS	0x17

/* Feature numbers */
#define UF_ENDPOINT_HALT	0
#define UF_DEVICE_REMOTE_WAKEUP	1
#define UF_TEST_MODE		2
#define UF_DEVICE_B_HNP_ENABLE	3
#define UF_DEVICE_A_HNP_SUPPORT	4
#define UF_DEVICE_A_ALT_HNP_SUPPORT 5
#define WUF_WUSB		3
#define  WUF_TX_DRPIE		0x0
#define  WUF_DEV_XMIT_PACKET	0x1
#define  WUF_COUNT_PACKETS	0x2
#define  WUF_CAPTURE_PACKETS	0x3
#define UF_FUNCTION_SUSPEND	0
#define UF_U1_ENABLE		48
#define UF_U2_ENABLE		49
#define UF_LTM_ENABLE		50

/* Class requests from the USB 2.0 hub spec, table 11-15 */
#define UCR_CLEAR_HUB_FEATURE		(0x2000 | UR_CLEAR_FEATURE)
#define UCR_CLEAR_PORT_FEATURE		(0x2300 | UR_CLEAR_FEATURE)
#define UCR_GET_HUB_DESCRIPTOR		(0xa000 | UR_GET_DESCRIPTOR)
#define UCR_GET_HUB_STATUS		(0xa000 | UR_GET_STATUS)
#define UCR_GET_PORT_STATUS		(0xa300 | UR_GET_STATUS)
#define UCR_SET_HUB_FEATURE		(0x2000 | UR_SET_FEATURE)
#define UCR_SET_PORT_FEATURE		(0x2300 | UR_SET_FEATURE)
#define UCR_SET_AND_TEST_PORT_FEATURE	(0xa300 | UR_SET_AND_TEST_FEATURE)

#ifdef _MSC_VER
#include <pshpack1.h>
#endif

typedef struct {
	uByte		bLength;
	uByte		bDescriptorType;
	uByte		bEndpointAddress;
#define UE_GET_DIR(a)	((a) & 0x80)
#define UE_SET_DIR(a,d)	((a) | (((d)&1) << 7))
#define UE_DIR_IN	0x80
#define UE_DIR_OUT	0x00
#define UE_ADDR		0x0f
#define UE_GET_ADDR(a)	((a) & UE_ADDR)
	uByte		bmAttributes;
#define UE_XFERTYPE	0x03
#define  UE_CONTROL	0x00
#define  UE_ISOCHRONOUS	0x01
#define  UE_BULK	0x02
#define  UE_INTERRUPT	0x03
#define UE_GET_XFERTYPE(a)	((a) & UE_XFERTYPE)
#define UE_ISO_TYPE	0x0c
#define  UE_ISO_ASYNC	0x04
#define  UE_ISO_ADAPT	0x08
#define  UE_ISO_SYNC	0x0c
#define UE_GET_ISO_TYPE(a)	((a) & UE_ISO_TYPE)
	uWord		wMaxPacketSize;
	uByte		bInterval;
} UPACKED usb_endpoint_descriptor_t;
#define USB_ENDPOINT_DESCRIPTOR_SIZE 7

/* Hub specific request */
#define UR_GET_BUS_STATE	0x02
#define UR_CLEAR_TT_BUFFER	0x08
#define UR_RESET_TT		0x09
#define UR_GET_TT_STATE		0x0a
#define UR_STOP_TT		0x0b

/* Hub features */
#define UHF_C_HUB_LOCAL_POWER	0
#define UHF_C_HUB_OVER_CURRENT	1
#define UHF_PORT_CONNECTION	0
#define UHF_PORT_ENABLE		1
#define UHF_PORT_SUSPEND	2
#define UHF_PORT_OVER_CURRENT	3
#define UHF_PORT_RESET		4
#define UHF_PORT_L1		5
#define UHF_PORT_POWER		8
#define UHF_PORT_LOW_SPEED	9
#define UHF_PORT_HIGH_SPEED	10
#define UHF_C_PORT_CONNECTION	16
#define UHF_C_PORT_ENABLE	17
#define UHF_C_PORT_SUSPEND	18
#define UHF_C_PORT_OVER_CURRENT	19
#define UHF_C_PORT_RESET	20
#define UHF_C_PORT_L1		23
#define UHF_PORT_TEST		21
#define UHF_PORT_INDICATOR	22

typedef struct {
	uByte		bDescLength;
	uByte		bDescriptorType;
	uByte		bNbrPorts;
	uWord		wHubCharacteristics;
#define UHD_PWR			0x0003
#define  UHD_PWR_GANGED		0x0000
#define  UHD_PWR_INDIVIDUAL	0x0001
#define  UHD_PWR_NO_SWITCH	0x0002
#define UHD_COMPOUND		0x0004
#define UHD_OC			0x0018
#define  UHD_OC_GLOBAL		0x0000
#define  UHD_OC_INDIVIDUAL	0x0008
#define  UHD_OC_NONE		0x0010
#define UHD_TT_THINK		0x0060
#define  UHD_TT_THINK_8		0x0000
#define  UHD_TT_THINK_16	0x0020
#define  UHD_TT_THINK_24	0x0040
#define  UHD_TT_THINK_32	0x0060
#define UHD_PORT_IND		0x0080
	uByte		bPwrOn2PwrGood;	/* delay in 2 ms units */
#define UHD_PWRON_FACTOR 2
	uByte		bHubContrCurrent;
	uByte		DeviceRemovable[32]; /* max 255 ports */
#define UHD_NOT_REMOV(desc, i) \
    (((desc)->DeviceRemovable[(i)/8] >> ((i) % 8)) & 1)
	/* deprecated */ uByte		PortPowerCtrlMask[1];
} UPACKED usb_hub_descriptor_t;
#define USB_HUB_DESCRIPTOR_SIZE 9 /* includes deprecated PortPowerCtrlMask */

#ifdef _MSC_VER
#include <poppack.h>
#endif

#ifdef __cplusplus
}
#endif

#endif /* _USB_H_ */
