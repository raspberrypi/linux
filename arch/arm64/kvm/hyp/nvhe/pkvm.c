// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 Google LLC
 * Author: Fuad Tabba <tabba@google.com>
 */

#include <linux/kvm_host.h>
#include <linux/mm.h>

#include <kvm/arm_hypercalls.h>
#include <kvm/arm_psci.h>

#include <asm/kvm_emulate.h>

#include <nvhe/alloc.h>
#include <nvhe/arm-smccc.h>
#include <nvhe/mem_protect.h>
#include <nvhe/memory.h>
#include <nvhe/mm.h>
#include <nvhe/pkvm.h>
#include <nvhe/rwlock.h>
#include <nvhe/trap_handler.h>

/* Used by icache_is_vpipt(). */
unsigned long __icache_flags;

/* Used by kvm_get_vttbr(). */
unsigned int kvm_arm_vmid_bits;

unsigned int kvm_sve_max_vl;

unsigned int kvm_host_sve_max_vl;

/*
 * The currently loaded hyp vCPU for each physical CPU. Used only when
 * protected KVM is enabled, but for both protected and non-protected VMs.
 */
static DEFINE_PER_CPU(struct pkvm_hyp_vcpu *, loaded_hyp_vcpu);

/*
 * Host fp state for all cpus. This could include the host simd state, as well
 * as the sve and sme states if supported. Written to when the guest accesses
 * its own FPSIMD state, and read when the guest state is live and we need to
 * switch back to the host.
 *
 * Only valid when (fp_state == FP_STATE_GUEST_OWNED) in the hyp vCPU structure.
 */
unsigned long __ro_after_init kvm_arm_hyp_host_fp_state[NR_CPUS];

static void *__get_host_fpsimd_bytes(void)
{
	/*
	 * The addresses in this array have been converted to hyp addresses
	 * in finalize_init_hyp_mode().
	 */
	return (void *)kvm_arm_hyp_host_fp_state[hyp_smp_processor_id()];
}

struct user_fpsimd_state *get_host_fpsimd_state(struct kvm_vcpu *vcpu)
{
	if (likely(!is_protected_kvm_enabled()))
		return vcpu->arch.host_fpsimd_state;

	WARN_ON(system_supports_sve());
	return __get_host_fpsimd_bytes();
}

struct kvm_host_sve_state *get_host_sve_state(struct kvm_vcpu *vcpu)
{
	WARN_ON(!system_supports_sve());
	WARN_ON(!is_protected_kvm_enabled());
	return __get_host_fpsimd_bytes();
}

/*
 * Set trap register values based on features in ID_AA64PFR0.
 */
static void pvm_init_traps_aa64pfr0(struct kvm_vcpu *vcpu)
{
	const u64 feature_ids = pvm_read_id_reg(vcpu, SYS_ID_AA64PFR0_EL1);
	u64 hcr_set = HCR_RW;
	u64 hcr_clear = 0;
	u64 cptr_set = 0;
	u64 cptr_clear = 0;

	/* Protected KVM does not support AArch32 guests. */
	BUILD_BUG_ON(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_EL0),
		PVM_ID_AA64PFR0_RESTRICT_UNSIGNED) != ID_AA64PFR0_EL1_ELx_64BIT_ONLY);
	BUILD_BUG_ON(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_EL1),
		PVM_ID_AA64PFR0_RESTRICT_UNSIGNED) != ID_AA64PFR0_EL1_ELx_64BIT_ONLY);

	/*
	 * Linux guests assume support for floating-point and Advanced SIMD. Do
	 * not change the trapping behavior for these from the KVM default.
	 */
	BUILD_BUG_ON(!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_FP),
				PVM_ID_AA64PFR0_ALLOW));
	BUILD_BUG_ON(!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_AdvSIMD),
				PVM_ID_AA64PFR0_ALLOW));

	if (has_hvhe())
		hcr_set |= HCR_E2H;

	/* Trap RAS unless all current versions are supported */
	if (FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_RAS), feature_ids) <
	    ID_AA64PFR0_EL1_RAS_V1P1) {
		hcr_set |= HCR_TERR | HCR_TEA;
		hcr_clear |= HCR_FIEN;
	}

	/* Trap AMU */
	if (!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_AMU), feature_ids)) {
		hcr_clear |= HCR_AMVOFFEN;
		cptr_set |= CPTR_EL2_TAM;
	}

	/* Trap SVE */
	if (!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_SVE), feature_ids)) {
		if (has_hvhe())
			cptr_clear |= CPACR_EL1_ZEN_EL0EN | CPACR_EL1_ZEN_EL1EN;
		else
			cptr_set |= CPTR_EL2_TZ;
	}

	vcpu->arch.hcr_el2 |= hcr_set;
	vcpu->arch.hcr_el2 &= ~hcr_clear;
	vcpu->arch.cptr_el2 |= cptr_set;
	vcpu->arch.cptr_el2 &= ~cptr_clear;
}

/*
 * Set trap register values based on features in ID_AA64PFR1.
 */
static void pvm_init_traps_aa64pfr1(struct kvm_vcpu *vcpu)
{
	const u64 feature_ids = pvm_read_id_reg(vcpu, SYS_ID_AA64PFR1_EL1);
	u64 hcr_set = 0;
	u64 hcr_clear = 0;
	u64 cptr_set = 0;
	u64 cptr_clear = 0;

	/* Memory Tagging: Trap and Treat as Untagged if not supported. */
	if (!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_MTE), feature_ids)) {
		hcr_set |= HCR_TID5;
		hcr_clear |= HCR_DCT | HCR_ATA;
	}

	/* No SME supprot in KVM. */
	BUG_ON(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_SME), feature_ids));
	if (has_hvhe())
		cptr_clear |= CPACR_EL1_SMEN_EL1EN | CPACR_EL1_SMEN_EL0EN;
	else
		cptr_set |= CPTR_EL2_TSM;

	vcpu->arch.hcr_el2 |= hcr_set;
	vcpu->arch.hcr_el2 &= ~hcr_clear;
	vcpu->arch.cptr_el2 |= cptr_set;
	vcpu->arch.cptr_el2 &= ~cptr_clear;
}

/*
 * Set trap register values based on features in ID_AA64DFR0.
 */
static void pvm_init_traps_aa64dfr0(struct kvm_vcpu *vcpu)
{
	const u64 feature_ids = pvm_read_id_reg(vcpu, SYS_ID_AA64DFR0_EL1);
	u64 mdcr_set = 0;
	u64 mdcr_clear = 0;
	u64 cptr_set = 0;

	/* Trap/constrain PMU */
	if (!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64DFR0_EL1_PMUVer), feature_ids)) {
		mdcr_set |= MDCR_EL2_TPM | MDCR_EL2_TPMCR;
		mdcr_clear |= MDCR_EL2_HPME | MDCR_EL2_MTPME |
			      MDCR_EL2_HPMN_MASK;
	}

	/* Trap Debug */
	if (!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64DFR0_EL1_DebugVer), feature_ids))
		mdcr_set |= MDCR_EL2_TDRA | MDCR_EL2_TDA;

	/* Trap OS Double Lock */
	if (!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64DFR0_EL1_DoubleLock), feature_ids))
		mdcr_set |= MDCR_EL2_TDOSA;

	/* Trap SPE */
	if (!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64DFR0_EL1_PMSVer), feature_ids)) {
		mdcr_set |= MDCR_EL2_TPMS;
		mdcr_clear |= MDCR_EL2_E2PB_MASK << MDCR_EL2_E2PB_SHIFT;
	}

	/* Trap Trace Filter */
	if (!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64DFR0_EL1_TraceFilt), feature_ids))
		mdcr_set |= MDCR_EL2_TTRF;

	/* Trap Trace */
	if (!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64DFR0_EL1_TraceVer), feature_ids)) {
		if (has_hvhe())
			cptr_set |= CPACR_EL1_TTA;
		else
			cptr_set |= CPTR_EL2_TTA;
	}

	vcpu->arch.mdcr_el2 |= mdcr_set;
	vcpu->arch.mdcr_el2 &= ~mdcr_clear;
	vcpu->arch.cptr_el2 |= cptr_set;
}

/*
 * Set trap register values based on features in ID_AA64MMFR0.
 */
static void pvm_init_traps_aa64mmfr0(struct kvm_vcpu *vcpu)
{
	const u64 feature_ids = pvm_read_id_reg(vcpu, SYS_ID_AA64MMFR0_EL1);
	u64 mdcr_set = 0;

	/* Trap Debug Communications Channel registers */
	if (!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR0_EL1_FGT), feature_ids))
		mdcr_set |= MDCR_EL2_TDCC;

	vcpu->arch.mdcr_el2 |= mdcr_set;
}

/*
 * Set trap register values based on features in ID_AA64MMFR1.
 */
static void pvm_init_traps_aa64mmfr1(struct kvm_vcpu *vcpu)
{
	const u64 feature_ids = pvm_read_id_reg(vcpu, SYS_ID_AA64MMFR1_EL1);
	u64 hcr_set = 0;

	/* Trap LOR */
	if (!FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR1_EL1_LO), feature_ids))
		hcr_set |= HCR_TLOR;

	vcpu->arch.hcr_el2 |= hcr_set;
}

