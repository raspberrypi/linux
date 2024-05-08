// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah_vm_mgr: " fmt

#include <asm/gunyah.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#include "rsc_mgr.h"
#include "vm_mgr.h"

#define WRITE_TAG (1 << 0)
#define SHARE_TAG (1 << 1)

static inline struct gunyah_resource *
__first_resource(struct gunyah_vm_resource_ticket *ticket)
{
	return list_first_entry_or_null(&ticket->resources,
					struct gunyah_resource, list);
}

int gunyah_vm_parcel_to_paged(struct gunyah_vm *ghvm,
			      struct gunyah_rm_mem_parcel *parcel, u64 gfn,
			      u64 nr)
{
	struct gunyah_rm_mem_entry *entry;
	unsigned long i, tag = 0;
	struct folio *folio;
	pgoff_t off = 0;
	int ret;

	if (parcel->n_acl_entries > 1)
		tag |= SHARE_TAG;
	if (parcel->acl_entries[0].perms & GUNYAH_RM_ACL_W)
		tag |= WRITE_TAG;

	for (i = 0; i < parcel->n_mem_entries; i++) {
		entry = &parcel->mem_entries[i];

		folio = pfn_folio(PHYS_PFN(le64_to_cpu(entry->phys_addr)));
		ret = mtree_insert_range(&ghvm->mm, gfn + off,
					 gfn + off + folio_nr_pages(folio) - 1,
					 xa_tag_pointer(folio, tag),
					 GFP_KERNEL);
		if (ret) {
			WARN_ON(ret != -ENOMEM);
			gunyah_vm_mm_erase_range(ghvm, gfn, off - 1);
			return ret;
		}
		off += folio_nr_pages(folio);
	}
	BUG_ON(off != nr);

	return 0;
}

/**
 * gunyah_vm_mm_erase_range() - Erases a range of folios from ghvm's mm
 * @ghvm: gunyah vm
 * @gfn: start guest frame number
 * @nr: number of pages to erase
 *
 * Do not use this function unless rolling back gunyah_vm_parcel_to_paged.
 */
void gunyah_vm_mm_erase_range(struct gunyah_vm *ghvm, u64 gfn, u64 nr)
{
	struct folio *folio;
	u64 off = gfn;

	while (off < gfn + nr) {
		folio = xa_untag_pointer(mtree_erase(&ghvm->mm, off));
		if (!folio)
			return;
		off += folio_nr_pages(folio);
	}
}

static inline u32 donate_flags(bool share)
{
	if (share)
		return FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					GUNYAH_MEMEXTENT_DONATE_TO_SIBLING);
	else
		return FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					GUNYAH_MEMEXTENT_DONATE_TO_PROTECTED);
}

static inline u32 reclaim_flags(bool share, bool sync)
{
	u32 flags = 0;

	if (share)
		flags |= FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					  GUNYAH_MEMEXTENT_DONATE_TO_SIBLING);
	else
		flags |= FIELD_PREP_CONST(
			GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
			GUNYAH_MEMEXTENT_DONATE_FROM_PROTECTED);

	if (!sync)
		flags |= GUNYAH_MEMEXTENT_OPTION_NOSYNC;

	return flags;
}

