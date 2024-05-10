/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ARM64_KVM_PKVM_MODULE_H__
#define __ARM64_KVM_PKVM_MODULE_H__

#include <asm/kvm_pgtable.h>
#include <linux/android_kabi.h>
#include <linux/export.h>
#include <linux/android_kabi.h>

typedef void (*dyn_hcall_t)(struct user_pt_regs *);
struct kvm_hyp_iommu;
struct iommu_iotlb_gather;
struct kvm_hyp_iommu_domain;

#ifdef CONFIG_MODULES
enum pkvm_psci_notification {
	PKVM_PSCI_CPU_SUSPEND,
	PKVM_PSCI_SYSTEM_SUSPEND,
	PKVM_PSCI_CPU_ENTRY,
};

/**
 * struct pkvm_module_ops - pKVM modules callbacks
 * @create_private_mapping:	Map a memory region into the hypervisor private
 *				range. @haddr returns the virtual address where
 *				the mapping starts. It can't be unmapped. Host
 *				access permissions are unaffected.
 * @alloc_module_va:		Reserve a range of VA space in the hypervisor
 *				private range. This is handy for modules that
 *				need to map plugin code in a similar fashion to
 *				how pKVM maps module code. That space could also
 *				be used to map memory temporarily, when the
 *				fixmap granularity (PAGE_SIZE) is too small.
 * @map_module_page:		Used in conjunction with @alloc_module_va. When
 *				@is_protected is not set, the page is also
 *				unmapped from the host stage-2.
 * @register_serial_driver:	Register a driver for a serial interface. The
 *				framework only needs a single callback
 *				@hyp_putc_cb which is expected to print a single
 *				character.
 * @puts:			If a serial interface is registered, print a
 *				string, else does nothing.
 * @putx64:			If a serial interface is registered, print a
 *				64-bit number, else does nothing.
 * @fixmap_map:			Map a page in the per-CPU hypervisor fixmap.
 *				This is intended to be used for temporary
 *				mappings in the hypervisor VA space.
 *				@fixmap_unmap must be called between each
 *				mapping to do cache maintenance and ensure the
 *				new mapping is visible.
 * @fixmap_unmap:		Unmap a page from the hypervisor fixmap. This
 * 				call is required between each @fixmap_map().
 * @linear_map_early:		Map a large portion of memory into the
 *				hypervisor linear VA space. This is intended to
 *				be used only for module bootstrap and must be
 *				unmapped before the host is deprivilged.
 * @linear_unmap_early:		See @linear_map_early.
 * @flush_dcache_to_poc:	Clean the data cache to the point of coherency.
 *				This is not a requirement for any other of the
 *				pkvm_module_ops callbacks.
 * @update_hcr_el2:		Modify the running value of HCR_EL2. pKVM will
 *				save/restore the new value across power
 *				management transitions.
 * @update_hfgwtr_el2:		Modify the running value of HFGWTR_EL2. pKVM
 *				will save/restore the new value across power
 *				management transitions.
 * @register_host_perm_fault_handler:
 *				@cb is called whenever the host generates an
 *				abort with the fault status code Permission
 *				Fault. This is useful when a module changes the
 *				host stage-2 permissions for certain pages.
 *				Up-to 16 handlers can be registered. Returning
 *				-EPERM lets pKVM handle the abort while on 0,
 *				the next handler will be called. The handler
 *				order depends on the registration order.
 * @host_stage2_mod_prot:	Apply @prot to the page @pfn. This requires a
 *				permission fault handler to be registered (see
 *				@register_host_perm_fault_handler), otherwise
 *				pKVM will be unable to handle this fault and the
 *				CPU will be stuck in an infinite loop. @nr_pages
 *				allows to apply this prot on a range of
 *				contiguous memory.
 * @host_stage2_get_leaf:	Query the host's stage2 page-table entry for
 *				the page @phys.
 * @register_host_smc_handler:	@cb is called whenever the host issues an SMC
 *				pKVM couldn't handle. If @cb returns false, the
 *				SMC will be forwarded to EL3.
 * @register_default_trap_handler:
 *				@cb is called whenever EL2 traps EL1 and pKVM
 *				has not handled it. If @cb returns false, the
 *				hypervisor will panic. This trap handler must be
 *				registered whenever changes are made to HCR
 *				(@update_hcr_el2) or HFGWTR
 *				(@update_hfgwtr_el2).
 * @register_illegal_abt_notifier:
 *				To notify the module of a pending illegal abort
 *				from the host. On @cb return, the abort will be
 *				injected back into the host.
 * @register_psci_notifier:	To notify the module of a pending PSCI event.
 * @register_hyp_panic_notifier:
 *				To notify the module of a pending hypervisor
 *				panic. On return from @cb, the panic will occur.
 * @register_unmask_serror:	When @unmask returns true, the hypervisor will
 * 				unmask SErrors at EL2. Although the hypervisor
 *				cannot recover from an SError (and will panic if
 *				one occurs), they can be useful for debugging in
 *				some situations. @mask is the @unmask twin and
 *				is called before remasking SErrors.
 * @host_donate_hyp:		The page @pfn is unmapped from the host and
 *				full control is given to the hypervisor.
 * @host_donate_hyp_prot:	As host_donate_hyp_prot, but this variant sets
 *				the prot of the hyp.
 * @hyp_donate_host:		The page @pfn whom control has previously been
 *				given to the hypervisor (@host_donate_hyp) is
 *				given back to the host.
 * @host_share_hyp:		The page @pfn will be shared between the host
 *				and the hypervisor. Must be followed by
 *				@pin_shared_mem.
 * @host_unshare_hyp:		The page @pfn will be unshared and unmapped from
 *				the hypervisor. Must be called after
 *				@unpin_shared_mem.
 * @pin_shared_mem:		After @host_share_hyp, the newly shared page is
 *				still owned by the host. @pin_shared_mem will
 *				prevent the host from reclaiming that page until
 *				the hypervisor releases it (@unpin_shared_mem)
 * @unpin_shared_mem:		Enable the host to reclaim the shared memory
 *				(@host_unshare_hyp).
 * @memcpy:			Same as kernel memcpy, but use hypervisor VAs.
 * @memset:			Same as kernel memset, but use a hypervisor VA.
 * @hyp_pa:			Return the physical address for a hypervisor
 *				virtual address in the linear range.
 * @hyp_va:			Convert a physical address into a virtual one.
 * @kern_hyp_va:		Convert a kernel virtual address into an
 *				hypervisor virtual one.
 * @hyp_alloc:			Allocate memory in hyp VA space.
 * @hyp_alloc_errno:		Error in case hyp_alloc() returns NULL.
 * @hyp_free:			Free memory allocated  from hyp_alloc().
 * @iommu_donate_pages:		Allocate memory from IOMMU pool.
 * @iommu_reclaim_pages:	Reclaim memory from iommu_donate_pages()
 * @iommu_request:		Fill a request that is returned from the entry HVC (see hyp-main.c).
 * @iommu_init_device:		Initialize common IOMMU fields.
 * @udelay:			Delay in us.
 * @hyp_alloc_missing_donations:
				Missing donations if allocator returns NULL
 * @__list_add_valid_or_report: Needed if the code uses linked lists.
 * @__list_del_entry_valid_or_report:
				Needed if the code uses linked lists.
 * @iommu_iotlb_gather_add_page: Add a page to the iotlb_gather druing unmap for the IOMMU.
 * @iommu_donate_pages_atomic:	Allocate memory from IOMMU identity pool.
 * @iommu_reclaim_pages_atomic:	Reclaim memory from iommu_donate_pages_atomic()
 * @hyp_smp_processor_id:	Current CPU id
 */