/*
 * Set baseline trap register values.
 */
static void pvm_init_trap_regs(struct kvm_vcpu *vcpu)
{
	/*
	 * Always trap:
	 * - Feature id registers: to control features exposed to guests
	 * - Implementation-defined features
	 */
	vcpu->arch.hcr_el2 = HCR_GUEST_FLAGS |
			     HCR_TID3 | HCR_TACR | HCR_TIDCP | HCR_TID1;

	if (cpus_have_const_cap(ARM64_HAS_RAS_EXTN)) {
		/* route synchronous external abort exceptions to EL2 */
		vcpu->arch.hcr_el2 |= HCR_TEA;
		/* trap error record accesses */
		vcpu->arch.hcr_el2 |= HCR_TERR;
	}

	if (cpus_have_const_cap(ARM64_HAS_STAGE2_FWB))
		vcpu->arch.hcr_el2 |= HCR_FWB;

	if (cpus_have_const_cap(ARM64_MISMATCHED_CACHE_TYPE))
		vcpu->arch.hcr_el2 |= HCR_TID2;

	if (!has_hvhe()) {
		vcpu->arch.cptr_el2 |= CPTR_NVHE_EL2_RES1;
		vcpu->arch.cptr_el2 &= ~(CPTR_NVHE_EL2_RES0);
	}
}

/*
 * Initialize trap register values for protected VMs.
 */
static void pkvm_vcpu_init_traps(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	hyp_vcpu->vcpu.arch.cptr_el2 = kvm_get_reset_cptr_el2(&hyp_vcpu->vcpu);
	hyp_vcpu->vcpu.arch.mdcr_el2 = 0;

	if (!pkvm_hyp_vcpu_is_protected(hyp_vcpu)) {
		u64 hcr = READ_ONCE(hyp_vcpu->host_vcpu->arch.hcr_el2);

		hyp_vcpu->vcpu.arch.hcr_el2 = HCR_GUEST_FLAGS | hcr;
		return;
	}

	pvm_init_trap_regs(&hyp_vcpu->vcpu);
	pvm_init_traps_aa64pfr0(&hyp_vcpu->vcpu);
	pvm_init_traps_aa64pfr1(&hyp_vcpu->vcpu);
	pvm_init_traps_aa64dfr0(&hyp_vcpu->vcpu);
	pvm_init_traps_aa64mmfr0(&hyp_vcpu->vcpu);
	pvm_init_traps_aa64mmfr1(&hyp_vcpu->vcpu);
}

/*
 * Start the VM table handle at the offset defined instead of at 0.
 * Mainly for sanity checking and debugging.
 */
#define HANDLE_OFFSET 0x1000

static unsigned int vm_handle_to_idx(pkvm_handle_t handle)
{
	return handle - HANDLE_OFFSET;
}

static pkvm_handle_t idx_to_vm_handle(unsigned int idx)
{
	return idx + HANDLE_OFFSET;
}

/* Rwlock for protecting state related to the VM table. */
static DEFINE_HYP_RWLOCK(vm_table_lock);

/*
 * The table of VM entries for protected VMs in hyp.
 * Allocated at hyp initialization and setup.
 */
static struct pkvm_hyp_vm **vm_table;

void pkvm_hyp_vm_table_init(void *tbl)
{
	WARN_ON(vm_table);
	vm_table = tbl;
}

static void *map_donated_memory_noclear(unsigned long host_va, size_t size)
{
	void *va = (void *)kern_hyp_va(host_va);

	if (!PAGE_ALIGNED(va))
		return NULL;

	if (__pkvm_host_donate_hyp(hyp_virt_to_pfn(va),
				   PAGE_ALIGN(size) >> PAGE_SHIFT))
		return NULL;

	return va;
}

static void __unmap_donated_memory(void *va, size_t size)
{
	kvm_flush_dcache_to_poc(va, size);
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(va),
				       PAGE_ALIGN(size) >> PAGE_SHIFT));
}

static void unmap_donated_memory(void *va, size_t size)
{
	if (!va)
		return;

	memset(va, 0, size);
	__unmap_donated_memory(va, size);
}

static void unmap_donated_memory_noclear(void *va, size_t size)
{
	if (!va)
		return;

	__unmap_donated_memory(va, size);
}

/*
 * Return the hyp vm structure corresponding to the handle.
 */
static struct pkvm_hyp_vm *get_vm_by_handle(pkvm_handle_t handle)
{
	unsigned int idx = vm_handle_to_idx(handle);

	if (unlikely(idx >= KVM_MAX_PVMS))
		return NULL;

	return vm_table[idx];
}

int __pkvm_reclaim_dying_guest_page(pkvm_handle_t handle, u64 pfn, u64 gfn, u8 order)
{
	struct pkvm_hyp_vm *hyp_vm;
	int ret = -EINVAL;

	hyp_read_lock(&vm_table_lock);
	hyp_vm = get_vm_by_handle(handle);
	if (!hyp_vm || !hyp_vm->is_dying)
		goto unlock;

	ret = __pkvm_host_reclaim_page(hyp_vm, pfn, gfn << PAGE_SHIFT, order);
	if (ret)
		goto unlock;

	drain_hyp_pool(hyp_vm, &hyp_vm->host_kvm->arch.pkvm.stage2_teardown_mc);
unlock:
	hyp_read_unlock(&vm_table_lock);

	return ret;
}

struct pkvm_hyp_vm *pkvm_get_hyp_vm(pkvm_handle_t handle)
{
	struct pkvm_hyp_vm *hyp_vm;

	hyp_read_lock(&vm_table_lock);
	hyp_vm = get_vm_by_handle(handle);
	if (hyp_vm) {
		if (WARN_ON(hyp_vm->is_dying))
			hyp_vm = NULL;
		else
			hyp_refcount_inc(hyp_vm->refcount);
	}
	hyp_read_unlock(&vm_table_lock);

	return hyp_vm;
}

void pkvm_put_hyp_vm(struct pkvm_hyp_vm *hyp_vm)
{
	hyp_refcount_dec(hyp_vm->refcount);
}

struct pkvm_hyp_vcpu *pkvm_load_hyp_vcpu(pkvm_handle_t handle,
					 unsigned int vcpu_idx)
{
	struct pkvm_hyp_vcpu *hyp_vcpu = NULL;
	struct pkvm_hyp_vm *hyp_vm;

	/* Cannot load a new vcpu without putting the old one first. */
	if (__this_cpu_read(loaded_hyp_vcpu))
		return NULL;

	hyp_read_lock(&vm_table_lock);
	hyp_vm = get_vm_by_handle(handle);
	if (!hyp_vm || hyp_vm->is_dying || READ_ONCE(hyp_vm->nr_vcpus) <= vcpu_idx)
		goto unlock;

	hyp_vcpu = hyp_vm->vcpus[vcpu_idx];

	/* Ensure vcpu isn't loaded on more than one cpu simultaneously. */
	if (unlikely(cmpxchg_relaxed(&hyp_vcpu->loaded_hyp_vcpu, NULL,
				     this_cpu_ptr(&loaded_hyp_vcpu)))) {
		hyp_vcpu = NULL;
		goto unlock;
	}

	hyp_refcount_inc(hyp_vm->refcount);
unlock:
	hyp_read_unlock(&vm_table_lock);

	if (hyp_vcpu)
		__this_cpu_write(loaded_hyp_vcpu, hyp_vcpu);
	return hyp_vcpu;
}

void pkvm_put_hyp_vcpu(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct pkvm_hyp_vm *hyp_vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	__this_cpu_write(loaded_hyp_vcpu, NULL);

	/*
	 * Clearing the 'loaded_hyp_vcpu' field allows the 'hyp_vcpu' to
	 * be loaded by another physical CPU, so make sure we're done
	 * with the vCPU before letting somebody else play with it.
	 */
	smp_store_release(&hyp_vcpu->loaded_hyp_vcpu, NULL);

	/*
	 * We don't hold the 'vm_table_lock'. Once the refcount hits
	 * zero, VM teardown can destroy the VM's data structures and
	 * so this must come last.
	 */
	smp_wmb();
	hyp_refcount_dec(hyp_vm->refcount);
}

struct pkvm_hyp_vcpu *pkvm_get_loaded_hyp_vcpu(void)
{
	return __this_cpu_read(loaded_hyp_vcpu);
}

