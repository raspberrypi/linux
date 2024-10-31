/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (C) 2023 2023-2024 Raspberry Pi Ltd.
 */

#ifndef __SOC_RP1_FIRMWARE_OPS_H__
#define __SOC_RP1_FIRMWARE_OPS_H__

#include <linux/rp1-firmware.h>

#define FOURCC_PIO RP1_FOURCC("PIO ")

enum rp1_pio_ops {
	PIO_CAN_ADD_PROGRAM,	// u16 num_instrs, u16 origin -> origin
	PIO_ADD_PROGRAM,	// u16 num_instrs, u16 origin, u16 prog[] -> rc
	PIO_REMOVE_PROGRAM,	// u16 num_instrs, u16 origin
	PIO_CLEAR_INSTR_MEM,	// -

	PIO_SM_CLAIM,		// u16 mask -> sm
	PIO_SM_UNCLAIM,		// u16 mask
	PIO_SM_IS_CLAIMED,	// u16 mask -> claimed

	PIO_SM_INIT,		// u16 sm, u16 initial_pc, u32 sm_config[4]
	PIO_SM_SET_CONFIG,	// u16 sm, u16 rsvd, u32 sm_config[4]
	PIO_SM_EXEC,		// u16 sm, u16 instr, u8 blocking, u8 rsvd
	PIO_SM_CLEAR_FIFOS,	// u16 sm
	PIO_SM_SET_CLKDIV,	// u16 sm, u16 div_int, u8 div_frac, u8 rsvd
	PIO_SM_SET_PINS,	// u16 sm, u16 rsvd, u32 values, u32 mask
	PIO_SM_SET_PINDIRS,	// u16 sm, u16 rsvd, u32 dirs, u32 mask
	PIO_SM_SET_ENABLED,	// u16 mask, u8 enable, u8 rsvd
	PIO_SM_RESTART,		// u16 mask
	PIO_SM_CLKDIV_RESTART,	// u16 mask
	PIO_SM_ENABLE_SYNC,	// u16 mask
	PIO_SM_PUT,		// u16 sm, u8 blocking, u8 rsvd, u32 data
	PIO_SM_GET,		// u16 sm, u8 blocking, u8 rsvd -> u32 data
	PIO_SM_SET_DMACTRL,	// u16 sm, u16 is_tx, u32 ctrl

	GPIO_INIT,		// u16 gpio
	GPIO_SET_FUNCTION,	// u16 gpio, u16 fn
	GPIO_SET_PULLS,		// u16 gpio, u8 up, u8 down
	GPIO_SET_OUTOVER,	// u16 gpio, u16 value
	GPIO_SET_INOVER,	// u16 gpio, u16 value
	GPIO_SET_OEOVER,	// u16 gpio, u16 value
	GPIO_SET_INPUT_ENABLED,	// u16 gpio, u16 value
	GPIO_SET_DRIVE_STRENGTH,	// u16 gpio, u16 value

	READ_HW,		// src address, len -> data bytes
	WRITE_HW,		// dst address, data

	PIO_SM_FIFO_STATE,	// u16 sm, u8 tx -> u16 level, u8 empty, u8 full
	PIO_SM_DRAIN_TX,	// u16 sm

	PIO_COUNT
};

#endif
