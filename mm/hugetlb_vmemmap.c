// SPDX-License-Identifier: GPL-2.0
/*
 * HugeTLB Vmemmap Optimization (HVO)
 *
 * Copyright (c) 2020, ByteDance. All rights reserved.
 *
 *     Author: Muchun Song <songmuchun@bytedance.com>
 *
 * See Documentation/mm/vmemmap_dedup.rst
 */
#define pr_fmt(fmt)	"HugeTLB: " fmt

#include <linux/pgtable.h>
#include <linux/moduleparam.h>
#include <linux/bootmem_info.h>
#include <linux/mmdebug.h>
#include <linux/pagewalk.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include "hugetlb_vmemmap.h"

/**
 * struct vmemmap_remap_walk - walk vmemmap page table
 *
 * @remap_pte:		called for each lowest-level entry (PTE).
 * @nr_walked:		the number of walked pte.
 * @vmemmap_head:	the page to be installed as first in the vmemmap range
 * @vmemmap_tail:	the page to be installed as non-first in the vmemmap range
 * @vmemmap_pages:	the list head of the vmemmap pages that can be freed
 *			or is mapped from.
 * @flags:		used to modify behavior in vmemmap page table walking
 *			operations.
 */
struct vmemmap_remap_walk {
	void			(*remap_pte)(pte_t *pte, unsigned long addr,
					     struct vmemmap_remap_walk *walk);

	unsigned long		nr_walked;
	struct page		*vmemmap_head;
	struct page		*vmemmap_tail;
	struct list_head	*vmemmap_pages;


/* Skip the TLB flush when we split the PMD */
#define VMEMMAP_SPLIT_NO_TLB_FLUSH	BIT(0)
/* Skip the TLB flush when we remap the PTE */
#define VMEMMAP_REMAP_NO_TLB_FLUSH	BIT(1)
/* synchronize_rcu() to avoid writes from page_ref_add_unless() */
#define VMEMMAP_SYNCHRONIZE_RCU		BIT(2)
	unsigned long		flags;
};

static int vmemmap_split_pmd(pmd_t *pmd, struct page *head, unsigned long start,
			     struct vmemmap_remap_walk *walk)
{
	pmd_t __pmd;
	int i;
	unsigned long addr = start;
	pte_t *pgtable;

	pgtable = pte_alloc_one_kernel(&init_mm);
	if (!pgtable)
		return -ENOMEM;

	pmd_populate_kernel(&init_mm, &__pmd, pgtable);

	for (i = 0; i < PTRS_PER_PTE; i++, addr += PAGE_SIZE) {
		pte_t entry, *pte;
		pgprot_t pgprot = PAGE_KERNEL;

		entry = mk_pte(head + i, pgprot);
		pte = pte_offset_kernel(&__pmd, addr);
		set_pte_at(&init_mm, addr, pte, entry);
	}

	spin_lock(&init_mm.page_table_lock);
	if (likely(pmd_leaf(*pmd))) {
		/*
		 * Higher order allocations from buddy allocator must be able to
		 * be treated as indepdenent small pages (as they can be freed
		 * individually).
		 */
		if (!PageReserved(head))
			split_page(head, get_order(PMD_SIZE));

		/* Make pte visible before pmd. See comment in pmd_install(). */
		smp_wmb();
		pmd_populate_kernel(&init_mm, pmd, pgtable);
		if (!(walk->flags & VMEMMAP_SPLIT_NO_TLB_FLUSH))
			flush_tlb_kernel_range(start, start + PMD_SIZE);
	} else {
		pte_free_kernel(&init_mm, pgtable);
	}
	spin_unlock(&init_mm.page_table_lock);

	return 0;
}

