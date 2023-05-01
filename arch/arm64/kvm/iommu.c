// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <asm/kvm_mmu.h>
#include <linux/kvm_host.h>
#include <kvm/iommu.h>

struct kvm_iommu_driver *iommu_driver;
extern struct kvm_iommu_ops *kvm_nvhe_sym(kvm_iommu_ops);

int kvm_iommu_register_driver(struct kvm_iommu_driver *kern_ops, struct kvm_iommu_ops *el2_ops)
{
	int ret;

	BUG_ON(!kern_ops || !el2_ops);

	/*
	 * Paired with smp_load_acquire(&iommu_driver)
	 * Ensure memory stores happening during a driver
	 * init are observed before executing kvm iommu callbacks.
	 */
	ret = cmpxchg_release(&iommu_driver, NULL, kern_ops) ? -EBUSY : 0;
	if (ret)
		return ret;

	kvm_nvhe_sym(kvm_iommu_ops) = el2_ops;
	return 0;
}
EXPORT_SYMBOL(kvm_iommu_register_driver);

int kvm_iommu_init_driver(void)
{
	if (WARN_ON(!smp_load_acquire(&iommu_driver)))
		return -ENODEV;

	return iommu_driver->init_driver();
}

void kvm_iommu_remove_driver(void)
{
	if (smp_load_acquire(&iommu_driver))
		iommu_driver->remove_driver();
}
