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
};

#define smmu_to_host(_smmu) \
	container_of(_smmu, struct host_arm_smmu_device, smmu);

struct kvm_arm_smmu_master {
	struct arm_smmu_device		*smmu;
	struct device			*dev;
	struct kvm_arm_smmu_domain	*domain;
};

struct kvm_arm_smmu_domain {
	struct iommu_domain		domain;
	struct arm_smmu_device		*smmu;
	struct mutex			init_mutex;
	pkvm_handle_t			id;
};

#define to_kvm_smmu_domain(_domain) \
	container_of(_domain, struct kvm_arm_smmu_domain, domain)

static size_t				kvm_arm_smmu_cur;
static size_t				kvm_arm_smmu_count;
static struct hyp_arm_smmu_v3_device	*kvm_arm_smmu_array;
static DEFINE_IDA(kvm_arm_smmu_domain_ida);

extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_ops);

static int kvm_arm_smmu_topup_memcache(struct arm_smmu_device *smmu,
				       struct arm_smccc_res *res)
{
	struct kvm_hyp_req req;

	hyp_reqs_smccc_decode(res, &req);

	if ((res->a1 == -ENOMEM) && (req.type != KVM_HYP_REQ_TYPE_MEM)) {
		/*
		 * There is no way for drivers to populate hyp_alloc requests,
		 * so -ENOMEM + no request indicates that.
		 */
		return __pkvm_topup_hyp_alloc(1);
	} else if (req.type != KVM_HYP_REQ_TYPE_MEM) {
		return -EBADE;
	}

	if (req.mem.dest == REQ_MEM_DEST_HYP_IOMMU) {
		return __pkvm_topup_hyp_alloc_mgt(HYP_ALLOC_MGT_IOMMU_ID,
+					 	  req.mem.nr_pages, req.mem.sz_alloc);
	} else if (req.mem.dest == REQ_MEM_DEST_HYP_ALLOC) {
		/* Fill hyp alloc*/
		return __pkvm_topup_hyp_alloc(req.mem.nr_pages);
	}

	dev_err(smmu->dev, "Bogus mem request");
	return -EBADE;
}

/*
 * Issue hypercall, and retry after filling the memcache if necessary.
 */
#define kvm_call_hyp_nvhe_mc(smmu, ...)					\
({									\
	struct arm_smccc_res __res;					\
	do {								\
		__res = kvm_call_hyp_nvhe_smccc(__VA_ARGS__);		\
	} while (__res.a1 && !kvm_arm_smmu_topup_memcache(smmu, &__res));\
	__res.a1;							\
})

static struct platform_driver kvm_arm_smmu_driver;

static struct arm_smmu_device *
kvm_arm_smmu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev;

	dev = driver_find_device_by_fwnode(&kvm_arm_smmu_driver.driver, fwnode);
	put_device(dev);
	return dev ? dev_get_drvdata(dev) : NULL;
}

static struct iommu_ops kvm_arm_smmu_ops;

static struct iommu_device *kvm_arm_smmu_probe_device(struct device *dev)
{
	struct arm_smmu_device *smmu;
	struct kvm_arm_smmu_master *master;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	if (!fwspec || fwspec->ops != &kvm_arm_smmu_ops)
		return ERR_PTR(-ENODEV);

	if (WARN_ON_ONCE(dev_iommu_priv_get(dev)))
		return ERR_PTR(-EBUSY);

	smmu = kvm_arm_smmu_get_by_fwnode(fwspec->iommu_fwnode);
	if (!smmu)
		return ERR_PTR(-ENODEV);

	master = kzalloc(sizeof(*master), GFP_KERNEL);
	if (!master)
		return ERR_PTR(-ENOMEM);

	master->dev = dev;
	master->smmu = smmu;
	dev_iommu_priv_set(dev, master);

	return &smmu->iommu;
}

static void kvm_arm_smmu_release_device(struct device *dev)
{
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);

	kfree(master);
	iommu_fwspec_free(dev);
}

static struct iommu_domain *kvm_arm_smmu_domain_alloc(unsigned type)
{
	struct kvm_arm_smmu_domain *kvm_smmu_domain;

	/*
	 * We don't support
	 * - IOMMU_DOMAIN_IDENTITY because we rely on the host telling the
	 *   hypervisor which pages are used for DMA.
	 * - IOMMU_DOMAIN_DMA_FQ because lazy unmap would clash with memory
	 *   donation to guests.
	 */
	if (type != IOMMU_DOMAIN_DMA &&
	    type != IOMMU_DOMAIN_UNMANAGED)
		return NULL;

	kvm_smmu_domain = kzalloc(sizeof(*kvm_smmu_domain), GFP_KERNEL);
	if (!kvm_smmu_domain)
		return NULL;

