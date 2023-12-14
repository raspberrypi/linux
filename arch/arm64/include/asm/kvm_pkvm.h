// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 - Google LLC
 * Author: Quentin Perret <qperret@google.com>
 * Author: Fuad Tabba <tabba@google.com>
 */
#ifndef __ARM64_KVM_PKVM_H__
#define __ARM64_KVM_PKVM_H__

#include <linux/arm_ffa.h>
#include <linux/memblock.h>
#include <linux/scatterlist.h>
#include <asm/kvm_pgtable.h>
#include <asm/sysreg.h>

/*
 * Stores the sve state for the host in protected mode.
 */
struct kvm_host_sve_state {
	u64 zcr_el1;

	/*
	 * Ordering is important since __sve_save_state/__sve_restore_state
	 * relies on it.
	 */
	u32 fpsr;
	u32 fpcr;

	/* Must be SVE_VQ_BYTES (128 bit) aligned. */
	char sve_regs[];
};

/* Maximum number of VMs that can co-exist under pKVM. */
#define KVM_MAX_PVMS 255

#define HYP_MEMBLOCK_REGIONS 128
#define PVMFW_INVALID_LOAD_ADDR	(-1)

int pkvm_vm_ioctl_enable_cap(struct kvm *kvm,struct kvm_enable_cap *cap);
int pkvm_init_host_vm(struct kvm *kvm, unsigned long type);
int pkvm_create_hyp_vm(struct kvm *kvm);
void pkvm_destroy_hyp_vm(struct kvm *kvm);
void pkvm_host_reclaim_page(struct kvm *host_kvm, phys_addr_t ipa);

/*
 * Definitions for features to be allowed or restricted for guest virtual
 * machines, depending on the mode KVM is running in and on the type of guest
 * that is running.
 *
 * The ALLOW masks represent a bitmask of feature fields that are allowed
 * without any restrictions as long as they are supported by the system.
 *
 * The RESTRICT_UNSIGNED masks, if present, represent unsigned fields for
 * features that are restricted to support at most the specified feature.
 *
 * If a feature field is not present in either, than it is not supported.
 *
 * The approach taken for protected VMs is to allow features that are:
 * - Needed by common Linux distributions (e.g., floating point)
 * - Trivial to support, e.g., supporting the feature does not introduce or
 * require tracking of additional state in KVM
 * - Cannot be trapped or prevent the guest from using anyway
 */

/*
 * Allow for protected VMs:
 * - Floating-point and Advanced SIMD
 * - GICv3(+) system register interface
 * - Data Independent Timing
 */
#define PVM_ID_AA64PFR0_ALLOW (\
	ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_FP) | \
	ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_AdvSIMD) | \
	ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_GIC) | \
	ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_DIT) \
	)

/*
 * Restrict to the following *unsigned* features for protected VMs:
 * - AArch64 guests only (no support for AArch32 guests):
 *	AArch32 adds complexity in trap handling, emulation, condition codes,
 *	etc...
 * - SVE
 * - RAS (v1)
 *	Supported by KVM
 */
#define PVM_ID_AA64PFR0_RESTRICT_UNSIGNED (\
	FIELD_PREP(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_EL0), ID_AA64PFR0_EL1_ELx_64BIT_ONLY) | \
	FIELD_PREP(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_EL1), ID_AA64PFR0_EL1_ELx_64BIT_ONLY) | \
	FIELD_PREP(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_EL2), ID_AA64PFR0_EL1_ELx_64BIT_ONLY) | \
	FIELD_PREP(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_EL3), ID_AA64PFR0_EL1_ELx_64BIT_ONLY) | \
	FIELD_PREP(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_SVE), ID_AA64PFR0_EL1_SVE_IMP) | \
	FIELD_PREP(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_RAS), ID_AA64PFR0_EL1_RAS_IMP) \
	)

/*
 * Allow for protected VMs:
 * - Branch Target Identification
 * - Speculative Store Bypassing
 */
#define PVM_ID_AA64PFR1_ALLOW (\
	ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_BT) | \
	ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_SSBS) \
	)

