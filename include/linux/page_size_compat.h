/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PAGE_SIZE_COMPAT_H
#define __LINUX_PAGE_SIZE_COMPAT_H

/*
 * include/linux/page_size_compat.h
 *
 * Page Size Emulation
 *
 * Copyright (c) 2024, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@goole.com>

 * Helper macros for page size emulation.
 *
 * The macros for use with the emulated page size are all
 * namespaced by the prefix '__'.
 *
 * The valid range of androidboot.page_shift is [13, 16].
 * In other words page sizes of 8KB, 16KB, 32KB and 64KB can
 * be emulated.
 */

#include <asm/page.h>

#include <linux/align.h>
#include <linux/jump_label.h>

DECLARE_STATIC_KEY_FALSE(page_shift_compat_enabled);
extern int page_shift_compat;

static __always_inline unsigned __page_shift(void)
{
	if (static_branch_unlikely(&page_shift_compat_enabled))
		return page_shift_compat;
	else
		return PAGE_SHIFT;
}

#define __PAGE_SHIFT 			__page_shift()
#define __PAGE_SIZE 			(_AC(1,UL) << __PAGE_SHIFT)
#define __PAGE_MASK 			(~(__PAGE_SIZE-1))
#define __PAGE_ALIGN(addr) 		ALIGN(addr, __PAGE_SIZE)
#define __PAGE_ALIGN_DOWN(addr)	ALIGN_DOWN(addr, __PAGE_SIZE)
#define __PAGE_ALIGNED(addr)	IS_ALIGNED((unsigned long)(addr), __PAGE_SIZE)
#define __offset_in_page(p)		((unsigned long)(p) & ~__PAGE_MASK)

#endif /* __LINUX_PAGE_SIZE_COMPAT_H */
