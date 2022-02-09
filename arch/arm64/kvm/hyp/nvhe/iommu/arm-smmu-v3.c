// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM hyp driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_hyp.h>
#include <kvm/arm_smmu_v3.h>
#include <nvhe/iommu.h>

size_t __ro_after_init kvm_hyp_arm_smmu_v3_count;
struct hyp_arm_smmu_v3_device *kvm_hyp_arm_smmu_v3_smmus;

static int smmu_init(void)
{
	return -ENOSYS;
}

static struct kvm_iommu_ops smmu_ops = {
	.init				= smmu_init,
};

int kvm_arm_smmu_v3_register(void)
{
	kvm_iommu_ops = smmu_ops;
	return 0;
}
