// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM host driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <linux/of_platform.h>

#include <kvm/arm_smmu_v3.h>

#include "arm-smmu-v3.h"

static int kvm_arm_smmu_probe(struct platform_device *pdev)
{
	return -ENOSYS;
}

static int kvm_arm_smmu_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id arm_smmu_of_match[] = {
	{ .compatible = "arm,smmu-v3", },
	{ },
};

static struct platform_driver kvm_arm_smmu_driver = {
	.driver = {
		.name = "kvm-arm-smmu-v3",
		.of_match_table = arm_smmu_of_match,
	},
	.remove = kvm_arm_smmu_remove,
};

/**
 * kvm_arm_smmu_v3_init() - Reserve the SMMUv3 for KVM
 * @count: on success, number of SMMUs successfully initialized
 *
 * Return 0 if all present SMMUv3 were probed successfully, or an error.
 *   If no SMMU was found, return 0, with a count of 0.
 */
int kvm_arm_smmu_v3_init(unsigned int *count)
{
	int ret;

	ret = platform_driver_probe(&kvm_arm_smmu_driver, kvm_arm_smmu_probe);
	if (ret)
		return ret;

	*count = 0;
	return 0;
}

void kvm_arm_smmu_v3_remove(void)
{
	platform_driver_unregister(&kvm_arm_smmu_driver);
}