static void pkvm_vcpu_init_features_from_host(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct kvm_vcpu *host_vcpu = hyp_vcpu->host_vcpu;
	DECLARE_BITMAP(allowed_features, KVM_VCPU_MAX_FEATURES);

	/* No restrictions for non-protected VMs. */
	if (!pkvm_hyp_vcpu_is_protected(hyp_vcpu)) {
		bitmap_copy(hyp_vcpu->vcpu.arch.features,
			    host_vcpu->arch.features,
			    KVM_VCPU_MAX_FEATURES);
		return;
	}

	bitmap_zero(allowed_features, KVM_VCPU_MAX_FEATURES);

	/*
	 * For protected vms, always allow:
	 * - PSCI v0.2
	 */
	set_bit(KVM_ARM_VCPU_PSCI_0_2, allowed_features);

	/*
	 * Check if remaining features are allowed:
	 * - Performance Monitoring
	 * - Scalable Vectors
	 * - Pointer Authentication
	 */
	if (FIELD_GET(ARM64_FEATURE_MASK(ID_AA64DFR0_EL1_PMUVer), PVM_ID_AA64DFR0_ALLOW))
		set_bit(KVM_ARM_VCPU_PMU_V3, allowed_features);

	if (FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_SVE), PVM_ID_AA64PFR0_RESTRICT_UNSIGNED))
		set_bit(KVM_ARM_VCPU_SVE, allowed_features);

	if (FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_API), PVM_ID_AA64ISAR1_ALLOW) &&
	    FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_APA), PVM_ID_AA64ISAR1_ALLOW))
		set_bit(KVM_ARM_VCPU_PTRAUTH_ADDRESS, allowed_features);

	if (FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_GPI), PVM_ID_AA64ISAR1_ALLOW) &&
	    FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_GPA), PVM_ID_AA64ISAR1_ALLOW))
		set_bit(KVM_ARM_VCPU_PTRAUTH_GENERIC, allowed_features);

	bitmap_and(hyp_vcpu->vcpu.arch.features, host_vcpu->arch.features,
		   allowed_features, KVM_VCPU_MAX_FEATURES);

	/*
	 * Now sanitise the configuration flags that we have inherited
	 * from the host, as they may refer to features that protected
	 * mode doesn't support.
	 */
	if (!vcpu_has_feature(&hyp_vcpu->vcpu,(KVM_ARM_VCPU_SVE))) {
		vcpu_clear_flag(&hyp_vcpu->vcpu, GUEST_HAS_SVE);
		vcpu_clear_flag(&hyp_vcpu->vcpu, VCPU_SVE_FINALIZED);
	}

	if (!vcpu_has_feature(&hyp_vcpu->vcpu, KVM_ARM_VCPU_PTRAUTH_ADDRESS) ||
	    !vcpu_has_feature(&hyp_vcpu->vcpu, KVM_ARM_VCPU_PTRAUTH_GENERIC))
		vcpu_clear_flag(&hyp_vcpu->vcpu, GUEST_HAS_PTRAUTH);
}

static int pkvm_vcpu_init_ptrauth(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	int ret = 0;

	if (test_bit(KVM_ARM_VCPU_PTRAUTH_ADDRESS, vcpu->arch.features) ||
	    test_bit(KVM_ARM_VCPU_PTRAUTH_GENERIC, vcpu->arch.features))
		ret = kvm_vcpu_enable_ptrauth(vcpu);

	return ret;
}

static int pkvm_vcpu_init_psci(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct vcpu_reset_state *reset_state = &hyp_vcpu->vcpu.arch.reset_state;
	struct pkvm_hyp_vm *hyp_vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	if (hyp_vcpu->vcpu.arch.mp_state.mp_state == KVM_MP_STATE_STOPPED) {
		reset_state->reset = false;
		hyp_vcpu->power_state = PSCI_0_2_AFFINITY_LEVEL_OFF;
	} else if (pkvm_hyp_vm_has_pvmfw(hyp_vm)) {
		if (hyp_vm->pvmfw_entry_vcpu)
			return -EINVAL;

		hyp_vm->pvmfw_entry_vcpu = hyp_vcpu;
		reset_state->reset = true;
		hyp_vcpu->power_state = PSCI_0_2_AFFINITY_LEVEL_ON_PENDING;
	} else {
		struct kvm_vcpu *host_vcpu = hyp_vcpu->host_vcpu;

		reset_state->pc = READ_ONCE(host_vcpu->arch.ctxt.regs.pc);
		reset_state->r0 = READ_ONCE(host_vcpu->arch.ctxt.regs.regs[0]);
		reset_state->reset = true;
		hyp_vcpu->power_state = PSCI_0_2_AFFINITY_LEVEL_ON_PENDING;
	}

	return 0;
}

static void unpin_host_vcpu(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct kvm_vcpu *host_vcpu = hyp_vcpu->host_vcpu;
	void *hyp_reqs = hyp_vcpu->vcpu.arch.hyp_reqs;

	if (host_vcpu)
		hyp_unpin_shared_mem(host_vcpu, host_vcpu + 1);
	if (hyp_reqs)
		hyp_unpin_shared_mem(hyp_reqs, hyp_reqs + 1);
}

static void unpin_host_sve_state(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	void *sve_state;

	if (!test_bit(KVM_ARM_VCPU_SVE, hyp_vcpu->vcpu.arch.features))
		return;

	sve_state = kern_hyp_va(hyp_vcpu->vcpu.arch.sve_state);
	hyp_unpin_shared_mem(sve_state,
			     sve_state + vcpu_sve_state_size(&hyp_vcpu->vcpu));
}

static void teardown_sve_state(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct pkvm_hyp_vm *hyp_vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);
	void *sve_state = hyp_vcpu->vcpu.arch.sve_state;

	if (sve_state)
		hyp_free_account(sve_state, hyp_vm->host_kvm);
}

static void unpin_host_vcpus(struct pkvm_hyp_vcpu *hyp_vcpus[],
			     unsigned int nr_vcpus)
{
	int i;

	for (i = 0; i < nr_vcpus; i++) {
		struct pkvm_hyp_vcpu *hyp_vcpu = hyp_vcpus[i];

		unpin_host_vcpu(hyp_vcpu);

		if (!pkvm_hyp_vcpu_is_protected(hyp_vcpu))
			unpin_host_sve_state(hyp_vcpu);
	}
}

static size_t pkvm_get_last_ran_size(void)
{
	return array_size(hyp_nr_cpus, sizeof(int));
}

static void init_pkvm_hyp_vm(struct kvm *host_kvm, struct pkvm_hyp_vm *hyp_vm,
			     int *last_ran, unsigned int nr_vcpus)
{
	u64 pvmfw_load_addr = PVMFW_INVALID_LOAD_ADDR;

	hyp_vm->host_kvm = host_kvm;
	hyp_vm->kvm.created_vcpus = nr_vcpus;
	hyp_vm->kvm.arch.vtcr = host_mmu.arch.vtcr;
	hyp_vm->kvm.arch.pkvm.enabled = READ_ONCE(host_kvm->arch.pkvm.enabled);

	if (hyp_vm->kvm.arch.pkvm.enabled)
		pvmfw_load_addr = READ_ONCE(host_kvm->arch.pkvm.pvmfw_load_addr);
	hyp_vm->kvm.arch.pkvm.pvmfw_load_addr = pvmfw_load_addr;

	hyp_vm->kvm.arch.mmu.last_vcpu_ran = (int __percpu *)last_ran;
	memset(last_ran, -1, pkvm_get_last_ran_size());
	hyp_spin_lock_init(&hyp_vm->vcpus_lock);
}

static int init_pkvm_hyp_vcpu_sve(struct pkvm_hyp_vcpu *hyp_vcpu, struct kvm_vcpu *host_vcpu)
{
	void *sve_state = kern_hyp_va(READ_ONCE(host_vcpu->arch.sve_state));
	unsigned int sve_max_vl = READ_ONCE(host_vcpu->arch.sve_max_vl);
	size_t sve_state_size = _vcpu_sve_state_size(sve_max_vl);
	int ret = 0;

	if (!sve_state && !pkvm_hyp_vcpu_is_protected(hyp_vcpu)) {
		ret = -EINVAL;
		goto err;
	}

	if (!sve_state_size || (sve_max_vl > kvm_sve_max_vl)) {
		ret = -EINVAL;
		goto err;
	}

	if (pkvm_hyp_vcpu_is_protected(hyp_vcpu)) {
		struct pkvm_hyp_vm *hyp_vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

		sve_state = hyp_alloc_account(sve_state_size,
					      hyp_vm->host_kvm);
		if (!sve_state) {
			ret = hyp_alloc_errno();
			goto err;
		}
	} else {
		ret = hyp_pin_shared_mem(sve_state, sve_state + sve_state_size);
		if (ret)
			goto err;
	}

	hyp_vcpu->vcpu.arch.sve_state = sve_state;
	hyp_vcpu->vcpu.arch.sve_max_vl = sve_max_vl;

	return 0;
err:
	clear_bit(KVM_ARM_VCPU_SVE, hyp_vcpu->vcpu.arch.features);
	return ret;
}

