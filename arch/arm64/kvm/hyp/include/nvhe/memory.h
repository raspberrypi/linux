/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __KVM_HYP_MEMORY_H
#define __KVM_HYP_MEMORY_H

#include <asm/kvm_mmu.h>
#include <asm/page.h>

#include <linux/types.h>
#include <nvhe/refcount.h>

/*
 * Accesses to struct hyp_page flags are serialized by the host stage-2
 * page-table lock.
 */
#define MODULE_OWNED_PAGE		BIT(0)

struct hyp_page {
	unsigned short refcount;
	u8 order;
	u8 flags;
};

extern u64 __hyp_vmemmap;
#define hyp_vmemmap ((struct hyp_page *)__hyp_vmemmap)

#define __hyp_va(phys)	((void *)((phys_addr_t)(phys) - hyp_physvirt_offset))

static inline void *hyp_phys_to_virt(phys_addr_t phys)
{
	return __hyp_va(phys);
}

static inline phys_addr_t hyp_virt_to_phys(void *addr)
{
	return __hyp_pa(addr);
}

#define hyp_phys_to_pfn(phys)	((phys) >> PAGE_SHIFT)
#define hyp_pfn_to_phys(pfn)	((phys_addr_t)((pfn) << PAGE_SHIFT))
#define hyp_phys_to_page(phys)	(&hyp_vmemmap[hyp_phys_to_pfn(phys)])
#define hyp_virt_to_page(virt)	hyp_phys_to_page(__hyp_pa(virt))
#define hyp_virt_to_pfn(virt)	hyp_phys_to_pfn(__hyp_pa(virt))

#define hyp_page_to_pfn(page)	((struct hyp_page *)(page) - hyp_vmemmap)
#define hyp_page_to_phys(page)  hyp_pfn_to_phys((hyp_page_to_pfn(page)))
#define hyp_page_to_virt(page)	__hyp_va(hyp_page_to_phys(page))
#define hyp_page_to_pool(page)	(((struct hyp_page *)page)->pool)

/*
 * Refcounting wrappers for 'struct hyp_page'.
 */
static inline int hyp_page_count(void *addr)
{
	struct hyp_page *p = hyp_virt_to_page(addr);

	return hyp_refcount_get(p->refcount);
}

static inline void hyp_page_ref_inc(struct hyp_page *p)
{
	hyp_refcount_inc(p->refcount);
}

static inline void hyp_page_ref_dec(struct hyp_page *p)
{
	hyp_refcount_dec(p->refcount);
}

static inline int hyp_page_ref_dec_and_test(struct hyp_page *p)
{
	return hyp_refcount_dec(p->refcount) == 0;
}

static inline void hyp_set_page_refcounted(struct hyp_page *p)
{
	hyp_refcount_set(p->refcount, 1);
}
#endif /* __KVM_HYP_MEMORY_H */
