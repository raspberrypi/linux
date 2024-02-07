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
#include <linux/printk.h>
#include <linux/sched.h>

#define pgcompat_err(fmt, ...) \
	pr_err("pgcompat [%i (%s)]: " fmt, task_pid_nr(current), current->comm, ## __VA_ARGS__)

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

#define __offset_in_page(p)		((unsigned long)(p) & ~__PAGE_MASK)

#define __offset_in_page_log(addr)							\
({											\
	if (static_branch_unlikely(&page_shift_compat_enabled) &&			\
			__offset_in_page(addr))						\
		pgcompat_err("%s: addr (0x%08lx) not page aligned", __func__, addr);	\
	(__offset_in_page(addr));							\
})

#define __PAGE_ALIGNED(addr)    (!__offset_in_page_log(addr))

/*
 * Increases @size by an adequate amount to allow __PAGE_SIZE alignment
 * by rounding up; given that @size is already a multiple of the
 * base page size (PAGE_SIZE).
 *
 * Example:
 *     If __PAGE_SHIFT == PAGE_SHIFT == 12
 *         @size is increased by 0
 *             ((1 << (0)) - 1) << PAGE_SHIFT
 *             (1        ) - 1) << PAGE_SHIFT
 *             (0             ) << PAGE_SHIFT
 *
 *     If __PAGE_SHIFT == 13 and PAGE_SHIFT == 12
 *         @size is increased by PAGE_SIZE (4KB):
 *             ((1 << (1)) - 1) << PAGE_SHIFT
 *             (2        ) - 1) << PAGE_SHIFT
 *             (1             ) << PAGE_SHIFT
 *     If __PAGE_SHIFT == 14 and PAGE_SHIFT == 12
 *         @size is increased by 3xPAGE_SIZE (12KB):
 *             ((1 << (2)) - 1) << PAGE_SHIFT
 *             (4        ) - 1) << PAGE_SHIFT
 *             (3             ) << PAGE_SHIFT
 *     ...
 */
#define __PAGE_SIZE_ROUND_UP_ADJ(size) \
	((size) + (((1 << (__PAGE_SHIFT - PAGE_SHIFT)) - 1) << PAGE_SHIFT))

#endif /* __LINUX_PAGE_SIZE_COMPAT_H */