static int vmemmap_pmd_entry(pmd_t *pmd, unsigned long addr,
			     unsigned long next, struct mm_walk *walk)
{
	int ret = 0;
	struct page *head;
	struct vmemmap_remap_walk *vmemmap_walk = walk->private;

	/* Only splitting, not remapping the vmemmap pages. */
	if (!vmemmap_walk->remap_pte)
		walk->action = ACTION_CONTINUE;

	spin_lock(&init_mm.page_table_lock);
	head = pmd_leaf(*pmd) ? pmd_page(*pmd) : NULL;
	/*
	 * Due to HugeTLB alignment requirements and the vmemmap
	 * pages being at the start of the hotplugged memory
	 * region in memory_hotplug.memmap_on_memory case. Checking
	 * the vmemmap page associated with the first vmemmap page
	 * if it is self-hosted is sufficient.
	 *
	 * [                  hotplugged memory                  ]
	 * [        section        ][...][        section        ]
	 * [ vmemmap ][              usable memory               ]
	 *   ^  | ^                        |
	 *   +--+ |                        |
	 *        +------------------------+
	 */
	if (IS_ENABLED(CONFIG_MEMORY_HOTPLUG) && unlikely(!vmemmap_walk->nr_walked)) {
		struct page *page = head ? head + pte_index(addr) :
				    pte_page(ptep_get(pte_offset_kernel(pmd, addr)));

		if (PageVmemmapSelfHosted(page))
			ret = -ENOTSUPP;
	}
	spin_unlock(&init_mm.page_table_lock);
	if (!head || ret)
		return ret;

	return vmemmap_split_pmd(pmd, head, addr & PMD_MASK, vmemmap_walk);
}

static int vmemmap_pte_entry(pte_t *pte, unsigned long addr,
			     unsigned long next, struct mm_walk *walk)
{
	struct vmemmap_remap_walk *vmemmap_walk = walk->private;

	vmemmap_walk->remap_pte(pte, addr, vmemmap_walk);
	vmemmap_walk->nr_walked++;

	return 0;
}

static const struct mm_walk_ops vmemmap_remap_ops = {
	.pmd_entry	= vmemmap_pmd_entry,
	.pte_entry	= vmemmap_pte_entry,
};

static int vmemmap_remap_range(unsigned long start, unsigned long end,
			       struct vmemmap_remap_walk *walk)
{
	int ret;

	VM_BUG_ON(!PAGE_ALIGNED(start | end));

	mmap_read_lock(&init_mm);
	ret = walk_kernel_page_table_range(start, end, &vmemmap_remap_ops,
				    NULL, walk);
	mmap_read_unlock(&init_mm);
	if (ret)
		return ret;

	if (walk->remap_pte && !(walk->flags & VMEMMAP_REMAP_NO_TLB_FLUSH))
		flush_tlb_kernel_range(start, end);

	return 0;
}

/*
 * Free a vmemmap page. A vmemmap page can be allocated from the memblock
 * allocator or buddy allocator. If the PG_reserved flag is set, it means
 * that it allocated from the memblock allocator, just free it via the
 * free_bootmem_page(). Otherwise, use __free_page().
 */
static inline void free_vmemmap_page(struct page *page)
{
	if (PageReserved(page)) {
		memmap_boot_pages_add(-1);
		free_bootmem_page(page);
	} else {
		memmap_pages_add(-1);
		__free_page(page);
	}
}

/* Free a list of the vmemmap pages */
static void free_vmemmap_page_list(struct list_head *list)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, list, lru)
		free_vmemmap_page(page);
}

static void vmemmap_remap_pte(pte_t *pte, unsigned long addr,
			      struct vmemmap_remap_walk *walk)
{
	struct page *page = pte_page(ptep_get(pte));
	pte_t entry;

	/* Remapping the head page requires r/w */
	if (unlikely(walk->nr_walked == 0 && walk->vmemmap_head)) {
		list_del(&walk->vmemmap_head->lru);

		/*
		 * Makes sure that preceding stores to the page contents from
		 * vmemmap_remap_free() become visible before the set_pte_at()
		 * write.
		 */
		smp_wmb();

		entry = mk_pte(walk->vmemmap_head, PAGE_KERNEL);
	} else {
		/*
		 * Remap the tail pages as read-only to catch illegal write
		 * operation to the tail pages.
		 */
		entry = mk_pte(walk->vmemmap_tail, PAGE_KERNEL_RO);
	}

	list_add(&page->lru, walk->vmemmap_pages);
	set_pte_at(&init_mm, addr, pte, entry);
}