#define PVM_ID_AA64PFR2_ALLOW (0ULL)

/*
 * Allow for protected VMs:
 * - Mixed-endian
 * - Distinction between Secure and Non-secure Memory
 * - Mixed-endian at EL0 only
 * - Non-context synchronizing exception entry and exit
 */
#define PVM_ID_AA64MMFR0_ALLOW (\
	ARM64_FEATURE_MASK(ID_AA64MMFR0_EL1_BIGEND) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR0_EL1_SNSMEM) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR0_EL1_BIGENDEL0) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR0_EL1_EXS) \
	)

/*
 * Restrict to the following *unsigned* features for protected VMs:
 * - 40-bit IPA
 * - 16-bit ASID
 */
#define PVM_ID_AA64MMFR0_RESTRICT_UNSIGNED (\
	FIELD_PREP(ARM64_FEATURE_MASK(ID_AA64MMFR0_EL1_PARANGE), ID_AA64MMFR0_EL1_PARANGE_40) | \
	FIELD_PREP(ARM64_FEATURE_MASK(ID_AA64MMFR0_EL1_ASIDBITS), ID_AA64MMFR0_EL1_ASIDBITS_16) \
	)

/*
 * Allow for protected VMs:
 * - Hardware translation table updates to Access flag and Dirty state
 * - Number of VMID bits from CPU
 * - Hierarchical Permission Disables
 * - Privileged Access Never
 * - SError interrupt exceptions from speculative reads
 * - Enhanced Translation Synchronization
 * - Control for cache maintenance permission
 */
#define PVM_ID_AA64MMFR1_ALLOW (\
	ARM64_FEATURE_MASK(ID_AA64MMFR1_EL1_HAFDBS) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR1_EL1_VMIDBits) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR1_EL1_HPDS) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR1_EL1_PAN) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR1_EL1_SpecSEI) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR1_EL1_ETS) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR1_EL1_CMOW) \
	)

/*
 * Allow for protected VMs:
 * - Common not Private translations
 * - User Access Override
 * - IESB bit in the SCTLR_ELx registers
 * - Unaligned single-copy atomicity and atomic functions
 * - ESR_ELx.EC value on an exception by read access to feature ID space
 * - TTL field in address operations.
 * - Break-before-make sequences when changing translation block size
 * - E0PDx mechanism
 */
#define PVM_ID_AA64MMFR2_ALLOW (\
	ARM64_FEATURE_MASK(ID_AA64MMFR2_EL1_CnP) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR2_EL1_UAO) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR2_EL1_IESB) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR2_EL1_AT) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR2_EL1_IDS) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR2_EL1_TTL) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR2_EL1_BBM) | \
	ARM64_FEATURE_MASK(ID_AA64MMFR2_EL1_E0PD) \
	)

#define PVM_ID_AA64MMFR3_ALLOW (0ULL)

/*
 * No restrictions for Scalable Vectors (SVE).
 */
#define PVM_ID_AA64ZFR0_ALLOW (\
	ARM64_FEATURE_MASK(ID_AA64ZFR0_EL1_SVEver) | \
	ARM64_FEATURE_MASK(ID_AA64ZFR0_EL1_AES) | \
	ARM64_FEATURE_MASK(ID_AA64ZFR0_EL1_BitPerm) | \
	ARM64_FEATURE_MASK(ID_AA64ZFR0_EL1_BF16) | \
	ARM64_FEATURE_MASK(ID_AA64ZFR0_EL1_SHA3) | \
	ARM64_FEATURE_MASK(ID_AA64ZFR0_EL1_SM4) | \
	ARM64_FEATURE_MASK(ID_AA64ZFR0_EL1_I8MM) | \
	ARM64_FEATURE_MASK(ID_AA64ZFR0_EL1_F32MM) | \
	ARM64_FEATURE_MASK(ID_AA64ZFR0_EL1_F64MM) \
	)

/*
 * No support for debug, including breakpoints, and watchpoints for protected
 * VMs:
 *	The Arm architecture mandates support for at least the Armv8 debug
 *	architecture, which would include at least 2 hardware breakpoints and
 *	watchpoints. Providing that support to protected guests adds
 *	considerable state and complexity. Therefore, the reserved value of 0 is
 *	used for debug-related fields.
 */
