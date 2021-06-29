/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_KVM_NVHE_IOMMU_H__
#define __ARM64_KVM_NVHE_IOMMU_H__

struct kvm_iommu_ops {
	int (*init)(void);
};

extern struct kvm_iommu_ops kvm_iommu_ops;

#endif /* __ARM64_KVM_NVHE_IOMMU_H__ */