static void vmemmap_restore_pte(pte_t *pte, unsigned long addr,
				struct vmemmap_remap_walk *walk)
{
	struct page *page;
	struct page *from, *to;

	page = list_first_entry(walk->vmemmap_pages, struct page, lru);
	list_del(&page->lru);

	/*
	 * Initialize tail pages in the newly allocated vmemmap page.
	 *
	 * There is folio-scope metadata that is encoded in the first few
	 * tail pages.
	 *
	 * Use the value last tail page in the page with the head page
	 * to initialize the rest of tail pages.
	 */
	from = compound_head((struct page *)addr) +
		PAGE_SIZE / sizeof(struct page) - 1;
	to = page_to_virt(page);
	for (int i = 0; i < PAGE_SIZE / sizeof(struct page); i++, to++)
		*to = *from;

	/*
	 * Makes sure that preceding stores to the page contents become visible
	 * before the set_pte_at() write.
	 */
	smp_wmb();
	set_pte_at(&init_mm, addr, pte, mk_pte(page, PAGE_KERNEL));
}

/**
 * vmemmap_remap_split - split the vmemmap virtual address range [@start, @end)
 *                      backing PMDs of the directmap into PTEs
 * @start:     start address of the vmemmap virtual address range that we want
 *             to remap.
 * @end:       end address of the vmemmap virtual address range that we want to
 *             remap.
 * Return: %0 on success, negative error code otherwise.
 */
static int vmemmap_remap_split(unsigned long start, unsigned long end)
{
	struct vmemmap_remap_walk walk = {
		.remap_pte	= NULL,
		.flags		= VMEMMAP_SPLIT_NO_TLB_FLUSH,
	};

	return vmemmap_remap_range(start, end, &walk);
}

/**
 * vmemmap_remap_free - remap the vmemmap virtual address range [@start, @end)
 *			to use @vmemmap_head/tail, then free vmemmap which
 *			the range are mapped to.
 * @start:	start address of the vmemmap virtual address range that we want
 *		to remap.
 * @end:	end address of the vmemmap virtual address range that we want to
 *		remap.
 * @vmemmap_head: the page to be installed as first in the vmemmap range
 * @vmemmap_tail: the page to be installed as non-first in the vmemmap range
 * @vmemmap_pages: list to deposit vmemmap pages to be freed.  It is callers
 *		responsibility to free pages.
 * @flags:	modifications to vmemmap_remap_walk flags
 *
 * Return: %0 on success, negative error code otherwise.
 */
static int vmemmap_remap_free(unsigned long start, unsigned long end,
			      struct page *vmemmap_head,
			      struct page *vmemmap_tail,
			      struct list_head *vmemmap_pages,
			      unsigned long flags)
{
	int ret;
	struct vmemmap_remap_walk walk = {
		.remap_pte	= vmemmap_remap_pte,
		.vmemmap_head	= vmemmap_head,
		.vmemmap_tail	= vmemmap_tail,
		.vmemmap_pages	= vmemmap_pages,
		.flags		= flags,
	};

	ret = vmemmap_remap_range(start, end, &walk);
	if (!ret || !walk.nr_walked)
		return ret;

	end = start + walk.nr_walked * PAGE_SIZE;

	/*
	 * vmemmap_pages contains pages from the previous vmemmap_remap_range()
	 * call which failed.  These are pages which were removed from
	 * the vmemmap. They will be restored in the following call.
	 */
	walk = (struct vmemmap_remap_walk) {
		.remap_pte	= vmemmap_restore_pte,
		.vmemmap_pages	= vmemmap_pages,
		.flags		= 0,
	};

	vmemmap_remap_range(start, end, &walk);

	return ret;
}

static int alloc_vmemmap_page_list(unsigned long start, unsigned long end,
				   struct list_head *list)
{
	gfp_t gfp_mask = GFP_KERNEL | __GFP_RETRY_MAYFAIL;
	unsigned long nr_pages = (end - start) >> PAGE_SHIFT;
	int nid = page_to_nid((struct page *)start);
	struct page *page, *next;
	int i;