#define PVM_ID_AA64DFR0_ALLOW (0ULL)
#define PVM_ID_AA64DFR1_ALLOW (0ULL)

/*
 * No support for implementation defined features.
 */
#define PVM_ID_AA64AFR0_ALLOW (0ULL)
#define PVM_ID_AA64AFR1_ALLOW (0ULL)

/*
 * No restrictions on instructions implemented in AArch64.
 */
#define PVM_ID_AA64ISAR0_ALLOW (\
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_AES) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_SHA1) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_SHA2) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_CRC32) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_ATOMIC) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_RDM) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_SHA3) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_SM3) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_SM4) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_DP) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_FHM) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_TS) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_TLB) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_RNDR) \
	)

/* Restrict pointer authentication to the basic version. */
#define PVM_ID_AA64ISAR1_RESTRICT_UNSIGNED (\
	FIELD_PREP(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_APA), ID_AA64ISAR1_EL1_APA_PAuth) | \
	FIELD_PREP(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_API), ID_AA64ISAR1_EL1_API_PAuth) \
	)

#define PVM_ID_AA64ISAR2_RESTRICT_UNSIGNED (\
	FIELD_PREP(ARM64_FEATURE_MASK(ID_AA64ISAR2_EL1_APA3), ID_AA64ISAR2_EL1_APA3_PAuth) \
	)

#define PVM_ID_AA64ISAR1_ALLOW (\
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_DPB) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_JSCVT) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_FCMA) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_LRCPC) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_GPA) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_GPI) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_FRINTTS) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_SB) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_SPECRES) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_BF16) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_DGH) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_I8MM) \
	)

#define PVM_ID_AA64ISAR2_ALLOW (\
	ARM64_FEATURE_MASK(ID_AA64ISAR2_EL1_ATS1A) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR2_EL1_GPA3) | \
	ARM64_FEATURE_MASK(ID_AA64ISAR2_EL1_MOPS) \
	)


/* All HAFGRTR_EL2 bits are AMU */
#define HAFGRTR_AMU	__HAFGRTR_EL2_MASK

#define PVM_HAFGRTR_EL2_SET \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_AMU), PVM_ID_AA64PFR0_ALLOW) ? 0ULL : HAFGRTR_AMU)

#define PVM_HAFGRTR_EL2_CLR (0ULL)

/* No support for debug, trace, of PMU for protected VMs */
#define PVM_HDFGRTR_EL2_SET __HDFGRTR_EL2_MASK
#define PVM_HDFGRTR_EL2_CLR __HDFGRTR_EL2_nMASK

#define PVM_HDFGWTR_EL2_SET __HDFGWTR_EL2_MASK
#define PVM_HDFGWTR_EL2_CLR __HDFGWTR_EL2_nMASK

#define HFGxTR_RAS_IMP 	(\
			HFGxTR_EL2_ERXADDR_EL1 | \
			HFGxTR_EL2_ERXPFGF_EL1 | \
			HFGxTR_EL2_ERXMISCn_EL1 | \
			HFGxTR_EL2_ERXSTATUS_EL1 | \
			HFGxTR_EL2_ERXCTLR_EL1 | \
			HFGxTR_EL2_ERXFR_EL1 | \
			HFGxTR_EL2_ERRSELR_EL1 | \
			HFGxTR_EL2_ERRIDR_EL1 \
			)
#define HFGxTR_RAS_V1P1 (\
			HFGxTR_EL2_ERXPFGCDN_EL1 | \
			HFGxTR_EL2_ERXPFGCTL_EL1 \
			)
#define HFGxTR_GIC	HFGxTR_EL2_ICC_IGRPENn_EL1
#define HFGxTR_CSV2	(\
			HFGxTR_EL2_SCXTNUM_EL0 | \
			HFGxTR_EL2_SCXTNUM_EL1 \
			)
