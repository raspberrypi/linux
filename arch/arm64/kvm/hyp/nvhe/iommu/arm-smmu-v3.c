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
#include <nvhe/mem_protect.h>
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

#define smmu_wait_event(_smmu, _cond)				\
({								\
	if ((_smmu)->features & ARM_SMMU_FEAT_SEV) {		\
		while (!(_cond))				\
			wfe();					\
	}							\
	smmu_wait(_cond);					\
})

static int smmu_write_cr0(struct hyp_arm_smmu_v3_device *smmu, u32 val)
{
	writel_relaxed(val, smmu->base + ARM_SMMU_CR0);
	return smmu_wait(readl_relaxed(smmu->base + ARM_SMMU_CR0ACK) == val);
}

#define Q_WRAP(smmu, reg)	((reg) & (1 << (smmu)->cmdq_log2size))
#define Q_IDX(smmu, reg)	((reg) & ((1 << (smmu)->cmdq_log2size) - 1))

static bool smmu_cmdq_full(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 cons = readl_relaxed(smmu->base + ARM_SMMU_CMDQ_CONS);

	return Q_IDX(smmu, smmu->cmdq_prod) == Q_IDX(smmu, cons) &&
	       Q_WRAP(smmu, smmu->cmdq_prod) != Q_WRAP(smmu, cons);
}

static bool smmu_cmdq_empty(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 cons = readl_relaxed(smmu->base + ARM_SMMU_CMDQ_CONS);

	return Q_IDX(smmu, smmu->cmdq_prod) == Q_IDX(smmu, cons) &&
	       Q_WRAP(smmu, smmu->cmdq_prod) == Q_WRAP(smmu, cons);
}

static int smmu_add_cmd(struct hyp_arm_smmu_v3_device *smmu,
			struct arm_smmu_cmdq_ent *ent)
{
	int i;
	int ret;
	u64 cmd[CMDQ_ENT_DWORDS] = {};
	int idx = Q_IDX(smmu, smmu->cmdq_prod);
	u64 *slot = smmu->cmdq_base + idx * CMDQ_ENT_DWORDS;

	ret = smmu_wait_event(smmu, !smmu_cmdq_full(smmu));
	if (ret)
		return ret;

	cmd[0] |= FIELD_PREP(CMDQ_0_OP, ent->opcode);

	switch (ent->opcode) {
	case CMDQ_OP_CFGI_ALL:
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_RANGE, 31);
		break;
	case CMDQ_OP_CFGI_STE:
		cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SID, ent->cfgi.sid);
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_LEAF, ent->cfgi.leaf);
		break;
	case CMDQ_OP_TLBI_NSNH_ALL:
		break;
	case CMDQ_OP_TLBI_S12_VMALL:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);
		break;
	case CMDQ_OP_TLBI_S2_IPA:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_NUM, ent->tlbi.num);
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_SCALE, ent->tlbi.scale);
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_LEAF, ent->tlbi.leaf);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TTL, ent->tlbi.ttl);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TG, ent->tlbi.tg);
		cmd[1] |= ent->tlbi.addr & CMDQ_TLBI_1_IPA_MASK;
		break;
	case CMDQ_OP_CMD_SYNC:
		cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_SEV);
		break;
	default:
		return -EINVAL;
	}

	for (i = 0; i < CMDQ_ENT_DWORDS; i++)
		slot[i] = cpu_to_le64(cmd[i]);

	smmu->cmdq_prod++;
	writel(Q_IDX(smmu, smmu->cmdq_prod) | Q_WRAP(smmu, smmu->cmdq_prod),
	       smmu->base + ARM_SMMU_CMDQ_PROD);
	return 0;
}

static int smmu_sync_cmd(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CMD_SYNC,
	};

	ret = smmu_add_cmd(smmu, &cmd);
	if (ret)
		return ret;

	return smmu_wait_event(smmu, smmu_cmdq_empty(smmu));
}

static int smmu_send_cmd(struct hyp_arm_smmu_v3_device *smmu,
			 struct arm_smmu_cmdq_ent *cmd)
{
	int ret = smmu_add_cmd(smmu, cmd);

	if (ret)
		return ret;

	return smmu_sync_cmd(smmu);
}

__maybe_unused
static int smmu_sync_ste(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CFGI_STE,
		.cfgi.sid = sid,
		.cfgi.leaf = true,
	};

	return smmu_send_cmd(smmu, &cmd);
}

static int smmu_alloc_l2_strtab(struct hyp_arm_smmu_v3_device *smmu, u32 idx)
{
	void *table;
	u64 l2ptr, span;

	/* Leaf tables must be page-sized */
	if (smmu->strtab_split + ilog2(STRTAB_STE_DWORDS) + 3 != PAGE_SHIFT)
		return -EINVAL;

	span = smmu->strtab_split + 1;
	if (WARN_ON(span < 1 || span > 11))
		return -EINVAL;

	table = kvm_iommu_donate_page();
	if (!table)
		return -ENOMEM;

	l2ptr = hyp_virt_to_phys(table);
	if (l2ptr & (~STRTAB_L1_DESC_L2PTR_MASK | ~PAGE_MASK))
		return -EINVAL;

	/* Ensure the empty stream table is visible before the descriptor write */
	wmb();

	WRITE_ONCE(smmu->strtab_base[idx], l2ptr | span);

	return 0;
}

