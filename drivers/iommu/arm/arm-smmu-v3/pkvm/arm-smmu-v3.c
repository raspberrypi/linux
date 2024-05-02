// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM hyp driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include "arm_smmu_v3.h"

#include <asm/arm-smmu-v3-regs.h>
#include <asm/kvm_hyp.h>
#include <nvhe/iommu.h>
#include <nvhe/alloc.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>
#include <nvhe/pkvm.h>
#include <nvhe/rwlock.h>
#include <nvhe/trap_handler.h>


#include "arm-smmu-v3-module.h"

#ifdef MODULE
void *memset(void *dst, int c, size_t count)
{
	return CALL_FROM_OPS(memset, dst, c, count);
}

#ifdef CONFIG_LIST_HARDENED
bool __list_add_valid_or_report(struct list_head *new,
				struct list_head *prev,
				struct list_head *next)
{
	return CALL_FROM_OPS(list_add_valid_or_report, new, prev, next);
}

bool __list_del_entry_valid_or_report(struct list_head *entry)
{
	return CALL_FROM_OPS(list_del_entry_valid_or_report, entry);
}
#endif

const struct pkvm_module_ops		*mod_ops;
#endif

#define ARM_SMMU_POLL_TIMEOUT_US	100000 /* 100ms arbitrary timeout */

size_t __ro_after_init kvm_hyp_arm_smmu_v3_count;
struct hyp_arm_smmu_v3_device *kvm_hyp_arm_smmu_v3_smmus;

struct domain_iommu_node {
	struct kvm_hyp_iommu *iommu;
	struct list_head list;
	unsigned long ref;
};

struct hyp_arm_smmu_v3_domain {
	struct kvm_hyp_iommu_domain     *domain;
	struct list_head		iommu_list;
	u32				type;
	hyp_rwlock_t			lock; /* Protects iommu_list. */
	hyp_spinlock_t			pgt_lock; /* protects page table. */
	struct io_pgtable		*pgtable;
};

struct kvm_iommu_walk_data {
	struct kvm_iommu_paddr_cache *cache;
	struct iommu_iotlb_gather *iotlb_gather;
	void *cookie;
};

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

/* Request non-device memory */
static void *smmu_alloc(size_t size)
{
	void *p;
	struct kvm_hyp_req req;

	p = hyp_alloc(size);
	/* We can't handle any other errors. */
	if (!p) {
		BUG_ON(hyp_alloc_errno() != -ENOMEM);
		req.type = KVM_HYP_REQ_TYPE_MEM;
		req.mem.dest = REQ_MEM_DEST_HYP_ALLOC;
		req.mem.nr_pages = hyp_alloc_missing_donations();
		req.mem.sz_alloc = PAGE_SIZE;
		kvm_iommu_request(&req);
	}

	return p;
}

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

	if (smmu->iommu.power_is_off)
		return -EPIPE;

	ret = smmu_wait_event(smmu, !smmu_cmdq_full(smmu));
	if (ret)
		return ret;

	cmd[0] |= FIELD_PREP(CMDQ_0_OP, ent->opcode);

	switch (ent->opcode) {
	case CMDQ_OP_CFGI_ALL:
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_RANGE, 31);
		break;
	case CMDQ_OP_CFGI_CD:
		cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SSID, ent->cfgi.ssid);
		fallthrough;
	case CMDQ_OP_CFGI_STE:
		cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SID, ent->cfgi.sid);
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_LEAF, ent->cfgi.leaf);
		break;
	case CMDQ_OP_TLBI_NH_VA:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_NUM, ent->tlbi.num);
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_SCALE, ent->tlbi.scale);
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_ASID, ent->tlbi.asid);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_LEAF, ent->tlbi.leaf);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TTL, ent->tlbi.ttl);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TG, ent->tlbi.tg);
		cmd[1] |= ent->tlbi.addr & CMDQ_TLBI_1_VA_MASK;
		break;
	case CMDQ_OP_TLBI_NSNH_ALL:
		break;
	case CMDQ_OP_TLBI_NH_ASID:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_ASID, ent->tlbi.asid);
		fallthrough;
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

static int smmu_sync_ste(struct hyp_arm_smmu_v3_device *smmu, u64 *step, u32 sid)
{
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CFGI_STE,
		.cfgi.sid = sid,
		.cfgi.leaf = true,
	};

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		kvm_flush_dcache_to_poc(step, STRTAB_STE_DWORDS << 3);

	if (smmu->iommu.power_is_off && smmu->caches_clean_on_power_on)
		return 0;

	return smmu_send_cmd(smmu, &cmd);
}

static int smmu_sync_cd(struct hyp_arm_smmu_v3_device *smmu, u64 *cd, u32 sid, u32 ssid)
{
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CFGI_CD,
		.cfgi.sid	= sid,
		.cfgi.ssid	= ssid,
		.cfgi.leaf = true,
	};

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		kvm_flush_dcache_to_poc(cd, CTXDESC_CD_DWORDS << 3);

	if (smmu->iommu.power_is_off && smmu->caches_clean_on_power_on)
		return 0;

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