#define HFGxTR_LOR 	(\
			HFGxTR_EL2_LORSA_EL1 | \
			HFGxTR_EL2_LORN_EL1 | \
			HFGxTR_EL2_LORID_EL1 | \
			HFGxTR_EL2_LOREA_EL1 | \
			HFGxTR_EL2_LORC_EL1 \
			)
#define HFGxTR_PAUTH	(\
			HFGxTR_EL2_APIBKey | \
			HFGxTR_EL2_APIAKey | \
			HFGxTR_EL2_APGAKey | \
			HFGxTR_EL2_APDBKey | \
			HFGxTR_EL2_APDAKey \
			)
#define HFGxTR_nAIE	(\
			HFGxTR_EL2_nAMAIR2_EL1 | \
			HFGxTR_EL2_nMAIR2_EL1 \
			)
#define HFGxTR_nS2POE	HFGxTR_EL2_nS2POR_EL1
#define HFGxTR_nS1POE 	(\
			HFGxTR_EL2_nPOR_EL1 | \
			HFGxTR_EL2_nPOR_EL0 \
			)
#define HFGxTR_nS1PIE 	(\
			HFGxTR_EL2_nPIR_EL1 | \
			HFGxTR_EL2_nPIRE0_EL1 \
			)
#define HFGxTR_nTHE 	HFGxTR_EL2_nRCWMASK_EL1
#define HFGxTR_nSME 	(\
			HFGxTR_EL2_nTPIDR2_EL0 | \
			HFGxTR_EL2_nSMPRI_EL1 \
			)
#define HFGxTR_nGCS 	(\
			HFGxTR_EL2_nGCS_EL1 | \
			HFGxTR_EL2_nGCS_EL0 \
			)
#define HFGxTR_nLS64 	HFGxTR_EL2_nACCDATA_EL1

#define PVM_HFGXTR_EL2_SET \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_RAS), PVM_ID_AA64PFR0_RESTRICT_UNSIGNED) >= ID_AA64PFR0_EL1_RAS_IMP ? 0ULL : HFGxTR_RAS_IMP) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_RAS), PVM_ID_AA64PFR0_RESTRICT_UNSIGNED) >= ID_AA64PFR0_EL1_RAS_V1P1 ? 0ULL : HFGxTR_RAS_V1P1) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_GIC), PVM_ID_AA64PFR0_ALLOW) ? 0ULL : HFGxTR_GIC) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR0_EL1_CSV2), PVM_ID_AA64PFR0_ALLOW) ? 0ULL : HFGxTR_CSV2) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR1_EL1_LO), PVM_ID_AA64MMFR1_ALLOW) ? 0ULL : HFGxTR_LOR) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_APA), PVM_ID_AA64ISAR1_ALLOW) ? 0ULL : HFGxTR_PAUTH) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_API), PVM_ID_AA64ISAR1_ALLOW) ? 0ULL : HFGxTR_PAUTH) | \
	0

#define PVM_HFGXTR_EL2_CLR \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR3_EL1_AIE), PVM_ID_AA64MMFR3_ALLOW) ? 0ULL : HFGxTR_nAIE) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR3_EL1_S2POE), PVM_ID_AA64MMFR3_ALLOW) ? 0ULL : HFGxTR_nS2POE) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR3_EL1_S1POE), PVM_ID_AA64MMFR3_ALLOW) ? 0ULL : HFGxTR_nS1POE) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR3_EL1_S1PIE), PVM_ID_AA64MMFR3_ALLOW) ? 0ULL : HFGxTR_nS1PIE) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_THE), PVM_ID_AA64PFR1_ALLOW) ? 0ULL : HFGxTR_nTHE) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_SME), PVM_ID_AA64PFR1_ALLOW) ? 0ULL : HFGxTR_nSME) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_GCS), PVM_ID_AA64PFR1_ALLOW) ? 0ULL : HFGxTR_nGCS) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_LS64), PVM_ID_AA64ISAR1_ALLOW) ? 0ULL : HFGxTR_nLS64) | \
	0

