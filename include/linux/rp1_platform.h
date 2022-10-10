/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021-2022 Raspberry Pi Ltd.
 * All rights reserved.
 */

#ifndef _RP1_PLATFORM_H
#define _RP1_PLATFORM_H

#include <vdso/bits.h>

#define RP1_B0_CHIP_ID 0x10001927
#define RP1_C0_CHIP_ID 0x20001927

#define RP1_PLATFORM_ASIC BIT(1)
#define RP1_PLATFORM_FPGA BIT(0)

void rp1_get_platform(u32 *chip_id, u32 *platform);

#endif
