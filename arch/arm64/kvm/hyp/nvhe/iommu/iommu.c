// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU operations for pKVM
 *
 * Copyright (C) 2022 Linaro Ltd.
 */

#include <asm/kvm_hyp.h>
#include <kvm/iommu.h>
#include <nvhe/alloc_mgt.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>

#define KVM_IOMMU_PADDR_CACHE_MAX		((size_t)511)
struct kvm_iommu_paddr_cache {
	unsigned short	ptr;
	u64		paddr[KVM_IOMMU_PADDR_CACHE_MAX];
};
static DEFINE_PER_CPU(struct kvm_iommu_paddr_cache, kvm_iommu_unmap_cache);

/*
 * This lock protect domain operations, that can't be done using the atomic refcount
 * It is used for alloc/free domains, so it shouldn't have a lot of overhead as
 * these are rare operations, while map/unmap are left lockless.
 */
static DEFINE_HYP_SPINLOCK(iommu_domains_lock);
void **kvm_hyp_iommu_domains;

static struct hyp_pool iommu_host_pool;

DECLARE_PER_CPU(struct kvm_hyp_req, host_hyp_reqs);

void *kvm_iommu_donate_pages(u8 order, bool request)
{
	void *p;
	struct kvm_hyp_req *req = this_cpu_ptr(&host_hyp_reqs);

	p = hyp_alloc_pages(&iommu_host_pool, order);
	if (p)
		return p;

	if (request) {
		req->type = KVM_HYP_REQ_TYPE_MEM;
		req->mem.dest = REQ_MEM_DEST_HYP_IOMMU;
		req->mem.sz_alloc = (1 << order) * PAGE_SIZE;
		req->mem.nr_pages = 1;
	}
	return NULL;
}

void kvm_iommu_reclaim_pages(void *p, u8 order)
{
	/*
	 * Order MUST be same allocated page, however the buddy allocator
	 * is allowed to give higher order pages.
	 */
	BUG_ON(order > hyp_virt_to_page(p)->order);

	hyp_put_page(&iommu_host_pool, p);
}

/* Request to hypervisor. */
int kvm_iommu_request(struct kvm_hyp_req *req)
{
	struct kvm_hyp_req *cur_req = this_cpu_ptr(&host_hyp_reqs);

	if (cur_req->type != KVM_HYP_LAST_REQ)
		return -EBUSY;

	memcpy(cur_req, req, sizeof(struct kvm_hyp_req));

	return 0;
}

int kvm_iommu_refill(struct kvm_hyp_memcache *host_mc)
{
	return refill_hyp_pool(&iommu_host_pool, host_mc);
}

void kvm_iommu_reclaim(struct kvm_hyp_memcache *host_mc, int target)
{
	reclaim_hyp_pool(&iommu_host_pool, host_mc, target);
}

int kvm_iommu_reclaimable(void)
{
	return hyp_pool_free_pages(&iommu_host_pool);
}

struct hyp_mgt_allocator_ops kvm_iommu_allocator_ops = {
	.refill = kvm_iommu_refill,
	.reclaim = kvm_iommu_reclaim,
	.reclaimable = kvm_iommu_reclaimable,
};

static struct kvm_hyp_iommu_domain *
handle_to_domain(pkvm_handle_t domain_id)
{
	int idx;
	struct kvm_hyp_iommu_domain *domains;

	if (domain_id >= KVM_IOMMU_MAX_DOMAINS)
		return NULL;
	domain_id = array_index_nospec(domain_id, KVM_IOMMU_MAX_DOMAINS);

	idx = domain_id >> KVM_IOMMU_DOMAIN_ID_SPLIT;
	domains = (struct kvm_hyp_iommu_domain *)READ_ONCE(kvm_hyp_iommu_domains[idx]);
	if (!domains) {
		domains = kvm_iommu_donate_page();
		if (!domains)
			return NULL;
		/*
		 * handle_to_domain() does not have to be called under a lock,
		 * but even though we allocate a leaf in all cases, it's only
		 * really a valid thing to do under alloc_domain(), which uses a
		 * lock. Races are therefore a host bug and we don't need to be
		 * delicate about it.
		 */
		if (WARN_ON(cmpxchg64_relaxed(&kvm_hyp_iommu_domains[idx], 0,
					      (void *)domains) != 0)) {
			kvm_iommu_reclaim_page(domains);
			return NULL;
		}
	}

	return &domains[domain_id & KVM_IOMMU_DOMAIN_ID_LEAF_MASK];
}

