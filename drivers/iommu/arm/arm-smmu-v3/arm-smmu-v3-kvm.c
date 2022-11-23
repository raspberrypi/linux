// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM host driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_pkvm.h>
#include <asm/kvm_mmu.h>
#include <linux/local_lock.h>
#include <linux/of_platform.h>

#include <kvm/arm_smmu_v3.h>

#include "arm-smmu-v3.h"

struct host_arm_smmu_device {
	struct arm_smmu_device		smmu;
	pkvm_handle_t			id;
	u32				boot_gbpa;
	unsigned int			pgd_order;
};

#define smmu_to_host(_smmu) \
	container_of(_smmu, struct host_arm_smmu_device, smmu);

static size_t				kvm_arm_smmu_cur;
static size_t				kvm_arm_smmu_count;
static struct hyp_arm_smmu_v3_device	*kvm_arm_smmu_array;
static struct kvm_hyp_iommu_memcache	*kvm_arm_smmu_memcache;

static DEFINE_PER_CPU(local_lock_t, memcache_lock) =
				INIT_LOCAL_LOCK(memcache_lock);

static void *kvm_arm_smmu_alloc_page(void *opaque)
{
	struct arm_smmu_device *smmu = opaque;
	struct page *p;

	/* No __GFP_ZERO because KVM zeroes the page */
	p = alloc_pages_node(dev_to_node(smmu->dev), GFP_ATOMIC, 0);
	if (!p)
		return NULL;

	return page_address(p);
}

static void kvm_arm_smmu_free_page(void *va, void *opaque)
{
	free_page((unsigned long)va);
}

static phys_addr_t kvm_arm_smmu_host_pa(void *va)
{
	return __pa(va);
}

static void *kvm_arm_smmu_host_va(phys_addr_t pa)
{
	return __va(pa);
}

__maybe_unused
static int kvm_arm_smmu_topup_memcache(struct arm_smmu_device *smmu, int ret)
{
	struct kvm_hyp_memcache *mc;
	int cpu = raw_smp_processor_id();

	lockdep_assert_held(this_cpu_ptr(&memcache_lock));
	mc = &kvm_arm_smmu_memcache[cpu].pages;

	if (kvm_arm_smmu_memcache[cpu].needs_page) {
		kvm_arm_smmu_memcache[cpu].needs_page = false;
		return  __topup_hyp_memcache(mc, 1, kvm_arm_smmu_alloc_page,
					     kvm_arm_smmu_host_pa, smmu);
	} else if (ret == -ENOMEM) {
		return __pkvm_topup_hyp_alloc(1);
	}

	return -EBADE;
}

__maybe_unused
static void kvm_arm_smmu_reclaim_memcache(void)
{
	struct kvm_hyp_memcache *mc;
	int cpu = raw_smp_processor_id();

	lockdep_assert_held(this_cpu_ptr(&memcache_lock));
	mc = &kvm_arm_smmu_memcache[cpu].pages;

	__free_hyp_memcache(mc, kvm_arm_smmu_free_page,
			    kvm_arm_smmu_host_va, NULL);
}

/*
 * Issue hypercall, and retry after filling the memcache if necessary.
 * After the call, reclaim pages pushed in the memcache by the hypervisor.
 */
#define kvm_call_hyp_nvhe_mc(smmu, ...)				\
({								\
	int __ret;						\
	do {							\
		__ret = kvm_call_hyp_nvhe(__VA_ARGS__);		\
	} while (!kvm_arm_smmu_topup_memcache(smmu, __ret));	\
	kvm_arm_smmu_reclaim_memcache();			\
	__ret;							\
})