#define PVM_HFGRTR_EL2_SET	PVM_HFGXTR_EL2_SET
#define PVM_HFGWTR_EL2_SET	PVM_HFGXTR_EL2_SET
#define PVM_HFGRTR_EL2_CLR	PVM_HFGXTR_EL2_CLR
#define PVM_HFGWTR_EL2_CLR	PVM_HFGXTR_EL2_CLR

#define HFGITR_SPECRES	(\
			HFGITR_EL2_CPPRCTX | \
			HFGITR_EL2_DVPRCTX | \
			HFGITR_EL2_CFPRCTX \
			)
#define HFGITR_TLBIOS	(\
			HFGITR_EL2_TLBIVAALE1OS | \
			HFGITR_EL2_TLBIVALE1OS | \
			HFGITR_EL2_TLBIVAAE1OS | \
			HFGITR_EL2_TLBIASIDE1OS | \
			HFGITR_EL2_TLBIVAE1OS | \
			HFGITR_EL2_TLBIVMALLE1OS \
			)
#define HFGITR_TLBIRANGE \
			(\
			HFGITR_TLBIOS | \
			HFGITR_EL2_TLBIRVAALE1 | \
			HFGITR_EL2_TLBIRVALE1 | \
			HFGITR_EL2_TLBIRVAAE1 | \
			HFGITR_EL2_TLBIRVAE1 | \
			HFGITR_EL2_TLBIRVAE1 | \
			HFGITR_EL2_TLBIRVAALE1IS | \
			HFGITR_EL2_TLBIRVALE1IS | \
			HFGITR_EL2_TLBIRVAAE1IS | \
			HFGITR_EL2_TLBIRVAE1IS | \
			HFGITR_EL2_TLBIVAALE1IS | \
			HFGITR_EL2_TLBIVALE1IS | \
			HFGITR_EL2_TLBIVAAE1IS | \
			HFGITR_EL2_TLBIASIDE1IS | \
			HFGITR_EL2_TLBIVAE1IS | \
			HFGITR_EL2_TLBIVMALLE1IS | \
			HFGITR_EL2_TLBIRVAALE1OS | \
			HFGITR_EL2_TLBIRVALE1OS | \
			HFGITR_EL2_TLBIRVAAE1OS | \
			HFGITR_EL2_TLBIRVAE1OS \
			)
#define HFGITR_TLB	HFGITR_TLBIRANGE
#define HFGITR_PAN2	(\
			HFGITR_EL2_ATS1E1WP | \
			HFGITR_EL2_ATS1E1RP | \
			HFGITR_EL2_ATS1E0W | \
			HFGITR_EL2_ATS1E0R | \
			HFGITR_EL2_ATS1E1W | \
			HFGITR_EL2_ATS1E1R \
			)
#define HFGITR_PAN	HFGITR_PAN2
#define HFGITR_DPB2	HFGITR_EL2_DCCVADP
#define HFGITR_DPB_IMP	HFGITR_EL2_DCCVAP
#define HFGITR_DPB	(HFGITR_DPB_IMP | HFGITR_DPB2)
#define HFGITR_nGCS	(\
			HFGITR_EL2_nGCSEPP | \
			HFGITR_EL2_nGCSSTR_EL1 | \
			HFGITR_EL2_nGCSPUSHM_EL1 \
			)
#define HFGITR_nBRBE	(\
			HFGITR_EL2_nBRBIALL | \
			HFGITR_EL2_nBRBINJ \
			)

#define PVM_HFGITR_EL2_SET \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR2_EL1_ATS1A), PVM_ID_AA64ISAR2_ALLOW) ? 0ULL : HFGITR_EL2_ATS1E1A) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_SPECRES), PVM_ID_AA64ISAR1_ALLOW) ? 0ULL : HFGITR_SPECRES) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR0_EL1_TLB), PVM_ID_AA64ISAR0_ALLOW) ? 0ULL : HFGITR_TLB) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR1_EL1_PAN), PVM_ID_AA64MMFR1_ALLOW) ? 0ULL : HFGITR_PAN) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_DPB), PVM_ID_AA64ISAR1_ALLOW) ? 0ULL : HFGITR_DPB) | \
	0