static u64 *smmu_get_cd_ptr(u64 *cdtab, u32 ssid)
{
	/* Assume linear for now. */
	return cdtab + ssid * CTXDESC_CD_DWORDS;
}

static u64 *smmu_alloc_cd(u32 pasid_bits)
{
	u64 *cd_table;
	u32 requested_order  = get_order((1 << pasid_bits) * (CTXDESC_CD_DWORDS << 3));

	/* We support max of 64K linear tables only, this should be enough for 128 pasids */
	BUG_ON(requested_order > 4);

	cd_table = kvm_iommu_donate_pages(requested_order, true);
	if (!cd_table)
		return NULL;
	return (u64 *)hyp_virt_to_phys(cd_table);
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

static int smmu_reset_device(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	struct arm_smmu_cmdq_ent cfgi_cmd = {
		.opcode = CMDQ_OP_CFGI_ALL,
	};
	struct arm_smmu_cmdq_ent tlbi_cmd = {
		.opcode = CMDQ_OP_TLBI_NSNH_ALL,
	};

	/* Invalidate all cached configs and TLBs */
	ret = smmu_write_cr0(smmu, CR0_CMDQEN);
	if (ret)
		return ret;

	ret = smmu_add_cmd(smmu, &cfgi_cmd);
	if (ret)
		goto err_disable_cmdq;

	ret = smmu_add_cmd(smmu, &tlbi_cmd);
	if (ret)
		goto err_disable_cmdq;

	ret = smmu_sync_cmd(smmu);
	if (ret)
		goto err_disable_cmdq;

	/* Enable translation */
	return smmu_write_cr0(smmu, CR0_SMMUEN | CR0_CMDQEN | CR0_ATSCHK | CR0_EVTQEN);

err_disable_cmdq:
	return smmu_write_cr0(smmu, 0);
}

static struct hyp_arm_smmu_v3_device *to_smmu(struct kvm_hyp_iommu *iommu)
{
	return container_of(iommu, struct hyp_arm_smmu_v3_device, iommu);
}

static void smmu_tlb_flush_all(void *cookie)
{
	struct kvm_hyp_iommu_domain *domain = cookie;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	struct hyp_arm_smmu_v3_device *smmu;
	struct domain_iommu_node *iommu_node;
	struct arm_smmu_cmdq_ent cmd;

	if (smmu_domain->pgtable->cfg.fmt == ARM_64_LPAE_S2) {
		cmd.opcode = CMDQ_OP_TLBI_S12_VMALL;
		cmd.tlbi.vmid = domain->domain_id;
	} else {
		cmd.opcode = CMDQ_OP_TLBI_NH_ASID;
		cmd.tlbi.asid = domain->domain_id;
		/* Domain ID is unique across all VMs. */
		cmd.tlbi.vmid = 0;
	}

	hyp_read_lock(&smmu_domain->lock);
	list_for_each_entry(iommu_node, &smmu_domain->iommu_list, list) {
		smmu = to_smmu(iommu_node->iommu);
		hyp_spin_lock(&smmu->iommu.lock);
		if (smmu->iommu.power_is_off && smmu->caches_clean_on_power_on) {
			hyp_spin_unlock(&smmu->iommu.lock);
			continue;
		}
		WARN_ON(smmu_send_cmd(smmu, &cmd));
		hyp_spin_unlock(&smmu->iommu.lock);
	}
	hyp_read_unlock(&smmu_domain->lock);
}

static int smmu_tlb_inv_range_smmu(struct hyp_arm_smmu_v3_device *smmu,
				   struct kvm_hyp_iommu_domain *domain,
				   struct arm_smmu_cmdq_ent *cmd,
				   unsigned long iova, size_t size, size_t granule)
{
	int ret = 0;
	unsigned long end = iova + size, num_pages = 0, tg = 0;
	size_t inv_range = granule;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;

	hyp_spin_lock(&smmu->iommu.lock);
	if (smmu->iommu.power_is_off && smmu->caches_clean_on_power_on)
		goto out_ret;

	/* Almost copy-paste from the kernel dirver. */
	if (smmu->features & ARM_SMMU_FEAT_RANGE_INV) {
		/* Get the leaf page size */
		tg = __ffs(smmu_domain->pgtable->cfg.pgsize_bitmap);

		num_pages = size >> tg;

		/* Convert page size of 12,14,16 (log2) to 1,2,3 */
		cmd->tlbi.tg = (tg - 10) / 2;

		/*
		 * Determine what level the granule is at. For non-leaf, both
		 * io-pgtable and SVA pass a nominal last-level granule because
		 * they don't know what level(s) actually apply, so ignore that
		 * and leave TTL=0. However for various errata reasons we still
		 * want to use a range command, so avoid the SVA corner case
		 * where both scale and num could be 0 as well.
		 */
		if (cmd->tlbi.leaf)
			cmd->tlbi.ttl = 4 - ((ilog2(granule) - 3) / (tg - 3));
		else if ((num_pages & CMDQ_TLBI_RANGE_NUM_MAX) == 1)
			num_pages++;
	}

	while (iova < end) {
		if (smmu->features & ARM_SMMU_FEAT_RANGE_INV) {
			/*
			 * On each iteration of the loop, the range is 5 bits
			 * worth of the aligned size remaining.
			 * The range in pages is:
			 *
			 * range = (num_pages & (0x1f << __ffs(num_pages)))
			 */
			unsigned long scale, num;

			/* Determine the power of 2 multiple number of pages */
			scale = __ffs(num_pages);
			cmd->tlbi.scale = scale;

			/* Determine how many chunks of 2^scale size we have */
			num = (num_pages >> scale) & CMDQ_TLBI_RANGE_NUM_MAX;
			cmd->tlbi.num = num - 1;

			/* range is num * 2^scale * pgsize */
			inv_range = num << (scale + tg);

			/* Clear out the lower order bits for the next iteration */
			num_pages -= num << scale;
		}
		cmd->tlbi.addr = iova;
		WARN_ON(smmu_add_cmd(smmu, cmd));
		BUG_ON(iova + inv_range < iova);
		iova += inv_range;
	}

	ret = smmu_sync_cmd(smmu);
out_ret:
	hyp_spin_unlock(&smmu->iommu.lock);
	return ret;
}

static void smmu_tlb_inv_range(struct kvm_hyp_iommu_domain *domain,
			       unsigned long iova, size_t size, size_t granule,
			       bool leaf)
{
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	struct hyp_arm_smmu_v3_device *smmu;
	struct domain_iommu_node *iommu_node;
	unsigned long end = iova + size;
	struct arm_smmu_cmdq_ent cmd;

	cmd.tlbi.leaf = leaf;
	if (smmu_domain->pgtable->cfg.fmt == ARM_64_LPAE_S2) {
		cmd.opcode = CMDQ_OP_TLBI_S2_IPA;
		cmd.tlbi.vmid = domain->domain_id;
	} else {
		cmd.opcode = CMDQ_OP_TLBI_NH_VA;
		cmd.tlbi.asid = domain->domain_id;
		cmd.tlbi.vmid = 0;
	}
	/*
	 * There are no mappings at high addresses since we don't use TTB1, so
	 * no overflow possible.
	 */
	BUG_ON(end < iova);

	hyp_read_lock(&smmu_domain->lock);
	list_for_each_entry(iommu_node, &smmu_domain->iommu_list, list) {
		smmu = to_smmu(iommu_node->iommu);
		WARN_ON(smmu_tlb_inv_range_smmu(smmu, domain, &cmd, iova, size, granule));
	}
	hyp_read_unlock(&smmu_domain->lock);
}

static void smmu_tlb_flush_walk(unsigned long iova, size_t size,
				size_t granule, void *cookie)
{
	smmu_tlb_inv_range(cookie, iova, size, granule, false);
}

static void smmu_tlb_add_page(struct iommu_iotlb_gather *gather,
			      unsigned long iova, size_t granule,
			      void *cookie)
{
	if (gather)
		kvm_iommu_iotlb_gather_add_page(cookie, gather, iova, granule);
	else
		smmu_tlb_inv_range(cookie, iova, granule, granule, true);
}

static void smmu_iotlb_sync(struct kvm_hyp_iommu_domain *domain,
			    struct iommu_iotlb_gather *gather)
{
	size_t size;

	if (!gather->pgsize)
		return;
	size = gather->end - gather->start + 1;
	smmu_tlb_inv_range(domain, gather->start, size,  gather->pgsize, true);
}

static const struct iommu_flush_ops smmu_tlb_ops = {
	.tlb_flush_all	= smmu_tlb_flush_all,
	.tlb_flush_walk = smmu_tlb_flush_walk,
	.tlb_add_page	= smmu_tlb_add_page,
};

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
	smmu->pgtable_cfg_s1.tlb = &smmu_tlb_ops;
	smmu->pgtable_cfg_s2.tlb = &smmu_tlb_ops;

	ret = smmu_init_registers(smmu);
	if (ret)
		return ret;

	ret = smmu_init_cmdq(smmu);
	if (ret)
		return ret;

	ret = smmu_init_strtab(smmu);
	if (ret)
		return ret;

	ret = smmu_reset_device(smmu);
	if (ret)
		return ret;

	return kvm_iommu_init_device(&smmu->iommu);
}