	for (i = 0; i < nr_pages; i++) {
		page = alloc_pages_node(nid, gfp_mask, 0);
		if (!page)
			goto out;
		list_add(&page->lru, list);
	}
	memmap_pages_add(nr_pages);

	return 0;
out:
	list_for_each_entry_safe(page, next, list, lru)
		__free_page(page);
	return -ENOMEM;
}

/**
 * vmemmap_remap_alloc - remap the vmemmap virtual address range [@start, end)
 *			 to the page which is from the @vmemmap_pages
 *			 respectively.
 * @start:	start address of the vmemmap virtual address range that we want
 *		to remap.
 * @end:	end address of the vmemmap virtual address range that we want to
 *		remap.
 * @flags:	modifications to vmemmap_remap_walk flags
 *
 * Return: %0 on success, negative error code otherwise.
 */
static int vmemmap_remap_alloc(unsigned long start, unsigned long end,
			       unsigned long flags)
{
	LIST_HEAD(vmemmap_pages);
	struct vmemmap_remap_walk walk = {
		.remap_pte	= vmemmap_restore_pte,
		.vmemmap_pages	= &vmemmap_pages,
		.flags		= flags,
	};

	if (alloc_vmemmap_page_list(start, end, &vmemmap_pages))
		return -ENOMEM;

	return vmemmap_remap_range(start, end, &walk);
}

DEFINE_STATIC_KEY_FALSE(hugetlb_optimize_vmemmap_key);
EXPORT_SYMBOL(hugetlb_optimize_vmemmap_key);

static bool vmemmap_optimize_enabled = IS_ENABLED(CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP_DEFAULT_ON);
core_param(hugetlb_free_vmemmap, vmemmap_optimize_enabled, bool, 0);

static int __hugetlb_vmemmap_restore_folio(const struct hstate *h,
					   struct folio *folio, unsigned long flags)
{
	int ret;
	unsigned long vmemmap_start, vmemmap_end;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_hugetlb(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_ref_count(folio), folio);

	if (!folio_test_hugetlb_vmemmap_optimized(folio))
		return 0;

	if (flags & VMEMMAP_SYNCHRONIZE_RCU)
		synchronize_rcu();

	vmemmap_start	= (unsigned long)&folio->page;
	vmemmap_end	= vmemmap_start + hugetlb_vmemmap_size(h);

	vmemmap_start	+= HUGETLB_VMEMMAP_RESERVE_SIZE;

	/*
	 * The pages which the vmemmap virtual address range [@vmemmap_start,
	 * @vmemmap_end) are mapped to are freed to the buddy allocator.
	 * When a HugeTLB page is freed to the buddy allocator, previously
	 * discarded vmemmap pages must be allocated and remapping.
	 */
	ret = vmemmap_remap_alloc(vmemmap_start, vmemmap_end, flags);
	if (!ret) {
		folio_clear_hugetlb_vmemmap_optimized(folio);
		static_branch_dec(&hugetlb_optimize_vmemmap_key);
	}

	return ret;
}

/**
 * hugetlb_vmemmap_restore_folio - restore previously optimized (by
 *				hugetlb_vmemmap_optimize_folio()) vmemmap pages which
 *				will be reallocated and remapped.
 * @h:		struct hstate.
 * @folio:     the folio whose vmemmap pages will be restored.
 *
 * Return: %0 if @folio's vmemmap pages have been reallocated and remapped,
 * negative error code otherwise.
 */
int hugetlb_vmemmap_restore_folio(const struct hstate *h, struct folio *folio)
{
	return __hugetlb_vmemmap_restore_folio(h, folio, VMEMMAP_SYNCHRONIZE_RCU);
}

/**
 * hugetlb_vmemmap_restore_folios - restore vmemmap for every folio on the list.
 * @h:			hstate.
 * @folio_list:		list of folios.
 * @non_hvo_folios:	Output list of folios for which vmemmap exists.
 *
 * Return: number of folios for which vmemmap was restored, or an error code
 *		if an error was encountered restoring vmemmap for a folio.
 *		Folios that have vmemmap are moved to the non_hvo_folios
 *		list.  Processing of entries stops when the first error is
 *		encountered. The folio that experienced the error and all
 *		non-processed folios will remain on folio_list.
 */