static bool kvm_arm_smmu_validate_features(struct arm_smmu_device *smmu)
{
	unsigned long oas;
	unsigned int required_features =
		ARM_SMMU_FEAT_TRANS_S2 |
		ARM_SMMU_FEAT_TT_LE;
	unsigned int forbidden_features =
		ARM_SMMU_FEAT_STALL_FORCE;
	unsigned int keep_features =
		ARM_SMMU_FEAT_2_LVL_STRTAB	|
		ARM_SMMU_FEAT_2_LVL_CDTAB	|
		ARM_SMMU_FEAT_TT_LE		|
		ARM_SMMU_FEAT_SEV		|
		ARM_SMMU_FEAT_COHERENCY		|
		ARM_SMMU_FEAT_TRANS_S1		|
		ARM_SMMU_FEAT_TRANS_S2		|
		ARM_SMMU_FEAT_VAX		|
		ARM_SMMU_FEAT_RANGE_INV;

	if (smmu->options & ARM_SMMU_OPT_PAGE0_REGS_ONLY) {
		dev_err(smmu->dev, "unsupported layout\n");
		return false;
	}

	if ((smmu->features & required_features) != required_features) {
		dev_err(smmu->dev, "missing features 0x%x\n",
			required_features & ~smmu->features);
		return false;
	}

	if (smmu->features & forbidden_features) {
		dev_err(smmu->dev, "features 0x%x forbidden\n",
			smmu->features & forbidden_features);
		return false;
	}

	smmu->features &= keep_features;

	/*
	 * This can be relaxed (although the spec says that OAS "must match
	 * the system physical address size."), but requires some changes. All
	 * table and queue allocations must use GFP_DMA* to ensure the SMMU can
	 * access them.
	 */
	oas = get_kvm_ipa_limit();
	if (smmu->oas < oas) {
		dev_err(smmu->dev, "incompatible address size\n");
		return false;
	}

	return true;
}

static int kvm_arm_smmu_device_reset(struct host_arm_smmu_device *host_smmu)
{
	int ret;
	u32 reg;
	struct arm_smmu_device *smmu = &host_smmu->smmu;

	reg = readl_relaxed(smmu->base + ARM_SMMU_CR0);
	if (reg & CR0_SMMUEN)
		dev_warn(smmu->dev, "SMMU currently enabled! Resetting...\n");

	/* Disable bypass */
	host_smmu->boot_gbpa = readl_relaxed(smmu->base + ARM_SMMU_GBPA);
	ret = arm_smmu_update_gbpa(smmu, GBPA_ABORT, 0);
	if (ret)
		return ret;

	ret = arm_smmu_device_disable(smmu);
	if (ret)
		return ret;

	/* Stream table */
	writeq_relaxed(smmu->strtab_cfg.strtab_base,
		       smmu->base + ARM_SMMU_STRTAB_BASE);
	writel_relaxed(smmu->strtab_cfg.strtab_base_cfg,
		       smmu->base + ARM_SMMU_STRTAB_BASE_CFG);

	/* Command queue */
	writeq_relaxed(smmu->cmdq.q.q_base, smmu->base + ARM_SMMU_CMDQ_BASE);

	return 0;
}

static int kvm_arm_smmu_probe(struct platform_device *pdev)
{
	int ret;
	bool bypass;
	struct resource *res;
	phys_addr_t mmio_addr;
	struct io_pgtable_cfg cfg;
	size_t mmio_size, pgd_size;
	struct arm_smmu_device *smmu;
	struct device *dev = &pdev->dev;
	struct host_arm_smmu_device *host_smmu;
	struct hyp_arm_smmu_v3_device *hyp_smmu;

	if (kvm_arm_smmu_cur >= kvm_arm_smmu_count)
		return -ENOSPC;

	hyp_smmu = &kvm_arm_smmu_array[kvm_arm_smmu_cur];

	host_smmu = devm_kzalloc(dev, sizeof(*host_smmu), GFP_KERNEL);
	if (!host_smmu)
		return -ENOMEM;

	smmu = &host_smmu->smmu;
	smmu->dev = dev;

	ret = arm_smmu_fw_probe(pdev, smmu, &bypass);
	if (ret || bypass)
		return ret ?: -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mmio_size = resource_size(res);
	if (mmio_size < SZ_128K) {
		dev_err(dev, "unsupported MMIO region size (%pr)\n", res);
		return -EINVAL;
	}
	mmio_addr = res->start;
	host_smmu->id = kvm_arm_smmu_cur;

	smmu->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(smmu->base))
		return PTR_ERR(smmu->base);

	/* Use one page per level-2 table */
	smmu->strtab_cfg.split = PAGE_SHIFT - (ilog2(STRTAB_STE_DWORDS) + 3);

	ret = arm_smmu_device_hw_probe(smmu);
	if (ret)
		return ret;

	if (!kvm_arm_smmu_validate_features(smmu))
		return -ENODEV;

	/*
	 * Stage-1 should be easy to support, though we do need to allocate a
	 * context descriptor table.
	 */
	cfg = (struct io_pgtable_cfg) {
		.fmt = ARM_64_LPAE_S2,
		.pgsize_bitmap = smmu->pgsize_bitmap,
		.ias = smmu->ias,
		.oas = smmu->oas,
		.coherent_walk = smmu->features & ARM_SMMU_FEAT_COHERENCY,
	};

	/*
	 * Choose the page and address size. Compute the PGD size as well, so we
	 * know how much memory to pre-allocate.
	 */
	ret = io_pgtable_configure(&cfg, &pgd_size);
	if (ret)
		return ret;

	host_smmu->pgd_order = get_order(pgd_size);
	smmu->pgsize_bitmap = cfg.pgsize_bitmap;
	smmu->ias = cfg.ias;
	smmu->oas = cfg.oas;

	ret = arm_smmu_init_one_queue(smmu, &smmu->cmdq.q, smmu->base,
				      ARM_SMMU_CMDQ_PROD, ARM_SMMU_CMDQ_CONS,
				      CMDQ_ENT_DWORDS, "cmdq");
	if (ret)
		return ret;

	ret = arm_smmu_init_strtab(smmu);
	if (ret)
		return ret;

	ret = kvm_arm_smmu_device_reset(host_smmu);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, host_smmu);

	/* Hypervisor parameters */
	hyp_smmu->mmio_addr = mmio_addr;
	hyp_smmu->mmio_size = mmio_size;
	hyp_smmu->features = smmu->features;
	hyp_smmu->pgtable_cfg = cfg;

	kvm_arm_smmu_cur++;

	return 0;
}