static int smmu_init(unsigned long init_arg)
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

static struct kvm_hyp_iommu *smmu_id_to_iommu(pkvm_handle_t smmu_id)
{
	if (smmu_id >= kvm_hyp_arm_smmu_v3_count)
		return NULL;
	smmu_id = array_index_nospec(smmu_id, kvm_hyp_arm_smmu_v3_count);

	return &kvm_hyp_arm_smmu_v3_smmus[smmu_id].iommu;
}

int smmu_domain_config_s2(struct kvm_hyp_iommu_domain *domain, u64 *ent)
{
	struct io_pgtable_cfg *cfg;
	u64 ts, sl, ic, oc, sh, tg, ps;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;

	cfg = &smmu_domain->pgtable->cfg;
	ps = cfg->arm_lpae_s2_cfg.vtcr.ps;
	tg = cfg->arm_lpae_s2_cfg.vtcr.tg;
	sh = cfg->arm_lpae_s2_cfg.vtcr.sh;
	oc = cfg->arm_lpae_s2_cfg.vtcr.orgn;
	ic = cfg->arm_lpae_s2_cfg.vtcr.irgn;
	sl = cfg->arm_lpae_s2_cfg.vtcr.sl;
	ts = cfg->arm_lpae_s2_cfg.vtcr.tsz;

	ent[0] = STRTAB_STE_0_V |
		 FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_S2_TRANS);
	ent[1] = FIELD_PREP(STRTAB_STE_1_SHCFG, STRTAB_STE_1_SHCFG_INCOMING);
	ent[2] = FIELD_PREP(STRTAB_STE_2_VTCR,
			FIELD_PREP(STRTAB_STE_2_VTCR_S2PS, ps) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2TG, tg) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2SH0, sh) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2OR0, oc) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2IR0, ic) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2SL0, sl) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2T0SZ, ts)) |
		 FIELD_PREP(STRTAB_STE_2_S2VMID, domain->domain_id) |
		 STRTAB_STE_2_S2AA64 | STRTAB_STE_2_S2R;
	ent[3] = cfg->arm_lpae_s2_cfg.vttbr & STRTAB_STE_3_S2TTB_MASK;

	return 0;
}

