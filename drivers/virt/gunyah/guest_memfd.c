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

/**
 * struct gunyah_gmem_binding - Represents a binding of guestmem to a Gunyah VM
 * @gfn: Guest address to place acquired folios
 * @ghvm: Pointer to Gunyah VM in this binding
 * @i_off: offset into the guestmem to grab folios from
 * @file: Pointer to guest_memfd
 * @i_entry: list entry for inode->i_private_list
 * @flags: Access flags for the binding
 * @nr: Number of pages covered by this binding
 */
struct gunyah_gmem_binding {
	u64 gfn;
	struct gunyah_vm *ghvm;

	pgoff_t i_off;
	struct file *file;
	struct list_head i_entry;

	u32 flags;
	unsigned long nr;
};

static inline pgoff_t gunyah_gfn_to_off(struct gunyah_gmem_binding *b, u64 gfn)
{
	return gfn - b->gfn + b->i_off;
}

static inline u64 gunyah_off_to_gfn(struct gunyah_gmem_binding *b, pgoff_t off)
{
	return off - b->i_off + b->gfn;
}

static inline bool gunyah_guest_mem_is_lend(struct gunyah_vm *ghvm, u32 flags)
{
	u8 access = flags & GUNYAH_MEM_ACCESS_MASK;

	if (access == GUNYAH_MEM_FORCE_LEND)
		return true;
	else if (access == GUNYAH_MEM_FORCE_SHARE)
		return false;

	/* RM requires all VMs to be protected (isolated) */
	return true;
}

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

/**
 * gunyah_gmem_launder_folio() - Tries to unmap one folio from virtual machine(s)
 * @folio: The folio to unmap
 *
 * Returns - 0 if the folio has been reclaimed from any virtual machine(s) that
 *           folio was mapped into.
 */
static int gunyah_gmem_launder_folio(struct folio *folio)
{
	struct address_space *const mapping = folio->mapping;
	struct gunyah_gmem_binding *b;
	pgoff_t index = folio_index(folio);
	int ret = 0;
	u64 gfn;

	filemap_invalidate_lock_shared(mapping);
	list_for_each_entry(b, &mapping->i_private_list, i_entry) {
		/* if the mapping doesn't cover this folio: skip */
		if (b->i_off > index || index > b->i_off + b->nr)
			continue;

		gfn = gunyah_off_to_gfn(b, index);
		ret = gunyah_vm_reclaim_folio(b->ghvm, gfn, folio);
		if (WARN_RATELIMIT(ret, "failed to reclaim gfn: %08llx %d\n",
				   gfn, ret))
			break;
	}
	filemap_invalidate_unlock_shared(mapping);

	return ret;
}

static vm_fault_t gunyah_gmem_host_fault(struct vm_fault *vmf)
{
	struct folio *folio;

	folio = gunyah_gmem_get_folio(file_inode(vmf->vma->vm_file),
				      vmf->pgoff);
	if (!folio)
		return VM_FAULT_SIGBUS;

	/* If the folio is lent to a VM, try to reclaim it */
	if (folio_test_private(folio) && gunyah_gmem_launder_folio(folio)) {
		folio_unlock(folio);
		folio_put(folio);
		return VM_FAULT_SIGBUS;
	}
	/* gunyah_gmem_launder_folio should clear the private bit if it returns 0 */
	BUG_ON(folio_test_private(folio));

	vmf->page = folio_file_page(folio, vmf->pgoff);

	return VM_FAULT_LOCKED;
}

static const struct vm_operations_struct gunyah_gmem_vm_ops = {
	.fault = gunyah_gmem_host_fault,
};

