/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _GUNYAH_VM_MGR_PRIV_H
#define _GUNYAH_VM_MGR_PRIV_H

#include <linux/device.h>
#include <linux/kref.h>
#include <linux/maple_tree.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/set_memory.h>
#include <linux/wait.h>

#include <uapi/linux/gunyah.h>

#include "rsc_mgr.h"

enum gunyah_vm_mem_share_type {
	VM_MEM_SHARE,
	VM_MEM_LEND,
};

struct gunyah_vm_gup_binding {
	enum gunyah_vm_mem_share_type share_type;
	u64 guest_phys_addr;
	u64 userspace_addr;
	u64 size;
	u32 flags;
};

static inline u64 gunyah_gpa_to_gfn(u64 gpa)
{
	return gpa >> PAGE_SHIFT;
}

static inline u64 gunyah_gfn_to_gpa(u64 gfn)
{
	return gfn << PAGE_SHIFT;
}

long gunyah_dev_vm_mgr_ioctl(struct gunyah_rm *rm, unsigned int cmd,
			     unsigned long arg);

/**
 * struct gunyah_vm - Main representation of a Gunyah Virtual machine
 * @vmid: Gunyah's VMID for this virtual machine
 * @mm: A maple tree of all memory that has been mapped to a VM.
 *      Indices are guest frame numbers; entries are either folios or
 *      RM mem parcels
 * @bindings: A maple tree of guest memfd bindings. Indices are guest frame
 *            numbers; entries are &struct gunyah_gmem_binding
 * @bindings_lock: For serialization to @bindings
 * @mm_s: Userspace tied to this vm
 * @addrspace_ticket: Resource ticket to the capability for guest VM's
 *                    address space
 * @host_private_extent_ticket: Resource ticket to the capability for our
 *                              memory extent from which to lend private
 *                              memory to the guest
 * @host_shared_extent_ticket: Resource ticket to the capaiblity for our
 *                             memory extent from which to share memory
 *                             with the guest. Distinction with
 *                             @host_private_extent_ticket needed for
 *                             current Qualcomm platforms; on non-Qualcomm
 *                             platforms, this is the same capability ID
 * @guest_private_extent_ticket: Resource ticket to the capaiblity for
 *                               the guest's memory extent to lend private
 *                               memory to
 * @guest_shared_extent_ticket: Resource ticket to the capability for
 *                              the memory extent that represents
 *                              memory shared with the guest.
 * @mmio_handler_root: RB tree of MMIO handlers.
 *                     Entries are &struct gunyah_vm_io_handler
 * @mmio_handler_lock: Serialization of traversing @mmio_handler_root
 * @rm: Pointer to the resource manager struct to make RM calls
 * @parent: For logging
 * @nb: Notifier block for RM notifications
 * @vm_status: Current state of the VM, as last reported by RM
 * @vm_status_wait: Wait queue for status @vm_status changes
 * @status_lock: Serializing state transitions
 * @exit_info: Breadcrumbs why VM is not running anymore
 * @kref: Reference counter for VM functions
 * @fn_lock: Serialization addition of functions
 * @functions: List of &struct gunyah_vm_function_instance that have been
 *             created by user for this VM.
 * @resource_lock: Serializing addition of resources and resource tickets
 * @resources: List of &struct gunyah_resource that are associated with this VM
 * @resource_tickets: List of &struct gunyah_vm_resource_ticket
 * @auth: Authentication mechanism to be used by resource manager when
 *        launching the VM
 * @dtb: For tracking dtb configuration when launching the VM
 * @dtb.config: Location of the DTB in the guest memory
 * @dtb.parcel_start: Guest frame number where the memory parcel that we lent to
 *                    VM (DTB could start in middle of folio; we lend entire
 *                    folio; parcel_start is start of the folio)
 * @dtb.parcel_pages: Number of pages lent for the memory parcel
 * @dtb.parcel: Data for resource manager to lend the parcel
 * @boot_context: Requested initial boot context to set when launching the VM
 *
 * Members are grouped by hot path.
 */
struct gunyah_vm {
	u16 vmid;
	struct maple_tree mm;
	struct maple_tree bindings;
	struct rw_semaphore bindings_lock;
	struct mm_struct *mm_s;
	struct gunyah_vm_resource_ticket addrspace_ticket,
		host_private_extent_ticket, host_shared_extent_ticket,
		guest_private_extent_ticket, guest_shared_extent_ticket;
	struct rb_root mmio_handler_root;
	struct rw_semaphore mmio_handler_lock;