int smmu_domain_config_s1(struct hyp_arm_smmu_v3_device *smmu,
			  struct kvm_hyp_iommu_domain *domain,
			  u32 sid, u32 pasid, u32 pasid_bits,
			  u64 *ent, bool *update_ste)
{
	u64 *cd_table;
	u64 *ste;
	u32 nr_entries;
	u64 val;
	u64 *cd_entry;
	struct io_pgtable_cfg *cfg;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;

	cfg = &smmu_domain->pgtable->cfg;
	ste = smmu_get_ste_ptr(smmu, sid);
	val = le64_to_cpu(ste[0]);

	/* The host trying to attach stage-1 domain to an already stage-2 attached device. */
	if (FIELD_GET(STRTAB_STE_0_CFG, val) == STRTAB_STE_0_CFG_S2_TRANS)
		return -EBUSY;

	cd_table = (u64 *)(FIELD_GET(STRTAB_STE_0_S1CTXPTR_MASK, val) << 6);
	nr_entries = 1 << FIELD_GET(STRTAB_STE_0_S1CDMAX, val);
	*update_ste = false;
	/* This is the first pasid attached to this device. */
	if (!cd_table) {
		cd_table = smmu_alloc_cd(pasid_bits);
		if (!cd_table)
			return -ENOMEM;
		nr_entries = 1 << pasid_bits;
		ent[1] = FIELD_PREP(STRTAB_STE_1_S1DSS, STRTAB_STE_1_S1DSS_SSID0) |
			 FIELD_PREP(STRTAB_STE_1_S1CIR, STRTAB_STE_1_S1C_CACHE_WBRA) |
			 FIELD_PREP(STRTAB_STE_1_S1COR, STRTAB_STE_1_S1C_CACHE_WBRA) |
			 FIELD_PREP(STRTAB_STE_1_S1CSH, ARM_SMMU_SH_ISH);
		ent[0] = ((u64)cd_table & STRTAB_STE_0_S1CTXPTR_MASK) |
			 FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_S1_TRANS) |
			 FIELD_PREP(STRTAB_STE_0_S1CDMAX, pasid_bits) |
			 FIELD_PREP(STRTAB_STE_0_S1FMT, STRTAB_STE_0_S1FMT_LINEAR) |
			 STRTAB_STE_0_V;
		*update_ste = true;
	}

	if (pasid >= nr_entries)
		return -E2BIG;
	/* Write CD. */
	cd_entry = smmu_get_cd_ptr(hyp_phys_to_virt((u64)cd_table), pasid);

	/* CD already used by another device. */
	if (cd_entry[0])
		return -EBUSY;

	cd_entry[1] = cpu_to_le64(cfg->arm_lpae_s1_cfg.ttbr & CTXDESC_CD_1_TTB0_MASK);
	cd_entry[2] = 0;
	cd_entry[3] = cpu_to_le64(cfg->arm_lpae_s1_cfg.mair);
	/* STE is live. */
	if (!(*update_ste))
		smmu_sync_cd(smmu, cd_entry, sid, pasid);
	val =  FIELD_PREP(CTXDESC_CD_0_TCR_T0SZ, cfg->arm_lpae_s1_cfg.tcr.tsz) |
	       FIELD_PREP(CTXDESC_CD_0_TCR_TG0, cfg->arm_lpae_s1_cfg.tcr.tg) |
	       FIELD_PREP(CTXDESC_CD_0_TCR_IRGN0, cfg->arm_lpae_s1_cfg.tcr.irgn) |
	       FIELD_PREP(CTXDESC_CD_0_TCR_ORGN0, cfg->arm_lpae_s1_cfg.tcr.orgn) |
	       FIELD_PREP(CTXDESC_CD_0_TCR_SH0, cfg->arm_lpae_s1_cfg.tcr.sh) |
	       FIELD_PREP(CTXDESC_CD_0_TCR_IPS, cfg->arm_lpae_s1_cfg.tcr.ips) |
	       CTXDESC_CD_0_TCR_EPD1 | CTXDESC_CD_0_AA64 |
	       CTXDESC_CD_0_R | CTXDESC_CD_0_A |
	       CTXDESC_CD_0_ASET |
	       FIELD_PREP(CTXDESC_CD_0_ASID, domain->domain_id) |
	       CTXDESC_CD_0_V;
	WRITE_ONCE(cd_entry[0], cpu_to_le64(val));
	/* STE is live. */
	if (!(*update_ste))
		smmu_sync_cd(smmu, cd_entry, sid, pasid);
	return 0;
}

