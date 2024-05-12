/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_KVM_NVHE_IOMMU_H__
#define __ARM64_KVM_NVHE_IOMMU_H__

#include <asm/kvm_pgtable.h>

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
		   struct kvm_hyp_memcache *atomic_mc,
		   unsigned long init_arg);
int kvm_iommu_init_device(struct kvm_hyp_iommu *iommu);
void *kvm_iommu_donate_pages(u8 order, bool request);
void kvm_iommu_reclaim_pages(void *p, u8 order);
int kvm_iommu_request(struct kvm_hyp_req *req);

#define kvm_iommu_donate_page()			kvm_iommu_donate_pages(0, true)
#define kvm_iommu_reclaim_page(p)		kvm_iommu_reclaim_pages(p, 0)
#define kvm_iommu_donate_pages_request(order)	kvm_iommu_donate_pages(order, true)

/* alloc/free from atomic pool. */
void *kvm_iommu_donate_pages_atomic(u8 order);
void kvm_iommu_reclaim_pages_atomic(void *p, u8 order);

/* Hypercall handlers */
int kvm_iommu_alloc_domain(pkvm_handle_t domain_id, u32 type);
int kvm_iommu_free_domain(pkvm_handle_t domain_id);
int kvm_iommu_attach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid, u32 pasid_bits);
int kvm_iommu_detach_dev(pkvm_handle_t iommu_id, pkvm_handle_t domain_id,
			 u32 endpoint_id, u32 pasid);
size_t kvm_iommu_map_pages(pkvm_handle_t domain_id,
			   unsigned long iova, phys_addr_t paddr, size_t pgsize,
			   size_t pgcount, int prot);
size_t kvm_iommu_unmap_pages(pkvm_handle_t domain_id,
			     unsigned long iova, size_t pgsize, size_t pgcount);
phys_addr_t kvm_iommu_iova_to_phys(pkvm_handle_t domain_id, unsigned long iova);
bool kvm_iommu_host_dabt_handler(struct kvm_cpu_context *host_ctxt, u64 esr, u64 addr);
void kvm_iommu_iotlb_gather_add_page(struct kvm_hyp_iommu_domain *domain,
				     struct iommu_iotlb_gather *gather,
				     unsigned long iova,
				     size_t size);
void kvm_iommu_host_stage2_idmap(phys_addr_t start, phys_addr_t end,
				 enum kvm_pgtable_prot prot);
int kvm_iommu_snapshot_host_stage2(struct kvm_hyp_iommu_domain *domain);

#define KVM_IOMMU_PADDR_CACHE_MAX		((size_t)511)
/**
 * struct kvm_iommu_paddr_cache - physical address cache, passed with unmap calls
 *  which is expected to hold all the unmapped physical addresses so the
 *  hypervisor can keep track of available pages for donation.
 *  It is guaranteed the unmap call will not unmap more tham KVM_IOMMU_PADDR_CACHE_MAX
 * @ptr: Current pointer to empty entry.
 * @paddr: Physical address.
 * @pgsize: Size of physical address.
 */
struct kvm_iommu_paddr_cache {
	unsigned short	ptr;
	u64		paddr[KVM_IOMMU_PADDR_CACHE_MAX];
	size_t		pgsize[KVM_IOMMU_PADDR_CACHE_MAX];
};

/**
 * struct kvm_iommu_ops - KVM iommu ops
 * @init: init the driver called once before the kernel de-privilege
 * @get_iommu_by_id: Return kvm_hyp_iommu from an ID passed from the kernel.
 *		     It is driver specific how the driver assign IDs.
 * @alloc_domain: allocate iommu domain.
 * @free_domain: free iommu domain.
 * @attach_dev: Attach a device to a domain.
 * @detach_dev: Detach a device from a domain.
 * @dabt_handler: data abort for MMIO, can be used for emulating access to IOMMU.
 * @suspend: Power suspended.
 * @resume: Power resumed.
 * @iotlb_sync: Sync iotlb_gather (similar to the kernel).
 * @host_stage2_idmap: Identity map a range.
 * @map_pages: Map pages in a domain.
 * @unmap_pages: Unmap pages from a domain.
 * @iova_to_phys: get physical address from IOVA in a domain.
 */
struct kvm_iommu_ops {
	int (*init)(unsigned long arg);
	struct kvm_hyp_iommu *(*get_iommu_by_id)(pkvm_handle_t smmu_id);
	int (*alloc_domain)(struct kvm_hyp_iommu_domain *domain, u32 type);
	void (*free_domain)(struct kvm_hyp_iommu_domain *domain);
	int (*attach_dev)(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			  u32 endpoint_id, u32 pasid, u32 pasid_bits);
	int (*detach_dev)(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			  u32 endpoint_id, u32 pasid);

	bool (*dabt_handler)(struct kvm_cpu_context *host_ctxt, u64 esr, u64 addr);
	int (*suspend)(struct kvm_hyp_iommu *iommu);
	int (*resume)(struct kvm_hyp_iommu *iommu);
	void (*iotlb_sync)(struct kvm_hyp_iommu_domain *domain,
			   struct iommu_iotlb_gather *gather);
	void (*host_stage2_idmap)(struct kvm_hyp_iommu_domain *domain,
				  phys_addr_t start, phys_addr_t end, int prot);
	int (*map_pages)(struct kvm_hyp_iommu_domain *domain, unsigned long iova,
			 phys_addr_t paddr, size_t pgsize,
			 size_t pgcount, int prot, size_t *total_mapped);
	size_t (*unmap_pages)(struct kvm_hyp_iommu_domain *domain, unsigned long iova,
			      size_t pgsize, size_t pgcount,
			      struct iommu_iotlb_gather *gather,
			      struct kvm_iommu_paddr_cache *cache);
	phys_addr_t (*iova_to_phys)(struct kvm_hyp_iommu_domain *domain, unsigned long iova);
	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
	ANDROID_KABI_RESERVE(3);
	ANDROID_KABI_RESERVE(4);
	ANDROID_KABI_RESERVE(5);
	ANDROID_KABI_RESERVE(6);
	ANDROID_KABI_RESERVE(7);
	ANDROID_KABI_RESERVE(8);
};

extern struct kvm_iommu_ops *kvm_iommu_ops;
extern struct hyp_mgt_allocator_ops kvm_iommu_allocator_ops;

#endif /* __ARM64_KVM_NVHE_IOMMU_H__ */