	struct gunyah_rm *rm;

	struct notifier_block nb;
	enum gunyah_rm_vm_status vm_status;
	wait_queue_head_t vm_status_wait;
	struct rw_semaphore status_lock;
	struct gunyah_vm_exit_info exit_info;

	struct kref kref;
	struct mutex fn_lock;
	struct list_head functions;
	struct mutex resources_lock;
	struct list_head resources;
	struct list_head resource_tickets;

	struct device *parent;
	enum gunyah_rm_vm_auth_mechanism auth;
	struct {
		struct gunyah_vm_dtb_config config;
		u64 parcel_start, parcel_pages;
		struct gunyah_rm_mem_parcel parcel;
	} dtb;
	struct {
		struct gunyah_vm_firmware_config config;
		u64 parcel_start, parcel_pages;
		struct gunyah_rm_mem_parcel parcel;
	} fw;
	struct xarray boot_context;
};

int gunyah_vm_mmio_write(struct gunyah_vm *ghvm, u64 addr, u32 len, u64 data);

/**
 * folio_mmapped() - Returns true if the folio is mapped into any vma
 * @folio: Folio to test
 */
static bool folio_mmapped(struct folio *folio)
{
	struct address_space *mapping = folio->mapping;
	struct vm_area_struct *vma;
	bool ret = false;

	i_mmap_lock_read(mapping);
	vma_interval_tree_foreach(vma, &mapping->i_mmap, folio_index(folio),
				  folio_index(folio) + folio_nr_pages(folio)) {
		ret = true;
		break;
	}
	i_mmap_unlock_read(mapping);
	return ret;
}

/**
 * gunyah_folio_lend_safe() - Returns true if folio is ready to be lent to guest
 * @folio: Folio to prepare
 *
 * Tests if the folio is mapped anywhere outside the kernel logical map
 * and whether any userspace has a vma containing the folio, even if it hasn't
 * paged it in. We want to avoid causing fault to userspace.
 * If userspace doesn't have it mapped anywhere, then unmap from kernel
 * logical map to prevent accidental access (e.g. by load_unaligned_zeropad)
 */
static inline bool gunyah_folio_lend_safe(struct folio *folio)
{
	long i;

	if (folio_mapped(folio) || folio_mmapped(folio))
		return false;

	for (i = 0; i < folio_nr_pages(folio); i++)
		set_direct_map_invalid_noflush(folio_page(folio, i));
	/**
	 * No need to flush tlb on armv8/9: hypervisor will flush when it
	 * removes from our stage 2
	 */
	return true;
}

/**
 * gunyah_folio_host_reclaim() - Restores kernel logical map to folio
 * @folio: folio to reclaim by host
 *
 * See also gunyah_folio_lend_safe().
 */
static inline void gunyah_folio_host_reclaim(struct folio *folio)
{
	long i;
	for (i = 0; i < folio_nr_pages(folio); i++)
		set_direct_map_default_noflush(folio_page(folio, i));
}

int gunyah_vm_parcel_to_paged(struct gunyah_vm *ghvm,
			      struct gunyah_rm_mem_parcel *parcel, u64 gfn,
			      u64 nr);
void gunyah_vm_mm_erase_range(struct gunyah_vm *ghvm, u64 gfn, u64 nr);
int gunyah_vm_reclaim_parcel(struct gunyah_vm *ghvm,
			     struct gunyah_rm_mem_parcel *parcel, u64 gfn);
int gunyah_vm_provide_folio(struct gunyah_vm *ghvm, struct folio *folio,
			    u64 gfn, bool share, bool write);
int gunyah_vm_reclaim_folio(struct gunyah_vm *ghvm, u64 gfn, struct folio *folio);
int gunyah_vm_reclaim_range(struct gunyah_vm *ghvm, u64 gfn, u64 nr);

int gunyah_vm_binding_alloc(struct gunyah_vm *ghvm,
			    struct gunyah_userspace_memory_region *region,
			    bool lend);
int gunyah_gup_setup_demand_paging(struct gunyah_vm *ghvm);
int gunyah_gup_share_parcel(struct gunyah_vm *ghvm,
			      struct gunyah_rm_mem_parcel *parcel,
			      u64 *gfn, u64 *nr);
int gunyah_gup_demand_page(struct gunyah_vm *ghvm, u64 gpa, bool write);

#endif
