/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Header providing constants for RP1 pinctrl bindings.
 *
 * Copyright (C) 2019-2022 Raspberry Pi Ltd.
 */

#ifndef __DT_BINDINGS_PINCTRL_RP1_H__
#define __DT_BINDINGS_PINCTRL_RP1_H__

/* brcm,function property */
#define RP1_FSEL_GPIO_IN	0
#define RP1_FSEL_GPIO_OUT	1
#define RP1_FSEL_ALT0_LEGACY	4
#define RP1_FSEL_ALT1_LEGACY	5
#define RP1_FSEL_ALT2_LEGACY	6
#define RP1_FSEL_ALT3_LEGACY	7
#define RP1_FSEL_ALT4_LEGACY	3
#define RP1_FSEL_ALT5_LEGACY	2
#define RP1_FSEL_ALT0		0x08
#define RP1_FSEL_ALT0INV	0x09
#define RP1_FSEL_ALT1		0x0a
#define RP1_FSEL_ALT1INV	0x0b
#define RP1_FSEL_ALT2		0x0c
#define RP1_FSEL_ALT2INV	0x0d
#define RP1_FSEL_ALT3		0x0e
#define RP1_FSEL_ALT3INV	0x0f
#define RP1_FSEL_ALT4		0x10
#define RP1_FSEL_ALT4INV	0x11
#define RP1_FSEL_ALT5		0x12
#define RP1_FSEL_ALT5INV	0x13
#define RP1_FSEL_ALT6		0x14
#define RP1_FSEL_ALT6INV	0x15
#define RP1_FSEL_ALT7		0x16
#define RP1_FSEL_ALT7INV	0x17
#define RP1_FSEL_ALT8		0x18
#define RP1_FSEL_ALT8INV	0x19
#define RP1_FSEL_NONE		0x1a

/* brcm,pull property */
#define RP1_PUD_OFF		0
#define RP1_PUD_DOWN		1
#define RP1_PUD_UP		2
#define RP1_PUD_KEEP		3

#endif /* __DT_BINDINGS_PINCTRL_RP1_H__ */
