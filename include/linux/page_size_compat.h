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
#include <linux/mman.h>
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

/*
 * VMA is exempt from emulated page align requirements
 *
 * NOTE: __MAP_NO_COMPAT is not new UABI it is only ever set by the kernel
 *       in ___filemap_fixup()
 */
#define __VM_NO_COMPAT      (_AC(1,ULL) << 59)
#define __MAP_NO_COMPAT     (_AC(1,UL) << 31)

/*
 * Conditional page-alignment based on mmap flags
 *
 * If the VMA is allowed to not respect the emulated page size, align using the
 * base PAGE_SIZE, else align using the emulated __PAGE_SIZE.
 */
#define __COMPAT_PAGE_ALIGN(size, flags) \
	(flags & __MAP_NO_COMPAT) ? PAGE_ALIGN(size) : __PAGE_ALIGN(size)

/*
 * Combines the mmap "flags" argument into "vm_flags"
 *
 * If page size emulation is enabled, adds translation of the no-compat flag.
 */
static __always_inline unsigned long calc_vm_flag_bits(unsigned long flags)
{
	unsigned long flag_bits = __calc_vm_flag_bits(flags);

	if (static_branch_unlikely(&page_shift_compat_enabled))
		flag_bits |= _calc_vm_trans(flags, __MAP_NO_COMPAT,  __VM_NO_COMPAT );

	return flag_bits;
}

extern unsigned long ___filemap_len(struct inode *inode, unsigned long pgoff,
				    unsigned long len, unsigned long flags);

extern void ___filemap_fixup(unsigned long addr, unsigned long prot, unsigned long old_len,
			     unsigned long new_len);

static __always_inline unsigned long __filemap_len(struct inode *inode, unsigned long pgoff,
						   unsigned long len, unsigned long flags)
{
	if (static_branch_unlikely(&page_shift_compat_enabled))
		return ___filemap_len(inode, pgoff, len, flags);
	else
		return len;
}

static __always_inline void __filemap_fixup(unsigned long addr, unsigned long prot,
					    unsigned long old_len, unsigned long new_len)
{

	if (static_branch_unlikely(&page_shift_compat_enabled))
		___filemap_fixup(addr, prot, old_len, new_len);
}

#endif /* __LINUX_PAGE_SIZE_COMPAT_H */