static int gunyah_gmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct address_space *const mapping = file->f_mapping;
	struct gunyah_gmem_binding *b;
	pgoff_t end_off;
	int ret = 0;
	u64 gfn, nr;

	/* No support for private mappings to avoid COW.  */
	if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) !=
	    (VM_SHARED | VM_MAYSHARE)) {
		return -EINVAL;
	}

	filemap_invalidate_lock_shared(mapping);
	/**
	 * userspace can only mmap if the folios covered by the requested
	 * offset are not lent to the guest
	 */
	list_for_each_entry(b, &mapping->i_private_list, i_entry) {
		if (!gunyah_guest_mem_is_lend(b->ghvm, b->flags))
			continue;

		/* if the binding doesn't cover this vma: skip */
		if (vma->vm_pgoff + vma_pages(vma) < b->i_off)
			continue;
		if (vma->vm_pgoff > b->i_off + b->nr)
			continue;

		gfn = gunyah_off_to_gfn(b, vma->vm_pgoff);
		end_off = max(vma->vm_pgoff + vma_pages(vma), b->i_off + b->nr);
		nr = gunyah_off_to_gfn(b, end_off) - gfn;
		ret = gunyah_vm_reclaim_range(b->ghvm, gfn, nr);
		if (ret)
			break;
	}
	filemap_invalidate_unlock_shared(mapping);

	if (!ret) {
		file_accessed(file);
		vma->vm_ops = &gunyah_gmem_vm_ops;
	}

	return ret;
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
	return invalidate_inode_pages2_range(inode->i_mapping, offset, offset + len - 1);
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
	/**
	 * each binding increments refcount on file, so we shouldn't be here
	 * if i_private_list not empty.
	 */
	BUG_ON(!list_empty(&inode->i_mapping->i_private_list));

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

static bool gunyah_gmem_release_folio(struct folio *folio, gfp_t gfp_flags)
{
	/* should return true if released; launder folio returns 0 if freed */
	return !gunyah_gmem_launder_folio(folio);
}

static int gunyah_gmem_remove_folio(struct address_space *mapping,
				    struct folio *folio)
{
	if (mapping != folio->mapping)
		return -EINVAL;

	return gunyah_gmem_launder_folio(folio);
}

static const struct address_space_operations gunyah_gmem_aops = {
	.dirty_folio = noop_dirty_folio,
	.release_folio = gunyah_gmem_release_folio,
	.launder_folio = gunyah_gmem_launder_folio,
	.error_remove_folio = gunyah_gmem_remove_folio,
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
	mapping_set_release_always(inode->i_mapping);
	/* Unmovable mappings are supposed to be marked unevictable as well. */
	WARN_ON_ONCE(!mapping_unevictable(inode->i_mapping));

	fd_install(fd, file);
	return fd;

err_fd:
	put_unused_fd(fd);
	return err;
}

void gunyah_gmem_remove_binding(struct gunyah_gmem_binding *b)
{
	WARN_ON(gunyah_vm_reclaim_range(b->ghvm, b->gfn, b->nr));
	mtree_erase(&b->ghvm->bindings, b->gfn);
	list_del(&b->i_entry);
	fput(b->file);
	kfree(b);
}

static inline unsigned long gunyah_gmem_page_mask(struct file *file)
{
	unsigned long gmem_flags = (unsigned long)file_inode(file)->i_private;

	if (gmem_flags & GHMF_ALLOW_HUGEPAGE) {
#if IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)
		return HPAGE_PMD_MASK;
#else
		return ULONG_MAX;
#endif
	}

	return PAGE_MASK;
}

static int gunyah_gmem_init_binding(struct gunyah_vm *ghvm, struct file *file,
				    struct gunyah_map_mem_args *args,
				    struct gunyah_gmem_binding *binding)
{
	const unsigned long page_mask = ~gunyah_gmem_page_mask(file);

	if (args->flags & ~(GUNYAH_MEM_ALLOW_RWX | GUNYAH_MEM_ACCESS_MASK))
		return -EINVAL;

	if (args->guest_addr & page_mask)
		return -EINVAL;

	if (args->offset & page_mask)
		return -EINVAL;

