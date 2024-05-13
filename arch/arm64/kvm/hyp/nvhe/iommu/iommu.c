// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU operations for pKVM
 *
 * Copyright (C) 2022 Linaro Ltd.
 */

#include <asm/kvm_hyp.h>

#include <hyp/adjust_pc.h>

#include <kvm/iommu.h>
#include <nvhe/alloc_mgt.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>

static DEFINE_PER_CPU(struct kvm_iommu_paddr_cache, kvm_iommu_unmap_cache);

void **kvm_hyp_iommu_domains;

static struct hyp_pool iommu_host_pool;
static struct hyp_pool iommu_atomic_pool;

DECLARE_PER_CPU(struct kvm_hyp_req, host_hyp_reqs);

static atomic_t kvm_iommu_idmap_initialized;

/*
 * All domain operations are lockless thanks to atomics, except for
 * alloc/free domain where:
 * alloc_domain: Wants to initialize the domain and only after set the refcount
 *   so it can be used when fully initialized.
 * free_domain: Wants to clear the domain refcount, then clear the domain, so
 *   no other call can use it while being freed.
 * This creates a race when a domain refcount is 0 and still in the free domain
 * and another alloc_domain is called for the same ID.
 * This should never happen with well behaved host.
 */
static DEFINE_HYP_SPINLOCK(kvm_iommu_domain_lock);

static inline void kvm_iommu_idmap_init_done(void)
{
	atomic_set_release(&kvm_iommu_idmap_initialized, 1);
}

static inline bool kvm_iommu_is_ready(void)
{
	return atomic_read_acquire(&kvm_iommu_idmap_initialized) == 1;
}

void *__kvm_iommu_donate_pages(struct hyp_pool *pool, u8 order, bool request)
{
	void *p;
	struct kvm_hyp_req *req = this_cpu_ptr(&host_hyp_reqs);

	p = hyp_alloc_pages(pool, order);
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

void __kvm_iommu_reclaim_pages(struct hyp_pool *pool, void *p, u8 order)
{
	/*
	 * Order MUST be same allocated page, however the buddy allocator
	 * is allowed to give higher order pages.
	 */
	BUG_ON(order > hyp_virt_to_page(p)->order);

	hyp_put_page(pool, p);
}

void *kvm_iommu_donate_pages(u8 order, bool request)
{
	return __kvm_iommu_donate_pages(&iommu_host_pool, order, request);
}

void kvm_iommu_reclaim_pages(void *p, u8 order)
{
	__kvm_iommu_reclaim_pages(&iommu_host_pool, p, order);
}

void *kvm_iommu_donate_pages_atomic(u8 order)
{
	return __kvm_iommu_donate_pages(&iommu_atomic_pool, order, false);
}

void kvm_iommu_reclaim_pages_atomic(void *p, u8 order)
{
	__kvm_iommu_reclaim_pages(&iommu_atomic_pool, p, order);
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
	if (!kvm_iommu_ops)
		return -EINVAL;

	/* Paired with smp_wmb() in kvm_iommu_init() */
	smp_rmb();
	return refill_hyp_pool(&iommu_host_pool, host_mc);
}

void kvm_iommu_reclaim(struct kvm_hyp_memcache *host_mc, int target)
{
	if (!kvm_iommu_ops)
		return;

	smp_rmb();
	reclaim_hyp_pool(&iommu_host_pool, host_mc, target);
}

int kvm_iommu_reclaimable(void)
{
	if (!kvm_iommu_ops)
		return 0;

	smp_rmb();
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

	idx = domain_id / KVM_IOMMU_DOMAINS_PER_PAGE;
	domains = (struct kvm_hyp_iommu_domain *)READ_ONCE(kvm_hyp_iommu_domains[idx]);
	if (!domains) {
		if (domain_id == KVM_IOMMU_DOMAIN_IDMAP_ID)
			domains = kvm_iommu_donate_pages_atomic(0);
		else
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
			if (domain_id == KVM_IOMMU_DOMAIN_IDMAP_ID)
				kvm_iommu_reclaim_pages_atomic(domains, 0);
			else
				kvm_iommu_reclaim_page(domains);
			return NULL;
		}
	}
	return &domains[domain_id % KVM_IOMMU_DOMAINS_PER_PAGE];
}

