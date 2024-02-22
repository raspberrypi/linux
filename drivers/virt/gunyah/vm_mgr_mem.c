// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah_vm_mgr: " fmt

#include <asm/gunyah.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

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

static inline u32 reclaim_flags(bool share)
{
	if (share)
		return FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					GUNYAH_MEMEXTENT_DONATE_TO_SIBLING);
	else
		return FIELD_PREP_CONST(GUNYAH_MEMEXTENT_OPTION_TYPE_MASK,
					GUNYAH_MEMEXTENT_DONATE_FROM_PROTECTED);
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
	int ret;

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
		folio_unlock(folio);
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

	/* don't lend a folio that is (or could be) mapped by Linux */
	if (!share && !gunyah_folio_lend_safe(folio)) {
		ret = -EPERM;
		goto remove;
	}

	if (share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RW;
	else if (share && !write)
		access = GUNYAH_PAGETABLE_ACCESS_R;
	else if (!share && write)
		access = GUNYAH_PAGETABLE_ACCESS_RWX;
	else /* !share && !write */
		access = GUNYAH_PAGETABLE_ACCESS_RX;

	gunyah_error = gunyah_hypercall_memextent_donate(donate_flags(share),
							 host_extent->capid,
							 guest_extent->capid,
							 pa, size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err("Failed to donate memory for guest address 0x%016llx: %d\n",
		       gpa, gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto remove;
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
	if (!share)
		folio_set_private(folio);
	return 0;
memextent_reclaim:
	gunyah_error = gunyah_hypercall_memextent_donate(reclaim_flags(share),
							 guest_extent->capid,
							 host_extent->capid, pa,
							 size);
	if (gunyah_error != GUNYAH_ERROR_OK)
		pr_err("Failed to reclaim memory donation for guest address 0x%016llx: %d\n",
		       gpa, gunyah_error);
remove:
	mtree_erase(&ghvm->mm, gfn);
	return ret;
}

static int __gunyah_vm_reclaim_folio_locked(struct gunyah_vm *ghvm, void *entry,
					    u64 gfn, const bool sync)
{
	u32 map_flags = BIT(GUNYAH_ADDRSPACE_MAP_FLAG_PARTIAL);
	struct gunyah_resource *guest_extent, *host_extent, *addrspace;
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

	gunyah_error = gunyah_hypercall_memextent_donate(reclaim_flags(share),
							 guest_extent->capid,
							 host_extent->capid, pa,
							 size);
	if (gunyah_error != GUNYAH_ERROR_OK) {
		pr_err_ratelimited(
			"Failed to reclaim memory donation for guest address 0x%016llx: %d\n",
			gunyah_gfn_to_gpa(gfn), gunyah_error);
		ret = gunyah_error_remap(gunyah_error);
		goto err;
	}

	BUG_ON(mtree_erase(&ghvm->mm, gfn) != entry);

	if (folio_test_private(folio)) {
		gunyah_folio_host_reclaim(folio);
		folio_clear_private(folio);
	}

	folio_put(folio);
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