long hugetlb_vmemmap_restore_folios(const struct hstate *h,
					struct list_head *folio_list,
					struct list_head *non_hvo_folios)
{
	struct folio *folio, *t_folio;
	long restored = 0;
	long ret = 0;
	unsigned long flags = VMEMMAP_REMAP_NO_TLB_FLUSH | VMEMMAP_SYNCHRONIZE_RCU;

	list_for_each_entry_safe(folio, t_folio, folio_list, lru) {
		if (folio_test_hugetlb_vmemmap_optimized(folio)) {
			ret = __hugetlb_vmemmap_restore_folio(h, folio, flags);
			/* only need to synchronize_rcu() once for each batch */
			flags &= ~VMEMMAP_SYNCHRONIZE_RCU;

			if (ret)
				break;
			restored++;
		}

		/* Add non-optimized folios to output list */
		list_move(&folio->lru, non_hvo_folios);
	}

	if (restored)
		flush_tlb_all();
	if (!ret)
		ret = restored;
	return ret;
}

/* Return true iff a HugeTLB whose vmemmap should and can be optimized. */
static bool vmemmap_should_optimize_folio(const struct hstate *h, struct folio *folio)
{
	if (folio_test_hugetlb_vmemmap_optimized(folio))
		return false;

	if (!READ_ONCE(vmemmap_optimize_enabled))
		return false;

	if (!hugetlb_vmemmap_optimizable(h))
		return false;

	return true;
}

static int __hugetlb_vmemmap_optimize_folio(const struct hstate *h,
					    struct folio *folio,
					    struct list_head *vmemmap_pages,
					    unsigned long flags)
{
	unsigned long vmemmap_start, vmemmap_end;
	struct page *vmemmap_head, *vmemmap_tail;
	int nid, ret = 0;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_hugetlb(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_ref_count(folio), folio);

	if (!vmemmap_should_optimize_folio(h, folio))
		return ret;

	static_branch_inc(&hugetlb_optimize_vmemmap_key);

	if (flags & VMEMMAP_SYNCHRONIZE_RCU)
		synchronize_rcu();
	/*
	 * Very Subtle
	 * If VMEMMAP_REMAP_NO_TLB_FLUSH is set, TLB flushing is not performed
	 * immediately after remapping.  As a result, subsequent accesses
	 * and modifications to struct pages associated with the hugetlb
	 * page could be to the OLD struct pages.  Set the vmemmap optimized
	 * flag here so that it is copied to the new head page.  This keeps
	 * the old and new struct pages in sync.
	 * If there is an error during optimization, we will immediately FLUSH
	 * the TLB and clear the flag below.
	 */
	folio_set_hugetlb_vmemmap_optimized(folio);

	nid = folio_nid(folio);
	vmemmap_head = alloc_pages_node(nid, GFP_KERNEL, 0);
	if (!vmemmap_head) {
		ret = -ENOMEM;
		goto out;
	}

	copy_page(page_to_virt(vmemmap_head), folio);
	list_add(&vmemmap_head->lru, vmemmap_pages);
	memmap_pages_add(1);

	vmemmap_tail	= vmemmap_head;
	vmemmap_start	= (unsigned long)&folio->page;
	vmemmap_end	= vmemmap_start + hugetlb_vmemmap_size(h);

	/*
	 * Remap the vmemmap virtual address range [@vmemmap_start, @vmemmap_end).
	 * Add pages previously mapping the range to vmemmap_pages list so that
	 * they can be freed by the caller.
	 */
	ret = vmemmap_remap_free(vmemmap_start, vmemmap_end,
				 vmemmap_head, vmemmap_tail,
				 vmemmap_pages, flags);
out:
	if (ret) {
		static_branch_dec(&hugetlb_optimize_vmemmap_key);
		folio_clear_hugetlb_vmemmap_optimized(folio);
	}

	return ret;
}