static int domain_get(struct kvm_hyp_iommu_domain *domain)
{
	int old = atomic_fetch_inc_acquire(&domain->refs);

	BUG_ON(!old || (old + 1 < 0));
	return 0;
}

static void domain_put(struct kvm_hyp_iommu_domain *domain)
{
	BUG_ON(!atomic_dec_return_release(&domain->refs));
}

int kvm_iommu_alloc_domain(pkvm_handle_t domain_id, u32 type)
{
	int ret = -EINVAL;
	struct kvm_hyp_iommu_domain *domain;

	domain = handle_to_domain(domain_id);
	if (!domain)
		return -ENOMEM;

	hyp_spin_lock(&kvm_iommu_domain_lock);
	if (atomic_read(&domain->refs))
		goto out_unlock;

	domain->domain_id = domain_id;
	ret = kvm_iommu_ops->alloc_domain(domain, type);
	if (ret)
		goto out_unlock;

	atomic_set_release(&domain->refs, 1);
out_unlock:
	hyp_spin_unlock(&kvm_iommu_domain_lock);
	return ret;
}

int kvm_iommu_free_domain(pkvm_handle_t domain_id)
{
	int ret = 0;
	struct kvm_hyp_iommu_domain *domain;

	domain = handle_to_domain(domain_id);
	if (!domain)
		return -EINVAL;

	hyp_spin_lock(&kvm_iommu_domain_lock);
	if (WARN_ON(atomic_cmpxchg_acquire(&domain->refs, 1, 0) != 1)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	kvm_iommu_ops->free_domain(domain);

	/* Set domain->refs to 0 and mark it as unused. */
	memset(domain, 0, sizeof(*domain));

out_unlock:
	hyp_spin_unlock(&kvm_iommu_domain_lock);

	return ret;
}

int kvm_iommu_attach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid, u32 pasid_bits)
{
	int ret;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return -EINVAL;

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		return -EINVAL;

	ret = kvm_iommu_ops->attach_dev(iommu, domain, endpoint_id, pasid, pasid_bits);
	if (ret)
		domain_put(domain);
	return ret;
}

int kvm_iommu_detach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid)
{
	int ret;
	struct kvm_hyp_iommu *iommu;
	struct kvm_hyp_iommu_domain *domain;

	iommu = kvm_iommu_ops->get_iommu_by_id(iommu_id);
	if (!iommu)
		return -EINVAL;

	domain = handle_to_domain(domain_id);
	if (!domain || atomic_read(&domain->refs) <= 1)
		return -EINVAL;

	ret = kvm_iommu_ops->detach_dev(iommu, domain, endpoint_id, pasid);
	if (ret)
		return ret;
	domain_put(domain);
	return ret;
}

#define IOMMU_PROT_MASK (IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE |\
			 IOMMU_NOEXEC | IOMMU_MMIO | IOMMU_PRIV)

size_t kvm_iommu_map_pages(pkvm_handle_t domain_id, unsigned long iova,
			   phys_addr_t paddr, size_t pgsize,
			   size_t pgcount, int prot)
{
	size_t size;
	int ret;
	size_t total_mapped = 0;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops || !kvm_iommu_ops->map_pages)
		return 0;

	if (prot & ~IOMMU_PROT_MASK)
		return 0;

	if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
	    iova + size < iova || paddr + size < paddr)
		return 0;

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		return 0;

	ret = __pkvm_host_use_dma(paddr, size);
	if (ret)
		return 0;

	kvm_iommu_ops->map_pages(domain, iova, paddr, pgsize, pgcount, prot, &total_mapped);

	pgcount -= total_mapped / pgsize;
	/*
	 * unuse the bits that haven't been mapped yet. The host calls back
	 * either to continue mapping, or to unmap and unuse what's been done
	 * so far.
	 */
	if (pgcount)
		__pkvm_host_unuse_dma(paddr, pgcount * pgsize);

	domain_put(domain);
	return total_mapped;
}