static int init_pkvm_hyp_vcpu(struct pkvm_hyp_vcpu *hyp_vcpu,
			      struct pkvm_hyp_vm *hyp_vm,
			      struct kvm_vcpu *host_vcpu,
			      unsigned int vcpu_idx)
{
	int ret = 0;
	u32 mp_state;

	if (hyp_pin_shared_mem(host_vcpu, host_vcpu + 1))
		return -EBUSY;

	hyp_vcpu->vcpu.arch.hyp_reqs = kern_hyp_va(host_vcpu->arch.hyp_reqs);
	if (hyp_pin_shared_mem(hyp_vcpu->vcpu.arch.hyp_reqs,
			       hyp_vcpu->vcpu.arch.hyp_reqs + 1)) {
		hyp_unpin_shared_mem(host_vcpu, host_vcpu + 1);
		return -EBUSY;
	}

	if (host_vcpu->vcpu_idx != vcpu_idx) {
		ret = -EINVAL;
		goto done;
	}

	mp_state = READ_ONCE(host_vcpu->arch.mp_state.mp_state);
	if (mp_state != KVM_MP_STATE_RUNNABLE && mp_state != KVM_MP_STATE_STOPPED) {
		ret = -EINVAL;
		goto done;
	}

	hyp_vcpu->host_vcpu = host_vcpu;

	hyp_vcpu->vcpu.kvm = &hyp_vm->kvm;
	hyp_vcpu->vcpu.vcpu_id = READ_ONCE(host_vcpu->vcpu_id);
	hyp_vcpu->vcpu.vcpu_idx = vcpu_idx;

	hyp_vcpu->vcpu.arch.hw_mmu = &hyp_vm->kvm.arch.mmu;
	hyp_vcpu->vcpu.arch.cflags = READ_ONCE(host_vcpu->arch.cflags);
	hyp_vcpu->vcpu.arch.mp_state.mp_state = mp_state;
	hyp_vcpu->vcpu.arch.debug_ptr = &host_vcpu->arch.vcpu_debug_state;
	hyp_vcpu->vcpu.arch.hyp_reqs->type = KVM_HYP_LAST_REQ;

	pkvm_vcpu_init_features_from_host(hyp_vcpu);

	ret = pkvm_vcpu_init_ptrauth(hyp_vcpu);
	if (ret)
		goto done;

	ret = pkvm_vcpu_init_psci(hyp_vcpu);
	if (ret)
		goto done;

	if (test_bit(KVM_ARM_VCPU_SVE, hyp_vcpu->vcpu.arch.features)) {
		ret = init_pkvm_hyp_vcpu_sve(hyp_vcpu, host_vcpu);
		if (ret)
			goto done;
	}

	pkvm_vcpu_init_traps(hyp_vcpu);
	kvm_reset_pvm_sys_regs(&hyp_vcpu->vcpu);
done:
	if (ret)
		unpin_host_vcpu(hyp_vcpu);
	return ret;
}

static int find_free_vm_table_entry(struct kvm *host_kvm)
{
	int i;

	for (i = 0; i < KVM_MAX_PVMS; ++i) {
		if (!vm_table[i])
			return i;
	}

	return -ENOMEM;
}

/*
 * Allocate a VM table entry and insert a pointer to the new vm.
 *
 * Return a unique handle to the protected VM on success,
 * negative error code on failure.
 */
static pkvm_handle_t insert_vm_table_entry(struct kvm *host_kvm,
					   struct pkvm_hyp_vm *hyp_vm)
{
	struct kvm_s2_mmu *mmu = &hyp_vm->kvm.arch.mmu;
	int idx;

	hyp_assert_write_lock_held(&vm_table_lock);

	/*
	 * Initializing protected state might have failed, yet a malicious
	 * host could trigger this function. Thus, ensure that 'vm_table'
	 * exists.
	 */
	if (unlikely(!vm_table))
		return -EINVAL;

	idx = find_free_vm_table_entry(host_kvm);
	if (idx < 0)
		return idx;

	hyp_vm->kvm.arch.pkvm.handle = idx_to_vm_handle(idx);

	/* VMID 0 is reserved for the host */
	atomic64_set(&mmu->vmid.id, idx + 1);

	mmu->arch = &hyp_vm->kvm.arch;
	mmu->pgt = &hyp_vm->pgt;

	vm_table[idx] = hyp_vm;
	return hyp_vm->kvm.arch.pkvm.handle;
}

/*
 * Deallocate and remove the VM table entry corresponding to the handle.
 */
static void remove_vm_table_entry(pkvm_handle_t handle)
{
	hyp_assert_write_lock_held(&vm_table_lock);
	vm_table[vm_handle_to_idx(handle)] = NULL;
}

static size_t pkvm_get_hyp_vm_size(unsigned int nr_vcpus)
{
	return size_add(sizeof(struct pkvm_hyp_vm),
		size_mul(sizeof(struct pkvm_hyp_vcpu *), nr_vcpus));
}

/*
 * Initialize the hypervisor copy of the protected VM state using the
 * memory donated by the host.
 *
 * Unmaps the donated memory from the host at stage 2.
 *
 * host_kvm: A pointer to the host's struct kvm.
 * pgd_hva: The host va of the area being donated for the stage-2 PGD for
 *	    the VM. Must be page aligned. Its size is implied by the VM's
 *	    VTCR.
 * Return a unique handle to the protected VM on success,
 * negative error code on failure.
 */
int __pkvm_init_vm(struct kvm *host_kvm, unsigned long pgd_hva)
{
	struct pkvm_hyp_vm *hyp_vm = NULL;
	int *last_ran = NULL;
	unsigned int nr_vcpus;
	void *pgd = NULL;
	size_t pgd_size;
	int ret;

	ret = hyp_pin_shared_mem(host_kvm, host_kvm + 1);
	if (ret)
		return ret;

	nr_vcpus = READ_ONCE(host_kvm->created_vcpus);
	if (nr_vcpus < 1) {
		ret = -EINVAL;
		goto err_unpin_kvm;
	}

	hyp_vm = hyp_alloc_account(pkvm_get_hyp_vm_size(nr_vcpus),
				   host_kvm);
	if (!hyp_vm) {
		ret = hyp_alloc_errno();
		goto err_unpin_kvm;
	}

	last_ran = hyp_alloc_account(pkvm_get_last_ran_size(), host_kvm);
	if (!last_ran) {
		ret = hyp_alloc_errno();
		goto err_free_vm;
	}

	ret = -EINVAL;
	pgd_size = kvm_pgtable_stage2_pgd_size(host_mmu.arch.vtcr);
	pgd = map_donated_memory_noclear(pgd_hva, pgd_size);
	if (!pgd)
		goto err_free_last_ran;

	init_pkvm_hyp_vm(host_kvm, hyp_vm, last_ran, nr_vcpus);

	hyp_write_lock(&vm_table_lock);
	ret = insert_vm_table_entry(host_kvm, hyp_vm);
	if (ret < 0)
		goto err_unlock;

	ret = kvm_guest_prepare_stage2(hyp_vm, pgd);
	if (ret)
		goto err_remove_vm_table_entry;
	hyp_write_unlock(&vm_table_lock);

	return hyp_vm->kvm.arch.pkvm.handle;

err_remove_vm_table_entry:
	remove_vm_table_entry(hyp_vm->kvm.arch.pkvm.handle);
err_unlock:
	hyp_write_unlock(&vm_table_lock);
	unmap_donated_memory(pgd, pgd_size);
err_free_last_ran:
	hyp_free_account(last_ran, host_kvm);
err_free_vm:
	hyp_free_account(hyp_vm, host_kvm);
err_unpin_kvm:
	hyp_unpin_shared_mem(host_kvm, host_kvm + 1);
	return ret;
}

/*
 * Initialize the hypervisor copy of the protected vCPU state using the
 * memory donated by the host.
 *
 * handle: The handle for the protected vm.
 * host_vcpu: A pointer to the corresponding host vcpu.
 *
 * Return 0 on success, negative error code on failure.
 */
int __pkvm_init_vcpu(pkvm_handle_t handle, struct kvm_vcpu *host_vcpu)
{
	struct pkvm_hyp_vcpu *hyp_vcpu;
	struct pkvm_hyp_vm *hyp_vm;
	unsigned int idx;
	int ret;

	hyp_read_lock(&vm_table_lock);

	hyp_vm = get_vm_by_handle(handle);
	if (!hyp_vm) {
		ret = -ENOENT;
		goto unlock_vm;
	}

	hyp_vcpu = hyp_alloc_account(sizeof(*hyp_vcpu), hyp_vm->host_kvm);
	if (!hyp_vcpu) {
		ret = hyp_alloc_errno();
		goto unlock_vm;
	}

	hyp_spin_lock(&hyp_vm->vcpus_lock);
	idx = hyp_vm->nr_vcpus;
	if (idx >= hyp_vm->kvm.created_vcpus) {
		ret = -EINVAL;
		goto unlock_vcpus;
	}

	ret = init_pkvm_hyp_vcpu(hyp_vcpu, hyp_vm, host_vcpu, idx);
	if (ret)
		goto unlock_vcpus;

	hyp_vm->vcpus[idx] = hyp_vcpu;
	hyp_vm->nr_vcpus++;

unlock_vcpus:
	hyp_spin_unlock(&hyp_vm->vcpus_lock);

	if (ret)
		hyp_free_account(hyp_vcpu, hyp_vm->host_kvm);
unlock_vm:
	hyp_read_unlock(&vm_table_lock);

	return ret;
}

