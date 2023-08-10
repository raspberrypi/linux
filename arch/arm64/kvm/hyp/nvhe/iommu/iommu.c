// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU operations for pKVM
 *
 * Copyright (C) 2022 Linaro Ltd.
 */

#include <asm/kvm_hyp.h>
#include <kvm/iommu.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>

#define KVM_IOMMU_PADDR_CACHE_MAX		((size_t)511)
struct kvm_iommu_paddr_cache {
	unsigned short	ptr;
	u64		paddr[KVM_IOMMU_PADDR_CACHE_MAX];
};
static DEFINE_PER_CPU(struct kvm_iommu_paddr_cache, kvm_iommu_unmap_cache);
struct kvm_hyp_iommu_memcache *kvm_hyp_iommu_memcaches;
/*
 * This lock protect domain operations, that can't be done using the atomic refcount
 * It is used for alloc/free domains, so it shouldn't have a lot of overhead as
 * these are rare operations, while map/unmap are left lockless.
 */
static DEFINE_HYP_SPINLOCK(iommu_domains_lock);

void *kvm_iommu_donate_page(void)
{
	void *p;
	int cpu = hyp_smp_processor_id();
	struct kvm_hyp_memcache tmp = kvm_hyp_iommu_memcaches[cpu].pages;

	if (!tmp.nr_pages) {
		kvm_hyp_iommu_memcaches[cpu].needs_page = true;
		return NULL;
	}

	if (__pkvm_host_donate_hyp(hyp_phys_to_pfn(tmp.head), 1))
		return NULL;

	p = pop_hyp_memcache(&tmp, hyp_phys_to_virt);
	if (!p)
		return NULL;

	kvm_hyp_iommu_memcaches[cpu].pages = tmp;
	memset(p, 0, PAGE_SIZE);
	return p;
}

void kvm_iommu_reclaim_page(void *p)
{
	int cpu = hyp_smp_processor_id();

	memset(p, 0, PAGE_SIZE);
	push_hyp_memcache(&kvm_hyp_iommu_memcaches[cpu].pages, p, hyp_virt_to_phys);
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(p), 1));
}

static struct kvm_hyp_iommu_domain *
handle_to_domain(struct kvm_hyp_iommu *iommu, pkvm_handle_t domain_id)
{
	int idx;
	struct kvm_hyp_iommu_domain *domains;

	if (domain_id >= iommu->nr_domains)
		return NULL;
	domain_id = array_index_nospec(domain_id, iommu->nr_domains);

	idx = domain_id >> KVM_IOMMU_DOMAIN_ID_SPLIT;
	domains = iommu->domains[idx];
	if (!domains) {
		domains = kvm_iommu_donate_page();
		if (!domains)
			return NULL;
		iommu->domains[idx] = domains;
	}

	return &domains[domain_id & KVM_IOMMU_DOMAIN_ID_LEAF_MASK];
}

int kvm_iommu_alloc_domain(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			   unsigned long pgd_hva)
{
	int ret;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return -EINVAL;

	hyp_spin_lock(&iommu_domains_lock);
	domain = handle_to_domain(iommu, domain_id);
	if (!domain)
		goto out_unlock;

	if (domain->refs)
		goto out_unlock;

	domain->domain_id = domain_id;
	domain->iommu = iommu;
	ret = kvm_iommu_ops->alloc_domain(domain, pgd_hva);
	if (ret)
		goto out_unlock;

	domain->refs = 1;
out_unlock:
	hyp_spin_unlock(&iommu_domains_lock);
	return ret;
}

int kvm_iommu_free_domain(pkvm_handle_t iommu_id, pkvm_handle_t domain_id)
{
	int ret = -EINVAL;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return -EINVAL;

	hyp_spin_lock(&iommu_domains_lock);
	domain = handle_to_domain(iommu, domain_id);
	if (!domain)
		goto out_unlock;

	if (domain->refs != 1)
		goto out_unlock;

	kvm_iommu_ops->free_domain(domain);

	/* Set domain->refs to 0 and mark it as unused. */
	memset(domain, 0, sizeof(*domain));

out_unlock:
	hyp_spin_unlock(&iommu_domains_lock);

	return ret;
}

int kvm_iommu_attach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id)
{
	int ret = -EINVAL;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return -EINVAL;

	hyp_spin_lock(&iommu_domains_lock);
	domain = handle_to_domain(iommu, domain_id);
	if (!domain || !domain->refs || domain->refs == UINT_MAX)
		goto out_unlock;

	ret = kvm_iommu_ops->attach_dev(iommu, domain, endpoint_id);
	if (ret)
		goto out_unlock;

	domain->refs++;
out_unlock:
	hyp_spin_unlock(&iommu_domains_lock);
	return ret;
}

int kvm_iommu_detach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id)
{
	int ret = -EINVAL;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return -EINVAL;

	hyp_spin_lock(&iommu_domains_lock);
	domain = handle_to_domain(iommu, domain_id);
	if (!domain || domain->refs <= 1)
		goto out_unlock;

	ret = kvm_iommu_ops->detach_dev(iommu, domain, endpoint_id);
	if (ret)
		goto out_unlock;

	domain->refs--;
out_unlock:
	hyp_spin_unlock(&iommu_domains_lock);
	return ret;
}

#define IOMMU_PROT_MASK (IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE |\
			 IOMMU_NOEXEC | IOMMU_MMIO | IOMMU_PRIV)