static int domain_get(struct kvm_hyp_iommu_domain *domain)
{
	int old = atomic_fetch_inc_acquire(&domain->refs);

	if (WARN_ON(!old))
		return -EINVAL;
	else if (old < 0 || old + 1 < 0)
		return -EOVERFLOW;
	return 0;
}

static void domain_put(struct kvm_hyp_iommu_domain *domain)
{
	BUG_ON(!atomic_dec_return_release(&domain->refs));
}

int kvm_iommu_alloc_domain(pkvm_handle_t domain_id)
{
	int ret = -EINVAL;
	struct kvm_hyp_iommu_domain *domain;

	hyp_spin_lock(&iommu_domains_lock);
	domain = handle_to_domain(domain_id);
	if (!domain || atomic_read(&domain->refs))
		goto out_unlock;

	domain->domain_id = domain_id;
	ret = kvm_iommu_ops->alloc_domain(domain);
	if (ret)
		goto out_unlock;

	atomic_set_release(&domain->refs, 1);
out_unlock:
	hyp_spin_unlock(&iommu_domains_lock);
	return ret;
}

int kvm_iommu_free_domain(pkvm_handle_t domain_id)
{
	int ret = 0;
	struct kvm_hyp_iommu_domain *domain;

	hyp_spin_lock(&iommu_domains_lock);
	domain = handle_to_domain(domain_id);
	if (!domain) {
		ret = -EINVAL;
		goto out_unlock;
	}

	/* Host is lying, the domain is alive! */
	if (WARN_ON(atomic_cmpxchg_release(&domain->refs, 1, 0) != 1))
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
	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		goto out_unlock;

	ret = kvm_iommu_ops->attach_dev(iommu, domain, endpoint_id);
	if (ret)
		goto err_put_domain;

	hyp_spin_unlock(&iommu_domains_lock);
	return 0;

err_put_domain:
	domain_put(domain);
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
	domain = handle_to_domain(domain_id);
	if (!domain || atomic_read(&domain->refs) <= 1)
		goto out_unlock;

	ret = kvm_iommu_ops->detach_dev(iommu, domain, endpoint_id);
	if (ret)
		goto out_unlock;

	domain_put(domain);
out_unlock:
	hyp_spin_unlock(&iommu_domains_lock);
	return ret;
}

#define IOMMU_PROT_MASK (IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE |\
			 IOMMU_NOEXEC | IOMMU_MMIO | IOMMU_PRIV)

size_t kvm_iommu_map_pages(pkvm_handle_t domain_id, unsigned long iova,
			   phys_addr_t paddr, size_t pgsize,
			   size_t pgcount, int prot)
{
	size_t size;
	size_t mapped;
	size_t granule;
	int ret = -EINVAL;
	size_t total_mapped = 0;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops)
		return 0;

	if (prot & ~IOMMU_PROT_MASK)
		return 0;

	if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
	    iova + size < iova || paddr + size < paddr)
		return 0;

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		return 0;

	if(!domain->pgtable || !domain->pgtable->ops.map_pages)
		goto out_put_domain;

	granule = 1UL << __ffs(domain->pgtable->cfg.pgsize_bitmap);
	if (!IS_ALIGNED(iova | paddr | pgsize, granule))
		goto out_put_domain;

	ret = __pkvm_host_use_dma(paddr, size);
	if (ret)
		goto out_put_domain;

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
out_put_domain:
	domain_put(domain);
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