int __pkvm_start_teardown_vm(pkvm_handle_t handle)
{
	struct pkvm_hyp_vm *hyp_vm;
	int ret = 0;

	hyp_write_lock(&vm_table_lock);
	hyp_vm = get_vm_by_handle(handle);
	if (!hyp_vm) {
		ret = -ENOENT;
		goto unlock;
	} else if (WARN_ON(hyp_refcount_get(hyp_vm->refcount))) {
		ret = -EBUSY;
		goto unlock;
	} else if (hyp_vm->is_dying) {
		ret = -EINVAL;
		goto unlock;
	}

	hyp_vm->is_dying = true;

unlock:
	hyp_write_unlock(&vm_table_lock);

	return ret;
}

int __pkvm_finalize_teardown_vm(pkvm_handle_t handle)
{
	struct kvm_hyp_memcache *mc;
	struct pkvm_hyp_vm *hyp_vm;
	struct kvm *host_kvm;
	unsigned int idx;
	int err;

	hyp_write_lock(&vm_table_lock);
	hyp_vm = get_vm_by_handle(handle);
	if (!hyp_vm) {
		err = -ENOENT;
		goto err_unlock;
	} else if (!hyp_vm->is_dying) {
		err = -EBUSY;
		goto err_unlock;
	}

	host_kvm = hyp_vm->host_kvm;

	/* Ensure the VMID is clean before it can be reallocated */
	__kvm_tlb_flush_vmid(&hyp_vm->kvm.arch.mmu);
	remove_vm_table_entry(handle);
	hyp_write_unlock(&vm_table_lock);

	/*
	 * At this point, the VM has been detached from the VM table and
	 * has a refcount of 0 so we're free to tear it down without
	 * worrying about anybody else.
	 */

	mc = &host_kvm->arch.pkvm.stage2_teardown_mc;
	destroy_hyp_vm_pgt(hyp_vm);
	drain_hyp_pool(hyp_vm, mc);
	unpin_host_vcpus(hyp_vm->vcpus, hyp_vm->nr_vcpus);

	/* Push the metadata pages to the teardown memcache */
	for (idx = 0; idx < hyp_vm->nr_vcpus; ++idx) {
		struct pkvm_hyp_vcpu *hyp_vcpu = hyp_vm->vcpus[idx];
		struct kvm_hyp_memcache *vcpu_mc;
		void *addr;

		vcpu_mc = &hyp_vcpu->vcpu.arch.stage2_mc;
		while (vcpu_mc->nr_pages) {
			unsigned long order;
			addr = pop_hyp_memcache(vcpu_mc, hyp_phys_to_virt, &order);
			/* We don't expect vcpu to have higher order pages. */
			WARN_ON(order);
			push_hyp_memcache(mc, addr, hyp_virt_to_phys, order);
			unmap_donated_memory_noclear(addr, PAGE_SIZE);
		}

		if (pkvm_hyp_vcpu_is_protected(hyp_vcpu))
			teardown_sve_state(hyp_vcpu);

		hyp_free_account(hyp_vcpu, host_kvm);
	}

	hyp_free_account((__force void *)hyp_vm->kvm.arch.mmu.last_vcpu_ran,
			 host_kvm);
	hyp_free_account(hyp_vm, host_kvm);
	hyp_unpin_shared_mem(host_kvm, host_kvm + 1);
	return 0;

err_unlock:
	hyp_write_unlock(&vm_table_lock);
	return err;
}

int pkvm_load_pvmfw_pages(struct pkvm_hyp_vm *vm, u64 ipa, phys_addr_t phys,
			  u64 size)
{
	struct kvm_protected_vm *pkvm = &vm->kvm.arch.pkvm;
	u64 npages, offset = ipa - pkvm->pvmfw_load_addr;
	void *src = hyp_phys_to_virt(pvmfw_base) + offset;

	if (offset >= pvmfw_size)
		return -EINVAL;

	size = min(size, pvmfw_size - offset);
	if (!PAGE_ALIGNED(size) || !PAGE_ALIGNED(src))
		return -EINVAL;

	npages = size >> PAGE_SHIFT;
	while (npages--) {
		/*
		 * No need for cache maintenance here, as the pgtable code will
		 * take care of this when installing the pte in the guest's
		 * stage-2 page table.
		 */
		memcpy(hyp_fixmap_map(phys), src, PAGE_SIZE);
		hyp_fixmap_unmap();

		src += PAGE_SIZE;
		phys += PAGE_SIZE;
	}

	return 0;
}

void pkvm_poison_pvmfw_pages(void)
{
	u64 npages = pvmfw_size >> PAGE_SHIFT;
	phys_addr_t addr = pvmfw_base;

	while (npages--) {
		hyp_poison_page(addr);
		addr += PAGE_SIZE;
	}
}

/*
 * This function sets the registers on the vcpu to their architecturally defined
 * reset values.
 *
 * Note: Can only be called by the vcpu on itself, after it has been turned on.
 */
void pkvm_reset_vcpu(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	struct vcpu_reset_state *reset_state = &vcpu->arch.reset_state;
	struct pkvm_hyp_vm *hyp_vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);

	WARN_ON(!reset_state->reset);

	pkvm_vcpu_init_ptrauth(hyp_vcpu);
	kvm_reset_vcpu_core(vcpu);
	kvm_reset_pvm_sys_regs(vcpu);

	/* Must be done after reseting sys registers. */
	kvm_reset_vcpu_psci(vcpu, reset_state);
	if (hyp_vm->pvmfw_entry_vcpu == hyp_vcpu) {
		struct kvm_vcpu *host_vcpu = hyp_vcpu->host_vcpu;
		u64 entry = hyp_vm->kvm.arch.pkvm.pvmfw_load_addr;
		int i;

		/* X0 - X14 provided by the VMM (preserved) */
		for (i = 0; i <= 14; ++i) {
			u64 val = vcpu_get_reg(host_vcpu, i);

			vcpu_set_reg(&hyp_vcpu->vcpu, i, val);
		}

		/* X15: Boot protocol version */
		vcpu_set_reg(&hyp_vcpu->vcpu, 15, 0);

		/* PC: IPA of pvmfw base */
		*vcpu_pc(&hyp_vcpu->vcpu) = entry;
		hyp_vm->pvmfw_entry_vcpu = NULL;

		/* Auto enroll MMIO guard */
		set_bit(KVM_ARCH_FLAG_MMIO_GUARD, &hyp_vm->kvm.arch.flags);
	}

	if (pkvm_hyp_vcpu_is_protected(hyp_vcpu) && vcpu_has_sve(vcpu))
		memset(vcpu->arch.sve_state, 0, vcpu_sve_state_size(vcpu));

	reset_state->reset = false;

	hyp_vcpu->exit_code = 0;

	WARN_ON(hyp_vcpu->power_state != PSCI_0_2_AFFINITY_LEVEL_ON_PENDING);
	WRITE_ONCE(vcpu->arch.mp_state.mp_state, KVM_MP_STATE_RUNNABLE);
	WRITE_ONCE(hyp_vcpu->power_state, PSCI_0_2_AFFINITY_LEVEL_ON);
}

struct kvm_hyp_req *pkvm_hyp_req_reserve(struct pkvm_hyp_vcpu *hyp_vcpu, u8 type)
{
	struct kvm_hyp_req *next, *hyp_req = hyp_vcpu->vcpu.arch.hyp_reqs;
	int i;

	for (i = 0; i < KVM_HYP_REQ_MAX; i++) {
		if (hyp_req->type == KVM_HYP_LAST_REQ)
			break;
		hyp_req++;
	}

	/* The last entry of the page _must_ be a LAST_REQ */
	WARN_ON(i >= KVM_HYP_REQ_MAX);

	/* We need at least one empty slot to write LAST_REQ */
	if (i + 1 >= KVM_HYP_REQ_MAX)
		return NULL;

	hyp_req->type = type;

	next = hyp_req + 1;
	next->type = KVM_HYP_LAST_REQ;

	return hyp_req;
}

struct pkvm_hyp_vcpu *pkvm_mpidr_to_hyp_vcpu(struct pkvm_hyp_vm *hyp_vm,
					     u64 mpidr)
{
	struct pkvm_hyp_vcpu *hyp_vcpu;
	int i;

	mpidr &= MPIDR_HWID_BITMASK;

	hyp_spin_lock(&hyp_vm->vcpus_lock);
	for (i = 0; i < hyp_vm->nr_vcpus; i++) {
		hyp_vcpu = hyp_vm->vcpus[i];

		if (mpidr == kvm_vcpu_get_mpidr_aff(&hyp_vcpu->vcpu))
			goto unlock;
	}
	hyp_vcpu = NULL;
unlock:
	hyp_spin_unlock(&hyp_vm->vcpus_lock);
	return hyp_vcpu;
}

/*
 * Returns true if the hypervisor has handled the PSCI call, and control should
 * go back to the guest, or false if the host needs to do some additional work
 * (i.e., wake up the vcpu).
 */
