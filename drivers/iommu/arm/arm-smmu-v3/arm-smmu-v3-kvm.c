// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM host driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_pkvm.h>
#include <asm/kvm_mmu.h>
#include <linux/local_lock.h>
#include <linux/moduleparam.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>

#include "pkvm/arm_smmu_v3.h"

#include "arm-smmu-v3.h"

struct host_arm_smmu_device {
	struct arm_smmu_device		smmu;
	pkvm_handle_t			id;
	u32				boot_gbpa;
	bool				hvc_pd;
	struct io_pgtable_cfg		cfg_s1;
	struct io_pgtable_cfg		cfg_s2;
};

#define smmu_to_host(_smmu) \
	container_of(_smmu, struct host_arm_smmu_device, smmu);

struct kvm_arm_smmu_master {
	struct arm_smmu_device		*smmu;
	struct device			*dev;
	struct xarray			domains;
	u32				ssid_bits;
	bool				idmapped; /* Stage-2 is transparently identity mapped*/
};

struct kvm_arm_smmu_domain {
	struct iommu_domain		domain;
	struct arm_smmu_device		*smmu;
	struct mutex			init_mutex;
	pkvm_handle_t			id;
	unsigned long			type;
};

#define to_kvm_smmu_domain(_domain) \
	container_of(_domain, struct kvm_arm_smmu_domain, domain)

#ifdef MODULE
static unsigned long                   pkvm_module_token;

#define ksym_ref_addr_nvhe(x) \
	((typeof(kvm_nvhe_sym(x)) *)(pkvm_el2_mod_va(&kvm_nvhe_sym(x), pkvm_module_token)))
#else
#define ksym_ref_addr_nvhe(x) \
	((typeof(kvm_nvhe_sym(x)) *)(kern_hyp_va(lm_alias(&kvm_nvhe_sym(x)))))
#endif

static size_t				kvm_arm_smmu_cur;
static size_t				kvm_arm_smmu_count;
static struct hyp_arm_smmu_v3_device	*kvm_arm_smmu_array;
static DEFINE_IDA(kvm_arm_smmu_domain_ida);

int kvm_nvhe_sym(smmu_init_hyp_module)(const struct pkvm_module_ops *ops);
extern struct kvm_iommu_ops kvm_nvhe_sym(smmu_ops);

/*
 * Pre allocated pages that can be used from the EL2 part of the driver from atomic
 * context, ideally used for page table pages for identity domains.
 */
static int atomic_pages;
module_param(atomic_pages, int, 0);

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
	int ret;
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
	device_property_read_u32(dev, "pasid-num-bits", &master->ssid_bits);
	master->ssid_bits = min(smmu->ssid_bits, master->ssid_bits);
	xa_init(&master->domains);
	master->idmapped = device_property_read_bool(dev, "iommu-idmapped");
	dev_iommu_priv_set(dev, master);

	if (!device_link_add(dev, smmu->dev,
			     DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE |
			     DL_FLAG_AUTOREMOVE_SUPPLIER)) {
		ret = -ENOLINK;
		goto err_free;
	}

	return &smmu->iommu;

err_free:
	kfree(master);
	return ERR_PTR(ret);
}

static void kvm_arm_smmu_release_device(struct device *dev)
{
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);

	xa_destroy(&master->domains);
	kfree(master);
	iommu_fwspec_free(dev);
}

static struct iommu_domain *kvm_arm_smmu_domain_alloc(unsigned type)
{
	struct kvm_arm_smmu_domain *kvm_smmu_domain;

	/*
	 * We don't support
	 * - IOMMU_DOMAIN_DMA_FQ because lazy unmap would clash with memory
	 *   donation to guests.
	 */
	if (type != IOMMU_DOMAIN_DMA &&
	    type != IOMMU_DOMAIN_UNMANAGED &&
	    type != IOMMU_DOMAIN_IDENTITY)
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
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	if (kvm_smmu_domain->smmu) {
		if (kvm_smmu_domain->smmu != smmu)
			return -EINVAL;
		return 0;
	}