#define PVM_HFGITR_EL2_CLR \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_GCS), PVM_ID_AA64PFR1_ALLOW) ? 0ULL : HFGITR_nGCS) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64DFR0_EL1_BRBE), PVM_ID_AA64DFR0_ALLOW) ? 0ULL : HFGITR_nBRBE) | \
	0

#define HCRX_NMI		HCRX_EL2_TALLINT

#define HCRX_nPAuth_LR		HCRX_EL2_PACMEn
#define HCRX_nFPMR		HCRX_EL2_EnFPM
#define HCRX_nGCS		HCRX_EL2_GCSEn
#define HCRX_nSYSREG128		HCRX_EL2_EnIDCP128
#define HCRX_nADERR		HCRX_EL2_EnSDERR
#define HCRX_nDoubleFault2	HCRX_EL2_TMEA
#define HCRX_nANERR		HCRX_EL2_EnSNERR
#define HCRX_nD128		HCRX_EL2_D128En
#define HCRX_nTHE		HCRX_EL2_PTTWI
#define HCRX_nSCTLR2		HCRX_EL2_SCTLR2En
#define HCRX_nTCR2		HCRX_EL2_TCR2En
#define HCRX_nMOPS		(HCRX_EL2_MSCEn | HCRX_EL2_MCE2)
#define HCRX_nCMOW		HCRX_EL2_CMOW
#define HCRX_nNMI		(HCRX_EL2_VFNMI | HCRX_EL2_VINMI)
#define HCRX_SME		HCRX_EL2_SMPME
#define HCRX_nXS		(HCRX_EL2_FGTnXS | HCRX_EL2_FnXS)
#define HCRX_nLS64		(HCRX_EL2_EnASR| HCRX_EL2_EnALS | HCRX_EL2_EnAS0)

#define PVM_HCRX_EL2_SET \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_NMI), PVM_ID_AA64PFR1_ALLOW) ? 0ULL : HCRX_NMI) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_SME), PVM_ID_AA64PFR1_ALLOW) ? 0ULL : HCRX_SME) | \
	0

#define PVM_HCRX_EL2_CLR \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_APA), PVM_ID_AA64ISAR1_RESTRICT_UNSIGNED) < ID_AA64ISAR1_EL1_APA_PAuth_LR ? 0ULL : HCRX_nPAuth_LR) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_API), PVM_ID_AA64ISAR1_RESTRICT_UNSIGNED) < ID_AA64ISAR1_EL1_APA_PAuth_LR ? 0ULL : HCRX_nPAuth_LR) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_GCS), PVM_ID_AA64PFR1_ALLOW) ? 0ULL : HCRX_nGCS) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR2_EL1_SYSREG_128), PVM_ID_AA64ISAR2_ALLOW) ? 0ULL : HCRX_nSYSREG128) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR3_EL1_ADERR), PVM_ID_AA64MMFR3_ALLOW) ? 0ULL : HCRX_nADERR) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_DF2), PVM_ID_AA64PFR1_ALLOW) ? 0ULL : HCRX_nDoubleFault2) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR3_EL1_ANERR), PVM_ID_AA64MMFR3_ALLOW) ? 0ULL : HCRX_nANERR) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR0_EL1_PARANGE), PVM_ID_AA64MMFR0_ALLOW) ? 0ULL : HCRX_nD128) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_THE), PVM_ID_AA64PFR1_ALLOW) ? 0ULL : HCRX_nTHE) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR3_EL1_SCTLRX), PVM_ID_AA64MMFR3_ALLOW) ? 0ULL : HCRX_nSCTLR2) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR3_EL1_TCRX), PVM_ID_AA64MMFR3_ALLOW) ? 0ULL : HCRX_nTCR2) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR2_EL1_MOPS), PVM_ID_AA64ISAR2_ALLOW) ? 0ULL : HCRX_nMOPS) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64MMFR1_EL1_CMOW), PVM_ID_AA64MMFR1_ALLOW) ? 0ULL : HCRX_nCMOW) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64PFR1_EL1_NMI), PVM_ID_AA64PFR1_ALLOW) ? 0ULL : HCRX_nNMI) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_XS), PVM_ID_AA64ISAR1_ALLOW) ? 0ULL : HCRX_nXS) | \
	(FIELD_GET(ARM64_FEATURE_MASK(ID_AA64ISAR1_EL1_LS64), PVM_ID_AA64ISAR1_ALLOW) ? 0ULL : HCRX_nLS64) | \
	0