size_t kvm_iommu_unmap_pages(pkvm_handle_t domain_id,
			     unsigned long iova, size_t pgsize, size_t pgcount)
{
	size_t size;
	size_t granule;
	size_t unmapped;
	size_t total_unmapped = 0;
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

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		return 0;

	if(!domain->pgtable || !domain->pgtable->ops.unmap_pages_walk)
		goto out_put_domain;

	granule = 1UL << __ffs(domain->pgtable->cfg.pgsize_bitmap);
	if (!IS_ALIGNED(iova | pgsize, granule))
		goto out_put_domain;

	while (total_unmapped < size) {
		max_pgcount = min_t(size_t, pgcount, KVM_IOMMU_PADDR_CACHE_MAX);
		unmapped = domain->pgtable->ops.unmap_pages_walk(&domain->pgtable->ops, iova, pgsize,
								 max_pgcount, NULL, &walker);
		if (!unmapped)
			goto out_put_domain;

		kvm_iommu_flush_unmap_cache(cache, pgsize);
		iova += unmapped;
		total_unmapped += unmapped;
		pgcount -= unmapped / pgsize;
	}

out_put_domain:
	domain_put(domain);
	return total_unmapped;
}

phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t domain_id, unsigned long iova)
{
	phys_addr_t phys = 0;
	struct kvm_hyp_iommu_domain *domain;

	hyp_spin_lock(&iommu_domains_lock);
	domain = handle_to_domain( domain_id);

	if (!domain || domain_get(domain))
		return 0;

	if (!domain->pgtable || !domain->pgtable->ops.iova_to_phys)
		goto out_unlock;

	phys = domain->pgtable->ops.iova_to_phys(&domain->pgtable->ops, iova);


out_unlock:
	domain_put(domain);
	return phys;
}

static int iommu_power_on(struct kvm_power_domain *pd)
{
	struct kvm_hyp_iommu *iommu = container_of(pd, struct kvm_hyp_iommu,
						   power_domain);
	bool prev;
	int ret;

	hyp_spin_lock(&iommu->lock);
	prev = iommu->power_is_off;
	iommu->power_is_off = false;
	ret = kvm_iommu_ops->resume ? kvm_iommu_ops->resume(iommu) : 0;
	if (ret)
		iommu->power_is_off = prev;
	hyp_spin_unlock(&iommu->lock);
	return ret;
}

static int iommu_power_off(struct kvm_power_domain *pd)
{
	struct kvm_hyp_iommu *iommu = container_of(pd, struct kvm_hyp_iommu,
						   power_domain);
	bool prev;
	int ret;

	hyp_spin_lock(&iommu->lock);
	prev = iommu->power_is_off;
	iommu->power_is_off = true;
	ret = kvm_iommu_ops->suspend ? kvm_iommu_ops->suspend(iommu) : 0;
	if (ret)
		iommu->power_is_off = prev;
	hyp_spin_unlock(&iommu->lock);
	return ret;
}

static const struct kvm_power_domain_ops iommu_power_ops = {
	.power_on	= iommu_power_on,
	.power_off	= iommu_power_off,
};

int kvm_iommu_init_device(struct kvm_hyp_iommu *iommu)
{
	/* See struct kvm_hyp_iommu */
	BUILD_BUG_ON(sizeof(u32) != sizeof(hyp_spinlock_t));

	return pkvm_init_power_domain(&iommu->power_domain, &iommu_power_ops);
}

int kvm_iommu_init(struct kvm_iommu_ops *ops, unsigned long init_arg)
{
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

	ret = pkvm_create_mappings(kvm_hyp_iommu_domains, kvm_hyp_iommu_domains +
				   KVM_IOMMU_DOMAINS_ROOT_ENTRIES, PAGE_HYP);
	if (ret)
		return ret;

	ret = hyp_pool_init_empty(&iommu_host_pool, 64 /* order = 6*/);

	kvm_iommu_ops = ops;
	return ret;
}