	kvm_smmu_domain->smmu = smmu;

	if (kvm_smmu_domain->domain.type == IOMMU_DOMAIN_IDENTITY) {
		kvm_smmu_domain->id = KVM_IOMMU_DOMAIN_IDMAP_ID;
		/*
		 * Identity domains doesn't use the DMA API, so no need to
		 * set the  domain aperture.
		 */
		return 0;
	}

	ret = ida_alloc_range(&kvm_arm_smmu_domain_ida, KVM_IOMMU_DOMAIN_NR_START,
			      KVM_IOMMU_MAX_DOMAINS, GFP_KERNEL);
	if (ret < 0)
		return ret;

	kvm_smmu_domain->id = ret;

	/* Default to stage-1. */
	if (smmu->features & ARM_SMMU_FEAT_TRANS_S1) {
		kvm_smmu_domain->type = KVM_ARM_SMMU_DOMAIN_S1;
		kvm_smmu_domain->domain.pgsize_bitmap = host_smmu->cfg_s1.pgsize_bitmap;
		kvm_smmu_domain->domain.geometry.aperture_end = (1UL << host_smmu->cfg_s1.ias) - 1;
	} else {
		kvm_smmu_domain->type = KVM_ARM_SMMU_DOMAIN_S2;
		kvm_smmu_domain->domain.pgsize_bitmap = host_smmu->cfg_s2.pgsize_bitmap;
		kvm_smmu_domain->domain.geometry.aperture_end = (1UL << host_smmu->cfg_s2.ias) - 1;
	}
	kvm_smmu_domain->domain.geometry.force_aperture = true;

	ret = kvm_call_hyp_nvhe_mc(smmu, __pkvm_host_iommu_alloc_domain,
				   kvm_smmu_domain->id, kvm_smmu_domain->type);

	return ret;
}

static void kvm_arm_smmu_domain_free(struct iommu_domain *domain)
{
	int ret;
	struct kvm_arm_smmu_domain *kvm_smmu_domain = to_kvm_smmu_domain(domain);
	struct arm_smmu_device *smmu = kvm_smmu_domain->smmu;

	if (smmu && (kvm_smmu_domain->domain.type != IOMMU_DOMAIN_IDENTITY)) {
		ret = kvm_call_hyp_nvhe(__pkvm_host_iommu_free_domain, kvm_smmu_domain->id);
		ida_free(&kvm_arm_smmu_domain_ida, kvm_smmu_domain->id);
	}
	kfree(kvm_smmu_domain);
}

static int kvm_arm_smmu_detach_dev_pasid(struct host_arm_smmu_device *host_smmu,
					 struct kvm_arm_smmu_master *master,
					 ioasid_t pasid)
{
	int i, ret;
	struct arm_smmu_device *smmu = &host_smmu->smmu;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(master->dev);
	struct kvm_arm_smmu_domain *domain = xa_load(&master->domains, pasid);

	if (!domain)
		return 0;

	for (i = 0; i < fwspec->num_ids; i++) {
		int sid = fwspec->ids[i];

		ret = kvm_call_hyp_nvhe(__pkvm_host_iommu_detach_dev,
					host_smmu->id, domain->id, sid, pasid);
		if (ret) {
			dev_err(smmu->dev, "cannot detach device %s (0x%x): %d\n",
				dev_name(master->dev), sid, ret);
			break;
		}
	}

	xa_erase(&master->domains, pasid);

	return ret;
}

static int kvm_arm_smmu_detach_dev(struct host_arm_smmu_device *host_smmu,
				   struct kvm_arm_smmu_master *master)
{
	return kvm_arm_smmu_detach_dev_pasid(host_smmu, master, 0);
}

static void kvm_arm_smmu_remove_dev_pasid(struct device *dev, ioasid_t pasid)
{
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(master->smmu);

	kvm_arm_smmu_detach_dev_pasid(host_smmu, master, pasid);
}