int gunyah_vm_provide_folio(struct gunyah_vm *ghvm, struct folio *folio,
			    u64 gfn, bool share, bool write)
{
	struct gunyah_resource *guest_extent, *host_extent, *addrspace;
	u32 map_flags = BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PARTIAL);
	u64 extent_attrs, gpa = gunyah_gfn_to_gpa(gfn);
	phys_addr_t pa = PFN_PHYS(folio_pfn(folio));
	enum gunyah_pagetable_access access;
	size_t size = folio_size(folio);
	enum gunyah_error gunyah_error;
	unsigned long tag = 0;
	int ret, tmp;

	/* clang-format off */
	if (share) {
		guest_extent = __first_resource(&ghvm->guest_shared_extent_ticket);
		host_extent = __first_resource(&ghvm->host_shared_extent_ticket);
	} else {
		guest_extent = __first_resource(&ghvm->guest_private_extent_ticket);
		host_extent = __first_resource(&ghvm->host_private_extent_ticket);
	}
	/* clang-format on */
	addrspace = __first_resource(&ghvm->addrspace_ticket);

	if (!addrspace || !guest_extent || !host_extent) {
		return -ENODEV;
	}

	if (share) {
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_VMMIO);
		tag |= SHARE_TAG;
	} else {
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PRIVATE);
	}

	if (write)
		tag |= WRITE_TAG;

	ret = mtree_insert_range(&ghvm->mm, gfn,
				 gfn + folio_nr_pages(folio) - 1,
				 xa_tag_pointer(folio, tag), GFP_KERNEL);
	if (ret == -EEXIST)
		ret = -EAGAIN;
	if (ret)
		return ret;

	if (share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RW;
	else if (share && !write)
		access = GUNYAH_PAGETABLE_ACCESS_R;
	else if (!share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RWX;
	else /* !share && !write */
		access = GUNYAH_PAGETABLE_ACCESS_RX;

	ret = gunyah_rm_platform_pre_demand_page(ghvm->rm, ghvm->vmid, access,
						 folio);
	if (ret)
		goto reclaim_host;

	gunyah_error = gunyah_hypercall_memextent_donate(donate_flags(share),
							 host_extent->capid,
							 guest_extent->capid,
							 pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err("Failed to donate memory for guest address 0x%016llx: %d\n",
		       gpa, gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto platform_release;
	}

	extent_attrs =
		FIELD_PREP_CONST(GUNYAH_MEMEXTENT_MAPPING_TYPE,
				 ARCH_GUNYAH_DEFAULT_MEMTYPE) |
		FIELD_PREP(GUNYAH_MEMEXTENT_MAPPING_USER_ACCESS, access) |
		FIELD_PREP(GUNYAH_MEMEXTENT_MAPPING_KERNEL_ACCESS, access);
	gunyah_error = gunyah_hypercall_addrspace_map(addrspace->capid,
						      guest_extent->capid, gpa,
						      extent_attrs, map_flags,
						      pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err("Failed to map guest address 0x%016llx: %d\n", gpa,
		       gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto memextent_reclaim;
	}

	folio_get(folio);
	return 0;
memextent_reclaim:
	gunyah_error = gunyah_hypercall_memextent_donate(
		reclaim_flags(share, true), guest_extent->capid,
		host_extent->capid, pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK)
		pr_err("Failed to reclaim memory donation for guest address 0x%016llx: %d\n",
		       gpa, gunyah_error);
platform_release:
	tmp = gunyah_rm_platform_reclaim_demand_page(ghvm->rm, ghvm->vmid,
						     access, folio);
	if (tmp) {
		pr_err("Platform failed to reclaim memory for guest address 0x%016llx: %d",
		       gpa, tmp);
		return ret;
	}
reclaim_host:
	gunyah_folio_host_reclaim(folio);
	mtree_erase(&ghvm->mm, gfn);
	return ret;
}

static int __gunyah_vm_reclaim_folio_locked(struct gunyah_vm *ghvm, void *entry,
					    u64 gfn, const bool sync)
{
	u32 map_flags = BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PARTIAL);
	struct gunyah_resource *guest_extent, *host_extent, *addrspace;
	enum gunyah_pagetable_access access;
	enum gunyah_error gunyah_error;
	struct folio *folio;
	bool write, share;
	phys_addr_t pa;
	size_t size;
	int ret;

	addrspace = __first_resource(&ghvm->addrspace_ticket);
	if (!addrspace)
		return -ENODEV;

	share = !!(xa_pointer_tag(entry) & SHARE_TAG);
	write = !!(xa_pointer_tag(entry) & WRITE_TAG);
	folio = xa_untag_pointer(entry);

	if (!sync)
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_NOSYNC);

	/* clang-format off */
	if (share) {
		guest_extent = __first_resource(&ghvm->guest_shared_extent_ticket);
		host_extent = __first_resource(&ghvm->host_shared_extent_ticket);
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_VMMIO);
	} else {
		guest_extent = __first_resource(&ghvm->guest_private_extent_ticket);
		host_extent = __first_resource(&ghvm->host_private_extent_ticket);
		map_flags |= BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PRIVATE);
	}
	/* clang-format on */

	pa = PFN_PHYS(folio_pfn(folio));
	size = folio_size(folio);

	gunyah_error = gunyah_hypercall_addrspace_unmap(addrspace->capid,
							guest_extent->capid,
							gunyah_gfn_to_gpa(gfn),
							map_flags, pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err_ratelimited(
			"Failed to unmap guest address 0x%016llx: %d\n",
			gunyah_gfn_to_gpa(gfn), gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto err;
	}

	gunyah_error = gunyah_hypercall_memextent_donate(
		reclaim_flags(share, sync), guest_extent->capid,
		host_extent->capid, pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err_ratelimited(
			"Failed to reclaim memory donation for guest address 0x%016llx: %d\n",
			gunyah_gfn_to_gpa(gfn), gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto err;
	}

	if (share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RW;
	else if (share && !write)
		access = GUNYAH_PAGETABLE_ACCESS_R;
	else if (!share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RWX;
	else /* !share && !write */
		access = GUNYAH_PAGETABLE_ACCESS_RX;

	ret = gunyah_rm_platform_reclaim_demand_page(ghvm->rm, ghvm->vmid,
						     access, folio);
	if (ret) {
		pr_err_ratelimited(
			"Platform failed to reclaim memory for guest address 0x%016llx: %d",
			gunyah_gfn_to_gpa(gfn), ret);
		goto err;
	}

	BUG_ON(mtree_erase(&ghvm->mm, gfn) != entry);

	unpin_user_page(folio_page(folio, 0));
	account_locked_vm(current->mm, 1, false);
	return 0;
err:
	return ret;
}

int gunyah_vm_reclaim_folio(struct gunyah_vm *ghvm, u64 gfn, struct folio *folio)
{
	void *entry;

	entry = mtree_load(&ghvm->mm, gfn);
	if (!entry)
		return 0;

	if (folio != xa_untag_pointer(entry))
		return -EAGAIN;

	return __gunyah_vm_reclaim_folio_locked(ghvm, entry, gfn, true);
}

int gunyah_vm_reclaim_range(struct gunyah_vm *ghvm, u64 gfn, u64 nr)
{
	unsigned long next = gfn, g;
	struct folio *folio;
	int ret, ret2 = 0;
	void *entry;
	bool sync;

	mt_for_each(&ghvm->mm, entry, next, gfn + nr) {
		folio = xa_untag_pointer(entry);
		g = next;
		sync = !!mt_find_after(&ghvm->mm, &g, gfn + nr);

		g = next - folio_nr_pages(folio);
		folio_get(folio);
		folio_lock(folio);
		if (mtree_load(&ghvm->mm, g) == entry)
			ret = __gunyah_vm_reclaim_folio_locked(ghvm, entry, g, sync);
		else
			ret = -EAGAIN;
		folio_unlock(folio);
		folio_put(folio);
		if (ret && ret2 != -EAGAIN)
			ret2 = ret;
	}

	return ret2;
}

int gunyah_vm_binding_alloc(struct gunyah_vm *ghvm,
			    struct gunyah_userspace_memory_region *region,
			    bool lend)
{
	struct gunyah_vm_gup_binding *binding;
	int ret = 0;

	if (!region->memory_size || !PAGE_ALIGNED(region->memory_size) ||
		!PAGE_ALIGNED(region->userspace_addr) ||
		!PAGE_ALIGNED(region->guest_phys_addr))
		return -EINVAL;

	if (overflows_type(region->guest_phys_addr + region->memory_size, u64))
		return -EOVERFLOW;

	binding = kzalloc(sizeof(*binding), GFP_KERNEL_ACCOUNT);
	if (!binding) {
		return -ENOMEM;
	}

	binding->userspace_addr = region->userspace_addr;
	binding->guest_phys_addr = region->guest_phys_addr;
	binding->size = region->memory_size;
	binding->flags = region->flags;

	if (lend) {
		binding->share_type = VM_MEM_LEND;
	} else {
		binding->share_type = VM_MEM_SHARE;
	}
	down_write(&ghvm->bindings_lock);
	ret = mtree_insert_range(&ghvm->bindings,
				 gunyah_gpa_to_gfn(binding->guest_phys_addr),
				 gunyah_gpa_to_gfn(binding->guest_phys_addr + region->memory_size - 1),
				 binding, GFP_KERNEL);


	if(ret != 0)
		kfree(binding);

	up_write(&ghvm->bindings_lock);

	return ret;
}

int gunyah_gup_demand_page(struct gunyah_vm *ghvm, u64 gpa, bool write)
{
	unsigned long gfn = gunyah_gpa_to_gfn(gpa);
	struct gunyah_vm_gup_binding *b;
	unsigned int gup_flags;
	u64 offset;
	int pinned, ret;
	struct page *page;
	struct folio *folio;

	down_read(&ghvm->bindings_lock);
	b = mtree_load(&ghvm->bindings, gfn);
	if (!b) {
		ret = -ENOENT;
		goto unlock;
	}

	if (write && !(b->flags & GUNYAH_MEM_ALLOW_WRITE)) {
		ret = -EPERM;
		goto unlock;
	}
	gup_flags = FOLL_LONGTERM;
	if (b->flags & GUNYAH_MEM_ALLOW_WRITE)
		gup_flags |= FOLL_WRITE;

	offset =  (gunyah_gfn_to_gpa(gfn) - b->guest_phys_addr);

	ret = account_locked_vm(current->mm, 1, true);
	if (ret)
		goto unlock;

	pinned = pin_user_pages_fast(b->userspace_addr + offset, 1,
					gup_flags, &page);

	if (pinned != 1) {
		ret = pinned;
		goto unlock_page;
	}

	folio = page_folio(page);

	if (!PageSwapBacked(page)) {
		ret = -EIO;
		goto unpin_page;
	}

	folio_lock(folio);
	ret = gunyah_vm_provide_folio(ghvm, folio, gfn - folio_page_idx(folio, page),
				      !(b->share_type == VM_MEM_LEND),
				      !!(b->flags & GUNYAH_MEM_ALLOW_WRITE));
	folio_unlock(folio);
	if (ret) {
		if (ret != -EAGAIN)
			pr_err_ratelimited(
				"Failed to provide folio for guest addr: %016llx: %d\n",
				gpa, ret);
		goto unpin_page;
	}
	goto unlock;

unpin_page:
	unpin_user_page(page);
unlock_page:
	account_locked_vm(current->mm, 1, false);
unlock:
	up_read(&ghvm->bindings_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_gup_demand_page);


int gunyah_gup_share_parcel(struct gunyah_vm *ghvm, struct gunyah_rm_mem_parcel *parcel,
			     u64 *gfn, u64 *nr)
{
	struct gunyah_vm_gup_binding *b;
	bool lend = false;
	struct page **pages;
	int pinned, ret;
	u16 vmid;
	struct folio *folio;
	unsigned int gup_flags;
	unsigned long i, offset;

	parcel->mem_handle = GUNYAH_MEM_HANDLE_INVAL;

	if (!*nr)
		return -EINVAL;
	down_read(&ghvm->bindings_lock);
	b = mtree_load(&ghvm->bindings, *gfn);
	if (!b) {
		ret = -ENOENT;
		goto unlock;
	}

	offset = gunyah_gfn_to_gpa(*gfn) - b->guest_phys_addr;
	pages = kcalloc(*nr, sizeof(*pages), GFP_KERNEL_ACCOUNT);
	if (!pages) {
		ret = -ENOMEM;
		goto unlock;
	}

	gup_flags = FOLL_LONGTERM;
	if (b->flags & GUNYAH_MEM_ALLOW_WRITE)
		gup_flags |= FOLL_WRITE;

	pinned = pin_user_pages_fast(b->userspace_addr + offset, *nr,
			gup_flags, pages);
	if (pinned < 0) {
		ret = pinned;
		goto free_pages;
	} else if (pinned != *nr) {
		ret = -EFAULT;
		goto unpin_pages;
	}

	ret = account_locked_vm(current->mm, pinned, true);
	if (ret)
		goto unpin_pages;

	if (b->share_type == VM_MEM_LEND) {
		parcel->n_acl_entries = 1;
		lend = true;
	} else {
		lend = false;
		parcel->n_acl_entries = 2;
	}
	parcel->acl_entries = kcalloc(parcel->n_acl_entries,
				      sizeof(*parcel->acl_entries), GFP_KERNEL);
	if (!parcel->acl_entries) {
		ret = -ENOMEM;
		goto unaccount_pages;
	}

	/* acl_entries[0].vmid will be this VM's vmid. We'll fill it when the
	 * VM is starting and we know the VM's vmid.
	 */
	parcel->acl_entries[0].vmid = cpu_to_le16(ghvm->vmid);
	if (b->flags & GUNYAH_MEM_ALLOW_READ)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_R;
	if (b->flags & GUNYAH_MEM_ALLOW_WRITE)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_W;
	if (b->flags & GUNYAH_MEM_ALLOW_EXEC)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_X;

	if (!lend) {
		ret = gunyah_rm_get_vmid(ghvm->rm, &vmid);
		if (ret)
			goto free_acl;

		parcel->acl_entries[1].vmid = cpu_to_le16(vmid);
		/* Host assumed to have all these permissions. Gunyah will not
		* grant new permissions if host actually had less than RWX
		*/
		parcel->acl_entries[1].perms = GUNYAH_RM_ACL_R | GUNYAH_RM_ACL_W | GUNYAH_RM_ACL_X;
	}

	parcel->mem_entries = kcalloc(pinned, sizeof(parcel->mem_entries[0]),
					GFP_KERNEL_ACCOUNT);
	if (!parcel->mem_entries) {
		ret = -ENOMEM;
		goto free_acl;
	}
	folio = page_folio(pages[0]);
	*gfn -= folio_page_idx(folio, pages[0]);
	parcel->mem_entries[0].size = cpu_to_le64(folio_size(folio));
	parcel->mem_entries[0].phys_addr = cpu_to_le64(PFN_PHYS(folio_pfn(folio)));

	for (i = 1; i < pinned; i++) {
		folio = page_folio(pages[i]);
		if (pages[i] == folio_page(folio, 0)) {
			parcel->mem_entries[i].size = cpu_to_le64(folio_size(folio));
			parcel->mem_entries[i].phys_addr = cpu_to_le64(PFN_PHYS(folio_pfn(folio)));
		} else {
			unpin_user_page(pages[i]);
			account_locked_vm(current->mm, 1, false);
		}
	}
	parcel->n_mem_entries = i;
	ret = gunyah_rm_mem_share(ghvm->rm, parcel);
	goto free_pages;

free_acl:
	kfree(parcel->acl_entries);
	parcel->acl_entries = NULL;
	kfree(parcel->mem_entries);
	parcel->mem_entries = NULL;
	parcel->n_mem_entries = 0;
unaccount_pages:
	account_locked_vm(current->mm, pinned, false);
unpin_pages:
	unpin_user_pages(pages, pinned);
free_pages:
	kfree(pages);
unlock:
	up_read(&ghvm->bindings_lock);
	return ret;
}
