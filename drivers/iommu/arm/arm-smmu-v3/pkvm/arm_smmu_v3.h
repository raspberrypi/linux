/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_ARM_SMMU_V3_H
#define __KVM_ARM_SMMU_V3_H

#include <asm/kvm_asm.h>
#include <linux/io-pgtable.h>
#include <kvm/iommu.h>

#if IS_ENABLED(CONFIG_ARM_SMMU_V3_PKVM)

/*
 * Parameters from the trusted host:
 * @mmio_addr		base address of the SMMU registers
 * @mmio_size		size of the registers resource
 * @caches_clean_on_power_on
 *			is it safe to elide cache and TLB invalidation commands
 *			while the SMMU is OFF
 *
 * Other members are filled and used at runtime by the SMMU driver.
 */
struct hyp_arm_smmu_v3_device {
	struct kvm_hyp_iommu	iommu;
	phys_addr_t		mmio_addr;
	size_t			mmio_size;
	unsigned long		features;
	bool			caches_clean_on_power_on;

	void __iomem		*base;
	u32			cmdq_prod;
	u64			*cmdq_base;
	size_t			cmdq_log2size;
	u64			*strtab_base;
	size_t			strtab_num_entries;
	size_t			strtab_num_l1_entries;
	u8			strtab_split;
	struct io_pgtable_cfg	pgtable_cfg_s1;
	struct io_pgtable_cfg	pgtable_cfg_s2;
	u32			ssid_bits; /* SSID has max of 20 bits*/
};

extern size_t kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count);
#define kvm_hyp_arm_smmu_v3_count kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count)

extern struct hyp_arm_smmu_v3_device *kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus);
#define kvm_hyp_arm_smmu_v3_smmus kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus)

enum kvm_arm_smmu_domain_stage {
	KVM_ARM_SMMU_DOMAIN_BYPASS = KVM_IOMMU_DOMAIN_IDMAP_TYPE,
	KVM_ARM_SMMU_DOMAIN_S1,
	KVM_ARM_SMMU_DOMAIN_S2,
};
#endif /* CONFIG_ARM_SMMU_V3_PKVM */

#endif /* __KVM_ARM_SMMU_V3_H */
