/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_MEMFD_H
#define __LINUX_MEMFD_H

#include <linux/fcntl.h>
#include <linux/file.h>

static inline int memfd_check_seals_mode(unsigned int seals, umode_t old_mode,
		umode_t new_mode)
{
	if ((seals & F_SEAL_EXEC) && ((old_mode ^ new_mode) & 0111))
		return -EPERM;
	return 0;
}

#ifdef CONFIG_MEMFD_CREATE
extern long memfd_fcntl(struct file *file, unsigned int cmd, unsigned int arg);
struct folio *memfd_alloc_folio(struct file *memfd, pgoff_t idx);
/*
 * Check for any existing seals on mmap, return an error if access is denied due
 * to sealing, or 0 otherwise.
 *
 * We also update VMA flags if appropriate by manipulating the VMA flags pointed
 * to by vm_flags_ptr.
 */
int memfd_check_seals_mmap(struct file *file, unsigned long *vm_flags_ptr);
#else
static inline long memfd_fcntl(struct file *f, unsigned int c, unsigned int a)
{
	return -EINVAL;
}
static inline struct folio *memfd_alloc_folio(struct file *memfd, pgoff_t idx)
{
	return ERR_PTR(-EINVAL);
}
static inline int memfd_check_seals_mmap(struct file *file,
					 unsigned long *vm_flags_ptr)
{
	return 0;
}
#endif

#endif /* __LINUX_MEMFD_H */