struct pkvm_module_ops {
	int (*create_private_mapping)(phys_addr_t phys, size_t size,
				      enum kvm_pgtable_prot prot,
				      unsigned long *haddr);
	void *(*alloc_module_va)(u64 nr_pages);
	int (*map_module_page)(u64 pfn, void *va, enum kvm_pgtable_prot prot, bool is_protected);
	int (*register_serial_driver)(void (*hyp_putc_cb)(char));
	void (*putc)(char c);
	void (*puts)(const char *s);
	void (*putx64)(u64 x);
	void *(*fixmap_map)(phys_addr_t phys);
	void (*fixmap_unmap)(void);
	void *(*linear_map_early)(phys_addr_t phys, size_t size, enum kvm_pgtable_prot prot);
	void (*linear_unmap_early)(void *addr, size_t size);
	void (*flush_dcache_to_poc)(void *addr, size_t size);
	void (*update_hcr_el2)(unsigned long set_mask, unsigned long clear_mask);
	void (*update_hfgwtr_el2)(unsigned long set_mask, unsigned long clear_mask);
	int (*register_host_perm_fault_handler)(int (*cb)(struct user_pt_regs *regs, u64 esr, u64 addr));
	int (*host_stage2_mod_prot)(u64 pfn, enum kvm_pgtable_prot prot, u64 nr_pages);
	int (*host_stage2_get_leaf)(phys_addr_t phys, kvm_pte_t *ptep, u32 *level);
	int (*register_host_smc_handler)(bool (*cb)(struct user_pt_regs *));
	int (*register_default_trap_handler)(bool (*cb)(struct user_pt_regs *));
	int (*register_illegal_abt_notifier)(void (*cb)(struct user_pt_regs *));
	int (*register_psci_notifier)(void (*cb)(enum pkvm_psci_notification, struct user_pt_regs *));
	int (*register_hyp_panic_notifier)(void (*cb)(struct user_pt_regs *));
	int (*register_unmask_serror)(bool (*unmask)(void), void (*mask)(void));
	int (*host_donate_hyp)(u64 pfn, u64 nr_pages, bool accept_mmio);
	int (*host_donate_hyp_prot)(u64 pfn, u64 nr_pages, bool accept_mmio, enum kvm_pgtable_prot prot);
	int (*hyp_donate_host)(u64 pfn, u64 nr_pages);
	int (*host_share_hyp)(u64 pfn);
	int (*host_unshare_hyp)(u64 pfn);
	int (*pin_shared_mem)(void *from, void *to);
	void (*unpin_shared_mem)(void *from, void *to);
	void* (*memcpy)(void *to, const void *from, size_t count);
	void* (*memset)(void *dst, int c, size_t count);
	phys_addr_t (*hyp_pa)(void *x);
	void* (*hyp_va)(phys_addr_t phys);
	unsigned long (*kern_hyp_va)(unsigned long x);
	void * (*hyp_alloc)(size_t size);
	int (*hyp_alloc_errno)(void);
	void (*hyp_free)(void *addr);
	void * (*iommu_donate_pages)(u8 order, bool request);
	void (*iommu_reclaim_pages)(void *p, u8 order);
	int (*iommu_request)(struct kvm_hyp_req *req);
	int (*iommu_init_device)(struct kvm_hyp_iommu *iommu);
	void (*udelay)(unsigned long usecs);
	u8 (*hyp_alloc_missing_donations)(void);
#ifdef CONFIG_LIST_HARDENED
	/* These 2 functions change calling convention based on CONFIG_DEBUG_LIST. */
	typeof(__list_add_valid_or_report) *list_add_valid_or_report;
	typeof(__list_del_entry_valid_or_report) *list_del_entry_valid_or_report;
#endif
	void (*iommu_iotlb_gather_add_page)(struct kvm_hyp_iommu_domain *domain,
					    struct iommu_iotlb_gather *gather,
					    unsigned long iova, size_t size);
	int (*register_hyp_event_ids)(unsigned long start, unsigned long end);
	void* (*tracing_reserve_entry)(unsigned long length);
	void (*tracing_commit_entry)(void);
	void * (*iommu_donate_pages_atomic)(u8 order);
	void (*iommu_reclaim_pages_atomic)(void *p, u8 order);
	int (*iommu_snapshot_host_stage2)(struct kvm_hyp_iommu_domain *domain);
	int (*hyp_smp_processor_id)(void);
	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
	ANDROID_KABI_RESERVE(3);
	ANDROID_KABI_RESERVE(4);
	ANDROID_KABI_RESERVE(5);
	ANDROID_KABI_RESERVE(6);
	ANDROID_KABI_RESERVE(7);
	ANDROID_KABI_RESERVE(8);
	ANDROID_KABI_RESERVE(9);
	ANDROID_KABI_RESERVE(10);
	ANDROID_KABI_RESERVE(11);
	ANDROID_KABI_RESERVE(12);
	ANDROID_KABI_RESERVE(13);
	ANDROID_KABI_RESERVE(14);
	ANDROID_KABI_RESERVE(15);
	ANDROID_KABI_RESERVE(16);
	ANDROID_KABI_RESERVE(17);
	ANDROID_KABI_RESERVE(18);
	ANDROID_KABI_RESERVE(19);
	ANDROID_KABI_RESERVE(20);
	ANDROID_KABI_RESERVE(21);
	ANDROID_KABI_RESERVE(22);
	ANDROID_KABI_RESERVE(23);
	ANDROID_KABI_RESERVE(24);
	ANDROID_KABI_RESERVE(25);
	ANDROID_KABI_RESERVE(26);
	ANDROID_KABI_RESERVE(27);
	ANDROID_KABI_RESERVE(28);
	ANDROID_KABI_RESERVE(29);
	ANDROID_KABI_RESERVE(30);
	ANDROID_KABI_RESERVE(31);
	ANDROID_KABI_RESERVE(32);
};