static int kvm_arm_smmu_set_dev_pasid(struct iommu_domain *domain,
				      struct device *dev, ioasid_t pasid)
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

	ret = kvm_arm_smmu_detach_dev_pasid(host_smmu, master, pasid);
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
					   sid, pasid, master->ssid_bits);
		if (ret) {
			dev_err(smmu->dev, "cannot attach device %s (0x%x): %d\n",
				dev_name(dev), sid, ret);
			goto out_ret;
		}
	}
	ret = xa_insert(&master->domains, pasid, kvm_smmu_domain, GFP_KERNEL);

out_ret:
	if (ret)
		kvm_arm_smmu_detach_dev(host_smmu, master);
	return ret;
}

static int kvm_arm_smmu_attach_dev(struct iommu_domain *domain,
				   struct device *dev)
{
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);

	/* If anything other than pasid 0 attached, we can't support through attach_dev. */
	if(!xa_empty(&master->domains) && !xa_load(&master->domains, 0))
		return -EBUSY;

	return kvm_arm_smmu_set_dev_pasid(domain, dev, 0);
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

static int kvm_arm_smmu_def_domain_type(struct device *dev)
{
	struct kvm_arm_smmu_master *master = dev_iommu_priv_get(dev);

	if (master->idmapped && atomic_pages)
		return IOMMU_DOMAIN_IDENTITY;
	return 0;
}

static struct iommu_ops kvm_arm_smmu_ops = {
	.capable		= arm_smmu_capable,
	.device_group		= arm_smmu_device_group,
	.of_xlate		= arm_smmu_of_xlate,
	.probe_device		= kvm_arm_smmu_probe_device,
	.release_device		= kvm_arm_smmu_release_device,
	.domain_alloc		= kvm_arm_smmu_domain_alloc,
	.pgsize_bitmap		= -1UL,
	.remove_dev_pasid	= kvm_arm_smmu_remove_dev_pasid,
	.owner			= THIS_MODULE,
	.def_domain_type	= kvm_arm_smmu_def_domain_type,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= kvm_arm_smmu_attach_dev,
		.free		= kvm_arm_smmu_domain_free,
		.map_pages	= kvm_arm_smmu_map_pages,
		.unmap_pages	= kvm_arm_smmu_unmap_pages,
		.iova_to_phys	= kvm_arm_smmu_iova_to_phys,
		.set_dev_pasid	= kvm_arm_smmu_set_dev_pasid,
	}
};

static bool kvm_arm_smmu_validate_features(struct arm_smmu_device *smmu)
{
	unsigned int required_features =
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

	return true;
}

static irqreturn_t kvm_arm_smmu_evt_handler(int irq, void *dev)
{
	int i;
	struct arm_smmu_device *smmu = dev;
	struct arm_smmu_queue *q = &smmu->evtq.q;
	struct arm_smmu_ll_queue *llq = &q->llq;
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	u64 evt[EVTQ_ENT_DWORDS];

	if (pm_runtime_get_if_in_use(smmu->dev) != 1) {
		dev_err(smmu->dev,"Skip EVTQ as device is OFF\n");
		return IRQ_HANDLED;
	}

	do {
		while (!queue_remove_raw(q, evt)) {
			u8 id = FIELD_GET(EVTQ_0_ID, evt[0]);

			if (!__ratelimit(&rs))
				continue;

			dev_info(smmu->dev, "event 0x%02x received:\n", id);
			for (i = 0; i < ARRAY_SIZE(evt); ++i)
				dev_info(smmu->dev, "\t0x%016llx\n",
					 (unsigned long long)evt[i]);

			cond_resched();
		}

		/*
		 * Not much we can do on overflow, so scream and pretend we're
		 * trying harder.
		 */
		if (queue_sync_prod_in(q) == -EOVERFLOW)
			dev_err(smmu->dev, "EVTQ overflow detected -- events lost\n");
	} while (!queue_empty(llq));

	/* Sync our overflow flag, as we believe we're up to speed */
	queue_sync_cons_ovf(q);
	pm_runtime_put(smmu->dev);
	return IRQ_HANDLED;
}