static bool pvm_psci_vcpu_on(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct pkvm_hyp_vm *hyp_vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);
	struct vcpu_reset_state *reset_state;
	struct pkvm_hyp_vcpu *target;
	unsigned long cpu_id, ret;
	int power_state;

	cpu_id = smccc_get_arg1(&hyp_vcpu->vcpu);
	if (!kvm_psci_valid_affinity(&hyp_vcpu->vcpu, cpu_id)) {
		ret = PSCI_RET_INVALID_PARAMS;
		goto error;
	}

	target = pkvm_mpidr_to_hyp_vcpu(hyp_vm, cpu_id);
	if (!target) {
		ret = PSCI_RET_INVALID_PARAMS;
		goto error;
	}

	/*
	 * Make sure the requested vcpu is not on to begin with.
	 * Atomic to avoid race between vcpus trying to power on the same vcpu.
	 */
	power_state = cmpxchg(&target->power_state,
			      PSCI_0_2_AFFINITY_LEVEL_OFF,
			      PSCI_0_2_AFFINITY_LEVEL_ON_PENDING);
	switch (power_state) {
	case PSCI_0_2_AFFINITY_LEVEL_ON_PENDING:
		ret = PSCI_RET_ON_PENDING;
		goto error;
	case PSCI_0_2_AFFINITY_LEVEL_ON:
		ret = PSCI_RET_ALREADY_ON;
		goto error;
	case PSCI_0_2_AFFINITY_LEVEL_OFF:
		break;
	default:
		ret = PSCI_RET_INTERNAL_FAILURE;
		goto error;
	}

	reset_state = &target->vcpu.arch.reset_state;
	reset_state->pc = smccc_get_arg2(&hyp_vcpu->vcpu);
	reset_state->r0 = smccc_get_arg3(&hyp_vcpu->vcpu);
	/* Propagate caller endianness */
	reset_state->be = kvm_vcpu_is_be(&hyp_vcpu->vcpu);
	reset_state->reset = true;

	/*
	 * Return to the host, which should make the KVM_REQ_VCPU_RESET request
	 * as well as kvm_vcpu_wake_up() to schedule the vcpu.
	 */
	return false;

error:
	/* If there's an error go back straight to the guest. */
	smccc_set_retval(&hyp_vcpu->vcpu, ret, 0, 0, 0);
	return true;
}

static bool pvm_psci_vcpu_affinity_info(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	unsigned long target_affinity_mask, target_affinity, lowest_affinity_level;
	struct pkvm_hyp_vm *hyp_vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	unsigned long mpidr, ret;
	int i, matching_cpus = 0;

	target_affinity = smccc_get_arg1(vcpu);
	lowest_affinity_level = smccc_get_arg2(vcpu);
	if (!kvm_psci_valid_affinity(vcpu, target_affinity)) {
		ret = PSCI_RET_INVALID_PARAMS;
		goto done;
	}

	/* Determine target affinity mask */
	target_affinity_mask = psci_affinity_mask(lowest_affinity_level);
	if (!target_affinity_mask) {
		ret = PSCI_RET_INVALID_PARAMS;
		goto done;
	}

	/* Ignore other bits of target affinity */
	target_affinity &= target_affinity_mask;
	ret = PSCI_0_2_AFFINITY_LEVEL_OFF;

	/*
	 * If at least one vcpu matching target affinity is ON then return ON,
	 * then if at least one is PENDING_ON then return PENDING_ON.
	 * Otherwise, return OFF.
	 */
	hyp_spin_lock(&hyp_vm->vcpus_lock);
	for (i = 0; i < hyp_vm->nr_vcpus; i++) {
		struct pkvm_hyp_vcpu *target = hyp_vm->vcpus[i];

		mpidr = kvm_vcpu_get_mpidr_aff(&target->vcpu);

		if ((mpidr & target_affinity_mask) == target_affinity) {
			int power_state;

			matching_cpus++;
			power_state = READ_ONCE(target->power_state);
			switch (power_state) {
			case PSCI_0_2_AFFINITY_LEVEL_ON_PENDING:
				ret = PSCI_0_2_AFFINITY_LEVEL_ON_PENDING;
				break;
			case PSCI_0_2_AFFINITY_LEVEL_ON:
				ret = PSCI_0_2_AFFINITY_LEVEL_ON;
				goto unlock;
			case PSCI_0_2_AFFINITY_LEVEL_OFF:
				break;
			default:
				ret = PSCI_RET_INTERNAL_FAILURE;
				goto unlock;
			}
		}
	}

	if (!matching_cpus)
		ret = PSCI_RET_INVALID_PARAMS;
unlock:
	hyp_spin_unlock(&hyp_vm->vcpus_lock);
done:
	/* Nothing to be handled by the host. Go back to the guest. */
	smccc_set_retval(vcpu, ret, 0, 0, 0);
	return true;
}

/*
 * Returns true if the hypervisor has handled the PSCI call, and control should
 * go back to the guest, or false if the host needs to do some additional work
 * (e.g., turn off and update vcpu scheduling status).
 */
static bool pvm_psci_vcpu_off(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;

	WARN_ON(vcpu->arch.mp_state.mp_state == KVM_MP_STATE_STOPPED);
	WARN_ON(hyp_vcpu->power_state != PSCI_0_2_AFFINITY_LEVEL_ON);

	WRITE_ONCE(vcpu->arch.mp_state.mp_state, KVM_MP_STATE_STOPPED);
	WRITE_ONCE(hyp_vcpu->power_state, PSCI_0_2_AFFINITY_LEVEL_OFF);

	/* Return to the host so that it can finish powering off the vcpu. */
	return false;
}

static bool pvm_psci_version(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	/* Nothing to be handled by the host. Go back to the guest. */
	smccc_set_retval(&hyp_vcpu->vcpu, KVM_ARM_PSCI_1_1, 0, 0, 0);
	return true;
}

static bool pvm_psci_not_supported(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	/* Nothing to be handled by the host. Go back to the guest. */
	smccc_set_retval(&hyp_vcpu->vcpu, PSCI_RET_NOT_SUPPORTED, 0, 0, 0);
	return true;
}

static bool pvm_psci_features(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u32 feature = smccc_get_arg1(vcpu);
	unsigned long val;

	switch (feature) {
	case PSCI_0_2_FN_PSCI_VERSION:
	case PSCI_0_2_FN_CPU_SUSPEND:
	case PSCI_0_2_FN64_CPU_SUSPEND:
	case PSCI_0_2_FN_CPU_OFF:
	case PSCI_0_2_FN_CPU_ON:
	case PSCI_0_2_FN64_CPU_ON:
	case PSCI_0_2_FN_AFFINITY_INFO:
	case PSCI_0_2_FN64_AFFINITY_INFO:
	case PSCI_0_2_FN_SYSTEM_OFF:
	case PSCI_0_2_FN_SYSTEM_RESET:
	case PSCI_1_0_FN_PSCI_FEATURES:
	case PSCI_1_1_FN_SYSTEM_RESET2:
	case PSCI_1_1_FN64_SYSTEM_RESET2:
	case ARM_SMCCC_VERSION_FUNC_ID:
		val = PSCI_RET_SUCCESS;
		break;
	default:
		val = PSCI_RET_NOT_SUPPORTED;
		break;
	}

	/* Nothing to be handled by the host. Go back to the guest. */
	smccc_set_retval(vcpu, val, 0, 0, 0);
	return true;
}

static bool pkvm_handle_psci(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u32 psci_fn = smccc_get_function(vcpu);

	switch (psci_fn) {
	case PSCI_0_2_FN_CPU_ON:
		kvm_psci_narrow_to_32bit(vcpu);
		fallthrough;
	case PSCI_0_2_FN64_CPU_ON:
		return pvm_psci_vcpu_on(hyp_vcpu);
	case PSCI_0_2_FN_CPU_OFF:
		return pvm_psci_vcpu_off(hyp_vcpu);
	case PSCI_0_2_FN_AFFINITY_INFO:
		kvm_psci_narrow_to_32bit(vcpu);
		fallthrough;
	case PSCI_0_2_FN64_AFFINITY_INFO:
		return pvm_psci_vcpu_affinity_info(hyp_vcpu);
	case PSCI_0_2_FN_PSCI_VERSION:
		return pvm_psci_version(hyp_vcpu);
	case PSCI_1_0_FN_PSCI_FEATURES:
		return pvm_psci_features(hyp_vcpu);
	case PSCI_0_2_FN_SYSTEM_RESET:
	case PSCI_0_2_FN_CPU_SUSPEND:
	case PSCI_0_2_FN64_CPU_SUSPEND:
	case PSCI_0_2_FN_SYSTEM_OFF:
	case PSCI_1_1_FN_SYSTEM_RESET2:
	case PSCI_1_1_FN64_SYSTEM_RESET2:
		return false; /* Handled by the host. */
	default:
		break;
	}

	return pvm_psci_not_supported(hyp_vcpu);
}

static int pkvm_handle_empty_memcache(struct pkvm_hyp_vcpu *hyp_vcpu,
				      u64 *exit_code)
{
	struct kvm_hyp_req *req;

	req = pkvm_hyp_req_reserve(hyp_vcpu, KVM_HYP_REQ_TYPE_MEM);
	if (!req)
		return -ENOMEM;

	req->mem.dest = REQ_MEM_DEST_VCPU_MEMCACHE;
	req->mem.nr_pages = kvm_mmu_cache_min_pages(hyp_vcpu->vcpu.kvm);

	write_sysreg_el2(read_sysreg_el2(SYS_ELR) - 4, SYS_ELR);

	*exit_code = ARM_EXCEPTION_HYP_REQ;

	return 0;
}

