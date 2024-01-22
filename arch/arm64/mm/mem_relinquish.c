/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022 Google LLC
 * Author: Keir Fraser <keirf@google.com>
 */

#include <linux/arm-smccc.h>
#include <linux/mem_relinquish.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/types.h>

#include <asm/hypervisor.h>

#ifndef ARM_SMCCC_KVM_FUNC_HYP_MEMINFO
#define ARM_SMCCC_KVM_FUNC_HYP_MEMINFO		2

#define ARM_SMCCC_VENDOR_HYP_KVM_HYP_MEMINFO_FUNC_ID			\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,				\
			   ARM_SMCCC_SMC_64,				\
			   ARM_SMCCC_OWNER_VENDOR_HYP,			\
			   ARM_SMCCC_KVM_FUNC_HYP_MEMINFO)
#endif	/* ARM_SMCCC_KVM_FUNC_HYP_MEMINFO */

#ifndef ARM_SMCCC_KVM_FUNC_MEM_RELINQUISH
#define ARM_SMCCC_KVM_FUNC_MEM_RELINQUISH	9

#define ARM_SMCCC_VENDOR_HYP_KVM_MEM_RELINQUISH_FUNC_ID			\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,				\
			   ARM_SMCCC_SMC_64,				\
			   ARM_SMCCC_OWNER_VENDOR_HYP,			\
			   ARM_SMCCC_KVM_FUNC_MEM_RELINQUISH)
#endif	/* ARM_SMCCC_KVM_FUNC_MEM_RELINQUISH */

static unsigned long memshare_granule_sz;

static void kvm_page_relinquish(struct page *page)
{
	phys_addr_t phys, end;
	u32 func_id = ARM_SMCCC_VENDOR_HYP_KVM_MEM_RELINQUISH_FUNC_ID;

	phys = page_to_phys(page);
	end = phys + PAGE_SIZE;

	while (phys < end) {
		struct arm_smccc_res res;

		arm_smccc_1_1_invoke(func_id, phys, 0, 0, &res);
		BUG_ON(res.a0 != SMCCC_RET_SUCCESS);

		phys += memshare_granule_sz;
	}
}

void kvm_init_memrelinquish_services(void)
{
	int i;
	struct arm_smccc_res res;
	const u32 funcs[] = {
		ARM_SMCCC_KVM_FUNC_HYP_MEMINFO,
		ARM_SMCCC_KVM_FUNC_MEM_RELINQUISH,
	};

	for (i = 0; i < ARRAY_SIZE(funcs); ++i) {
		if (!kvm_arm_hyp_service_available(funcs[i]))
			return;
	}

	arm_smccc_1_1_invoke(ARM_SMCCC_VENDOR_HYP_KVM_HYP_MEMINFO_FUNC_ID,
			     0, 0, 0, &res);
	if (res.a0 > PAGE_SIZE) /* Includes error codes */
		return;

	memshare_granule_sz = res.a0;

	if (memshare_granule_sz)
		hyp_ops.page_relinquish = kvm_page_relinquish;
}

void page_relinquish(struct page *page)
{
	if (hyp_ops.page_relinquish)
		hyp_ops.page_relinquish(page);
}
EXPORT_SYMBOL_GPL(page_relinquish);

void post_page_relinquish_tlb_inv(void)
{
	if (hyp_ops.post_page_relinquish_tlb_inv)
		hyp_ops.post_page_relinquish_tlb_inv();
}
EXPORT_SYMBOL_GPL(post_page_relinquish_tlb_inv);
