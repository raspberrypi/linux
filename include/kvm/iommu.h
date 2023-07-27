/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_IOMMU_H
#define __KVM_IOMMU_H

#include <asm/kvm_host.h>
#include <linux/io-pgtable.h>
#ifdef __KVM_NVHE_HYPERVISOR__
#include <nvhe/spinlock.h>
#else
#include "hyp_constants.h"
#endif

/*
 * Parameters from the trusted host:
 * @pgtable_cfg:	page table configuration
 *
 * Other members are filled and used at runtime by the IOMMU driver.
 */
struct kvm_hyp_iommu {
#ifdef __KVM_NVHE_HYPERVISOR__
	hyp_spinlock_t			lock;
#else
	u32						unused; /* This is verified in kvm_iommu_init_device() */
#endif
};

struct kvm_hyp_iommu_memcache {
	struct kvm_hyp_memcache	pages;
	bool needs_page;
} ____cacheline_aligned_in_smp;

extern struct kvm_hyp_iommu_memcache *kvm_nvhe_sym(kvm_hyp_iommu_memcaches);
#define kvm_hyp_iommu_memcaches kvm_nvhe_sym(kvm_hyp_iommu_memcaches)

extern void **kvm_nvhe_sym(kvm_hyp_iommu_domains);
#define kvm_hyp_iommu_domains kvm_nvhe_sym(kvm_hyp_iommu_domains)

struct kvm_hyp_iommu_domain {
	struct io_pgtable	*pgtable;
	u32			refs;
	pkvm_handle_t		domain_id;
	struct kvm_hyp_iommu	*iommu;
};

/*
 * At the moment the number of domains is limited by the ASID and VMID size on
 * Arm. With single-stage translation, that size is 2^8 or 2^16. On a lot of
 * platforms the number of devices is actually the limiting factor and we'll
 * only need a handful of domains, but with PASID or SR-IOV support that limit
 * can be reached.
 *
 * In practice we're rarely going to need a lot of domains. To avoid allocating
 * a large domain table, we use a two-level table, indexed by domain ID. With
 * 4kB pages and 16-bytes domains, the leaf table contains 256 domains, and the
 * root table 256 pointers. With 64kB pages, the leaf table contains 4096
 * domains and the root table 16 pointers. In this case, or when using 8-bit
 * VMIDs, it may be more advantageous to use a single level. But using two
 * levels allows to easily extend the domain size.
 */
#define KVM_IOMMU_MAX_DOMAINS	(1 << 16)

/* Number of entries in the level-2 domain table */
#define KVM_IOMMU_DOMAINS_PER_PAGE \
	(PAGE_SIZE / sizeof(struct kvm_hyp_iommu_domain))

/* Number of entries in the root domain table */
#define KVM_IOMMU_DOMAINS_ROOT_ENTRIES \
	(KVM_IOMMU_MAX_DOMAINS / KVM_IOMMU_DOMAINS_PER_PAGE)

#define KVM_IOMMU_DOMAINS_ROOT_SIZE \
	(KVM_IOMMU_DOMAINS_ROOT_ENTRIES * sizeof(void *))

/* Bits [16:split] index the root table, bits [split-1:0] index the leaf table */
#define KVM_IOMMU_DOMAIN_ID_SPLIT	ilog2(KVM_IOMMU_DOMAINS_PER_PAGE)

#define KVM_IOMMU_DOMAIN_ID_LEAF_MASK	((1 << KVM_IOMMU_DOMAIN_ID_SPLIT) - 1)

#endif /* __KVM_IOMMU_H */