int __pkvm_load_el2_module(struct module *this, unsigned long *token);

int __pkvm_register_el2_call(unsigned long hfn_hyp_va);

unsigned long pkvm_el2_mod_kern_va(unsigned long addr);
#else
static inline int __pkvm_load_el2_module(struct module *this,
					 unsigned long *token)
{
	return -ENOSYS;
}

static inline int __pkvm_register_el2_call(unsigned long hfn_hyp_va)
{
	return -ENOSYS;
}

static inline unsigned long pkvm_el2_mod_kern_va(unsigned long addr)
{
	return 0;
}
#endif /* CONFIG_MODULES */

int pkvm_load_early_modules(void);

#ifdef MODULE
/*
 * Convert an EL2 module addr from the kernel VA to the hyp VA
 */
#define pkvm_el2_mod_va(kern_va, token)					\
({									\
	unsigned long hyp_text_kern_va =				\
		(unsigned long)THIS_MODULE->arch.hyp.text.start;	\
	unsigned long offset;						\
									\
	offset = (unsigned long)kern_va - hyp_text_kern_va;		\
	token + offset;							\
})

#define pkvm_load_el2_module(init_fn, token)				\
({									\
	THIS_MODULE->arch.hyp.init = init_fn;				\
	__pkvm_load_el2_module(THIS_MODULE, token);			\
})

#define pkvm_register_el2_mod_call(hfn, token)				\
({									\
	__pkvm_register_el2_call(pkvm_el2_mod_va(hfn, token));		\
})

#define pkvm_el2_mod_call(id, ...)					\
	({								\
		struct arm_smccc_res res;				\
									\
		arm_smccc_1_1_hvc(KVM_HOST_SMCCC_ID(id),		\
				  ##__VA_ARGS__, &res);			\
		WARN_ON(res.a0 != SMCCC_RET_SUCCESS);			\
									\
		res.a1;							\
	})
#endif
#endif