static bool pkvm_memshare_call(struct pkvm_hyp_vcpu *hyp_vcpu, u64 *exit_code)
{
	struct pkvm_hyp_vm *hyp_vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u64 ipa = smccc_get_arg1(vcpu);
	u64 nr_pages = smccc_get_arg2(vcpu);
	u64 arg3 = smccc_get_arg3(vcpu);
	struct kvm_hyp_req *req;
	u64 nr_shared;
	int err;

	/* Legacy guests have arg2 set to 0 */
	if (nr_pages == 0)
		nr_pages = 1;

	if (arg3 || !PAGE_ALIGNED(ipa))
		goto out_guest_err;

	err = __pkvm_guest_share_host(hyp_vcpu, ipa, nr_pages, &nr_shared);
	switch (err) {
	case 0:
		atomic64_add(nr_shared * PAGE_SIZE,
			     &hyp_vm->host_kvm->stat.protected_shared_mem);
		smccc_set_retval(vcpu, SMCCC_RET_SUCCESS, nr_shared, 0, 0);

		return true;
	case -EFAULT:
		req = pkvm_hyp_req_reserve(hyp_vcpu, KVM_HYP_REQ_TYPE_MAP);
		if (!req)
			goto out_guest_err;

		req->map.guest_ipa = ipa;
		req->map.size = nr_pages << PAGE_SHIFT;

		/*
		 * We're about to go back to the host... let's not waste time
		 * and check for the memcache while at it.
		 */
		fallthrough;
	case -ENOMEM:
		if (pkvm_handle_empty_memcache(hyp_vcpu, exit_code))
			goto out_guest_err;

		goto out_host;
	}

out_guest_err:
	smccc_set_retval(vcpu, SMCCC_RET_INVALID_PARAMETER, 0, 0, 0);
	return true;

out_host:
	return false;
}

static bool pkvm_memunshare_call(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct pkvm_hyp_vm *hyp_vm = pkvm_hyp_vcpu_to_hyp_vm(hyp_vcpu);
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u64 ipa = smccc_get_arg1(vcpu);
	u64 nr_pages = smccc_get_arg2(vcpu);
	u64 arg3 = smccc_get_arg3(vcpu);
	u64 nr_unshared;
	int err;

	/* Legacy guests have arg2 set to 0 */
	if (nr_pages == 0)
		nr_pages = 1;

	if (arg3 || !PAGE_ALIGNED(ipa))
		goto out_guest_err;

	err = __pkvm_guest_unshare_host(hyp_vcpu, ipa, nr_pages, &nr_unshared);
	if (err)
		goto out_guest_err;

	atomic64_add(nr_unshared * PAGE_SIZE,
		     &hyp_vm->host_kvm->stat.protected_shared_mem);
	smccc_set_retval(vcpu, SMCCC_RET_SUCCESS, nr_unshared, 0, 0);
	return true;

out_guest_err:
	smccc_set_retval(vcpu, SMCCC_RET_INVALID_PARAMETER, 0, 0, 0);
	return true;
}

static bool pkvm_install_ioguard_page(struct pkvm_hyp_vcpu *hyp_vcpu,
				      u64 *exit_code)
{
	u64 ipa = smccc_get_arg1(&hyp_vcpu->vcpu);
	u64 nr_pages = smccc_get_arg2(&hyp_vcpu->vcpu);
	u32 fn = smccc_get_function(&hyp_vcpu->vcpu);
	u64 retval = SMCCC_RET_SUCCESS;
	u64 nr_guarded = 0;
	int ret = -EINVAL;

	/* Legacy non-range version, arg2|arg3 might be garbage */
	if (fn == ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_MAP_FUNC_ID)
		nr_pages = 1;
	else if (smccc_get_arg3(&hyp_vcpu->vcpu))
		goto out_guest_err;

	ret = __pkvm_install_ioguard_page(hyp_vcpu, ipa, nr_pages, &nr_guarded);
	if (ret == -ENOMEM && !pkvm_handle_empty_memcache(hyp_vcpu, exit_code))
		return false;

out_guest_err:
	if (ret)
		retval = SMCCC_RET_INVALID_PARAMETER;

	smccc_set_retval(&hyp_vcpu->vcpu, retval, nr_guarded, 0, 0);
	return true;
}

static bool pkvm_remove_ioguard_page(struct pkvm_hyp_vcpu *hyp_vcpu,
				     u64 *exit_code)
{
	u64 ipa = smccc_get_arg1(&hyp_vcpu->vcpu);
	u64 nr_pages = smccc_get_arg2(&hyp_vcpu->vcpu);
	u32 fn = smccc_get_function(&hyp_vcpu->vcpu);
	u64 retval = SMCCC_RET_SUCCESS;
	u64 nr_unguarded = 0;
	int ret = -EINVAL;

	/* Legacy non-range version, arg2|arg3 might be garbage */
	if (fn == ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_UNMAP_FUNC_ID)
		nr_pages = 1;
	else if (smccc_get_arg3(&hyp_vcpu->vcpu))
		goto out_guest_err;

	ret = __pkvm_remove_ioguard_page(hyp_vcpu, ipa, nr_pages, &nr_unguarded);

out_guest_err:
	if (ret)
		retval = SMCCC_RET_INVALID_PARAMETER;

	smccc_set_retval(&hyp_vcpu->vcpu, retval, nr_unguarded, 0, 0);
	return true;
}

static bool pkvm_meminfo_call(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u64 arg1 = smccc_get_arg1(vcpu);
	u64 arg2 = smccc_get_arg2(vcpu);
	u64 arg3 = smccc_get_arg3(vcpu);

	if (arg1 || arg2 || arg3)
		goto out_guest_err;

	smccc_set_retval(vcpu, PAGE_SIZE, KVM_FUNC_HAS_RANGE, 0, 0);
	return true;

out_guest_err:
	smccc_set_retval(vcpu, SMCCC_RET_INVALID_PARAMETER, 0, 0, 0);
	return true;
}

static bool pkvm_memrelinquish_call(struct pkvm_hyp_vcpu *hyp_vcpu,
				    u64 *exit_code)
{
	struct kvm_vcpu *vcpu = &hyp_vcpu->vcpu;
	u64 ipa = smccc_get_arg1(vcpu);
	u64 arg2 = smccc_get_arg2(vcpu);
	u64 arg3 = smccc_get_arg3(vcpu);
	u64 pa = 0;
	int ret;

	if (arg2 || arg3)
		goto out_guest_err;

	ret = __pkvm_guest_relinquish_to_host(hyp_vcpu, ipa, &pa);
	if (ret == -ENOMEM) {
		if (pkvm_handle_empty_memcache(hyp_vcpu, exit_code))
			goto out_guest_err;

		return false;
	} else if (ret) {
		goto out_guest_err;
	}

	if (pa != 0) {
		/* Now pass to host. */
		return false;
	}

	/* This was a NOP as no page was actually mapped at the IPA. */
	smccc_set_retval(vcpu, 0, 0, 0, 0);
	return true;

out_guest_err:
	smccc_set_retval(vcpu, SMCCC_RET_INVALID_PARAMETER, 0, 0, 0);
	return true;
}

bool smccc_trng_available;

static bool pkvm_forward_trng(struct kvm_vcpu *vcpu)
{
	u32 fn = smccc_get_function(vcpu);
	struct arm_smccc_res res;
	unsigned long arg1 = 0;

	/*
	 * Forward TRNG calls to EL3, as we can't trust the host to handle
	 * these for us.
	 */
	switch (fn) {
	case ARM_SMCCC_TRNG_FEATURES:
	case ARM_SMCCC_TRNG_RND32:
	case ARM_SMCCC_TRNG_RND64:
		arg1 = smccc_get_arg1(vcpu);
		fallthrough;
	case ARM_SMCCC_TRNG_VERSION:
	case ARM_SMCCC_TRNG_GET_UUID:
		arm_smccc_1_1_smc(fn, arg1, &res);
		smccc_set_retval(vcpu, res.a0, res.a1, res.a2, res.a3);
		memzero_explicit(&res, sizeof(res));
		break;
	}

	return true;
}

/*
 * Handler for protected VM HVC calls.
 *
 * Returns true if the hypervisor has handled the exit, and control should go
 * back to the guest, or false if it hasn't.
 */