static irqreturn_t kvm_arm_smmu_gerror_handler(int irq, void *dev)
{
	u32 gerror, gerrorn, active;
	struct arm_smmu_device *smmu = dev;

	if (pm_runtime_get_if_in_use(smmu->dev) != 1) {
		dev_err(smmu->dev,"Skip GERROR as device is OFF\n");
		return IRQ_HANDLED;
	}

	gerror = readl_relaxed(smmu->base + ARM_SMMU_GERROR);
	gerrorn = readl_relaxed(smmu->base + ARM_SMMU_GERRORN);

	active = gerror ^ gerrorn;
	if (!(active & GERROR_ERR_MASK)) {
		pm_runtime_put(smmu->dev);
		return IRQ_NONE; /* No errors pending */
	}

	dev_warn(smmu->dev,
		 "unexpected global error reported (0x%08x), this could be serious\n",
		 active);

	/* There is no API to reconfigure the device at the moment.*/
	if (active & GERROR_SFM_ERR)
		dev_err(smmu->dev, "device has entered Service Failure Mode!\n");

	if (active & GERROR_MSI_GERROR_ABT_ERR)
		dev_warn(smmu->dev, "GERROR MSI write aborted\n");

	if (active & GERROR_MSI_PRIQ_ABT_ERR)
		dev_warn(smmu->dev, "PRIQ MSI write aborted\n");

	if (active & GERROR_MSI_EVTQ_ABT_ERR)
		dev_warn(smmu->dev, "EVTQ MSI write aborted\n");

	if (active & GERROR_MSI_CMDQ_ABT_ERR)
		dev_warn(smmu->dev, "CMDQ MSI write aborted\n");

	if (active & GERROR_PRIQ_ABT_ERR)
		dev_err(smmu->dev, "PRIQ write aborted -- events may have been lost\n");

	if (active & GERROR_EVTQ_ABT_ERR)
		dev_err(smmu->dev, "EVTQ write aborted -- events may have been lost\n");

	if (active & GERROR_CMDQ_ERR) {
		dev_err(smmu->dev, "CMDQ ERR -- Hypervisor corruption\n");
		BUG();
	}

	writel(gerror, smmu->base + ARM_SMMU_GERRORN);

	pm_runtime_put(smmu->dev);
	return IRQ_HANDLED;
}

static irqreturn_t kvm_arm_smmu_pri_handler(int irq, void *dev)
{
	struct arm_smmu_device *smmu = dev;

	dev_err(smmu->dev, "PRI not supported in KVM driver!\n");

	return IRQ_HANDLED;
}

static int kvm_arm_smmu_device_reset(struct host_arm_smmu_device *host_smmu)
{
	int ret;
	u32 reg;
	struct arm_smmu_device *smmu = &host_smmu->smmu;
	u32 irqen_flags = IRQ_CTRL_EVTQ_IRQEN | IRQ_CTRL_GERROR_IRQEN;

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

	/* Event queue */
	writeq_relaxed(smmu->evtq.q.q_base, smmu->base + ARM_SMMU_EVTQ_BASE);
	writel_relaxed(smmu->evtq.q.llq.prod, smmu->base + SZ_64K + ARM_SMMU_EVTQ_PROD);
	writel_relaxed(smmu->evtq.q.llq.cons, smmu->base + SZ_64K + ARM_SMMU_EVTQ_CONS);

	/* Disable IRQs first */
	ret = arm_smmu_write_reg_sync(smmu, 0, ARM_SMMU_IRQ_CTRL,
				      ARM_SMMU_IRQ_CTRLACK);
	if (ret) {
		dev_err(smmu->dev, "failed to disable irqs\n");
		return ret;
	}

	/*
	 * We don't support combined irqs for now, no specific reason, they are uncommon
	 * so we just try to avoid bloating the code.
	 */
	if (smmu->combined_irq)
		dev_err(smmu->dev, "Combined irqs not supported by this driver\n");
	else
		arm_smmu_setup_unique_irqs(smmu, kvm_arm_smmu_evt_handler,
					   kvm_arm_smmu_gerror_handler,
					   kvm_arm_smmu_pri_handler);

	if (smmu->features & ARM_SMMU_FEAT_PRI)
		irqen_flags |= IRQ_CTRL_PRIQ_IRQEN;

	/* Enable interrupt generation on the SMMU */
	ret = arm_smmu_write_reg_sync(smmu, irqen_flags,
				      ARM_SMMU_IRQ_CTRL, ARM_SMMU_IRQ_CTRLACK);
	if (ret)
		dev_warn(smmu->dev, "failed to enable irqs\n");

	return 0;
}