/*
 * Returns the maximum number of breakpoints supported for protected VMs.
 */
static inline int pkvm_get_max_brps(void)
{
	int num = FIELD_GET(ARM64_FEATURE_MASK(ID_AA64DFR0_EL1_BRPs),
			    PVM_ID_AA64DFR0_ALLOW);

	/*
	 * If breakpoints are supported, the maximum number is 1 + the field.
	 * Otherwise, return 0, which is not compliant with the architecture,
	 * but is reserved and is used here to indicate no debug support.
	 */
	return num ? num + 1 : 0;
}

/*
 * Returns the maximum number of watchpoints supported for protected VMs.
 */
static inline int pkvm_get_max_wrps(void)
{
	int num = FIELD_GET(ARM64_FEATURE_MASK(ID_AA64DFR0_EL1_WRPs),
			    PVM_ID_AA64DFR0_ALLOW);

	return num ? num + 1 : 0;
}

enum pkvm_moveable_reg_type {
	PKVM_MREG_MEMORY,
	PKVM_MREG_PROTECTED_RANGE,
};

struct pkvm_moveable_reg {
	phys_addr_t start;
	u64 size;
	enum pkvm_moveable_reg_type type;
};

#define PKVM_NR_MOVEABLE_REGS 512
extern struct pkvm_moveable_reg kvm_nvhe_sym(pkvm_moveable_regs)[];
extern unsigned int kvm_nvhe_sym(pkvm_moveable_regs_nr);

extern struct memblock_region kvm_nvhe_sym(hyp_memory)[];
extern unsigned int kvm_nvhe_sym(hyp_memblock_nr);

extern phys_addr_t kvm_nvhe_sym(pvmfw_base);
extern phys_addr_t kvm_nvhe_sym(pvmfw_size);

static inline unsigned long
hyp_vmemmap_memblock_size(struct memblock_region *reg, size_t vmemmap_entry_size)
{
	unsigned long nr_pages = reg->size >> PAGE_SHIFT;
	unsigned long start, end;

	start = (reg->base >> PAGE_SHIFT) * vmemmap_entry_size;
	end = start + nr_pages * vmemmap_entry_size;
	start = ALIGN_DOWN(start, PAGE_SIZE);
	end = ALIGN(end, PAGE_SIZE);

	return end - start;
}

static inline unsigned long hyp_vmemmap_pages(size_t vmemmap_entry_size)
{
	unsigned long res = 0, i;

	for (i = 0; i < kvm_nvhe_sym(hyp_memblock_nr); i++) {
		res += hyp_vmemmap_memblock_size(&kvm_nvhe_sym(hyp_memory)[i],
						 vmemmap_entry_size);
	}

	return res >> PAGE_SHIFT;
}

static inline unsigned long hyp_vm_table_pages(void)
{
	return PAGE_ALIGN(KVM_MAX_PVMS * sizeof(void *)) >> PAGE_SHIFT;
}

static inline unsigned long __hyp_pgtable_max_pages(unsigned long nr_pages)
{
	unsigned long total = 0, i;

	/* Provision the worst case scenario */
	for (i = 0; i < KVM_PGTABLE_MAX_LEVELS; i++) {
		nr_pages = DIV_ROUND_UP(nr_pages, PTRS_PER_PTE);
		total += nr_pages;
	}

	return total;
}

static inline unsigned long __hyp_pgtable_moveable_regs_pages(void)
{
	unsigned long res = 0, i;

	/* Cover all of moveable regions with page-granularity */
	for (i = 0; i < kvm_nvhe_sym(pkvm_moveable_regs_nr); i++) {
		struct pkvm_moveable_reg *reg = &kvm_nvhe_sym(pkvm_moveable_regs)[i];
		res += __hyp_pgtable_max_pages(reg->size >> PAGE_SHIFT);
	}

	return res;
}

