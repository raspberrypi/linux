// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Arm Ltd.
 */
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <kvm/arm_smmu_v3.h>
#include <linux/types.h>
#include <linux/gfp_types.h>
#include <linux/io-pgtable-arm.h>

#include <nvhe/alloc.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>

bool __ro_after_init selftest_running;

void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp, struct io_pgtable_cfg *cfg)
{
	void *addr = kvm_iommu_donate_page();

	if(size != PAGE_SIZE)
		return NULL;

	if (addr && !cfg->coherent_walk)
		kvm_flush_dcache_to_poc(addr, size);

	return addr;
}

void __arm_lpae_free_pages(void *addr, size_t size, struct io_pgtable_cfg *cfg)
{
	BUG_ON(size != PAGE_SIZE);

	if (!cfg->coherent_walk)
		kvm_flush_dcache_to_poc(addr, size);

	kvm_iommu_reclaim_page(addr);
}

void __arm_lpae_sync_pte(arm_lpae_iopte *ptep, int num_entries,
			 struct io_pgtable_cfg *cfg)
{
	if (!cfg->coherent_walk)
		kvm_flush_dcache_to_poc(ptep, sizeof(*ptep) * num_entries);
}

int kvm_arm_io_pgtable_init(struct io_pgtable_cfg *cfg,
			    struct arm_lpae_io_pgtable *data)
{
	int ret = arm_lpae_init_pgtable_s2(cfg, data);

	if (ret)
		return ret;

	data->iop.cfg = *cfg;
	data->iop.fmt	= cfg->fmt;

	return 0;
}

struct io_pgtable *kvm_arm_io_pgtable_alloc(struct io_pgtable_cfg *cfg,
					   unsigned long pgd_hva,
					   void *cookie,
					   int *out_ret)
{
	size_t pgd_size, alignment;
	struct arm_lpae_io_pgtable *data;
	int ret;

	data = hyp_alloc(sizeof(*data));
	if (!data) {
		*out_ret = hyp_alloc_errno();
		return NULL;
	}

	ret = kvm_arm_io_pgtable_init(cfg, data);
	if (ret)
		goto out_free;

	pgd_size = ARM_LPAE_PGD_SIZE(data);
	data->pgd = (void *)kern_hyp_va(pgd_hva);
	/*
	 * If it has eight or more entries, the table must be aligned on
	 * its size. Otherwise 64 bytes.
	 */
	alignment = max(pgd_size, 8 * sizeof(arm_lpae_iopte));
	if (!IS_ALIGNED(hyp_virt_to_phys(data->pgd), alignment)) {
		ret = -EINVAL;
		goto out_free;
	}

	ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn(data->pgd), pgd_size >> PAGE_SHIFT);
	if (ret)
		goto out_free;
	memset(data->pgd, 0, pgd_size);

	data->iop.cookie = cookie;
	data->iop.cfg.arm_lpae_s2_cfg.vttbr = __arm_lpae_virt_to_phys(data->pgd);
	if (!data->iop.cfg.coherent_walk)
		kvm_flush_dcache_to_poc(data->pgd, pgd_size);

	/* Ensure the empty pgd is visible before any actual TTBR write */
	wmb();

	*out_ret = 0;
	return &data->iop;
out_free:
	hyp_free(data);
	*out_ret = ret;
	return NULL;
}

int kvm_arm_io_pgtable_free(struct io_pgtable *iopt)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_to_data(iopt);
	size_t pgd_size = ARM_LPAE_PGD_SIZE(data);

	if (!data->iop.cfg.coherent_walk)
		kvm_flush_dcache_to_poc(data->pgd, pgd_size);

	/* Free all tables but the pgd */
	__arm_lpae_free_pgtable(data, data->start_level, data->pgd, true);
	memset(data->pgd, 0, pgd_size);
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(data->pgd), pgd_size >> PAGE_SHIFT));
	return 0;
}
