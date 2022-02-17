// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM hyp driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/arm-smmu-v3-regs.h>
#include <asm/kvm_hyp.h>
#include <kvm/arm_smmu_v3.h>
#include <nvhe/iommu.h>
#include <nvhe/mm.h>
#include <nvhe/pkvm.h>

#define ARM_SMMU_POLL_TIMEOUT_US	100000 /* 100ms arbitrary timeout */

size_t __ro_after_init kvm_hyp_arm_smmu_v3_count;
struct hyp_arm_smmu_v3_device *kvm_hyp_arm_smmu_v3_smmus;

#define for_each_smmu(smmu) \
	for ((smmu) = kvm_hyp_arm_smmu_v3_smmus; \
	     (smmu) != &kvm_hyp_arm_smmu_v3_smmus[kvm_hyp_arm_smmu_v3_count]; \
	     (smmu)++)

/*
 * Wait until @cond is true.
 * Return 0 on success, or -ETIMEDOUT
 */
#define smmu_wait(_cond)					\
({								\
	int __i = 0;						\
	int __ret = 0;						\
								\
	while (!(_cond)) {					\
		if (++__i > ARM_SMMU_POLL_TIMEOUT_US) {		\
			__ret = -ETIMEDOUT;			\
			break;					\
		}						\
		pkvm_udelay(1);					\
	}							\
	__ret;							\
})

static int smmu_write_cr0(struct hyp_arm_smmu_v3_device *smmu, u32 val)
{
	writel_relaxed(val, smmu->base + ARM_SMMU_CR0);
	return smmu_wait(readl_relaxed(smmu->base + ARM_SMMU_CR0ACK) == val);
}

/* Transfer ownership of structures from host to hyp */
static void *smmu_take_pages(u64 phys, size_t size)
{
	WARN_ON(!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size));
	if (__pkvm_host_donate_hyp(phys >> PAGE_SHIFT, size >> PAGE_SHIFT))
		return NULL;

	return hyp_phys_to_virt(phys);
}

static int smmu_init_registers(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 val, old;
	int ret;

	if (!(readl_relaxed(smmu->base + ARM_SMMU_GBPA) & GBPA_ABORT))
		return -EINVAL;

	/* Initialize all RW registers that will be read by the SMMU */
	ret = smmu_write_cr0(smmu, 0);
	if (ret)
		return ret;

	val = FIELD_PREP(CR1_TABLE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_TABLE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_TABLE_IC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_QUEUE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_IC, CR1_CACHE_WB);
	writel_relaxed(val, smmu->base + ARM_SMMU_CR1);
	writel_relaxed(CR2_PTM, smmu->base + ARM_SMMU_CR2);
	writel_relaxed(0, smmu->base + ARM_SMMU_IRQ_CTRL);

	val = readl_relaxed(smmu->base + ARM_SMMU_GERROR);
	old = readl_relaxed(smmu->base + ARM_SMMU_GERRORN);
	/* Service Failure Mode is fatal */
	if ((val ^ old) & GERROR_SFM_ERR)
		return -EIO;
	/* Clear pending errors */
	writel_relaxed(val, smmu->base + ARM_SMMU_GERRORN);

	return 0;
}

static int smmu_init_device(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;

	if (!PAGE_ALIGNED(smmu->mmio_addr | smmu->mmio_size))
		return -EINVAL;

	ret = ___pkvm_host_donate_hyp(smmu->mmio_addr >> PAGE_SHIFT,
				      smmu->mmio_size >> PAGE_SHIFT,
				      /* accept_mmio */ true);
	if (ret)
		return ret;

	smmu->base = hyp_phys_to_virt(smmu->mmio_addr);

	ret = smmu_init_registers(smmu);
	if (ret)
		return ret;

	return 0;
}

static int smmu_init(void)
{
	int ret;
	struct hyp_arm_smmu_v3_device *smmu;
	int smmu_arr_size = PAGE_ALIGN(sizeof(*kvm_hyp_arm_smmu_v3_smmus) * kvm_hyp_arm_smmu_v3_count);

	kvm_hyp_arm_smmu_v3_smmus = kern_hyp_va(kvm_hyp_arm_smmu_v3_smmus);

	WARN_ON(!smmu_take_pages(hyp_virt_to_phys(kvm_hyp_arm_smmu_v3_smmus), smmu_arr_size));

	for_each_smmu(smmu) {
		ret = smmu_init_device(smmu);
		if (ret)
			return ret;
	}

	return 0;
}

static struct kvm_iommu_ops smmu_ops = {
	.init				= smmu_init,
};

int kvm_arm_smmu_v3_register(void)
{
	kvm_iommu_ops = smmu_ops;
	return 0;
}