/**
 * hugetlb_vmemmap_optimize_folio - optimize @folio's vmemmap pages.
 * @h:		struct hstate.
 * @folio:     the folio whose vmemmap pages will be optimized.
 *
 * This function only tries to optimize @folio's vmemmap pages and does not
 * guarantee that the optimization will succeed after it returns. The caller
 * can use folio_test_hugetlb_vmemmap_optimized(@folio) to detect if @folio's
 * vmemmap pages have been optimized.
 */
void hugetlb_vmemmap_optimize_folio(const struct hstate *h, struct folio *folio)
{
	LIST_HEAD(vmemmap_pages);

	__hugetlb_vmemmap_optimize_folio(h, folio, &vmemmap_pages, VMEMMAP_SYNCHRONIZE_RCU);
	free_vmemmap_page_list(&vmemmap_pages);
}

static int hugetlb_vmemmap_split_folio(const struct hstate *h, struct folio *folio)
{
	unsigned long vmemmap_start, vmemmap_end;

	if (!vmemmap_should_optimize_folio(h, folio))
		return 0;

	vmemmap_start	= (unsigned long)&folio->page;
	vmemmap_end	= vmemmap_start + hugetlb_vmemmap_size(h);

	/*
	 * Split PMDs on the vmemmap virtual address range [@vmemmap_start,
	 * @vmemmap_end]
	 */
	return vmemmap_remap_split(vmemmap_start, vmemmap_end);
}

void hugetlb_vmemmap_optimize_folios(struct hstate *h, struct list_head *folio_list)
{
	struct folio *folio;
	LIST_HEAD(vmemmap_pages);
	unsigned long flags = VMEMMAP_REMAP_NO_TLB_FLUSH | VMEMMAP_SYNCHRONIZE_RCU;

	list_for_each_entry(folio, folio_list, lru) {
		int ret = hugetlb_vmemmap_split_folio(h, folio);

		/*
		 * Spliting the PMD requires allocating a page, thus lets fail
		 * early once we encounter the first OOM. No point in retrying
		 * as it can be dynamically done on remap with the memory
		 * we get back from the vmemmap deduplication.
		 */
		if (ret == -ENOMEM)
			break;
	}

	flush_tlb_all();

	list_for_each_entry(folio, folio_list, lru) {
		int ret;

		ret = __hugetlb_vmemmap_optimize_folio(h, folio, &vmemmap_pages, flags);
		/* only need to synchronize_rcu() once for each batch */
		flags &= ~VMEMMAP_SYNCHRONIZE_RCU;

		/*
		 * Pages to be freed may have been accumulated.  If we
		 * encounter an ENOMEM,  free what we have and try again.
		 * This can occur in the case that both spliting fails
		 * halfway and head page allocation also failed. In this
		 * case __hugetlb_vmemmap_optimize_folio() would free memory
		 * allowing more vmemmap remaps to occur.
		 */
		if (ret == -ENOMEM && !list_empty(&vmemmap_pages)) {
			flush_tlb_all();
			free_vmemmap_page_list(&vmemmap_pages);
			INIT_LIST_HEAD(&vmemmap_pages);
			__hugetlb_vmemmap_optimize_folio(h, folio, &vmemmap_pages, flags);
		}
	}

	flush_tlb_all();
	free_vmemmap_page_list(&vmemmap_pages);
}

static struct ctl_table hugetlb_vmemmap_sysctls[] = {
	{
		.procname	= "hugetlb_optimize_vmemmap",
		.data		= &vmemmap_optimize_enabled,
		.maxlen		= sizeof(vmemmap_optimize_enabled),
		.mode		= 0644,
		.proc_handler	= proc_dobool,
	},
};

static int __init hugetlb_vmemmap_init(void)
{
	const struct hstate *h;

	/* HUGETLB_VMEMMAP_RESERVE_SIZE should cover all used struct pages */
	BUILD_BUG_ON(__NR_USED_SUBPAGE > HUGETLB_VMEMMAP_RESERVE_PAGES);

	for_each_hstate(h) {
		if (hugetlb_vmemmap_optimizable(h)) {
			register_sysctl_init("vm", hugetlb_vmemmap_sysctls);
			break;
		}
	}
	return 0;
}
late_initcall(hugetlb_vmemmap_init);