	if (args->size & page_mask)
		return -EINVAL;

	binding->gfn = gunyah_gpa_to_gfn(args->guest_addr);
	binding->ghvm = ghvm;
	binding->i_off = args->offset >> PAGE_SHIFT;
	binding->file = file;
	binding->flags = args->flags;
	binding->nr = args->size >> PAGE_SHIFT;

	return 0;
}

static int gunyah_gmem_trim_binding(struct gunyah_gmem_binding *b,
				    unsigned long start_delta,
				    unsigned long end_delta)
{
	struct gunyah_vm *ghvm = b->ghvm;
	int ret;

	down_write(&ghvm->bindings_lock);
	if (!start_delta && !end_delta) {
		ret = gunyah_vm_reclaim_range(ghvm, b->gfn, b->nr);
		if (ret)
			goto unlock;
		gunyah_gmem_remove_binding(b);
	} else if (start_delta && !end_delta) {
		/* keep the start */
		ret = gunyah_vm_reclaim_range(ghvm, b->gfn + start_delta,
					      b->gfn + b->nr);
		if (ret)
			goto unlock;
		mtree_erase(&ghvm->bindings, b->gfn);
		b->nr = start_delta;
		ret = mtree_insert_range(&ghvm->bindings, b->gfn,
					 b->gfn + b->nr - 1, b, GFP_KERNEL);
	} else if (!start_delta && end_delta) {
		/* keep the end */
		ret = gunyah_vm_reclaim_range(ghvm, b->gfn,
					      b->gfn + b->nr - end_delta);
		if (ret)
			goto unlock;
		mtree_erase(&ghvm->bindings, b->gfn);
		b->gfn += b->nr - end_delta;
		b->i_off += b->nr - end_delta;
		b->nr = end_delta;
		ret = mtree_insert_range(&ghvm->bindings, b->gfn,
					 b->gfn + b->nr - 1, b, GFP_KERNEL);
	} else {
		/* TODO: split the mapping into 2 */
		ret = -EINVAL;
	}

unlock:
	up_write(&ghvm->bindings_lock);
	return ret;
}

static int gunyah_gmem_remove_mapping(struct gunyah_vm *ghvm, struct file *file,
				      struct gunyah_map_mem_args *args)
{
	struct inode *inode = file_inode(file);
	struct gunyah_gmem_binding *b = NULL;
	unsigned long start_delta, end_delta;
	struct gunyah_gmem_binding remove;
	int ret;

	ret = gunyah_gmem_init_binding(ghvm, file, args, &remove);
	if (ret)
		return ret;

	ret = -ENOENT;
	filemap_invalidate_lock(inode->i_mapping);
	list_for_each_entry(b, &inode->i_mapping->i_private_list, i_entry) {
		if (b->ghvm != remove.ghvm || b->flags != remove.flags ||
		    WARN_ON(b->file != remove.file))
			continue;
		/**
		 * Test if the binding to remove is within this binding
		 *  [gfn       b          nr]
		 *   [gfn   remove   nr]
		 */
		if (b->gfn > remove.gfn)
			continue;
		if (b->gfn + b->nr < remove.gfn + remove.nr)
			continue;

		/**
		 * We found the binding!
		 * Compute the delta in gfn start and make sure the offset
		 * into guest memfd matches.
		 */
		start_delta = remove.gfn - b->gfn;
		if (remove.i_off - b->i_off != start_delta)
			break;
		end_delta = b->gfn + b->nr - remove.gfn - remove.nr;

		ret = gunyah_gmem_trim_binding(b, start_delta, end_delta);
		break;
	}

	filemap_invalidate_unlock(inode->i_mapping);
	return ret;
}

static bool gunyah_gmem_binding_overlaps(struct gunyah_gmem_binding *a,
						struct gunyah_gmem_binding *b)
{
	/* assumes we are operating on the same file, check to be sure */
	BUG_ON(a->file != b->file);