/* Based on  the kernel iommu_iotlb* but with some tweak, this can be unified later. */
static inline void kvm_iommu_iotlb_sync(struct kvm_hyp_iommu_domain *domain,
					struct iommu_iotlb_gather *iotlb_gather)
{
	if (kvm_iommu_ops->iotlb_sync)
		kvm_iommu_ops->iotlb_sync(domain, iotlb_gather);

	iommu_iotlb_gather_init(iotlb_gather);
}

static bool kvm_iommu_iotlb_gather_is_disjoint(struct iommu_iotlb_gather *gather,
					       unsigned long iova, size_t size)
{
	unsigned long start = iova, end = start + size - 1;

	return gather->end != 0 &&
		(end + 1 < gather->start || start > gather->end + 1);
}

static inline void kvm_iommu_iotlb_gather_add_range(struct iommu_iotlb_gather *gather,
						    unsigned long iova, size_t size)
{
	unsigned long end = iova + size - 1;

	if (gather->start > iova)
		gather->start = iova;
	if (gather->end < end)
		gather->end = end;
}

void kvm_iommu_iotlb_gather_add_page(struct kvm_hyp_iommu_domain *domain,
				     struct iommu_iotlb_gather *gather,
				     unsigned long iova,
				     size_t size)
{
	if ((gather->pgsize && gather->pgsize != size) ||
	    kvm_iommu_iotlb_gather_is_disjoint(gather, iova, size))
		kvm_iommu_iotlb_sync(domain, gather);

	gather->pgsize = size;
	kvm_iommu_iotlb_gather_add_range(gather, iova, size);
}

static void kvm_iommu_flush_unmap_cache(struct kvm_iommu_paddr_cache *cache)
{
	while (cache->ptr) {
		cache->ptr--;
		WARN_ON(__pkvm_host_unuse_dma(cache->paddr[cache->ptr],
					      cache->pgsize[cache->ptr]));
	}
}

size_t kvm_iommu_unmap_pages(pkvm_handle_t domain_id,
			     unsigned long iova, size_t pgsize, size_t pgcount)
{
	size_t size;
	size_t unmapped;
	size_t total_unmapped = 0;
	struct kvm_hyp_iommu_domain *domain;
	size_t max_pgcount;
	struct iommu_iotlb_gather iotlb_gather;
	struct kvm_iommu_paddr_cache *cache = this_cpu_ptr(&kvm_iommu_unmap_cache);

	if (!kvm_iommu_ops || !kvm_iommu_ops->unmap_pages)
		return 0;

	if (!pgsize || !pgcount)
		return 0;

	if (__builtin_mul_overflow(pgsize, pgcount, &size) ||
	    iova + size < iova)
		return 0;

	domain = handle_to_domain(domain_id);
	if (!domain || domain_get(domain))
		return 0;

	iommu_iotlb_gather_init(&iotlb_gather);

	while (total_unmapped < size) {
		max_pgcount = min_t(size_t, pgcount, KVM_IOMMU_PADDR_CACHE_MAX);
		unmapped = kvm_iommu_ops->unmap_pages(domain, iova, pgsize,
						      max_pgcount, &iotlb_gather, cache);
		if (!unmapped)
			break;
		kvm_iommu_iotlb_sync(domain, &iotlb_gather);
		kvm_iommu_flush_unmap_cache(cache);
		iova += unmapped;
		total_unmapped += unmapped;
		pgcount -= unmapped / pgsize;
	}

	domain_put(domain);
	return total_unmapped;
}

phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t domain_id, unsigned long iova)
{
	phys_addr_t phys = 0;
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_ops || !kvm_iommu_ops->iova_to_phys)
		return 0;

	domain = handle_to_domain( domain_id);

	if (!domain || domain_get(domain))
		return 0;

	phys = kvm_iommu_ops->iova_to_phys(domain, iova);
	domain_put(domain);
	return phys;
}

