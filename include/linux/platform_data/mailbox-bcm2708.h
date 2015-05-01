/*
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef _PLAT_MAILBOX_BCM2708_H
#define _PLAT_MAILBOX_BCM2708_H

/* Routines to handle I/O via the VideoCore "ARM control" registers
 * (semaphores, doorbells, mailboxes)
 */

/* Constants shared with the ARM identifying separate mailbox channels */
#define MBOX_CHAN_POWER    0 /* for use by the power management interface */
#define MBOX_CHAN_FB       1 /* for use by the frame buffer */
#define MBOX_CHAN_VCHIQ    3 /* for use by the VCHIQ interface */
#define MBOX_CHAN_PROPERTY 8 /* for use by the property channel */
#define MBOX_CHAN_COUNT    9

enum {
	VCMSG_PROCESS_REQUEST		= 0x00000000
};

enum {
	VCMSG_REQUEST_SUCCESSFUL	= 0x80000000,
	VCMSG_REQUEST_FAILED		= 0x80000001
};

/* Mailbox property tags */
enum {
	VCMSG_PROPERTY_END               = 0x00000000,
	VCMSG_GET_FIRMWARE_REVISION      = 0x00000001,
	VCMSG_GET_BOARD_MODEL            = 0x00010001,
	VCMSG_GET_BOARD_REVISION	 = 0x00010002,
	VCMSG_GET_BOARD_MAC_ADDRESS	 = 0x00010003,
	VCMSG_GET_BOARD_SERIAL		 = 0x00010004,
	VCMSG_GET_ARM_MEMORY		 = 0x00010005,
	VCMSG_GET_VC_MEMORY		 = 0x00010006,
	VCMSG_GET_CLOCKS		 = 0x00010007,
	VCMSG_GET_COMMAND_LINE           = 0x00050001,
	VCMSG_GET_DMA_CHANNELS           = 0x00060001,
	VCMSG_GET_POWER_STATE            = 0x00020001,
	VCMSG_GET_TIMING		 = 0x00020002,
	VCMSG_SET_POWER_STATE            = 0x00028001,
	VCMSG_GET_CLOCK_STATE            = 0x00030001,
	VCMSG_SET_CLOCK_STATE            = 0x00038001,
	VCMSG_GET_CLOCK_RATE             = 0x00030002,
	VCMSG_SET_CLOCK_RATE             = 0x00038002,
	VCMSG_GET_VOLTAGE                = 0x00030003,
	VCMSG_SET_VOLTAGE                = 0x00038003,
	VCMSG_GET_MAX_CLOCK              = 0x00030004,
	VCMSG_GET_MAX_VOLTAGE            = 0x00030005,
	VCMSG_GET_TEMPERATURE            = 0x00030006,
	VCMSG_GET_MIN_CLOCK              = 0x00030007,
	VCMSG_GET_MIN_VOLTAGE            = 0x00030008,
	VCMSG_GET_TURBO                  = 0x00030009,
	VCMSG_GET_MAX_TEMPERATURE        = 0x0003000a,
	VCMSG_GET_STC                    = 0x0003000b,
	VCMSG_SET_TURBO                  = 0x00038009,
	VCMSG_SET_ALLOCATE_MEM           = 0x0003000c,
	VCMSG_SET_LOCK_MEM               = 0x0003000d,
	VCMSG_SET_UNLOCK_MEM             = 0x0003000e,
	VCMSG_SET_RELEASE_MEM            = 0x0003000f,
	VCMSG_SET_EXECUTE_CODE           = 0x00030010,
	VCMSG_SET_EXECUTE_QPU            = 0x00030011,
	VCMSG_SET_ENABLE_QPU             = 0x00030012,
	VCMSG_GET_RESOURCE_HANDLE        = 0x00030014,
	VCMSG_GET_EDID_BLOCK             = 0x00030020,
	VCMSG_GET_CUSTOMER_OTP           = 0x00030021,
	VCMSG_SET_CUSTOMER_OTP           = 0x00038021,
	VCMSG_SET_ALLOCATE_BUFFER        = 0x00040001,
	VCMSG_SET_RELEASE_BUFFER         = 0x00048001,
	VCMSG_SET_BLANK_SCREEN           = 0x00040002,
	VCMSG_TST_BLANK_SCREEN           = 0x00044002,
	VCMSG_GET_PHYSICAL_WIDTH_HEIGHT  = 0x00040003,
	VCMSG_TST_PHYSICAL_WIDTH_HEIGHT  = 0x00044003,
	VCMSG_SET_PHYSICAL_WIDTH_HEIGHT  = 0x00048003,
	VCMSG_GET_VIRTUAL_WIDTH_HEIGHT   = 0x00040004,
	VCMSG_TST_VIRTUAL_WIDTH_HEIGHT   = 0x00044004,
	VCMSG_SET_VIRTUAL_WIDTH_HEIGHT   = 0x00048004,
	VCMSG_GET_DEPTH                  = 0x00040005,
	VCMSG_TST_DEPTH                  = 0x00044005,
	VCMSG_SET_DEPTH                  = 0x00048005,
	VCMSG_GET_PIXEL_ORDER            = 0x00040006,
	VCMSG_TST_PIXEL_ORDER            = 0x00044006,
	VCMSG_SET_PIXEL_ORDER            = 0x00048006,
	VCMSG_GET_ALPHA_MODE             = 0x00040007,
	VCMSG_TST_ALPHA_MODE             = 0x00044007,
	VCMSG_SET_ALPHA_MODE             = 0x00048007,
	VCMSG_GET_PITCH                  = 0x00040008,
	VCMSG_TST_PITCH                  = 0x00044008,
	VCMSG_SET_PITCH                  = 0x00048008,
	VCMSG_GET_VIRTUAL_OFFSET         = 0x00040009,
	VCMSG_TST_VIRTUAL_OFFSET         = 0x00044009,
	VCMSG_SET_VIRTUAL_OFFSET         = 0x00048009,
	VCMSG_GET_OVERSCAN               = 0x0004000a,
	VCMSG_TST_OVERSCAN               = 0x0004400a,
	VCMSG_SET_OVERSCAN               = 0x0004800a,
	VCMSG_GET_PALETTE                = 0x0004000b,
	VCMSG_TST_PALETTE                = 0x0004400b,
	VCMSG_SET_PALETTE                = 0x0004800b,
	VCMSG_GET_LAYER                  = 0x0004000c,
	VCMSG_TST_LAYER                  = 0x0004400c,
	VCMSG_SET_LAYER                  = 0x0004800c,
	VCMSG_GET_TRANSFORM              = 0x0004000d,
	VCMSG_TST_TRANSFORM              = 0x0004400d,
	VCMSG_SET_TRANSFORM              = 0x0004800d,
	VCMSG_TST_VSYNC                  = 0x0004400e,
	VCMSG_SET_VSYNC                  = 0x0004800e,
	VCMSG_SET_CURSOR_INFO            = 0x00008010,
	VCMSG_SET_CURSOR_STATE           = 0x00008011,
};

int bcm_mailbox_read(unsigned chan, uint32_t *data28);
int bcm_mailbox_write(unsigned chan, uint32_t data28);
int bcm_mailbox_property(void *data, int size);

#endif
