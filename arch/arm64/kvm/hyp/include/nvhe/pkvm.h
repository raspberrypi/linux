/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021 Google LLC
 * Author: Fuad Tabba <tabba@google.com>
 */

#ifndef __ARM64_KVM_NVHE_PKVM_H__
#define __ARM64_KVM_NVHE_PKVM_H__

#include <asm/kvm_pkvm.h>
#include <kvm/power_domain.h>

#include <nvhe/gfp.h>
#include <nvhe/spinlock.h>

/*
 * Holds the relevant data for maintaining the vcpu state completely at hyp.
 */
struct pkvm_hyp_vcpu {
	struct kvm_vcpu vcpu;

	/* Backpointer to the host's (untrusted) vCPU instance. */
	struct kvm_vcpu *host_vcpu;

	/*
	 * If this hyp vCPU is loaded, then this is a backpointer to the
	 * per-cpu pointer tracking us. Otherwise, NULL if not loaded.
	 */
	struct pkvm_hyp_vcpu **loaded_hyp_vcpu;

	/* Tracks exit code for the protected guest. */
	u32 exit_code;

	/*
	 * Track the power state transition of a protected vcpu.
	 * Can be in one of three states:
	 * PSCI_0_2_AFFINITY_LEVEL_ON
	 * PSCI_0_2_AFFINITY_LEVEL_OFF
	 * PSCI_0_2_AFFINITY_LEVEL_PENDING
	 */
	int power_state;
};

/*
 * Holds the relevant data for running a protected vm.
 */
struct pkvm_hyp_vm {
	struct kvm kvm;

	/* Backpointer to the host's (untrusted) KVM instance. */
	struct kvm *host_kvm;

	/* The guest's stage-2 page-table managed by the hypervisor. */
	struct kvm_pgtable pgt;
	struct kvm_pgtable_mm_ops mm_ops;
	struct hyp_pool pool;
	hyp_spinlock_t pgtable_lock;

	/* Primary vCPU pending entry to the pvmfw */
	struct pkvm_hyp_vcpu *pvmfw_entry_vcpu;

	unsigned short refcount;

	/*
	 * The number of vcpus initialized and ready to run.
	 */
	unsigned int nr_vcpus;
	hyp_spinlock_t vcpus_lock;

	/*
	 * True when the guest is being torn down. When in this state, the
	 * guest's vCPUs can't be loaded anymore, but its pages can be
	 * reclaimed by the host.
	 */
	bool is_dying;

	/* Array of the hyp vCPU structures for this VM. */
	struct pkvm_hyp_vcpu *vcpus[];
};

static inline struct pkvm_hyp_vm *
pkvm_hyp_vcpu_to_hyp_vm(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return container_of(hyp_vcpu->vcpu.kvm, struct pkvm_hyp_vm, kvm);
}

static inline bool kvm_is_protected(struct kvm *kvm)
{
	if (!is_protected_kvm_enabled())
		return false;

	return kvm->arch.pkvm.enabled;
}

static inline bool vcpu_is_protected(struct kvm_vcpu *vcpu)
{
	return kvm_is_protected(vcpu->kvm);
}

static inline bool pkvm_hyp_vm_is_protected(struct pkvm_hyp_vm *hyp_vm)
{
	return kvm_is_protected(&hyp_vm->kvm);
}

static inline bool pkvm_hyp_vcpu_is_protected(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	return vcpu_is_protected(&hyp_vcpu->vcpu);
}

extern phys_addr_t pvmfw_base;
extern phys_addr_t pvmfw_size;

void pkvm_hyp_vm_table_init(void *tbl);

struct kvm_hyp_req *
pkvm_hyp_req_reserve(struct pkvm_hyp_vcpu *hyp_vcpu, u8 type);

int __pkvm_init_vm(struct kvm *host_kvm, unsigned long pgd_hva);
int __pkvm_init_vcpu(pkvm_handle_t handle, struct kvm_vcpu *host_vcpu);
int __pkvm_start_teardown_vm(pkvm_handle_t handle);
int __pkvm_finalize_teardown_vm(pkvm_handle_t handle);
int __pkvm_reclaim_dying_guest_page(pkvm_handle_t handle, u64 pfn, u64 gfn, u8 order);