int smmu_domain_finalise(struct hyp_arm_smmu_v3_device *smmu,
			 struct kvm_hyp_iommu_domain *domain)
{
	int ret;
	struct io_pgtable_cfg *cfg;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	struct arm_lpae_io_pgtable *data;

	if (smmu_domain->type == KVM_ARM_SMMU_DOMAIN_S2)
		cfg = &smmu->pgtable_cfg_s2;
	else
		cfg = &smmu->pgtable_cfg_s1;

	smmu_domain->pgtable = kvm_arm_io_pgtable_alloc(cfg, domain, &ret);
	if (!smmu_domain->pgtable)
		return ret;

	data = io_pgtable_to_data(smmu_domain->pgtable);
	if (domain->domain_id == KVM_IOMMU_DOMAIN_IDMAP_ID) {
		data->idmapped = true;
		ret = kvm_iommu_snapshot_host_stage2(domain);
		if (ret)
			return ret;
	}

	return ret;
}

static bool smmu_domain_compat(struct hyp_arm_smmu_v3_device *smmu,
			       struct hyp_arm_smmu_v3_domain *smmu_domain)
{
	struct io_pgtable_cfg *cfg1, *cfg2;

	/* Domain is empty. */
	if (!smmu_domain->pgtable)
		return true;

	if (smmu_domain->type == KVM_ARM_SMMU_DOMAIN_S2) {
		if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S2))
			return false;
		cfg1 = &smmu->pgtable_cfg_s2;
	} else {
		if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S1))
			return false;
		cfg1 = &smmu->pgtable_cfg_s1;
	}

	cfg2 = &smmu_domain->pgtable->cfg;

	/* Best effort. */
	return (cfg1->ias == cfg2->ias) && (cfg1->oas == cfg2->oas) && (cfg1->fmt && cfg2->fmt) &&
	       (cfg1->pgsize_bitmap == cfg2->pgsize_bitmap) && (cfg1->quirks == cfg2->quirks);
}

static bool smmu_existing_in_domain(struct hyp_arm_smmu_v3_device *smmu,
				    struct hyp_arm_smmu_v3_domain *smmu_domain)
{
	struct domain_iommu_node *iommu_node;
	struct hyp_arm_smmu_v3_device *other;

	hyp_assert_write_lock_held(&smmu_domain->lock);

	list_for_each_entry(iommu_node, &smmu_domain->iommu_list, list) {
		other = to_smmu(iommu_node->iommu);
		if (other == smmu)
			return true;
	}

	return false;
}

static void smmu_get_ref_domain(struct hyp_arm_smmu_v3_device *smmu,
				struct hyp_arm_smmu_v3_domain *smmu_domain)
{
	struct domain_iommu_node *iommu_node;
	struct hyp_arm_smmu_v3_device *other;

	hyp_assert_write_lock_held(&smmu_domain->lock);

	list_for_each_entry(iommu_node, &smmu_domain->iommu_list, list) {
		other = to_smmu(iommu_node->iommu);
		if (other == smmu) {
			iommu_node->ref++;
			return;
		}
	}
}

static void smmu_put_ref_domain(struct hyp_arm_smmu_v3_device *smmu,
				struct hyp_arm_smmu_v3_domain *smmu_domain)
{
	struct domain_iommu_node *iommu_node, *temp;
	struct hyp_arm_smmu_v3_device *other;

	hyp_assert_write_lock_held(&smmu_domain->lock);

	list_for_each_entry_safe(iommu_node, temp, &smmu_domain->iommu_list, list) {
		other = to_smmu(iommu_node->iommu);
		if (other == smmu) {
			iommu_node->ref--;
			if (iommu_node->ref == 0) {
				list_del(&iommu_node->list);
				hyp_free(iommu_node);
			}
			return;
		}
	}
}

