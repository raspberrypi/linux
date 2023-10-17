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

int kvm_iommu_register_driver(struct kvm_iommu_driver *kern_ops)
{
	BUG_ON(!kern_ops);

	/*
	 * Paired with smp_load_acquire(&iommu_driver)
	 * Ensure memory stores happening during a driver
	 * init are observed before executing kvm iommu callbacks.
	 */
	return cmpxchg_release(&iommu_driver, NULL, kern_ops) ? -EBUSY : 0;
}
EXPORT_SYMBOL(kvm_iommu_register_driver);

int kvm_iommu_init_hyp(struct kvm_iommu_ops *hyp_ops,
		       unsigned long init_arg)
{
	BUG_ON(!hyp_ops);

	return kvm_call_hyp_nvhe(__pkvm_iommu_init, hyp_ops, init_arg);
}
EXPORT_SYMBOL(kvm_iommu_init_hyp);

int kvm_iommu_init_driver(void)
{
	if (WARN_ON(!smp_load_acquire(&iommu_driver)))
		return -ENODEV;
	/*
	 * init_driver is optional as the driver already registered it self.
	 * This call mainly notify the driver we are about to drop privilege.
	 */
	if (!iommu_driver->init_driver)
		return 0;
	kvm_hyp_iommu_domains = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
				get_order(KVM_IOMMU_DOMAINS_ROOT_SIZE));
	kvm_hyp_iommu_domains = kern_hyp_va(kvm_hyp_iommu_domains);
	if (!kvm_hyp_iommu_domains) {
		kvm_err("No enough mem for IOMMU domains");
		return -ENOMEM;
	}

	return iommu_driver->init_driver();
}

void kvm_iommu_remove_driver(void)
{
	if (smp_load_acquire(&iommu_driver))
		iommu_driver->remove_driver();
}