/* TODO: Move this. None of it is specific to SMMU */
static int kvm_arm_probe_power_domain(struct device *dev,
				      struct kvm_power_domain *pd)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	if (!of_get_property(dev->of_node, "power-domains", NULL)) {
		/* SMMU MUST RESET TO BLOCK DMA. */
		dev_warn(dev, "No power-domains assuming host control\n");
	}

	pd->type = KVM_POWER_DOMAIN_HOST_HVC;
	pd->device_id = kvm_arm_smmu_cur;
	host_smmu->hvc_pd = true;
	return 0;
}

static int kvm_arm_smmu_probe(struct platform_device *pdev)
{
	int ret;
	bool bypass;
	struct resource *res;
	phys_addr_t mmio_addr;
	struct io_pgtable_cfg cfg_s1, cfg_s2;
	size_t mmio_size, pgd_size;
	struct arm_smmu_device *smmu;
	struct device *dev = &pdev->dev;
	struct host_arm_smmu_device *host_smmu;
	struct hyp_arm_smmu_v3_device *hyp_smmu;
	struct kvm_power_domain power_domain = {};
	unsigned long ias;

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

	platform_set_drvdata(pdev, smmu);

	ret = kvm_arm_probe_power_domain(dev, &power_domain);
	if (ret)
		return ret;

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

	arm_smmu_probe_irq(pdev, smmu);

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

	ias = (smmu->features & ARM_SMMU_FEAT_VAX) ? 52 : 48;

	/*
	 * SMMU will hold possible configuration for both S1 and S2 as any of
	 * them can be chosen when a device is attached.
	 */
	cfg_s1 = (struct io_pgtable_cfg) {
		.fmt = ARM_64_LPAE_S1,
		.pgsize_bitmap = smmu->pgsize_bitmap,
		.ias = min_t(unsigned long, ias, VA_BITS),
		.oas = smmu->ias,
		.coherent_walk = smmu->features & ARM_SMMU_FEAT_COHERENCY,
	};
	cfg_s2 = (struct io_pgtable_cfg) {
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
	if (smmu->features & ARM_SMMU_FEAT_TRANS_S1) {
		ret = io_pgtable_configure(&cfg_s1, &pgd_size);
		if (ret)
			return ret;
		host_smmu->cfg_s1 = cfg_s1;
	}
	if (smmu->features & ARM_SMMU_FEAT_TRANS_S2) {
		ret = io_pgtable_configure(&cfg_s2, &pgd_size);
		if (ret)
			return ret;
		host_smmu->cfg_s2 = cfg_s2;
	}
	ret = arm_smmu_init_one_queue(smmu, &smmu->cmdq.q, smmu->base,
				      ARM_SMMU_CMDQ_PROD, ARM_SMMU_CMDQ_CONS,
				      CMDQ_ENT_DWORDS, "cmdq");
	if (ret)
		return ret;

	/* evtq */
	ret = arm_smmu_init_one_queue(smmu, &smmu->evtq.q, smmu->base + SZ_64K,
				      ARM_SMMU_EVTQ_PROD, ARM_SMMU_EVTQ_CONS,
				      EVTQ_ENT_DWORDS, "evtq");
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

	/* Hypervisor parameters */
	hyp_smmu->mmio_addr = mmio_addr;
	hyp_smmu->mmio_size = mmio_size;
	hyp_smmu->features = smmu->features;
	hyp_smmu->pgtable_cfg_s1 = cfg_s1;
	hyp_smmu->pgtable_cfg_s2 = cfg_s2;
	hyp_smmu->iommu.power_domain = power_domain;
	hyp_smmu->ssid_bits = smmu->ssid_bits;

	kvm_arm_smmu_cur++;

	/*
	 * The state of endpoints dictates when the SMMU is powered off. To turn
	 * the SMMU on and off, a genpd driver uses SCMI over the SMC transport,
	 * or some other platform-specific SMC. Those power requests are caught
	 * by the hypervisor, so that the hyp driver doesn't touch the hardware
	 * state while it is off.
	 *
	 * We are making a big assumption here, that TLBs and caches are invalid
	 * on power on, and therefore we don't need to wake the SMMU when
	 * modifying page tables, stream tables and context tables. If this
	 * assumption does not hold on some systems, then we'll need to grab RPM
	 * reference in map(), attach(), etc, so the hyp driver can send
	 * invalidations.
	 */
	hyp_smmu->caches_clean_on_power_on = true;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	/*
	 * Take a reference to keep the SMMU powered on while the hypervisor
	 * initializes it.
	 */
	pm_runtime_resume_and_get(dev);

	return 0;
}

static int kvm_arm_smmu_remove(struct platform_device *pdev)
{
	struct arm_smmu_device *smmu = platform_get_drvdata(pdev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	/*
	 * There was an error during hypervisor setup. The hyp driver may
	 * have already enabled the device, so disable it.
	 */
	pm_runtime_disable(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);
	arm_smmu_unregister_iommu(smmu);
	arm_smmu_device_disable(smmu);
	arm_smmu_update_gbpa(smmu, host_smmu->boot_gbpa, GBPA_ABORT);
	return 0;
}

int kvm_arm_smmu_suspend(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	if (host_smmu->hvc_pd)
		return pkvm_iommu_suspend(dev);
	return 0;
}

int kvm_arm_smmu_resume(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	if (host_smmu->hvc_pd)
		return pkvm_iommu_resume(dev);
	return 0;
}

static const struct dev_pm_ops kvm_arm_smmu_pm_ops = {
	SET_RUNTIME_PM_OPS(kvm_arm_smmu_suspend, kvm_arm_smmu_resume, NULL)
};

static const struct of_device_id arm_smmu_of_match[] = {
	{ .compatible = "arm,smmu-v3", },
	{ },
};

static struct platform_driver kvm_arm_smmu_driver = {
	.driver = {
		.name = "kvm-arm-smmu-v3",
		.of_match_table = arm_smmu_of_match,
		.pm = &kvm_arm_smmu_pm_ops,
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

int smmu_put_device(struct device *dev, void *data)
{
	pm_runtime_put_noidle(dev);
	return 0;
}

int smmu_unregister_smmu(struct device *dev, void *data)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);

	arm_smmu_unregister_iommu(smmu);
	return 0;
}

static int smmu_alloc_atomic_mc(struct kvm_hyp_memcache *atomic_mc)
{
	int ret;
#ifndef MODULE
	u64 i;
	phys_addr_t start, end;

	/*
	 * Allocate pages to cover mapping with PAGE_SIZE for all memory
	 * Then allocate extra for 1GB of MMIO.
	 * Add 10 extra pages as we map the rest with first level blocks
	 * for PAGE_SIZE = 4KB, that should cover 5TB of address space.
	 */
	for_each_mem_range(i, &start, &end) {
		atomic_pages += __hyp_pgtable_max_pages((end - start) >> PAGE_SHIFT);
	}

	atomic_pages += __hyp_pgtable_max_pages(SZ_1G >> PAGE_SHIFT) + 10;
#endif

	/* Module didn't set that parameter. */
	if (!atomic_pages)
		return 0;

	/* For PGD*/
	ret = topup_hyp_memcache(atomic_mc, 1, 3);
	if (ret)
		return ret;
	ret = topup_hyp_memcache(atomic_mc, atomic_pages, 0);
	if (ret)
		return ret;
	pr_info("smmuv3: Allocated %d MiB for atomic usage\n",
		(atomic_pages + (1 << 3)) >> 8);
	/* Topup hyp alloc so IOMMU driver can allocate domains. */
	__pkvm_topup_hyp_alloc(1);

	return ret;
}

/**
 * kvm_arm_smmu_v3_init() - Reserve the SMMUv3 for KVM
 * Return 0 if all present SMMUv3 were probed successfully, or an error.
 *   If no SMMU was found, return 0, with a count of 0.
 */
static int kvm_arm_smmu_v3_init(void)
{
	int ret;
	struct kvm_hyp_memcache atomic_mc = {};

	/*
	 * Check whether any device owned by the host is behind an SMMU.
	 */
	ret = kvm_arm_smmu_array_alloc();
	if (ret || !kvm_arm_smmu_count)
		return ret;

	ret = platform_driver_probe(&kvm_arm_smmu_driver, kvm_arm_smmu_probe);
	if (ret)
		goto err_unregister;

	if (kvm_arm_smmu_cur != kvm_arm_smmu_count) {
		/* A device exists but failed to probe */
		ret = -EUNATCH;
		goto err_unregister;
	}

#ifdef MODULE
	ret = pkvm_load_el2_module(kvm_nvhe_sym(smmu_init_hyp_module),
				   &pkvm_module_token);

	if (ret) {
		pr_err("Failed to load SMMUv3 IOMMU EL2 module: %d\n", ret);
		goto err_unregister;
	}
#endif
	/*
	 * These variables are stored in the nVHE image, and won't be accessible
	 * after KVM initialization. Ownership of kvm_arm_smmu_array will be
	 * transferred to the hypervisor as well.
	 *
	 * kvm_arm_smmu_memcache is shared between hypervisor and host.
	 */
	kvm_hyp_arm_smmu_v3_smmus = kvm_arm_smmu_array;
	kvm_hyp_arm_smmu_v3_count = kvm_arm_smmu_count;

	ret = smmu_alloc_atomic_mc(&atomic_mc);
	if (ret)
		goto err_free_mc;

	ret = kvm_iommu_init_hyp(ksym_ref_addr_nvhe(smmu_ops), &atomic_mc, 0);
	if (ret)
		goto err_free_mc;

	WARN_ON(driver_for_each_device(&kvm_arm_smmu_driver.driver, NULL,
				       NULL, smmu_put_device));
	return 0;
err_free_mc:
	free_hyp_memcache(&atomic_mc);
err_unregister:
	pr_err("pKVM SMMUv3 init failed with %d\n", ret);
	WARN_ON(driver_for_each_device(&kvm_arm_smmu_driver.driver, NULL,
				       NULL, smmu_unregister_smmu));
	return 0;
}

static void kvm_arm_smmu_v3_remove(void)
{
	platform_driver_unregister(&kvm_arm_smmu_driver);
}

pkvm_handle_t kvm_arm_smmu_v3_id(struct device *dev)
{
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);
	struct host_arm_smmu_device *host_smmu = smmu_to_host(smmu);

	return host_smmu->id;
}

struct kvm_iommu_driver kvm_smmu_v3_ops = {
	.init_driver = kvm_arm_smmu_v3_init,
	.remove_driver = kvm_arm_smmu_v3_remove,
	.get_iommu_id = kvm_arm_smmu_v3_id,
};

static int kvm_arm_smmu_v3_register(void)
{
	return kvm_iommu_register_driver(&kvm_smmu_v3_ops);
}

/*
 * Register must be run before de-privliage before kvm_iommu_init_driver
 * for module case, it should be loaded using pKVM early loading which
 * loads it before this point.
 * For builtin drivers we use core_initcall
 */
#ifdef MODULE
module_init(kvm_arm_smmu_v3_register);
#else
core_initcall(kvm_arm_smmu_v3_register);
#endif

MODULE_LICENSE("GPL v2");
