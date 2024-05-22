// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/soc/mediatek/gzvm_drv.h>

static int cmp_ppages(struct rb_node *node, const struct rb_node *parent)
{
	struct gzvm_pinned_page *a = container_of(node,
						  struct gzvm_pinned_page,
						  node);
	struct gzvm_pinned_page *b = container_of(parent,
						  struct gzvm_pinned_page,
						  node);

	if (a->ipa < b->ipa)
		return -1;
	if (a->ipa > b->ipa)
		return 1;
	return 0;
}

/* Invoker of this function is responsible for locking */
static int gzvm_insert_ppage(struct gzvm *vm, struct gzvm_pinned_page *ppage)
{
	if (rb_find_add(&ppage->node, &vm->pinned_pages, cmp_ppages))
		return -EEXIST;
	return 0;
}

static int rb_ppage_cmp(const void *key, const struct rb_node *node)
{
	struct gzvm_pinned_page *p = container_of(node,
						  struct gzvm_pinned_page,
						  node);
	phys_addr_t ipa = (phys_addr_t)key;

	return (ipa < p->ipa) ? -1 : (ipa > p->ipa);
}

/* Invoker of this function is responsible for locking */
static int gzvm_remove_ppage(struct gzvm *vm, phys_addr_t ipa)
{
	struct gzvm_pinned_page *ppage;
	struct rb_node *node;

	node = rb_find((void *)ipa, &vm->pinned_pages, rb_ppage_cmp);

	if (node)
		rb_erase(node, &vm->pinned_pages);
	else
		return 0;

	ppage = container_of(node, struct gzvm_pinned_page, node);
	unpin_user_pages_dirty_lock(&ppage->page, 1, true);
	kfree(ppage);

	return 0;
}

static int pin_one_page(struct gzvm *vm, unsigned long hva, u64 gpa,
			struct page **out_page)
{
	unsigned int flags = FOLL_HWPOISON | FOLL_LONGTERM | FOLL_WRITE;
	struct gzvm_pinned_page *ppage = NULL;
	struct mm_struct *mm = current->mm;
	struct page *page = NULL;
	int ret;

	ppage = kmalloc(sizeof(*ppage), GFP_KERNEL_ACCOUNT);
	if (!ppage)
		return -ENOMEM;

	mmap_read_lock(mm);
	ret = pin_user_pages(hva, 1, flags, &page);
	mmap_read_unlock(mm);

	if (ret != 1 || !page) {
		kfree(ppage);
		return -EFAULT;
	}

	ppage->page = page;
	ppage->ipa = gpa;

	mutex_lock(&vm->mem_lock);
	ret = gzvm_insert_ppage(vm, ppage);

	/**
	 * The return of -EEXIST from gzvm_insert_ppage is considered an
	 * expected behavior in this context.
	 * This situation arises when two or more VCPUs are concurrently
	 * engaged in demand paging handling. The initial VCPU has already
	 * allocated and pinned a page, while the subsequent VCPU attempts
	 * to pin the same page again. As a result, we prompt the unpinning
	 * and release of the allocated structure, followed by a return 0.
	 */
	if (ret == -EEXIST) {
		kfree(ppage);
		unpin_user_pages(&page, 1);
		ret = 0;
	}
	mutex_unlock(&vm->mem_lock);
	*out_page = page;

	return ret;
}

/**
 * gzvm_handle_relinquish() - Handle memory relinquish request from hypervisor
 *
 * @vcpu: Pointer to struct gzvm_vcpu_run in userspace
 * @ipa: Start address(gpa) of a reclaimed page
 *
 * Return: Always return 0 because there are no cases of failure
 */
int gzvm_handle_relinquish(struct gzvm_vcpu *vcpu, phys_addr_t ipa)
{
	struct gzvm *vm = vcpu->gzvm;

	mutex_lock(&vm->mem_lock);
	gzvm_remove_ppage(vm, ipa);
	mutex_unlock(&vm->mem_lock);

	return 0;
}