static int smmu_attach_dev(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			   u32 sid, u32 pasid, u32 pasid_bits)
{
	int i;
	int ret = -EINVAL;
	u64 *dst;
	u64 ent[STRTAB_STE_DWORDS] = {};
	struct hyp_arm_smmu_v3_device *smmu = to_smmu(iommu);
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	bool update_ste = true; /* Some S1 attaches might not update STE. */
	struct domain_iommu_node *iommu_node = NULL;

	hyp_write_lock(&smmu_domain->lock);
	hyp_spin_lock(&iommu->lock);
	dst = smmu_get_ste_ptr(smmu, sid);
	if (!dst)
		goto out_unlock;


	/*
	 * BYPASS domains only supported on stage-2 instances, that is over restrictive
	 * but for now as stage-1 is limited to VA_BITS to match the kernel, it might
	 * not cover the ia bits, we don't support it.
	 */
	if (smmu_domain->type == KVM_ARM_SMMU_DOMAIN_BYPASS) {
		if (smmu->features & ARM_SMMU_FEAT_TRANS_S2) {
			smmu_domain->type = KVM_ARM_SMMU_DOMAIN_S2;
		} else {
			ret = -EINVAL;
			goto out_unlock;
		}
	}

	if (!smmu_existing_in_domain(smmu, smmu_domain)) {
		if (!smmu_domain_compat(smmu, smmu_domain)) {
			ret = -EBUSY;
			goto out_unlock;
		}
		iommu_node = smmu_alloc(sizeof(struct domain_iommu_node));
		if (!iommu_node) {
			ret = -ENOMEM;
			goto out_unlock;
		}
		iommu_node->iommu = iommu;
		iommu_node->ref = 1;
	} else {
		smmu_get_ref_domain(smmu, smmu_domain);
	}

	/*
	 * First attach to the domain, this is over protected by the all domain locks,
	 * as there is no per-domain lock now, this can be improved later.
	 * However, as this operation is not on the hot path, it should be fine.
	 */
	if (!smmu_domain->pgtable) {
		ret = smmu_domain_finalise(smmu, domain);
		if (ret)
			goto out_unlock;
	}

	if (smmu_domain->type == KVM_ARM_SMMU_DOMAIN_S2) {
		/* Device already attached or pasid for s2. */
		if ((dst[0] & ~STRTAB_STE_0_S1CTXPTR_MASK) || pasid) {
			ret = -EBUSY;
			goto out_unlock;
		}
		ret = smmu_domain_config_s2(domain, ent);

		/* Don't lost the CD as we never free it. */
		ent[0] |= dst[0];
	} else {
		/*
		 * One drawback to this is that the first attach to this sid dictates
		 * how many pasid bits needed as we don't relocated CDs.
		 */
		pasid_bits = min(pasid_bits, smmu->ssid_bits);
		ret = smmu_domain_config_s1(smmu, domain, sid, pasid, pasid_bits,
					    ent, &update_ste);
	}
	if (ret)
		goto out_unlock;

	if (!update_ste)
		goto out_unlock;
	/*
	 * The SMMU may cache a disabled STE.
	 * Initialize all fields, sync, then enable it.
	 */
	for (i = 1; i < STRTAB_STE_DWORDS; i++)
		dst[i] = cpu_to_le64(ent[i]);

	ret = smmu_sync_ste(smmu, dst, sid);
	if (ret)
		goto out_unlock;

	WRITE_ONCE(dst[0], cpu_to_le64(ent[0]));
	ret = smmu_sync_ste(smmu, dst, sid);
	WARN_ON(ret);
	if (iommu_node)
		list_add_tail(&iommu_node->list, &smmu_domain->iommu_list);
out_unlock:
	if (ret && iommu_node)
		hyp_free(iommu_node);
	hyp_spin_unlock(&iommu->lock);
	hyp_write_unlock(&smmu_domain->lock);
	return ret;
}

static int smmu_detach_dev(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			   u32 sid, u32 pasid)
{
	u64 *dst;
	int i, ret = -ENODEV;
	struct hyp_arm_smmu_v3_device *smmu = to_smmu(iommu);
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	u32 nr_ssid;
	u64 *cd_table, *cd;

	hyp_write_lock(&smmu_domain->lock);
	hyp_spin_lock(&iommu->lock);
	dst = smmu_get_ste_ptr(smmu, sid);
	if (!dst)
		goto out_unlock;

	if (smmu_domain->type == KVM_ARM_SMMU_DOMAIN_S1) {
		nr_ssid = 1 << FIELD_GET(STRTAB_STE_0_S1CDMAX, dst[0]);
		if (pasid >= nr_ssid) {
			ret = -E2BIG;
			goto out_unlock;
		}
		cd_table = (u64 *)(FIELD_GET(STRTAB_STE_0_S1CTXPTR_MASK, dst[0]) << 6);
		/* This shouldn't happen*/
		BUG_ON(!cd_table);

		cd_table = hyp_phys_to_virt((phys_addr_t)cd_table);
		cd = smmu_get_cd_ptr(cd_table, pasid);

		WARN_ON(!FIELD_GET(CTXDESC_CD_0_V, cd[0]));

		/* Invalidate CD. */
		cd[0] = 0;
		smmu_sync_cd(smmu, cd, sid, pasid);
		cd[1] = 0;
		cd[2] = 0;
		cd[3] = 0;
		ret = smmu_sync_cd(smmu, cd, sid, pasid);
	} else {
		/* Don't clear CD ptr, as it would leak memory. */
		dst[0] &= STRTAB_STE_0_S1CTXPTR_MASK;
		ret = smmu_sync_ste(smmu, dst, sid);
		if (ret)
			goto out_unlock;

		for (i = 1; i < STRTAB_STE_DWORDS; i++)
			dst[i] = 0;

		ret = smmu_sync_ste(smmu, dst, sid);
	}

	smmu_put_ref_domain(smmu, smmu_domain);
out_unlock:
	hyp_spin_unlock(&iommu->lock);
	hyp_write_unlock(&smmu_domain->lock);
	return ret;
}

