/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_KVM_NVHE_IOMMU_H__
#define __ARM64_KVM_NVHE_IOMMU_H__

#include <kvm/iommu.h>
#include <linux/io-pgtable.h>

#if IS_ENABLED(CONFIG_ARM_SMMU_V3_PKVM)
#include <linux/io-pgtable-arm.h>

struct io_pgtable *kvm_arm_io_pgtable_alloc(struct io_pgtable_cfg *cfg,
					    void *cookie,
					    int *out_ret);
int kvm_arm_io_pgtable_free(struct io_pgtable *iop);
#endif /* CONFIG_ARM_SMMU_V3_PKVM */

int kvm_iommu_init(struct kvm_iommu_ops *ops,
		   unsigned long init_arg);
int kvm_iommu_init_device(struct kvm_hyp_iommu *iommu);
void *kvm_iommu_donate_pages(u8 order, bool request);
void kvm_iommu_reclaim_pages(void *p, u8 order);
int kvm_iommu_request(struct kvm_hyp_req *req);

#define kvm_iommu_donate_page()			kvm_iommu_donate_pages(0, true)
#define kvm_iommu_reclaim_page(p)		kvm_iommu_reclaim_pages(p, 0)
#define kvm_iommu_donate_pages_request(order)	kvm_iommu_donate_pages(order, true)

/* Hypercall handlers */
int kvm_iommu_alloc_domain(pkvm_handle_t domain_id);
int kvm_iommu_free_domain(pkvm_handle_t domain_id);
int kvm_iommu_attach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id);
int kvm_iommu_detach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id);
size_t kvm_iommu_map_pages(pkvm_handle_t domain_id,
			   unsigned long iova, phys_addr_t paddr, size_t pgsize,
			   size_t pgcount, int prot);
size_t kvm_iommu_unmap_pages(pkvm_handle_t domain_id,
			     unsigned long iova, size_t pgsize, size_t pgcount);
phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t domain_id, unsigned long iova);

struct kvm_iommu_ops {
	int (*init)(unsigned long arg);
	struct kvm_hyp_iommu *(*get_iommu_by_id)(pkvm_handle_t smmu_id);
	int (*alloc_domain)(struct kvm_hyp_iommu_domain *domain);
	void (*free_domain)(struct kvm_hyp_iommu_domain *domain);
	int (*attach_dev)(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			  u32 endpoint_id);
	int (*detach_dev)(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			  u32 endpoint_id);
	int (*suspend)(struct kvm_hyp_iommu *iommu);
	int (*resume)(struct kvm_hyp_iommu *iommu);
};

extern struct kvm_iommu_ops *kvm_iommu_ops;
extern struct hyp_mgt_allocator_ops kvm_iommu_allocator_ops;

#endif /* __ARM64_KVM_NVHE_IOMMU_H__ */