struct pkvm_hyp_vcpu *pkvm_load_hyp_vcpu(pkvm_handle_t handle,
					 unsigned int vcpu_idx);
void pkvm_put_hyp_vcpu(struct pkvm_hyp_vcpu *hyp_vcpu);
struct pkvm_hyp_vcpu *pkvm_get_loaded_hyp_vcpu(void);

struct pkvm_hyp_vm *pkvm_get_hyp_vm(pkvm_handle_t handle);
void pkvm_put_hyp_vm(struct pkvm_hyp_vm *hyp_vm);

u64 pvm_read_id_reg(const struct kvm_vcpu *vcpu, u32 id);
bool kvm_handle_pvm_sysreg(struct kvm_vcpu *vcpu, u64 *exit_code);
bool kvm_handle_pvm_restricted(struct kvm_vcpu *vcpu, u64 *exit_code);
void kvm_reset_pvm_sys_regs(struct kvm_vcpu *vcpu);
int kvm_check_pvm_sysreg_table(void);

void pkvm_reset_vcpu(struct pkvm_hyp_vcpu *hyp_vcpu);

bool kvm_handle_pvm_hvc64(struct kvm_vcpu *vcpu, u64 *exit_code);
bool kvm_hyp_handle_hvc64(struct kvm_vcpu *vcpu, u64 *exit_code);

struct pkvm_hyp_vcpu *pkvm_mpidr_to_hyp_vcpu(struct pkvm_hyp_vm *vm, u64 mpidr);

static inline bool pkvm_hyp_vm_has_pvmfw(struct pkvm_hyp_vm *vm)
{
	return vm->kvm.arch.pkvm.pvmfw_load_addr != PVMFW_INVALID_LOAD_ADDR;
}

static inline bool pkvm_ipa_range_has_pvmfw(struct pkvm_hyp_vm *vm,
					    u64 ipa_start, u64 ipa_end)
{
	struct kvm_protected_vm *pkvm = &vm->kvm.arch.pkvm;
	u64 pvmfw_load_end = pkvm->pvmfw_load_addr + pvmfw_size;

	if (!pkvm_hyp_vm_has_pvmfw(vm))
		return false;

	return ipa_end > pkvm->pvmfw_load_addr && ipa_start < pvmfw_load_end;
}

int pkvm_load_pvmfw_pages(struct pkvm_hyp_vm *vm, u64 ipa, phys_addr_t phys,
			  u64 size);
void pkvm_poison_pvmfw_pages(void);

int pkvm_timer_init(void);
void pkvm_udelay(unsigned long usecs);

#define MAX_POWER_DOMAINS		32

struct kvm_power_domain_ops {
	int (*power_on)(struct kvm_power_domain *pd);
	int (*power_off)(struct kvm_power_domain *pd);
};

int pkvm_init_hvc_pd(struct kvm_power_domain *pd,
		     const struct kvm_power_domain_ops *ops);

int pkvm_host_hvc_pd(u64 device_id, u64 on);

/*
 * Register a power domain. When the hypervisor catches power requests from the
 * host for this power domain, it calls the power ops with @pd as argument.
 */
static inline int pkvm_init_power_domain(struct kvm_power_domain *pd,
					 const struct kvm_power_domain_ops *ops)
{
	switch (pd->type) {
	case KVM_POWER_DOMAIN_NONE:
		return 0;
	case KVM_POWER_DOMAIN_HOST_HVC:
		return pkvm_init_hvc_pd(pd, ops);
	default:
		return -EOPNOTSUPP;
	}
}

#ifdef CONFIG_NVHE_EL2_DEBUG
int pkvm_stage2_snapshot_by_handle(struct kvm_pgtable_snapshot *snap,
				   pkvm_handle_t handle);
#endif /* CONFIG_NVHE_EL2_DEBUG */
#endif /* __ARM64_KVM_NVHE_PKVM_H__ */