int smmu_alloc_domain(struct kvm_hyp_iommu_domain *domain, u32 type)
{
	struct hyp_arm_smmu_v3_domain *smmu_domain;

	smmu_domain = smmu_alloc(sizeof(struct hyp_arm_smmu_v3_domain));
	if (!smmu_domain)
		return -ENOMEM;

	/* Can't do much without the IOMMU. */
	INIT_LIST_HEAD(&smmu_domain->iommu_list);
	smmu_domain->domain = domain;
	smmu_domain->type = type;
	hyp_rwlock_init(&smmu_domain->lock);
	hyp_spin_lock_init(&smmu_domain->pgt_lock);
	domain->priv = (void *)smmu_domain;

	return 0;
}

void smmu_free_domain(struct kvm_hyp_iommu_domain *domain)
{
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	/*
	 * As page table allocation is decoupled from alloc_domain, free_domain can
	 * be called with a domain that have never been attached.
	 */
	if (smmu_domain->pgtable)
		kvm_arm_io_pgtable_free(smmu_domain->pgtable);

	hyp_free(smmu_domain);
}

bool smmu_dabt_device(struct hyp_arm_smmu_v3_device *smmu,
		      struct kvm_cpu_context *host_ctxt, u64 esr, u32 off)
{
	bool is_write = esr & ESR_ELx_WNR;
	unsigned int len = BIT((esr & ESR_ELx_SAS) >> ESR_ELx_SAS_SHIFT);
	int rd = (esr & ESR_ELx_SRT_MASK) >> ESR_ELx_SRT_SHIFT;
	const u32 no_access  = 0;
	const u32 read_write = (u32)(-1);
	const u32 read_only = is_write ? no_access : read_write;
	u32 mask = no_access;

	/*
	 * Only handle MMIO access with u32 size and alignment.
	 * We don't need to change 64-bit registers for now.
	 */
	if ((len != sizeof(u32)) || (off & (sizeof(u32) - 1)))
		return false;

	switch (off) {
	case ARM_SMMU_EVTQ_PROD + SZ_64K:
		mask = read_write;
		break;
	case ARM_SMMU_EVTQ_CONS + SZ_64K:
		mask = read_write;
		break;
	case ARM_SMMU_GERROR:
		mask = read_only;
		break;
	case ARM_SMMU_GERRORN:
		mask = read_write;
		break;
	};

	if (!mask)
		return false;
	if (is_write)
		writel_relaxed(cpu_reg(host_ctxt, rd) & mask, smmu->base + off);
	else
		cpu_reg(host_ctxt, rd) = readl_relaxed(smmu->base + off);

	return true;
}

bool smmu_dabt_handler(struct kvm_cpu_context *host_ctxt, u64 esr, u64 addr)
{
	struct hyp_arm_smmu_v3_device *smmu;

	for_each_smmu(smmu) {
		if (addr < smmu->mmio_addr || addr >= smmu->mmio_addr + smmu->mmio_size)
			continue;
		return smmu_dabt_device(smmu, host_ctxt, esr, addr - smmu->mmio_addr);
	}
	return false;
}

int smmu_suspend(struct kvm_hyp_iommu *iommu)
{
	struct hyp_arm_smmu_v3_device *smmu = to_smmu(iommu);

	/*
	 * Disable translation, GBPA is validated at probe to be set, so all transaltion
	 * would be aborted when SMMU is disabled.
	 */
	if (iommu->power_domain.type == KVM_POWER_DOMAIN_HOST_HVC)
		return smmu_write_cr0(smmu, 0);
	return 0;
}

int smmu_resume(struct kvm_hyp_iommu *iommu)
{
	struct hyp_arm_smmu_v3_device *smmu = to_smmu(iommu);

	/*
	 * Re-enable and clean all caches.
	 */
	if (iommu->power_domain.type == KVM_POWER_DOMAIN_HOST_HVC)
		return smmu_reset_device(smmu);
	return 0;
}

int smmu_map_pages(struct kvm_hyp_iommu_domain *domain, unsigned long iova,
		   phys_addr_t paddr, size_t pgsize,
		   size_t pgcount, int prot, size_t *total_mapped)
{
	size_t mapped;
	size_t granule;
	int ret;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;

	granule = 1UL << __ffs(smmu_domain->pgtable->cfg.pgsize_bitmap);
	if (!IS_ALIGNED(iova | paddr | pgsize, granule))
		return -EINVAL;

	hyp_spin_lock(&smmu_domain->pgt_lock);
	while (pgcount && !ret) {
		mapped = 0;
		ret = smmu_domain->pgtable->ops.map_pages(&smmu_domain->pgtable->ops, iova,
							  paddr, pgsize, pgcount, prot, 0, &mapped);
		if (ret)
			break;
		WARN_ON(!IS_ALIGNED(mapped, pgsize));
		WARN_ON(mapped > pgcount * pgsize);

		pgcount -= mapped / pgsize;
		*total_mapped += mapped;
		iova += mapped;
		paddr += mapped;
	}
	hyp_spin_unlock(&smmu_domain->pgt_lock);

	return 0;
}

