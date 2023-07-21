/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Implementation of the memory encryption/decryption API.
 *
 * Amusingly, no crypto is actually performed. Rather, we call into the
 * hypervisor component of KVM to expose pages selectively to the host
 * for virtio "DMA" operations. In other words, "encrypted" pages are
 * not accessible to the host, whereas "decrypted" pages are.
 *
 * Author: Will Deacon <will@kernel.org>
 */
#include <linux/arm-smccc.h>
#include <linux/mem_encrypt.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/set_memory.h>
#include <linux/types.h>

#include <asm/hypervisor.h>

#ifndef ARM_SMCCC_KVM_FUNC_HYP_MEMINFO
#define ARM_SMCCC_KVM_FUNC_HYP_MEMINFO	2

#define ARM_SMCCC_VENDOR_HYP_KVM_HYP_MEMINFO_FUNC_ID			\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,				\
			   ARM_SMCCC_SMC_64,				\
			   ARM_SMCCC_OWNER_VENDOR_HYP,			\
			   ARM_SMCCC_KVM_FUNC_HYP_MEMINFO)
#endif	/* ARM_SMCCC_KVM_FUNC_HYP_MEMINFO */

#ifndef ARM_SMCCC_KVM_FUNC_MEM_SHARE
#define ARM_SMCCC_KVM_FUNC_MEM_SHARE	3

#define ARM_SMCCC_VENDOR_HYP_KVM_MEM_SHARE_FUNC_ID			\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,				\
			   ARM_SMCCC_SMC_64,				\
			   ARM_SMCCC_OWNER_VENDOR_HYP,			\
			   ARM_SMCCC_KVM_FUNC_MEM_SHARE)
#endif	/* ARM_SMCCC_KVM_FUNC_MEM_SHARE */

#ifndef ARM_SMCCC_KVM_FUNC_MEM_UNSHARE
#define ARM_SMCCC_KVM_FUNC_MEM_UNSHARE	4

#define ARM_SMCCC_VENDOR_HYP_KVM_MEM_UNSHARE_FUNC_ID			\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,				\
			   ARM_SMCCC_SMC_64,				\
			   ARM_SMCCC_OWNER_VENDOR_HYP,			\
			   ARM_SMCCC_KVM_FUNC_MEM_UNSHARE)
#endif	/* ARM_SMCCC_KVM_FUNC_MEM_UNSHARE */

static unsigned long memshare_granule_sz;
static bool memshare_has_range;

bool mem_encrypt_active(void)
{
	return memshare_granule_sz;
}
EXPORT_SYMBOL(mem_encrypt_active);

void kvm_init_memshare_services(void)
{
	int i;
	struct arm_smccc_res res;
	const u32 funcs[] = {
		ARM_SMCCC_KVM_FUNC_HYP_MEMINFO,
		ARM_SMCCC_KVM_FUNC_MEM_SHARE,
		ARM_SMCCC_KVM_FUNC_MEM_UNSHARE,
	};
	long ret;

	for (i = 0; i < ARRAY_SIZE(funcs); ++i) {
		if (!kvm_arm_hyp_service_available(funcs[i]))
			return;
	}

	arm_smccc_1_1_invoke(ARM_SMCCC_VENDOR_HYP_KVM_HYP_MEMINFO_FUNC_ID,
			     0, 0, 0, &res);
	ret = (long)res.a0;
	if (ret < 0)
		return;

	memshare_has_range = res.a1 & KVM_FUNC_HAS_RANGE;
	memshare_granule_sz = ret;
}

static int __invoke_memshare(unsigned long addr, int nr_granules, int func_id,
			     u64 *nr_xcrypted)
{
	u64 nr_granules_arg = memshare_has_range ? nr_granules : 0;
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(func_id, virt_to_phys((void *)addr),
			     nr_granules_arg, 0, &res);
	if (WARN_ON(res.a0 != SMCCC_RET_SUCCESS))
		return -EPERM;

	*nr_xcrypted = memshare_has_range ? res.a1 : 1;

	return 0;
}

static int set_memory_xcrypted(u32 func_id, unsigned long start, int numpages)
{
	int nr_granules;

	if (!memshare_granule_sz)
		return 0;

	if (WARN_ON(!PAGE_ALIGNED(start)))
		return -EINVAL;

	/* Prevent over-sharing when memshare_granule_sz > PAGE_SIZE */
	if (!IS_ALIGNED(start, memshare_granule_sz) ||
	    (PAGE_SIZE * numpages) % memshare_granule_sz)
		return -ERANGE;

	nr_granules = (numpages * PAGE_SIZE) / memshare_granule_sz;

	while (nr_granules > 0) {
		u64 nr_xcrypted;
		int ret;

		ret = __invoke_memshare(start, nr_granules, func_id, &nr_xcrypted);
		if (ret)
			return ret;

		WARN_ON(nr_xcrypted > nr_granules);

		nr_granules -= nr_xcrypted;
		start += nr_xcrypted * memshare_granule_sz;
	}

	return 0;
}

int set_memory_encrypted(unsigned long addr, int numpages)
{
	return set_memory_xcrypted(ARM_SMCCC_VENDOR_HYP_KVM_MEM_UNSHARE_FUNC_ID,
				   addr, numpages);
}
EXPORT_SYMBOL_GPL(set_memory_encrypted);

int set_memory_decrypted(unsigned long addr, int numpages)
{
	return set_memory_xcrypted(ARM_SMCCC_VENDOR_HYP_KVM_MEM_SHARE_FUNC_ID,
				   addr, numpages);
}
EXPORT_SYMBOL_GPL(set_memory_decrypted);