__maybe_unused
static u64 *smmu_get_ste_ptr(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	u32 idx;
	int ret;
	u64 l1std, span, *base;

	if (sid >= smmu->strtab_num_entries)
		return NULL;
	sid = array_index_nospec(sid, smmu->strtab_num_entries);

	if (!smmu->strtab_split)
		return smmu->strtab_base + sid * STRTAB_STE_DWORDS;

	idx = sid >> smmu->strtab_split;
	l1std = smmu->strtab_base[idx];
	if (!l1std) {
		ret = smmu_alloc_l2_strtab(smmu, idx);
		if (ret)
			return NULL;
		l1std = smmu->strtab_base[idx];
		if (WARN_ON(!l1std))
			return NULL;
	}

	span = l1std & STRTAB_L1_DESC_SPAN;
	idx = sid & ((1 << smmu->strtab_split) - 1);
	if (!span || idx >= (1 << (span - 1)))
		return NULL;

	base = hyp_phys_to_virt(l1std & STRTAB_L1_DESC_L2PTR_MASK);
	return base + idx * STRTAB_STE_DWORDS;
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

/* Transfer ownership of structures from host to hyp */
static void *smmu_take_pages(u64 phys, size_t size)
{
	WARN_ON(!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size));
	if (__pkvm_host_donate_hyp(phys >> PAGE_SHIFT, size >> PAGE_SHIFT))
		return NULL;

	return hyp_phys_to_virt(phys);
}

static int smmu_init_cmdq(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 cmdq_base;
	size_t cmdq_nr_entries, cmdq_size;
	int ret;
	enum kvm_pgtable_prot prot = PAGE_HYP;

	cmdq_base = readq_relaxed(smmu->base + ARM_SMMU_CMDQ_BASE);
	if (cmdq_base & ~(Q_BASE_RWA | Q_BASE_ADDR_MASK | Q_BASE_LOG2SIZE))
		return -EINVAL;

	smmu->cmdq_log2size = cmdq_base & Q_BASE_LOG2SIZE;
	cmdq_nr_entries = 1 << smmu->cmdq_log2size;
	cmdq_size = cmdq_nr_entries * CMDQ_ENT_DWORDS * 8;

	cmdq_base &= Q_BASE_ADDR_MASK;

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		prot |= KVM_PGTABLE_PROT_NC;

	ret = ___pkvm_host_donate_hyp_prot(cmdq_base >> PAGE_SHIFT,
					   PAGE_ALIGN(cmdq_size) >> PAGE_SHIFT,
					   false, prot);
	if (ret)
		return ret;

	smmu->cmdq_base = hyp_phys_to_virt(cmdq_base);

	memset(smmu->cmdq_base, 0, cmdq_size);
	writel_relaxed(0, smmu->base + ARM_SMMU_CMDQ_PROD);
	writel_relaxed(0, smmu->base + ARM_SMMU_CMDQ_CONS);

	return 0;
}

static int smmu_init_strtab(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 strtab_base;
	size_t strtab_size;
	u32 strtab_cfg, fmt;
	int split, log2size;

	strtab_base = readq_relaxed(smmu->base + ARM_SMMU_STRTAB_BASE);
	if (strtab_base & ~(STRTAB_BASE_ADDR_MASK | STRTAB_BASE_RA))
		return -EINVAL;

	strtab_cfg = readl_relaxed(smmu->base + ARM_SMMU_STRTAB_BASE_CFG);
	if (strtab_cfg & ~(STRTAB_BASE_CFG_FMT | STRTAB_BASE_CFG_SPLIT |
			   STRTAB_BASE_CFG_LOG2SIZE))
		return -EINVAL;

	fmt = FIELD_GET(STRTAB_BASE_CFG_FMT, strtab_cfg);
	split = FIELD_GET(STRTAB_BASE_CFG_SPLIT, strtab_cfg);
	log2size = FIELD_GET(STRTAB_BASE_CFG_LOG2SIZE, strtab_cfg);

	smmu->strtab_split = split;
	smmu->strtab_num_entries = 1 << log2size;

	switch (fmt) {
	case STRTAB_BASE_CFG_FMT_LINEAR:
		if (split)
			return -EINVAL;
		smmu->strtab_num_l1_entries = smmu->strtab_num_entries;
		strtab_size = smmu->strtab_num_l1_entries *
			      STRTAB_STE_DWORDS * 8;
		break;
	case STRTAB_BASE_CFG_FMT_2LVL:
		if (split != 6 && split != 8 && split != 10)
			return -EINVAL;
		smmu->strtab_num_l1_entries = 1 << max(0, log2size - split);
		strtab_size = smmu->strtab_num_l1_entries *
			      STRTAB_L1_DESC_DWORDS * 8;
		break;
	default:
		return -EINVAL;
	}

	strtab_base &= STRTAB_BASE_ADDR_MASK;
	smmu->strtab_base = smmu_take_pages(strtab_base, strtab_size);
	if (!smmu->strtab_base)
		return -EINVAL;

	/* Disable all STEs */
	memset(smmu->strtab_base, 0, strtab_size);
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

	ret = smmu_init_cmdq(smmu);
	if (ret)
		return ret;

	ret = smmu_init_strtab(smmu);
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