bool kvm_handle_pvm_hvc64(struct kvm_vcpu *vcpu, u64 *exit_code)
{
	u64 val[4] = { SMCCC_RET_NOT_SUPPORTED };
	u32 fn = smccc_get_function(vcpu);
	struct pkvm_hyp_vcpu *hyp_vcpu;

	hyp_vcpu = container_of(vcpu, struct pkvm_hyp_vcpu, vcpu);

	switch (fn) {
	case ARM_SMCCC_VERSION_FUNC_ID:
		/* Nothing to be handled by the host. Go back to the guest. */
		val[0] = ARM_SMCCC_VERSION_1_1;
		break;
	case ARM_SMCCC_VENDOR_HYP_CALL_UID_FUNC_ID:
		val[0] = ARM_SMCCC_VENDOR_HYP_UID_KVM_REG_0;
		val[1] = ARM_SMCCC_VENDOR_HYP_UID_KVM_REG_1;
		val[2] = ARM_SMCCC_VENDOR_HYP_UID_KVM_REG_2;
		val[3] = ARM_SMCCC_VENDOR_HYP_UID_KVM_REG_3;
		break;
	case ARM_SMCCC_VENDOR_HYP_KVM_FEATURES_FUNC_ID:
		val[0] = BIT(ARM_SMCCC_KVM_FUNC_FEATURES);
		val[0] |= BIT(ARM_SMCCC_KVM_FUNC_HYP_MEMINFO);
		val[0] |= BIT(ARM_SMCCC_KVM_FUNC_MEM_SHARE);
		val[0] |= BIT(ARM_SMCCC_KVM_FUNC_MEM_UNSHARE);
		val[0] |= BIT(ARM_SMCCC_KVM_FUNC_MMIO_GUARD_INFO);
		val[0] |= BIT(ARM_SMCCC_KVM_FUNC_MMIO_GUARD_ENROLL);
		val[0] |= BIT(ARM_SMCCC_KVM_FUNC_MMIO_GUARD_MAP);
		val[0] |= BIT(ARM_SMCCC_KVM_FUNC_MMIO_GUARD_UNMAP);
		val[0] |= BIT(ARM_SMCCC_KVM_FUNC_MMIO_RGUARD_MAP);
		val[0] |= BIT(ARM_SMCCC_KVM_FUNC_MMIO_RGUARD_UNMAP);
		val[0] |= BIT(ARM_SMCCC_KVM_FUNC_MEM_RELINQUISH);
		break;
	case ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_ENROLL_FUNC_ID:
		set_bit(KVM_ARCH_FLAG_MMIO_GUARD, &vcpu->kvm->arch.flags);
		val[0] = SMCCC_RET_SUCCESS;
		break;
	case ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_MAP_FUNC_ID:
	case ARM_SMCCC_VENDOR_HYP_KVM_MMIO_RGUARD_MAP_FUNC_ID:
		return pkvm_install_ioguard_page(hyp_vcpu, exit_code);
	case ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_UNMAP_FUNC_ID:
	case ARM_SMCCC_VENDOR_HYP_KVM_MMIO_RGUARD_UNMAP_FUNC_ID:
		return pkvm_remove_ioguard_page(hyp_vcpu, exit_code);
	case ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_INFO_FUNC_ID:
	case ARM_SMCCC_VENDOR_HYP_KVM_HYP_MEMINFO_FUNC_ID:
		return pkvm_meminfo_call(hyp_vcpu);
	case ARM_SMCCC_VENDOR_HYP_KVM_MEM_SHARE_FUNC_ID:
		return pkvm_memshare_call(hyp_vcpu, exit_code);
	case ARM_SMCCC_VENDOR_HYP_KVM_MEM_UNSHARE_FUNC_ID:
		return pkvm_memunshare_call(hyp_vcpu);
	case ARM_SMCCC_VENDOR_HYP_KVM_MEM_RELINQUISH_FUNC_ID:
		return pkvm_memrelinquish_call(hyp_vcpu, exit_code);
	case ARM_SMCCC_TRNG_VERSION ... ARM_SMCCC_TRNG_RND32:
	case ARM_SMCCC_TRNG_RND64:
		if (smccc_trng_available)
			return pkvm_forward_trng(vcpu);
		break;
	default:
		return pkvm_handle_psci(hyp_vcpu);
	}

	smccc_set_retval(vcpu, val[0], val[1], val[2], val[3]);
	return true;
}

/*
 * Handler for non-protected VM HVC calls.
 *
 * Returns true if the hypervisor has handled the exit, and control should go
 * back to the guest, or false if it hasn't.
 */
bool kvm_hyp_handle_hvc64(struct kvm_vcpu *vcpu, u64 *exit_code)
{
	u32 fn = smccc_get_function(vcpu);
	struct pkvm_hyp_vcpu *hyp_vcpu;

	hyp_vcpu = container_of(vcpu, struct pkvm_hyp_vcpu, vcpu);

	switch (fn) {
	case ARM_SMCCC_VENDOR_HYP_KVM_HYP_MEMINFO_FUNC_ID:
		return pkvm_meminfo_call(hyp_vcpu);
	case ARM_SMCCC_VENDOR_HYP_KVM_MEM_RELINQUISH_FUNC_ID:
		return pkvm_memrelinquish_call(hyp_vcpu, exit_code);
	}

	return false;
}

#ifdef CONFIG_NVHE_EL2_DEBUG
static inline phys_addr_t get_next_memcache_page(phys_addr_t head)
{
	return *((phys_addr_t *)hyp_phys_to_virt(head)) & PAGE_MASK;
}

static void *pkvm_setup_snapshot(struct kvm_pgtable_snapshot *snap_hva)
{
	unsigned long i;
	void *pgd, *used_pg;
	phys_addr_t mc_page, next_mc_page;
	struct kvm_pgtable_snapshot *snap;

	snap = (void *)kern_hyp_va(snap_hva);
	if (!PAGE_ALIGNED(snap))
		return NULL;

	if (__pkvm_host_donate_hyp(hyp_virt_to_pfn(snap), 1))
		return NULL;

	if (snap->pgd_pages == 0 || snap->num_used_pages == 0)
		return snap;

	pgd = kern_hyp_va(snap->pgd_hva);
	if (!PAGE_ALIGNED(pgd))
		goto error_with_snapshot;

	if (__pkvm_host_donate_hyp(hyp_virt_to_pfn(pgd), snap->pgd_pages))
		goto error_with_snapshot;

	mc_page = snap->mc.head & PAGE_MASK;
	for (i = 0; i < snap->mc.nr_pages; i++) {
		if (!PAGE_ALIGNED(mc_page))
			goto error_with_memcache;

		if (__pkvm_host_donate_hyp(hyp_phys_to_pfn(mc_page), 1))
			goto error_with_memcache;

		mc_page = get_next_memcache_page(mc_page);
	}

	used_pg = kern_hyp_va(snap->used_pages_hva);
	if (!PAGE_ALIGNED(used_pg))
		goto error_with_memcache;

	if (__pkvm_host_donate_hyp(hyp_virt_to_pfn(used_pg), snap->num_used_pages))
		goto error_with_memcache;

	return snap;
error_with_memcache:
	mc_page = snap->mc.head & PAGE_MASK;
	for (; i >= 0; i--) {
		next_mc_page = get_next_memcache_page(mc_page);
		WARN_ON(__pkvm_hyp_donate_host(hyp_phys_to_pfn(mc_page), 1));
		mc_page = next_mc_page;
	}

	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(pgd), snap->pgd_pages));
error_with_snapshot:
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(snap), 1));
	return NULL;
}

static void pkvm_teardown_snapshot(struct kvm_pgtable_snapshot *snap)
{
	size_t i;
	phys_addr_t mc_page, next_mc_page;
	u64 *used_pg = kern_hyp_va(snap->used_pages_hva);
	void *pgd = kern_hyp_va(snap->pgd_hva);

	if (snap->pgd_pages == 0 || snap->num_used_pages == 0)
		goto unmap_snapshot;

	for (i = 0; i < snap->used_pages_idx; i++) {
		mc_page = used_pg[i];
		WARN_ON(__pkvm_hyp_donate_host(hyp_phys_to_pfn(mc_page), 1));
	}

	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(used_pg),
				       snap->num_used_pages));

	mc_page = snap->mc.head & PAGE_MASK;
	for (i = 0; i < snap->mc.nr_pages; i++) {
		next_mc_page = get_next_memcache_page(mc_page);
		WARN_ON(__pkvm_hyp_donate_host(hyp_phys_to_pfn(mc_page), 1));
		mc_page = next_mc_page;
	}

	snap->pgtable.mm_ops = NULL;
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(pgd), snap->pgd_pages));
unmap_snapshot:
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(snap), 1));
}

int pkvm_stage2_snapshot_by_handle(struct kvm_pgtable_snapshot *snap_hva,
				   pkvm_handle_t handle)
{
	int ret = -EINVAL;
	struct pkvm_hyp_vm *vm;
	kvm_pte_t *pgd;
	struct kvm_pgtable_snapshot *snap;

	snap = pkvm_setup_snapshot(snap_hva);
	if (!snap)
		return -EINVAL;

	if (!handle)
		ret = __pkvm_host_stage2_snapshot(snap);
	else {
		hyp_read_lock(&vm_table_lock);
		vm = get_vm_by_handle(handle);
		if (vm)
			ret = __pkvm_guest_stage2_snapshot(snap, vm);
		hyp_read_unlock(&vm_table_lock);
	}

	if (!ret) {
		pgd = snap->pgtable.pgd;
		snap->pgtable.pgd = (kvm_pte_t *)__hyp_pa(pgd);
	}
	pkvm_teardown_snapshot(snap);
	return ret;
}
#endif /* CONFIG_NVHE_EL2_DEBUG */