bool kvm_iommu_host_dabt_handler(struct kvm_cpu_context *host_ctxt, u64 esr, u64 addr)
{
	bool ret = false;

	if (kvm_iommu_ops && kvm_iommu_ops->dabt_handler)
		ret = kvm_iommu_ops->dabt_handler(host_ctxt, esr, addr);

	if (ret)
		kvm_skip_host_instr();

	return ret;
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

static int kvm_iommu_init_idmap(struct kvm_hyp_memcache *atomic_mc)
{
	int ret;

	/* atomic_mc is optional. */
	if (!atomic_mc->head)
		return 0;
	ret = hyp_pool_init_empty(&iommu_atomic_pool, 1024 /* order = 10 */);
	if (ret)
		return ret;

	ret = refill_hyp_pool(&iommu_atomic_pool, atomic_mc);

	if (ret)
		return ret;

	/* The host must guarantee that the allocator can be used from this context. */
	return WARN_ON(kvm_iommu_alloc_domain(KVM_IOMMU_DOMAIN_IDMAP_ID,
					      KVM_IOMMU_DOMAIN_IDMAP_TYPE));
}

int kvm_iommu_init(struct kvm_iommu_ops *ops, struct kvm_hyp_memcache *atomic_mc,
		   unsigned long init_arg)
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

	ret = __pkvm_host_donate_hyp(__hyp_pa(kvm_hyp_iommu_domains) >> PAGE_SHIFT,
				     1 << get_order(KVM_IOMMU_DOMAINS_ROOT_SIZE));
	if (ret)
		return ret;

	ret = hyp_pool_init_empty(&iommu_host_pool, 64 /* order = 6*/);
	if (ret)
		return ret;

	/* Ensure iommu_host_pool is ready _before_ iommu_ops is set */
	smp_wmb();
	kvm_iommu_ops = ops;

	return kvm_iommu_init_idmap(atomic_mc);
}

static inline int pkvm_to_iommu_prot(int prot)
{
	switch (prot) {
	case PKVM_HOST_MEM_PROT:
		return IOMMU_READ | IOMMU_WRITE;
	case PKVM_HOST_MMIO_PROT:
		return IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO;
	case 0:
		return 0;
	default:
		/* We don't understand that, it might cause corruption, so panic. */
		BUG();
	}

	return 0;
}
void kvm_iommu_host_stage2_idmap(phys_addr_t start, phys_addr_t end,
				 enum kvm_pgtable_prot prot)
{
	struct kvm_hyp_iommu_domain *domain;

	if (!kvm_iommu_is_ready())
		return;

	domain = handle_to_domain(KVM_IOMMU_DOMAIN_IDMAP_ID);
	if (!domain)
		return;

	kvm_iommu_ops->host_stage2_idmap(domain, start, end, pkvm_to_iommu_prot(prot));
}

static int __snapshot_host_stage2(const struct kvm_pgtable_visit_ctx *ctx,
				  enum kvm_pgtable_walk_flags visit)
{
	u64 start = ctx->addr;
	kvm_pte_t pte = *ctx->ptep;
	u32 level = ctx->level;
	struct kvm_hyp_iommu_domain *domain = ctx->arg;
	u64 end = start + kvm_granule_size(level);
	int prot = IOMMU_READ | IOMMU_WRITE;

	if (!addr_is_memory(start))
		prot |= IOMMU_MMIO;

	if (!pte || kvm_pte_valid(pte))
		kvm_iommu_ops->host_stage2_idmap(domain, start, end, prot);

	return 0;
}

int kvm_iommu_snapshot_host_stage2(struct kvm_hyp_iommu_domain *domain)
{
	int ret;
	struct kvm_pgtable_walker walker = {
		.cb	= __snapshot_host_stage2,
		.flags	= KVM_PGTABLE_WALK_LEAF,
		.arg = domain,
	};
	struct kvm_pgtable *pgt = &host_mmu.pgt;

	hyp_spin_lock(&host_mmu.lock);
	ret = kvm_pgtable_walk(pgt, 0, BIT(pgt->ia_bits), &walker);
	/* Start receiving calls to host_stage2_idmap. */
	if (!ret)
		kvm_iommu_idmap_init_done();
	hyp_spin_unlock(&host_mmu.lock);

	return ret;
}
