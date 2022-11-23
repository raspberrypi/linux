/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_IOMMU_H
#define __KVM_IOMMU_H

#include <asm/kvm_host.h>

struct kvm_hyp_iommu_memcache {
	struct kvm_hyp_memcache	pages;
	bool needs_page;
} ____cacheline_aligned_in_smp;

extern struct kvm_hyp_iommu_memcache *kvm_nvhe_sym(kvm_hyp_iommu_memcaches);
#define kvm_hyp_iommu_memcaches kvm_nvhe_sym(kvm_hyp_iommu_memcaches)

#endif /* __KVM_IOMMU_H */