size_t kvm_iommu_map_pages(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			   unsigned long iova, phys_addr_t paddr, size_t pgsize,
			   size_t pgcount, int prot)
{
	size_t size;
	size_t mapped;
	size_t granule;
	int ret = -EINVAL;
	size_t total_mapped = 0;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops)
		return 0;

	if (prot & ~IOMMU_PROT_MASK)
		return 0;

	if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
	    iova + size < iova || paddr + size < paddr)
		return 0;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return 0;

	hyp_spin_lock(&iommu_domains_lock);
	domain = handle_to_domain(iommu, domain_id);
	if (!domain)
		goto err_unlock;

	granule = 1UL << __ffs(domain->pgtable->cfg.pgsize_bitmap);
	if (!IS_ALIGNED(iova | paddr | pgsize, granule))
		goto err_unlock;

	ret = __pkvm_host_use_dma(paddr, size);
	if (ret)
		goto err_unlock;

	while (pgcount && !ret) {
		mapped = 0;
		ret = domain->pgtable->ops.map_pages(&domain->pgtable->ops, iova, paddr, pgsize, pgcount, prot, 0, &mapped);

		WARN_ON(!IS_ALIGNED(mapped, pgsize));
		WARN_ON(mapped > pgcount * pgsize);

		pgcount -= mapped / pgsize;
		total_mapped += mapped;
		iova += mapped;
		paddr += mapped;
	}

	/*
	 * unuse the bits that haven't been mapped yet. The host calls back
	 * either to continue mapping, or to unmap and unuse what's been done
	 * so far.
	 */
	if (pgcount)
		__pkvm_host_unuse_dma(paddr, pgcount * pgsize);
err_unlock:
	hyp_spin_unlock(&iommu_domains_lock);
	return total_mapped;
}

static void kvm_iommu_flush_unmap_cache(struct kvm_iommu_paddr_cache *cache,
					size_t pgsize)
{
	while (cache->ptr)
		WARN_ON(__pkvm_host_unuse_dma(cache->paddr[--cache->ptr], PAGE_SIZE));
}

static void kvm_iommu_unmap_walker(struct io_pgtable_ctxt *ctxt)
{
	struct kvm_iommu_paddr_cache *cache = (struct kvm_iommu_paddr_cache *)ctxt->arg;

	cache->paddr[cache->ptr++] = ctxt->addr;

	/* Make more space. */
	if(cache->ptr == KVM_IOMMU_PADDR_CACHE_MAX)
		kvm_iommu_flush_unmap_cache(cache, ctxt->size);
}

size_t kvm_iommu_unmap_pages(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			     unsigned long iova, size_t pgsize, size_t pgcount)
{
	size_t size;
	size_t granule;
	size_t unmapped;
	size_t total_unmapped = 0;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;
	size_t max_pgcount;
	struct kvm_iommu_paddr_cache *cache = this_cpu_ptr(&kvm_iommu_unmap_cache);
	struct io_pgtable_walker walker = {
		.cb = kvm_iommu_unmap_walker,
		.arg = cache,
	};

	if (!kvm_iommu_ops)
		return 0;

	if (!pgsize || !pgcount)
		return 0;

	if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
	    iova + size < iova)
		return 0;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return 0;

	hyp_spin_lock(&iommu_domains_lock);
	domain = handle_to_domain(iommu, domain_id);
	if (!domain)
		goto out_unlock;

	granule = 1UL << __ffs(domain->pgtable->cfg.pgsize_bitmap);
	if (!IS_ALIGNED(iova | pgsize, granule))
		goto out_unlock;

	while (total_unmapped < size) {
		max_pgcount = min_t(size_t, pgcount, KVM_IOMMU_PADDR_CACHE_MAX);
		unmapped = domain->pgtable->ops.unmap_pages_walk(&domain->pgtable->ops, iova, pgsize,
								 max_pgcount, NULL, &walker);
		if (!unmapped)
			goto out_unlock;

		kvm_iommu_flush_unmap_cache(cache, pgsize);
		iova += unmapped;
		total_unmapped += unmapped;
		pgcount -= unmapped / pgsize;
	}

out_unlock:
	hyp_spin_unlock(&iommu_domains_lock);
	return total_unmapped;
}

phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t iommu_id,
				   pkvm_handle_t domain_id, unsigned long iova)
{
	phys_addr_t phys = 0;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return 0;

	hyp_spin_lock(&iommu_domains_lock);
	domain = handle_to_domain(iommu, domain_id);
	if (domain)
		phys = domain->pgtable->ops.iova_to_phys(&domain->pgtable->ops, iova);

	hyp_spin_unlock(&iommu_domains_lock);
	return phys;
}

int kvm_iommu_init_device(struct kvm_hyp_iommu *iommu)
{
	void *domains;

	/* See struct kvm_hyp_iommu */
	BUILD_BUG_ON(sizeof(u32) != sizeof(hyp_spinlock_t));

	domains = iommu->domains;
	iommu->domains = kern_hyp_va(domains);
	return pkvm_create_mappings(iommu->domains, iommu->domains +
				    KVM_IOMMU_DOMAINS_ROOT_ENTRIES, PAGE_HYP);
}

int kvm_iommu_init(struct kvm_iommu_ops *ops, struct kvm_hyp_iommu_memcache *mc,
		   unsigned long init_arg)
{
	enum kvm_pgtable_prot prot;
	int ret;

	if (WARN_ON(!ops->get_iommu_by_id ||
		    !ops->alloc_domain ||
		    !ops->free_domain ||
		    !ops->attach_dev ||
		    !ops->detach_dev))
		return -ENODEV;

	ret = ops->init ? ops->init(init_arg) : 0;
	if (ret)
		return ret;

	/* The memcache is shared with the host */
	prot = pkvm_mkstate(PAGE_HYP, PKVM_PAGE_SHARED_OWNED);
	ret = pkvm_create_mappings(mc, mc + NR_CPUS, prot);
	if (ret)
		return ret;

	kvm_iommu_ops = ops;
	kvm_hyp_iommu_memcaches = mc;
	return 0;
}