static inline unsigned long hyp_s1_pgtable_pages(void)
{
	unsigned long res;

	res = __hyp_pgtable_moveable_regs_pages();

	/* Allow 1 GiB for private mappings */
	res += __hyp_pgtable_max_pages(SZ_1G >> PAGE_SHIFT);

	return res;
}

static inline unsigned long host_s2_pgtable_pages(void)
{
	unsigned long res;

	/*
	 * Include an extra 16 pages to safely upper-bound the worst case of
	 * concatenated pgds.
	 */
	res = __hyp_pgtable_moveable_regs_pages() + 16;

	/* Allow 1 GiB for non-moveable regions */
	res += __hyp_pgtable_max_pages(SZ_1G >> PAGE_SHIFT);

	return res;
}

#define KVM_FFA_MBOX_NR_PAGES	1

/*
 * Maximum number of consitutents allowed in a descriptor. This number is
 * arbitrary, see comment below on SG_MAX_SEGMENTS in hyp_ffa_proxy_pages().
 */
#define KVM_FFA_MAX_NR_CONSTITUENTS	4096

static inline unsigned long hyp_ffa_proxy_pages(void)
{
	size_t desc_max;

	/*
	 * SG_MAX_SEGMENTS is supposed to bound the number of elements in an
	 * sglist, which should match the number of consituents in the
	 * corresponding FFA descriptor. As such, the EL2 buffer needs to be
	 * large enough to hold a descriptor with SG_MAX_SEGMENTS consituents
	 * at least. But the kernel's DMA code doesn't enforce the limit, and
	 * it is sometimes abused, so let's allow larger descriptors and hope
	 * for the best.
	 */
	BUILD_BUG_ON(KVM_FFA_MAX_NR_CONSTITUENTS < SG_MAX_SEGMENTS);

	/*
	 * The hypervisor FFA proxy needs enough memory to buffer a fragmented
	 * descriptor returned from EL3 in response to a RETRIEVE_REQ call.
	 */
	desc_max = sizeof(struct ffa_mem_region) +
		   sizeof(struct ffa_mem_region_attributes) +
		   sizeof(struct ffa_composite_mem_region) +
		   KVM_FFA_MAX_NR_CONSTITUENTS * sizeof(struct ffa_mem_region_addr_range);

	/* Plus a page each for the hypervisor's RX and TX mailboxes. */
	return (2 * KVM_FFA_MBOX_NR_PAGES) + DIV_ROUND_UP(desc_max, PAGE_SIZE);
}

static inline size_t pkvm_host_fp_state_size(void)
{
	if (system_supports_sve())
		return size_add(sizeof(struct kvm_host_sve_state),
		       SVE_SIG_REGS_SIZE(sve_vq_from_vl(kvm_host_sve_max_vl)));
	else
		return sizeof(struct user_fpsimd_state);
}

int __pkvm_topup_hyp_alloc(unsigned long nr_pages);

#define kvm_call_refill_hyp_nvhe(f, ...)				\
({									\
	struct arm_smccc_res res;					\
	int __ret;							\
	do {								\
		__ret = -1; 						\
		arm_smccc_1_1_hvc(KVM_HOST_SMCCC_FUNC(f),		\
				  ##__VA_ARGS__, &res);			\
		if (WARN_ON(res.a0 != SMCCC_RET_SUCCESS))		\
			break;						\
									\
		__ret = res.a1;						\
		if (__ret == -ENOMEM && res.a3) {			\
			__ret = __pkvm_topup_hyp_alloc(res.a3);		\
		} else {						\
			break;						\
		}							\
	} while (!__ret);						\
	__ret;								\
})

int pkvm_call_hyp_nvhe_ppage(struct kvm_pinned_page *ppage,
			     int (*call_hyp_nvhe)(u64, u64, u8, void*),
			     void *args, bool unmap);
#endif	/* __ARM64_KVM_PKVM_H__ */