static int kvm_arm_smmu_remove(struct platform_device *pdev)
{
	struct host_arm_smmu_device *host_smmu = platform_get_drvdata(pdev);
	struct arm_smmu_device *smmu = &host_smmu->smmu;

	/*
	 * There was an error during hypervisor setup. The hyp driver may
	 * have already enabled the device, so disable it.
	 */
	arm_smmu_device_disable(smmu);
	arm_smmu_update_gbpa(smmu, host_smmu->boot_gbpa, GBPA_ABORT);
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

static int kvm_arm_smmu_array_alloc(void)
{
	int smmu_order, mc_order;
	struct device_node *np;

	kvm_arm_smmu_count = 0;
	for_each_compatible_node(np, NULL, "arm,smmu-v3")
		kvm_arm_smmu_count++;

	if (!kvm_arm_smmu_count)
		return 0;

	/* Allocate the parameter list shared with the hypervisor */
	smmu_order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	kvm_arm_smmu_array = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
						      smmu_order);
	if (!kvm_arm_smmu_array)
		return -ENOMEM;

	mc_order = get_order(NR_CPUS * sizeof(*kvm_arm_smmu_memcache));
	kvm_arm_smmu_memcache = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
							 mc_order);
	if (!kvm_arm_smmu_memcache)
		goto err_free_array;

	return 0;

err_free_array:
	free_pages((unsigned long)kvm_arm_smmu_array, smmu_order);
	return -ENOMEM;
}

static void kvm_arm_smmu_array_free(void)
{
	int order;

	order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	free_pages((unsigned long)kvm_arm_smmu_array, order);
	order = get_order(NR_CPUS * sizeof(*kvm_arm_smmu_memcache));
	free_pages((unsigned long)kvm_arm_smmu_memcache, order);
}

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

	/*
	 * Check whether any device owned by the host is behind an SMMU.
	 */
	ret = kvm_arm_smmu_array_alloc();
	*count = kvm_arm_smmu_count;
	if (ret || !kvm_arm_smmu_count)
		return ret;

	ret = platform_driver_probe(&kvm_arm_smmu_driver, kvm_arm_smmu_probe);
	if (ret)
		goto err_free;

	if (kvm_arm_smmu_cur != kvm_arm_smmu_count) {
		/* A device exists but failed to probe */
		ret = -EUNATCH;
		goto err_free;
	}

	/*
	 * These variables are stored in the nVHE image, and won't be accessible
	 * after KVM initialization. Ownership of kvm_arm_smmu_array will be
	 * transferred to the hypervisor as well.
	 *
	 * kvm_arm_smmu_memcache is shared between hypervisor and host.
	 */
	kvm_hyp_arm_smmu_v3_smmus = kvm_arm_smmu_array;
	kvm_hyp_arm_smmu_v3_count = kvm_arm_smmu_count;
	kvm_hyp_iommu_memcaches = kern_hyp_va(kvm_arm_smmu_memcache);
	return 0;

err_free:
	kvm_arm_smmu_array_free();
	return ret;
}

void kvm_arm_smmu_v3_remove(void)
{
	platform_driver_unregister(&kvm_arm_smmu_driver);
}