	mutex_init(&kvm_smmu_domain->init_mutex);

	return &kvm_smmu_domain->domain;
}

static int kvm_arm_smmu_domain_finalize(struct kvm_arm_smmu_domain *kvm_smmu_domain,
					struct kvm_arm_smmu_master *master)
{
	int ret = 0;
	struct arm_smmu_device *smmu = master->smmu;

	if (kvm_smmu_domain->smmu) {
		if (kvm_smmu_domain->smmu != smmu)
			return -EINVAL;
		return 0;
	}

	ret = ida_alloc_range(&kvm_arm_smmu_domain_ida, 0, KVM_IOMMU_MAX_DOMAINS,
			      GFP_KERNEL);
	if (ret < 0)
		return ret;
	kvm_smmu_domain->id = ret;

	ret = kvm_call_hyp_nvhe_mc(smmu, __pkvm_host_iommu_alloc_domain,
				   kvm_smmu_domain->id);
	if (ret)
		return ret;

	kvm_smmu_domain->domain.pgsize_bitmap = smmu->pgsize_bitmap;
	kvm_smmu_domain->domain.geometry.aperture_end = (1UL << smmu->ias) - 1;
	kvm_smmu_domain->domain.geometry.force_aperture = true;
	kvm_smmu_domain->smmu = smmu;

	return 0;
}

static void kvm_arm_smmu_domain_free(struct iommu_domain *domain)
{
	int ret;
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);
	struct arm_smmu_device *smmu = kvm_smmu_domain->smmu;

	if (smmu) {
		ret = kvm_call_hyp_nvhe(__pkvm_host_iommu_free_domain, kvm_smmu_domain->id);
		ida_free(&kvm_arm_smmu_domain_ida, kvm_smmu_domain->id);
	}
	kfree(kvm_smmu_domain);
}

static int kvm_arm_smmu_detach_dev(struct host_arm_smmu_device *host_smmu,
				   struct kvm_arm_smmu_master *master)
{
	int i, ret;
	struct arm_smmu_device *smmu = &host_smmu->smmu;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(master->dev);

	if (!master->domain)
		return 0;

	for (i = 0; i < fwspec->num_ids; i++) {
		int sid = fwspec->ids[i];

		ret = kvm_call_hyp_nvhe(__pkvm_host_iommu_detach_dev,
					host_smmu->id, master->domain->id, sid);
		if (ret) {
			dev_err(smmu->dev, "cannot detach device %s (0x%x): %d\n",
				dev_name(master->dev), sid, ret);
			break;
		}
	}

	master->domain = NULL;

	return ret;
}

static int kvm_arm_smmu_attach_dev(struct iommu_domain *domain,
				   struct device *dev)
{
	int i, ret;
	struct arm_smmu_device *smmu;
	struct host_arm_smmu_device *host_smmu;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);

	if (!master)
		return -ENODEV;

	smmu = master->smmu;
	host_smmu = smmu_to_host(smmu);

	ret = kvm_arm_smmu_detach_dev(host_smmu, master);
	if (ret)
		return ret;

	mutex_lock(&kvm_smmu_domain->init_mutex);
	ret = kvm_arm_smmu_domain_finalize(kvm_smmu_domain, master);
	mutex_unlock(&kvm_smmu_domain->init_mutex);
	if (ret)
		return ret;

	for (i = 0; i < fwspec->num_ids; i++) {
		int sid = fwspec->ids[i];

		ret = kvm_call_hyp_nvhe_mc(smmu, __pkvm_host_iommu_attach_dev,
					   host_smmu->id, kvm_smmu_domain->id,
					   sid);
		if (ret) {
			dev_err(smmu->dev, "cannot attach device %s (0x%x): %d\n",
				dev_name(dev), sid, ret);
			goto out_ret;
		}
	}
	master->domain = kvm_smmu_domain;

out_ret:
	if (ret)
		kvm_arm_smmu_detach_dev(host_smmu, master);
	return ret;
}

static int kvm_arm_smmu_map_pages(struct iommu_domain *domain,
				  unsigned long iova, phys_addr_t paddr,
				  size_t pgsize, size_t pgcount, int prot,
				  gfp_t gfp, size_t *total_mapped)
{
	size_t mapped;
	size_t size = pgsize * pgcount;
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);
	struct arm_smmu_device *smmu = kvm_smmu_domain->smmu;
	struct arm_smccc_res res;

	do {
		res = kvm_call_hyp_nvhe_smccc(__pkvm_host_iommu_map_pages,
					      kvm_smmu_domain->id,
					      iova, paddr, pgsize, pgcount, prot);
		mapped = res.a1;
		iova += mapped;
		paddr += mapped;
		WARN_ON(mapped % pgsize);
		WARN_ON(mapped > pgcount * pgsize);
		pgcount -= mapped / pgsize;
		*total_mapped += mapped;
	} while (*total_mapped < size && !kvm_arm_smmu_topup_memcache(smmu, &res));
	if (*total_mapped < size)
		return -EINVAL;

	return 0;
}

