/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Google LLC
 * Author: Quentin Perret <qperret@google.com>
 */

#ifndef __KVM_NVHE_MEM_PROTECT__
#define __KVM_NVHE_MEM_PROTECT__
#include <linux/kvm_host.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pgtable.h>
#include <asm/virt.h>
#include <nvhe/memory.h>
#include <nvhe/pkvm.h>
#include <nvhe/spinlock.h>

struct host_mmu {
	struct kvm_arch arch;
	struct kvm_pgtable pgt;
	struct kvm_pgtable_mm_ops mm_ops;
	hyp_spinlock_t lock;
};
extern struct host_mmu host_mmu;

extern unsigned long hyp_nr_cpus;

int __pkvm_prot_finalize(void);
int __pkvm_host_share_hyp(u64 pfn);
int __pkvm_host_unshare_hyp(u64 pfn);
int __pkvm_host_reclaim_page(struct pkvm_hyp_vm *vm, u64 pfn, u64 ipa, u8 order);
int __pkvm_host_donate_hyp(u64 pfn, u64 nr_pages);
int ___pkvm_host_donate_hyp(u64 pfn, u64 nr_pages, bool accept_mmio);
int ___pkvm_host_donate_hyp_prot(u64 pfn, u64 nr_pages,
				 bool accept_mmio, enum kvm_pgtable_prot prot);
int __pkvm_host_donate_hyp_locked(u64 pfn, u64 nr_pages, enum kvm_pgtable_prot prot);
int __pkvm_hyp_donate_host(u64 pfn, u64 nr_pages);
int __pkvm_host_share_ffa(u64 pfn, u64 nr_pages);
int __pkvm_host_unshare_ffa(u64 pfn, u64 nr_pages);
int __pkvm_host_unshare_guest(struct pkvm_hyp_vm *vm, u64 pfn, u64 gfn,
			      u8 order);
int __pkvm_host_share_guest(struct pkvm_hyp_vcpu *vcpu, u64 pfn, u64 gfn,
			    u64 nr_pages, enum kvm_pgtable_prot prot);
int __pkvm_host_donate_guest(struct pkvm_hyp_vcpu *vcpu, u64 pfn, u64 gfn,
			     u64 nr_pages);
int __pkvm_guest_share_host(struct pkvm_hyp_vcpu *hyp_vcpu, u64 ipa,
			    u64 nr_pages, u64 *nr_shared);
int __pkvm_guest_unshare_host(struct pkvm_hyp_vcpu *hyp_vcpu, u64 ipa,
			      u64 nr_pages, u64 *nr_unshared);
int __pkvm_install_ioguard_page(struct pkvm_hyp_vcpu *hyp_vcpu, u64 ipa,
				u64 nr_pages, u64 *nr_guarded);
int __pkvm_remove_ioguard_page(struct pkvm_hyp_vcpu *hyp_vcpu, u64 ipa,
			       u64 nr_pages, u64 *nr_unguarded);
bool __pkvm_check_ioguard_page(struct pkvm_hyp_vcpu *hyp_vcpu);
int __pkvm_guest_relinquish_to_host(struct pkvm_hyp_vcpu *vcpu,
				    u64 ipa, u64 *ppa);
int __pkvm_relax_perms(struct pkvm_hyp_vcpu *vcpu,
		       u64 pfn, u64 gfn, u8 order, enum kvm_pgtable_prot prot);
int __pkvm_wrprotect(struct pkvm_hyp_vm *vm, u64 pfn, u64 gfn, u8 order);
int __pkvm_dirty_log(struct pkvm_hyp_vcpu *vcpu, u64 pfn, u64 gfn);
int __pkvm_host_use_dma(u64 phys_addr, size_t size);
int __pkvm_host_unuse_dma(u64 phys_addr, size_t size);
int __pkvm_guest_stage2_snapshot(struct kvm_pgtable_snapshot *snap, struct pkvm_hyp_vm *vm);
int __pkvm_host_stage2_snapshot(struct kvm_pgtable_snapshot *snap);

bool addr_is_memory(phys_addr_t phys);
int host_stage2_idmap_locked(phys_addr_t addr, u64 size,
			     enum kvm_pgtable_prot prot,
			     bool update_iommu);
int host_stage2_set_owner_locked(phys_addr_t addr, u64 size, u8 owner_id);
int host_stage2_unmap_reg_locked(phys_addr_t start, u64 size);
int kvm_host_prepare_stage2(void *pgt_pool_base);
int kvm_guest_prepare_stage2(struct pkvm_hyp_vm *vm, void *pgd);
void handle_host_mem_abort(struct kvm_cpu_context *host_ctxt);

int hyp_register_host_perm_fault_handler(int (*cb)(struct user_pt_regs *regs, u64 esr, u64 addr));
int hyp_pin_shared_mem(void *from, void *to);
void hyp_unpin_shared_mem(void *from, void *to);
int host_stage2_get_leaf(phys_addr_t phys, kvm_pte_t *ptep, u32 *level);
int refill_memcache(struct kvm_hyp_memcache *mc, unsigned long min_pages,
		    struct kvm_hyp_memcache *host_mc);

int refill_hyp_pool(struct hyp_pool *pool, struct kvm_hyp_memcache *host_mc);
int reclaim_hyp_pool(struct hyp_pool *pool, struct kvm_hyp_memcache *host_mc,
		     int nr_pages);

void destroy_hyp_vm_pgt(struct pkvm_hyp_vm *vm);
void drain_hyp_pool(struct pkvm_hyp_vm *vm, struct kvm_hyp_memcache *mc);

int module_change_host_page_prot(u64 pfn, enum kvm_pgtable_prot prot, u64 nr_pages, bool update_iommu);

void psci_mem_protect_inc(u64 n);
void psci_mem_protect_dec(u64 n);

static __always_inline void __load_host_stage2(void)
{
	if (static_branch_likely(&kvm_protected_mode_initialized))
		__load_stage2(&host_mmu.arch.mmu, &host_mmu.arch);
	else
		write_sysreg(0, vttbr_el2);
}
#endif /* __KVM_NVHE_MEM_PROTECT__ */
