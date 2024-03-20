// SPDX-License-Identifier: GPL-2.0
/*
 * Page Size Emulation
 *
 * Copyright (c) 2024, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@goole.com>
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kstrtox.h>
#include <linux/mm.h>
#include <linux/page_size_compat.h>

#define MIN_PAGE_SHIFT_COMPAT (PAGE_SHIFT + 1)
#define MAX_PAGE_SHIFT_COMPAT 16 /* Max of 64KB */
#define __MMAP_RND_BITS(x)      (x - (__PAGE_SHIFT - PAGE_SHIFT))

DEFINE_STATIC_KEY_FALSE(page_shift_compat_enabled);
EXPORT_SYMBOL_GPL(page_shift_compat_enabled);

int page_shift_compat = MIN_PAGE_SHIFT_COMPAT;
EXPORT_SYMBOL_GPL(page_shift_compat);

static int __init early_page_shift_compat(char *buf)
{
	int ret;

	ret = kstrtoint(buf, 10, &page_shift_compat);
	if (ret)
		return ret;

	/* Only supported on 4KB kernel */
	if (PAGE_SHIFT != 12)
		return -ENOTSUPP;

	if (page_shift_compat < MIN_PAGE_SHIFT_COMPAT ||
		page_shift_compat > MAX_PAGE_SHIFT_COMPAT)
		return -EINVAL;

	static_branch_enable(&page_shift_compat_enabled);

	return 0;
}
early_param("androidboot.page_shift", early_page_shift_compat);

static int __init init_mmap_rnd_bits(void)
{
	if (!static_branch_unlikely(&page_shift_compat_enabled))
		return 0;

#ifdef CONFIG_HAVE_ARCH_MMAP_RND_BITS
	mmap_rnd_bits_min = __MMAP_RND_BITS(CONFIG_ARCH_MMAP_RND_BITS_MIN);
	mmap_rnd_bits_max = __MMAP_RND_BITS(CONFIG_ARCH_MMAP_RND_BITS_MAX);
	mmap_rnd_bits = __MMAP_RND_BITS(CONFIG_ARCH_MMAP_RND_BITS);
#endif

	return 0;
}
core_initcall(init_mmap_rnd_bits);

/*
 * Updates len to avoid mapping off the end of the file.
 *
 * The length of the original mapping must be updated before
 * it's VMA is created to avoid an unaligned munmap in the
 * MAP_FIXED fixup mapping.
 */
unsigned long ___filemap_len(struct inode *inode, unsigned long pgoff, unsigned long len,
			     unsigned long flags)
{
	unsigned long file_size;
	unsigned long new_len;
	pgoff_t max_pgcount;
	pgoff_t last_pgoff;

	if (flags & __MAP_NO_COMPAT)
		return len;

	file_size = (unsigned long) i_size_read(inode);

	/*
	 * Round up, so that this is a count (not an index). This simplifies
	 * the following calculations.
	 */
	max_pgcount = DIV_ROUND_UP(file_size, PAGE_SIZE);
	last_pgoff = pgoff + (len >> PAGE_SHIFT);

	if (unlikely(last_pgoff >= max_pgcount)) {
		new_len = (max_pgcount - pgoff)  << PAGE_SHIFT;
		/* Careful of underflows in special files */
		if (new_len > 0 && new_len < len)
			return new_len;
	}

	return len;
}

/*
 * This is called to fill any holes created by ___filemap_len()
 * with an anonymous mapping.
 */
void ___filemap_fixup(unsigned long addr, unsigned long prot, unsigned long old_len,
		      unsigned long new_len)
{
	unsigned long anon_len = old_len - new_len;
	unsigned long anon_addr = addr + new_len;
	struct mm_struct *mm = current->mm;
	unsigned long populate = 0;
	struct vm_area_struct *vma;

	if (!anon_len)
		return;

	BUG_ON(new_len > old_len);

	/* The original do_mmap() failed */
	if (IS_ERR_VALUE(addr))
		return;

	vma = find_vma(mm, addr);

	/*
	 * This should never happen, VMA was inserted and we still
	 * haven't released the mmap write lock.
	 */
	BUG_ON(!vma);

	/* Only handle fixups for filemap faults */
	if (vma->vm_ops && vma->vm_ops->fault != filemap_fault)
		return;

	/*
	 * Override the end of the file mapping that is off the file
	 * with an anonymous mapping.
	 */
	anon_addr = do_mmap(NULL, anon_addr, anon_len, prot,
					MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|__MAP_NO_COMPAT,
					0, 0, &populate, NULL);
}
