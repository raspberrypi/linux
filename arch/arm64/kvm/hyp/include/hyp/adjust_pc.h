// SPDX-License-Identifier: GPL-2.0-only
/*
 * Guest PC manipulation helpers
 *
 * Copyright (C) 2012,2013 - ARM Ltd
 * Copyright (C) 2020 - Google LLC
 * Author: Marc Zyngier <maz@kernel.org>
 */

#ifndef __ARM64_KVM_HYP_ADJUST_PC_H__
#define __ARM64_KVM_HYP_ADJUST_PC_H__

#include <asm/kvm_emulate.h>
#include <asm/kvm_host.h>

#ifdef __KVM_NVHE_HYPERVISOR__
#include <nvhe/pkvm.h>

/* Under pKVM a host vCPU's ->kvm is host-writable. */
static inline struct kvm *vcpu_get_kvm(struct kvm_vcpu *vcpu)
{
	if (is_protected_kvm_enabled())
		return pkvm_vcpu_get_kvm(vcpu);

	return kern_hyp_va(vcpu->kvm);
}

static inline void vcpu_put_kvm(struct kvm_vcpu *vcpu, struct kvm *kvm)
{
	if (is_protected_kvm_enabled())
		pkvm_vcpu_put_kvm(vcpu, kvm);
}
#else
static inline struct kvm *vcpu_get_kvm(struct kvm_vcpu *vcpu)
{
	return vcpu->kvm;
}

static inline void vcpu_put_kvm(struct kvm_vcpu *vcpu, struct kvm *kvm)
{
}
#endif

static inline void kvm_skip_instr(struct kvm_vcpu *vcpu)
{
	if (vcpu_mode_is_32bit(vcpu)) {
		kvm_skip_instr32(vcpu);
	} else {
		*vcpu_pc(vcpu) += 4;
		*vcpu_cpsr(vcpu) &= ~PSR_BTYPE_MASK;
	}

	/* advance the singlestep state machine */
	*vcpu_cpsr(vcpu) &= ~DBG_SPSR_SS;
}

/*
 * Skip an instruction which has been emulated at hyp while most guest sysregs
 * are live.
 */
static inline void __kvm_skip_instr(struct kvm_vcpu *vcpu)
{
	*vcpu_pc(vcpu) = read_sysreg_el2(SYS_ELR);
	vcpu_gp_regs(vcpu)->pstate = read_sysreg_el2(SYS_SPSR);

	kvm_skip_instr(vcpu);

	write_sysreg_el2(vcpu_gp_regs(vcpu)->pstate, SYS_SPSR);
	write_sysreg_el2(*vcpu_pc(vcpu), SYS_ELR);
}

/*
 * Skip an instruction while host sysregs are live.
 * Assumes host is always 64-bit.
 */
static inline void kvm_skip_host_instr(void)
{
	write_sysreg_el2(read_sysreg_el2(SYS_ELR) + 4, SYS_ELR);
}

#endif