	/**
	 * Gunyah only guarantees we can share a page with one VM and
	 * doesn't (currently) allow us to share same page with multiple VMs,
	 * regardless whether host can also access.
	 * Gunyah supports, but Linux hasn't implemented mapping same page
	 * into 2 separate addresses in guest's address space. This doesn't
	 * seem reasonable today, but we could do it later.
	 * All this to justify: check that the `a` region doesn't overlap with
	 * `b` region w.r.t. file offsets.
	 */
	if (a->i_off + a->nr <= b->i_off)
		return false;
	if (a->i_off >= b->i_off + b->nr)
		return false;

	return true;
}

static int gunyah_gmem_add_mapping(struct gunyah_vm *ghvm, struct file *file,
				   struct gunyah_map_mem_args *args)
{
	struct gunyah_gmem_binding *b, *tmp = NULL;
	struct inode *inode = file_inode(file);
	int ret;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	ret = gunyah_gmem_init_binding(ghvm, file, args, b);
	if (ret)
		return ret;

	/**
	 * When lending memory, we need to unmap single page from kernel's
	 * logical map. To do that, we need can_set_direct_map().
	 * arm64 doesn't map at page granularity without rodata=full.
	 */
	if (gunyah_guest_mem_is_lend(ghvm, b->flags) && !can_set_direct_map()) {
		kfree(b);
		pr_warn_once("Cannot lend memory without rodata=full");
		return -EINVAL;
	}

	/**
	 * First, check that the region of guets memfd user is binding isn't
	 * already bound to some other guest region.
	 */
	filemap_invalidate_lock(inode->i_mapping);
	list_for_each_entry(tmp, &inode->i_mapping->i_private_list, i_entry) {
		if (gunyah_gmem_binding_overlaps(b, tmp)) {
			ret = -EEXIST;
			goto unlock;
		}
	}

	/**
	 * mtree_insert_range will check that user hasn't mapped some other guest
	 * memfd region to the same addresses.
	 */
	ret = mtree_insert_range(&ghvm->bindings, b->gfn, b->gfn + b->nr - 1, b,
				 GFP_KERNEL);
	if (ret)
		goto unlock;

	list_add(&b->i_entry, &inode->i_mapping->i_private_list);

unlock:
	filemap_invalidate_unlock(inode->i_mapping);
	return ret;
}

int gunyah_gmem_modify_mapping(struct gunyah_vm *ghvm,
			       struct gunyah_map_mem_args *args)
{
	u8 access = args->flags & GUNYAH_MEM_ACCESS_MASK;
	struct file *file;
	int ret = -EINVAL;

	file = fget(args->guest_mem_fd);
	if (!file)
		return -EINVAL;

	if (file->f_op != &gunyah_gmem_fops)
		goto err_file;

	if (args->flags & ~(GUNYAH_MEM_ALLOW_RWX | GUNYAH_MEM_UNMAP | GUNYAH_MEM_ACCESS_MASK))
		goto err_file;

	/* VM needs to have some permissions to the memory */
	if (!(args->flags & GUNYAH_MEM_ALLOW_RWX))
		goto err_file;

	if (access != GUNYAH_MEM_DEFAULT_ACCESS &&
	    access != GUNYAH_MEM_FORCE_LEND && access != GUNYAH_MEM_FORCE_SHARE)
		goto err_file;

	if (!PAGE_ALIGNED(args->guest_addr) || !PAGE_ALIGNED(args->offset) ||
	    !PAGE_ALIGNED(args->size))
		goto err_file;

	if (args->flags & GUNYAH_MEM_UNMAP) {
		args->flags &= ~GUNYAH_MEM_UNMAP;
		ret = gunyah_gmem_remove_mapping(ghvm, file, args);
	} else {
		ret = gunyah_gmem_add_mapping(ghvm, file, args);
	}

err_file:
	if (ret)
		fput(file);
	return ret;
}
