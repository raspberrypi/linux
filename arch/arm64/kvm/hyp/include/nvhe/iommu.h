/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_KVM_NVHE_IOMMU_H__
#define __ARM64_KVM_NVHE_IOMMU_H__

#include <kvm/iommu.h>
#include <linux/io-pgtable.h>

#if IS_ENABLED(CONFIG_ARM_SMMU_V3_PKVM)
#include <linux/io-pgtable-arm.h>

int kvm_arm_smmu_v3_register(void);

struct io_pgtable *kvm_arm_io_pgtable_alloc(struct io_pgtable_cfg *cfg,
					    unsigned long pgd_hva,
					    void *cookie,
					    int *out_ret);
int kvm_arm_io_pgtable_free(struct io_pgtable *iop);
#else /* CONFIG_ARM_SMMU_V3_PKVM */
static inline int kvm_arm_smmu_v3_register(void)
{
	return -EINVAL;
}
#endif /* CONFIG_ARM_SMMU_V3_PKVM */

#if IS_ENABLED(CONFIG_KVM_IOMMU)
int kvm_iommu_init(void);
int kvm_iommu_init_device(struct kvm_hyp_iommu *iommu);
void *kvm_iommu_donate_page(void);
void kvm_iommu_reclaim_page(void *p);

/* Hypercall handlers */
int kvm_iommu_alloc_domain(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			   unsigned long pgd_hva);
int kvm_iommu_free_domain(pkvm_handle_t iommu_id, pkvm_handle_t domain_id);
int kvm_iommu_attach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id);
int kvm_iommu_detach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id);
size_t kvm_iommu_map_pages(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			   unsigned long iova, phys_addr_t paddr, size_t pgsize,
			   size_t pgcount, int prot);
size_t kvm_iommu_unmap_pages(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			     unsigned long iova, size_t pgsize, size_t pgcount);
phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t iommu_id,
				   pkvm_handle_t domain_id, unsigned long iova);
#else /* !CONFIG_KVM_IOMMU */
static inline int kvm_iommu_alloc_domain(pkvm_handle_t iommu_id,
					 pkvm_handle_t domain_id,
					 unsigned long pgd_hva)
{
	return -ENODEV;
}

static inline int kvm_iommu_free_domain(pkvm_handle_t iommu_id,
					pkvm_handle_t domain_id)
{
	return -ENODEV;
}

static inline int kvm_iommu_attach_dev(pkvm_handle_t iommu_id,
				       pkvm_handle_t domain_id,
				       u32 endpoint_id)
{
	return -ENODEV;
}

static inline int kvm_iommu_detach_dev(pkvm_handle_t iommu_id,
				       pkvm_handle_t domain_id,
				       u32 endpoint_id)
{
	return -ENODEV;
}

static inline size_t kvm_iommu_map_pages(pkvm_handle_t iommu_id,
					 pkvm_handle_t domain_id,
					 unsigned long iova, phys_addr_t paddr,
					 size_t pgsize, size_t pgcount, int prot)
{
	return 0;
}

static inline size_t kvm_iommu_unmap_pages(pkvm_handle_t iommu_id,
					   pkvm_handle_t domain_id,
					   unsigned long iova, size_t pgsize,
					   size_t pgcount)
{
	return 0;
}

static inline phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t iommu_id,
						 pkvm_handle_t domain_id,
						 unsigned long iova)
{
	return 0;
}
#endif /* CONFIG_KVM_IOMMU */

struct kvm_iommu_ops {
	int (*init)(void);
	struct kvm_hyp_iommu *(*get_iommu_by_id)(pkvm_handle_t smmu_id);
	int (*alloc_domain)(struct kvm_hyp_iommu_domain *domain, unsigned long pgd_hva);
	void (*free_domain)(struct kvm_hyp_iommu_domain *domain);
	int (*attach_dev)(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			  u32 endpoint_id);
	int (*detach_dev)(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			  u32 endpoint_id);
};

extern struct kvm_iommu_ops kvm_iommu_ops;

#endif /* __ARM64_KVM_NVHE_IOMMU_H__ */