int gzvm_vm_allocate_guest_page(struct gzvm *vm, struct gzvm_memslot *slot,
				u64 gfn, u64 *pfn)
{
	struct page *page = NULL;
	unsigned long hva;
	int ret;

	if (gzvm_gfn_to_hva_memslot(slot, gfn, (u64 *)&hva) != 0)
		return -EINVAL;

	ret = pin_one_page(vm, hva, PFN_PHYS(gfn), &page);
	if (ret != 0)
		return ret;

	if (page == NULL)
		return -EFAULT;
	/**
	 * As `pin_user_pages` already gets the page struct, we don't need to
	 * call other APIs to reduce function call overhead.
	 */
	*pfn = page_to_pfn(page);

	return 0;
}

static int handle_single_demand_page(struct gzvm *vm, int memslot_id, u64 gfn)
{
	int ret;
	u64 pfn;

	ret = gzvm_vm_allocate_guest_page(vm, &vm->memslot[memslot_id], gfn, &pfn);
	if (unlikely(ret))
		return -EFAULT;

	ret = gzvm_arch_map_guest(vm->vm_id, memslot_id, pfn, gfn, 1);
	if (unlikely(ret))
		return -EFAULT;

	return ret;
}

static int handle_block_demand_page(struct gzvm *vm, int memslot_id, u64 gfn)
{
	u64 pfn, __gfn;
	int ret, i;

	u32 nr_entries = GZVM_BLOCK_BASED_DEMAND_PAGE_SIZE / PAGE_SIZE;
	struct gzvm_memslot *memslot = &vm->memslot[memslot_id];
	u64 start_gfn = ALIGN_DOWN(gfn, nr_entries);
	u32 total_pages = memslot->npages;
	u64 base_gfn = memslot->base_gfn;

	/*
	 * If the start/end gfn of this demand paging block is outside the
	 * memory region of memslot, adjust the start_gfn/nr_entries.
	 */
	if (start_gfn < base_gfn)
		start_gfn = base_gfn;

	if (start_gfn + nr_entries > base_gfn + total_pages)
		nr_entries = base_gfn + total_pages - start_gfn;

	mutex_lock(&vm->demand_paging_lock);
	for (i = 0, __gfn = start_gfn; i < nr_entries; i++, __gfn++) {
		ret = gzvm_vm_allocate_guest_page(vm, memslot, __gfn, &pfn);
		if (unlikely(ret)) {
			pr_notice("VM-%u failed to allocate page for GFN 0x%llx (%d)\n",
				  vm->vm_id, __gfn, ret);
			ret = -ERR_FAULT;
			goto err_unlock;
		}
		vm->demand_page_buffer[i] = pfn;
	}

	ret = gzvm_arch_map_guest_block(vm->vm_id, memslot_id, start_gfn,
					nr_entries);
	if (unlikely(ret)) {
		ret = -EFAULT;
		goto err_unlock;
	}

err_unlock:
	mutex_unlock(&vm->demand_paging_lock);

	return ret;
}

/**
 * gzvm_handle_page_fault() - Handle guest page fault, find corresponding page
 *                            for the faulting gpa
 * @vcpu: Pointer to struct gzvm_vcpu_run of the faulting vcpu
 *
 * Return:
 * * 0		- Success to handle guest page fault
 * * -EFAULT	- Failed to map phys addr to guest's GPA
 */
int gzvm_handle_page_fault(struct gzvm_vcpu *vcpu)
{
	struct gzvm *vm = vcpu->gzvm;
	int memslot_id;
	u64 gfn;

	gfn = PHYS_PFN(vcpu->run->exception.fault_gpa);
	memslot_id = gzvm_find_memslot(vm, gfn);
	if (unlikely(memslot_id < 0))
		return -EFAULT;

	if (unlikely(vm->mem_alloc_mode == GZVM_FULLY_POPULATED))
		return -EFAULT;

	if (vm->demand_page_gran == PAGE_SIZE)
		return handle_single_demand_page(vm, memslot_id, gfn);
	else
		return handle_block_demand_page(vm, memslot_id, gfn);
}