static void kvm_iommu_unmap_walker(struct io_pgtable_ctxt *ctxt)
{
	struct kvm_iommu_walk_data *data = (struct kvm_iommu_walk_data *)ctxt->arg;
	struct kvm_iommu_paddr_cache *cache = data->cache;

	cache->paddr[cache->ptr] = ctxt->addr;
	cache->pgsize[cache->ptr++] = ctxt->size;

	/*
	 * It is guaranteed unmap is called with max of the cache size,
	 * see kvm_iommu_unmap_pages()
	 */
	WARN_ON(cache->ptr == KVM_IOMMU_PADDR_CACHE_MAX);
}

static size_t smmu_unmap_pages(struct kvm_hyp_iommu_domain *domain, unsigned long iova,
			       size_t pgsize, size_t pgcount,
			       struct iommu_iotlb_gather *gather,
			       struct kvm_iommu_paddr_cache *cache)
{
	size_t granule, unmapped;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	struct kvm_iommu_walk_data data = {
		.iotlb_gather = gather,
		.cookie = smmu_domain->pgtable->cookie,
		.cache = cache,
	};
	struct io_pgtable_walker walker = {
		.cb = kvm_iommu_unmap_walker,
		.arg = &data,
	};

	granule = 1UL << __ffs(smmu_domain->pgtable->cfg.pgsize_bitmap);
	if (!IS_ALIGNED(iova | pgsize, granule))
		return 0;

	hyp_spin_lock(&smmu_domain->pgt_lock);
	unmapped = smmu_domain->pgtable->ops.unmap_pages_walk(&smmu_domain->pgtable->ops, iova,
							      pgsize, pgcount, gather, &walker);
	hyp_spin_unlock(&smmu_domain->pgt_lock);
	return unmapped;
}

static phys_addr_t smmu_iova_to_phys(struct kvm_hyp_iommu_domain *domain,
				     unsigned long iova)
{
	phys_addr_t paddr;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;

	hyp_spin_lock(&smmu_domain->pgt_lock);
	paddr = smmu_domain->pgtable->ops.iova_to_phys(&smmu_domain->pgtable->ops, iova);
	hyp_spin_unlock(&smmu_domain->pgt_lock);

	return paddr;
}

/*
 * Although SMMU can support multiple granules, it must at least support PAGE_SIZE
 * as the CPU, and for the IDMAP domains, we only use this granule.
 * As we optimize for memory usage and performance, we try to use block mappings
 * when possible.
 */
static size_t smmu_pgsize(size_t size)
{
	size_t pgsizes;
	const size_t pgsize_bitmask = PAGE_SIZE | (PAGE_SIZE * PTRS_PER_PTE) |
				      (PAGE_SIZE * PTRS_PER_PTE * PTRS_PER_PTE);

	pgsizes = pgsize_bitmask & GENMASK_ULL(__fls(size), 0);
	WARN_ON(!pgsizes);

	return BIT(__fls(pgsizes));
}

static void smmu_host_stage2_idmap(struct kvm_hyp_iommu_domain *domain,
				   phys_addr_t start, phys_addr_t end, int prot)
{
	size_t size = end - start;
	size_t pgsize, pgcount;
	size_t mapped, unmapped;
	int ret;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	struct io_pgtable *pgtable = smmu_domain->pgtable;

	end = min(end, BIT(pgtable->cfg.oas));
	if (start >= end)
		return;

	if (prot) {
		if (!(prot & IOMMU_MMIO) && pgtable->cfg.coherent_walk)
			prot |= IOMMU_CACHE;

		while (size) {
			mapped = 0;
			pgsize = smmu_pgsize(size);
			pgcount = size / pgsize;
			ret = pgtable->ops.map_pages(&pgtable->ops, start, start,
						     pgsize, pgcount, prot, 0, &mapped);
			size -= mapped;
			start += mapped;
			if (!mapped || ret)
				return;
		}
	} else {
		while (size) {
			pgsize = smmu_pgsize(size);
			pgcount = size / pgsize;
			unmapped = pgtable->ops.unmap_pages(&pgtable->ops, start,
							    pgsize, pgcount, NULL);
			size -= unmapped;
			start += unmapped;
			if (!unmapped)
				return;
		}
	}
}

#ifdef MODULE
int smmu_init_hyp_module(const struct pkvm_module_ops *ops)
{
	if (!ops)
		return -EINVAL;

	mod_ops = ops;
	return 0;
}
#endif

struct kvm_iommu_ops smmu_ops = {
	.init				= smmu_init,
	.get_iommu_by_id		= smmu_id_to_iommu,
	.alloc_domain			= smmu_alloc_domain,
	.free_domain			= smmu_free_domain,
	.attach_dev			= smmu_attach_dev,
	.detach_dev			= smmu_detach_dev,
	.dabt_handler			= smmu_dabt_handler,
	.suspend			= smmu_suspend,
	.resume				= smmu_resume,
	.iotlb_sync			= smmu_iotlb_sync,
	.host_stage2_idmap		= smmu_host_stage2_idmap,
	.map_pages			= smmu_map_pages,
	.unmap_pages			= smmu_unmap_pages,
	.iova_to_phys			= smmu_iova_to_phys,
};

