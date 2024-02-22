// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah_guest_mem: " fmt

#include <linux/anon_inodes.h>
#include <linux/types.h>
#include <linux/falloc.h>
#include <linux/file.h>
#include <linux/migrate.h>
#include <linux/pagemap.h>

#include <uapi/linux/gunyah.h>

#include "vm_mgr.h"

static struct folio *gunyah_gmem_get_huge_folio(struct inode *inode,
						pgoff_t index)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	unsigned long huge_index = round_down(index, HPAGE_PMD_NR);
	unsigned long flags = (unsigned long)inode->i_private;
	struct address_space *mapping = inode->i_mapping;
	gfp_t gfp = mapping_gfp_mask(mapping);
	struct folio *folio;

	if (!(flags & GHMF_ALLOW_HUGEPAGE))
		return NULL;

	if (filemap_range_has_page(mapping, huge_index << PAGE_SHIFT,
				   (huge_index + HPAGE_PMD_NR - 1)
					   << PAGE_SHIFT))
		return NULL;

	folio = filemap_alloc_folio(gfp, HPAGE_PMD_ORDER);
	if (!folio)
		return NULL;

	if (filemap_add_folio(mapping, folio, huge_index, gfp)) {
		folio_put(folio);
		return NULL;
	}

	return folio;
#else
	return NULL;
#endif
}

static struct folio *gunyah_gmem_get_folio(struct inode *inode, pgoff_t index)
{
	struct folio *folio;

	folio = gunyah_gmem_get_huge_folio(inode, index);
	if (!folio) {
		folio = filemap_grab_folio(inode->i_mapping, index);
		if (IS_ERR_OR_NULL(folio))
			return NULL;
	}

	/*
	 * Use the up-to-date flag to track whether or not the memory has been
	 * zeroed before being handed off to the guest.  There is no backing
	 * storage for the memory, so the folio will remain up-to-date until
	 * it's removed.
	 */
	if (!folio_test_uptodate(folio)) {
		unsigned long nr_pages = folio_nr_pages(folio);
		unsigned long i;

		for (i = 0; i < nr_pages; i++)
			clear_highpage(folio_page(folio, i));

		folio_mark_uptodate(folio);
	}

	/*
	 * Ignore accessed, referenced, and dirty flags.  The memory is
	 * unevictable and there is no storage to write back to.
	 */
	return folio;
}

static vm_fault_t gunyah_gmem_host_fault(struct vm_fault *vmf)
{
	struct folio *folio;

	folio = gunyah_gmem_get_folio(file_inode(vmf->vma->vm_file),
				      vmf->pgoff);
	if (!folio)
		return VM_FAULT_SIGBUS;

	vmf->page = folio_file_page(folio, vmf->pgoff);

	return VM_FAULT_LOCKED;
}

static const struct vm_operations_struct gunyah_gmem_vm_ops = {
	.fault = gunyah_gmem_host_fault,
};

static int gunyah_gmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	file_accessed(file);
	vma->vm_ops = &gunyah_gmem_vm_ops;
	return 0;
}

/**
 * gunyah_gmem_punch_hole() - try to reclaim a range of pages
 * @inode: guest memfd inode
 * @offset: Offset into memfd to start reclaim
 * @len: length to reclaim
 *
 * Will try to unmap from virtual machines any folios covered by
 * [offset, offset+len]. If unmapped, then tries to free those folios
 *
 * Returns - error code if any folio in the range couldn't be freed.
 */
static long gunyah_gmem_punch_hole(struct inode *inode, loff_t offset,
				   loff_t len)
{
	truncate_inode_pages_range(inode->i_mapping, offset, offset + len - 1);

	return 0;
}

static long gunyah_gmem_allocate(struct inode *inode, loff_t offset, loff_t len)
{
	struct address_space *mapping = inode->i_mapping;
	pgoff_t start, index, end;
	int r;

	/* Dedicated guest is immutable by default. */
	if (offset + len > i_size_read(inode))
		return -EINVAL;

	filemap_invalidate_lock_shared(mapping);

	start = offset >> PAGE_SHIFT;
	end = (offset + len) >> PAGE_SHIFT;

	r = 0;
	for (index = start; index < end;) {
		struct folio *folio;

		if (signal_pending(current)) {
			r = -EINTR;
			break;
		}

		folio = gunyah_gmem_get_folio(inode, index);
		if (!folio) {
			r = -ENOMEM;
			break;
		}

		index = folio_next_index(folio);

		folio_unlock(folio);
		folio_put(folio);

		/* 64-bit only, wrapping the index should be impossible. */
		if (WARN_ON_ONCE(!index))
			break;

		cond_resched();
	}

	filemap_invalidate_unlock_shared(mapping);

	return r;
}

static long gunyah_gmem_fallocate(struct file *file, int mode, loff_t offset,
				  loff_t len)
{
	long ret;

	if (!(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
		     FALLOC_FL_ZERO_RANGE))
		return -EOPNOTSUPP;

	if (!PAGE_ALIGNED(offset) || !PAGE_ALIGNED(len))
		return -EINVAL;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		ret = gunyah_gmem_punch_hole(file_inode(file), offset, len);
	else
		ret = gunyah_gmem_allocate(file_inode(file), offset, len);

	if (!ret)
		file_modified(file);
	return ret;
}

static int gunyah_gmem_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations gunyah_gmem_fops = {
	.owner = THIS_MODULE,
	.llseek = generic_file_llseek,
	.mmap = gunyah_gmem_mmap,
	.open = generic_file_open,
	.fallocate = gunyah_gmem_fallocate,
	.release = gunyah_gmem_release,
};

static const struct address_space_operations gunyah_gmem_aops = {
	.dirty_folio = noop_dirty_folio,
	.migrate_folio = migrate_folio,
	.error_remove_folio = generic_error_remove_folio,
};

int gunyah_guest_mem_create(struct gunyah_create_mem_args *args)
{
	const char *anon_name = "[gh-gmem]";
	unsigned long fd_flags = 0;
	struct inode *inode;
	struct file *file;
	int fd, err;

	if (!PAGE_ALIGNED(args->size))
		return -EINVAL;

	if (args->flags & ~(GHMF_CLOEXEC | GHMF_ALLOW_HUGEPAGE))
		return -EINVAL;

	if (args->flags & GHMF_CLOEXEC)
		fd_flags |= O_CLOEXEC;

	fd = get_unused_fd_flags(fd_flags);
	if (fd < 0)
		return fd;

	/*
	 * Use the so called "secure" variant, which creates a unique inode
	 * instead of reusing a single inode.  Each guest_memfd instance needs
	 * its own inode to track the size, flags, etc.
	 */
	file = anon_inode_create_getfile(anon_name, &gunyah_gmem_fops, NULL,
					 O_RDWR, NULL);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_fd;
	}

	file->f_flags |= O_LARGEFILE;

	inode = file->f_inode;
	WARN_ON(file->f_mapping != inode->i_mapping);

	inode->i_private = (void *)(unsigned long)args->flags;
	inode->i_mapping->a_ops = &gunyah_gmem_aops;
	inode->i_mode |= S_IFREG;
	inode->i_size = args->size;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_large_folios(inode->i_mapping);
	mapping_set_unmovable(inode->i_mapping);
	/* Unmovable mappings are supposed to be marked unevictable as well. */
	WARN_ON_ONCE(!mapping_unevictable(inode->i_mapping));

	fd_install(fd, file);
	return fd;

err_fd:
	put_unused_fd(fd);
	return err;
}
