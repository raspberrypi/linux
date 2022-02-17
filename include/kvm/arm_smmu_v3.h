/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_ARM_SMMU_V3_H
#define __KVM_ARM_SMMU_V3_H

#include <asm/kvm_asm.h>
#include <kvm/iommu.h>

#if IS_ENABLED(CONFIG_ARM_SMMU_V3_PKVM)

/*
 * Parameters from the trusted host:
 * @mmio_addr		base address of the SMMU registers
 * @mmio_size		size of the registers resource
 *
 * Other members are filled and used at runtime by the SMMU driver.
 */
struct hyp_arm_smmu_v3_device {
	struct kvm_hyp_iommu	iommu;
	phys_addr_t		mmio_addr;
	size_t			mmio_size;

	void __iomem		*base;
};

extern size_t kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count);
#define kvm_hyp_arm_smmu_v3_count kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count)

extern struct hyp_arm_smmu_v3_device *kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus);
#define kvm_hyp_arm_smmu_v3_smmus kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus)

#endif /* CONFIG_ARM_SMMU_V3_PKVM */

#endif /* __KVM_ARM_SMMU_V3_H */