static size_t kvm_arm_smmu_unmap_pages(struct iommu_domain *domain,
				       unsigned long iova, size_t pgsize,
				       size_t pgcount,
				       struct iommu_iotlb_gather *iotlb_gather)
{
	size_t unmapped;
	size_t total_unmapped = 0;
	size_t size = pgsize * pgcount;
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);
	struct arm_smmu_device *smmu = kvm_smmu_domain->smmu;
	struct arm_smccc_res res;

	do {
		res = kvm_call_hyp_nvhe_smccc(__pkvm_host_iommu_unmap_pages,
					      kvm_smmu_domain->id,
					      iova, pgsize, pgcount);
		unmapped = res.a1;
		total_unmapped += unmapped;
		iova += unmapped;
		WARN_ON(unmapped % pgsize);
		pgcount -= unmapped / pgsize;

		/*
		 * The page table driver can unmap less than we asked for. If it
		 * didn't unmap anything at all, then it either reached the end
		 * of the range, or it needs a page in the memcache to break a
		 * block mapping.
		 */
	} while (total_unmapped < size &&
		 (unmapped || !kvm_arm_smmu_topup_memcache(smmu, &res)));

	return total_unmapped;
}

static phys_addr_t kvm_arm_smmu_iova_to_phys(struct iommu_domain *domain,
					     dma_addr_t iova)
{
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);

	return kvm_call_hyp_nvhe(__pkvm_host_iommu_iova_to_phys, kvm_smmu_domain->id, iova);
}

static struct iommu_ops kvm_arm_smmu_ops = {
	.capable		= arm_smmu_capable,
	.device_group		= arm_smmu_device_group,
	.of_xlate		= arm_smmu_of_xlate,
	.probe_device		= kvm_arm_smmu_probe_device,
	.release_device		= kvm_arm_smmu_release_device,
	.domain_alloc		= kvm_arm_smmu_domain_alloc,
	.pgsize_bitmap		= -1UL,
	.owner			= THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= kvm_arm_smmu_attach_dev,
		.free		= kvm_arm_smmu_domain_free,
		.map_pages	= kvm_arm_smmu_map_pages,
		.unmap_pages	= kvm_arm_smmu_unmap_pages,
		.iova_to_phys	= kvm_arm_smmu_iova_to_phys,
	}
};

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

	if (kvm_arm_smmu_ops.pgsize_bitmap == -1UL)
		kvm_arm_smmu_ops.pgsize_bitmap = smmu->pgsize_bitmap;
	else
		kvm_arm_smmu_ops.pgsize_bitmap |= smmu->pgsize_bitmap;

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

	ret = arm_smmu_register_iommu(smmu, &kvm_arm_smmu_ops, mmio_addr);
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
	arm_smmu_unregister_iommu(smmu);
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
	int smmu_order;
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

	return 0;
}

static void kvm_arm_smmu_array_free(void)
{
	int order;

	order = get_order(kvm_arm_smmu_count * sizeof(*kvm_arm_smmu_array));
	free_pages((unsigned long)kvm_arm_smmu_array, order);
}

/**
 * kvm_arm_smmu_v3_init() - Reserve the SMMUv3 for KVM
 * Return 0 if all present SMMUv3 were probed successfully, or an error.
 *   If no SMMU was found, return 0, with a count of 0.
 */
static int kvm_arm_smmu_v3_init(void)
{
	int ret;

	/*
	 * Check whether any device owned by the host is behind an SMMU.
	 */
	ret = kvm_arm_smmu_array_alloc();
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

	return kvm_iommu_init_hyp(kern_hyp_va(lm_alias(&kvm_nvhe_sym(smmu_ops))), 0);

err_free:
	kvm_arm_smmu_array_free();
	return ret;
}

static void kvm_arm_smmu_v3_remove(void)
{
	platform_driver_unregister(&kvm_arm_smmu_driver);
}

struct kvm_iommu_driver kvm_smmu_v3_ops = {
	.init_driver = kvm_arm_smmu_v3_init,
	.remove_driver = kvm_arm_smmu_v3_remove
};

static int kvm_arm_smmu_v3_register(void)
{
	return kvm_iommu_register_driver(&kvm_smmu_v3_ops);
}

core_initcall(kvm_arm_smmu_v3_register);
