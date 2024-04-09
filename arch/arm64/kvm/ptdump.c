// SPDX-License-Identifier: GPL-2.0-only
/*
 * Debug helper used to dump the stage-2 pagetables of the system and their
 * associated permissions.
 *
 * Copyright (C) Google, 2023
 * Author: Sebastian Ene <sebastianene@google.com>
 */
#include <linux/debugfs.h>
#include <linux/kvm_host.h>
#include <linux/seq_file.h>

#include <asm/kvm_pgtable.h>
#include <asm/kvm_pkvm.h>
#include <kvm_ptdump.h>


#define MARKERS_LEN		(2)

struct kvm_ptdump_guest_state {
	union {
		struct kvm			*kvm;
		struct kvm_pgtable_snapshot	*snap;
	};
	struct pg_state		parser_state;
	struct addr_marker	ipa_marker[MARKERS_LEN];
	struct pg_level		level[KVM_PGTABLE_MAX_LEVELS];
	struct ptdump_range	range[MARKERS_LEN];
};

static const struct prot_bits stage2_pte_bits[] = {
	{
		.mask	= PTE_VALID,
		.val	= PTE_VALID,
		.set	= " ",
		.clear	= "F",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_HI_S2_XN | PTE_VALID,
		.val	= KVM_PTE_LEAF_ATTR_HI_S2_XN | PTE_VALID,
		.set	= "XN",
		.clear	= "  ",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_R | PTE_VALID,
		.val	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_R | PTE_VALID,
		.set	= "R",
		.clear	= " ",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_W | PTE_VALID,
		.val	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_W | PTE_VALID,
		.set	= "W",
		.clear	= " ",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_LO_S2_AF | PTE_VALID,
		.val	= KVM_PTE_LEAF_ATTR_LO_S2_AF | PTE_VALID,
		.set	= "AF",
		.clear	= "  ",
	}, {
		.mask	= PTE_NG,
		.val	= PTE_NG,
		.set	= "FnXS",
		.clear	= "  ",
	}, {
		.mask	= PTE_CONT | PTE_VALID,
		.val	= PTE_CONT | PTE_VALID,
		.set	= "CON",
		.clear	= "   ",
	}, {
		.mask	= PTE_TABLE_BIT | PTE_VALID,
		.val	= PTE_VALID,
		.set	= "BLK",
		.clear	= "   ",
	}, {
		.mask	= KVM_INVALID_PTE_OWNER_MASK,
		.val	= FIELD_PREP_CONST(KVM_INVALID_PTE_OWNER_MASK,
					   PKVM_ID_HYP),
		.set	= "HYP",
	}, {
		.mask	= KVM_INVALID_PTE_OWNER_MASK,
		.val	= FIELD_PREP_CONST(KVM_INVALID_PTE_OWNER_MASK,
					   PKVM_ID_FFA),
		.set	= "FF-A",
	}, {
		.mask	= KVM_INVALID_PTE_OWNER_MASK,
		.val	= FIELD_PREP_CONST(KVM_INVALID_PTE_OWNER_MASK,
					   PKVM_ID_GUEST),
		.set	= "GUEST",
	}
};

static int kvm_ptdump_visitor(const struct kvm_pgtable_visit_ctx *ctx,
			      enum kvm_pgtable_walk_flags visit)
{
	struct pg_state *st = ctx->arg;
	struct ptdump_state *pt_st = &st->ptdump;

	note_page(pt_st, ctx->addr, ctx->level, ctx->old);
	return 0;
}

static int kvm_ptdump_show_common(struct seq_file *m,
				  struct kvm_pgtable *pgtable,
				  struct pg_state *parser_state)
{
	struct kvm_pgtable_walker walker = (struct kvm_pgtable_walker) {
		.cb     = kvm_ptdump_visitor,
		.arg	= parser_state,
		.flags	= KVM_PGTABLE_WALK_LEAF,
	};

	parser_state->level = -1;
	parser_state->start_address = 0;

	return kvm_pgtable_walk(pgtable, 0, BIT(pgtable->ia_bits), &walker);
}

static int kvm_ptdump_build_levels(struct pg_level *level, u32 start_lvl)
{
	static const char * const level_names[] = {"PGD", "PUD", "PMD", "PTE"};
	u32 i = 0;
	u64 mask = 0;

	if (start_lvl > 2) {
		pr_err("invalid start_lvl %u\n", start_lvl);
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(stage2_pte_bits); i++)
		mask |= stage2_pte_bits[i].mask;

	for (i = start_lvl; i < KVM_PGTABLE_MAX_LEVELS; i++) {
		level[i].name	= level_names[i];
		level[i].num	= ARRAY_SIZE(stage2_pte_bits);
		level[i].bits	= stage2_pte_bits;
		level[i].mask	= mask;
	}

	if (start_lvl > 0)
		level[start_lvl].name = level_names[0];

	return 0;
}

static phys_addr_t get_host_pa(void *addr)
{
	return __pa(addr);
}

static void *get_host_va(phys_addr_t pa)
{
	return __va(pa);
}

static struct kvm_pgtable_mm_ops ptdump_mmops = {
	.phys_to_virt	= get_host_va,
	.virt_to_phys	= get_host_pa,
};

static void kvm_ptdump_put_snapshot(struct kvm_pgtable_snapshot *snap)
{
	void *mc_page;
	size_t i;

	if (!snap)
		return;

	free_hyp_memcache(&snap->mc);

	if (snap->pgd_hva)
		free_pages_exact(snap->pgd_hva, snap->pgd_pages * PAGE_SIZE);

	if (snap->used_pages_hva) {
		for (i = 0; i < snap->used_pages_idx; i++) {
			mc_page = get_host_va(snap->used_pages_hva[i]);
			free_page((unsigned long)mc_page);
		}

		free_pages_exact(snap->used_pages_hva, snap->num_used_pages * PAGE_SIZE);
	}

	free_page((unsigned long)snap);
}

static struct kvm_pgtable_snapshot *kvm_ptdump_get_snapshot(pkvm_handle_t handle,
							    size_t mc_pages,
							    size_t pgd_pages)
{
	struct kvm_pgtable_snapshot *snapshot;
	size_t used_buf_sz;
	void *pgd_hva, *used_pages_hva;
	int ret = -EINVAL;

	if (!(IS_ENABLED(CONFIG_NVHE_EL2_DEBUG))) {
		pr_warn("stage-2 snapshot not available under !NVHE_EL2_DEBUG\n");
		return ERR_PTR(-EINVAL);
	}

	snapshot = (void *)__get_free_page(GFP_KERNEL_ACCOUNT);
	if (!snapshot)
		return ERR_PTR(-ENOMEM);

	memset(snapshot, 0, sizeof(struct kvm_pgtable_snapshot));

	if (!mc_pages && !pgd_pages)
		goto copy_pgt_config;

	pgd_hva = alloc_pages_exact(pgd_pages * PAGE_SIZE, GFP_KERNEL_ACCOUNT);
	if (!pgd_hva)
		goto err_out;

	snapshot->pgd_hva = pgd_hva;
	snapshot->pgd_pages = pgd_pages;

	ret = topup_hyp_memcache(&snapshot->mc, mc_pages, 0);
	if (ret)
		goto err_out;

	used_buf_sz = PAGE_ALIGN(sizeof(phys_addr_t) * mc_pages);
	used_pages_hva = alloc_pages_exact(used_buf_sz, GFP_KERNEL_ACCOUNT);
	if (!used_pages_hva)
		goto err_out;

	snapshot->used_pages_hva = used_pages_hva;
	snapshot->num_used_pages = used_buf_sz >> PAGE_SHIFT;

copy_pgt_config:
	ret = kvm_call_hyp_nvhe(__pkvm_stage2_snapshot, snapshot, handle);
	if (ret) {
		pr_err("%d snapshot pagetables\n", ret);
		goto err_out;
	}

	snapshot->pgtable.pgd =
		(kvm_pteref_t)get_host_va((__force phys_addr_t)snapshot->pgtable.pgd);
	snapshot->pgtable.mm_ops = &ptdump_mmops;

	return snapshot;
err_out:
	kvm_ptdump_put_snapshot(snapshot);
	return ERR_PTR(ret);
}

static struct kvm_pgtable_snapshot *kvm_ptdump_get_guest_snapshot(struct kvm *kvm)
{
	pkvm_handle_t handle = kvm->arch.pkvm.handle;
	struct kvm_pgtable_snapshot *snap;
	size_t mc_pages, pgd_pages, new_mc_pages;

retry_mc_alloc:
	mc_pages = atomic64_read(&kvm->stat.protected_pgtable_mem) >> PAGE_SHIFT;
	pgd_pages = kvm_pgtable_stage2_pgd_size(kvm->arch.vtcr) >> PAGE_SHIFT;

	snap = kvm_ptdump_get_snapshot(handle, mc_pages, pgd_pages);
	if (PTR_ERR_OR_ZERO(snap) == -ENOMEM) {
		/* If we are accounting for memory and a fault happens at the
		 * same time we might need more memory for the snapshot.
		 */
		new_mc_pages = atomic64_read(&kvm->stat.protected_pgtable_mem);
		new_mc_pages = new_mc_pages >> PAGE_SHIFT;

		if (mc_pages < new_mc_pages)
			goto retry_mc_alloc;
	}

	return snap;
}

static size_t host_stage2_get_pgd_size(void)
{
	u32 phys_shift = get_kvm_ipa_limit();
	u64 vtcr = kvm_get_vtcr(read_sanitised_ftr_reg(SYS_ID_AA64MMFR0_EL1),
				read_sanitised_ftr_reg(SYS_ID_AA64MMFR1_EL1),
				phys_shift);
	return kvm_pgtable_stage2_pgd_size(vtcr);
}

static struct kvm_pgtable_snapshot *kvm_ptdump_get_host_snapshot(void *data)
{
	size_t mc_pages, pgd_pages;
	mc_pages = (size_t)data;
	pgd_pages = host_stage2_get_pgd_size() >> PAGE_SHIFT;
	return kvm_ptdump_get_snapshot(0, (size_t)data, pgd_pages);
}

static struct kvm_ptdump_guest_state
*kvm_ptdump_parser_init(struct kvm *kvm, char *decorator, void *data)
{
	struct kvm_ptdump_guest_state *st;
	struct kvm_pgtable *pgtable;
	struct kvm_pgtable_snapshot *snap;
	int ret;

	st = kzalloc(sizeof(struct kvm_ptdump_guest_state), GFP_KERNEL_ACCOUNT);
	if (!st)
		return ERR_PTR(-ENOMEM);

	if (!is_protected_kvm_enabled()) {
		pgtable = kvm->arch.mmu.pgt;
		st->kvm = kvm;
	} else {
		if (!data)
			snap = kvm_ptdump_get_guest_snapshot(kvm);
		else
			snap = kvm_ptdump_get_host_snapshot(data);

		if (IS_ERR(snap)) {
			ret = PTR_ERR(snap);
			goto free_with_state;
		}

		pgtable = &snap->pgtable;
		st->snap = snap;
	}

	ret = kvm_ptdump_build_levels(&st->level[0], pgtable->start_level);
	if (ret)
		goto free_with_state;

	st->ipa_marker[0].name		= decorator;
	st->ipa_marker[1].start_address = BIT(pgtable->ia_bits);
	st->range[0].end		= BIT(pgtable->ia_bits);

	st->parser_state = (struct pg_state) {
		.marker		= &st->ipa_marker[0],
		.level		= -1,
		.pg_level	= &st->level[0],
		.ptdump.range	= &st->range[0],
	};

	return st;
free_with_state:
	kfree(st);
	return ERR_PTR(ret);
}

static int kvm_ptdump_guest_show(struct seq_file *m, void *unused)
{
	struct kvm_ptdump_guest_state *st = m->private;
	struct kvm *kvm = st->kvm;
	struct kvm_s2_mmu *mmu = &kvm->arch.mmu;
	int ret;

	st->parser_state.seq = m;

	write_lock(&kvm->mmu_lock);
	ret = kvm_ptdump_show_common(m, mmu->pgt, &st->parser_state);
	write_unlock(&kvm->mmu_lock);

	return ret;
}

static int kvm_ptdump_protected_guest_show(struct seq_file *m, void *unused)
{
	struct kvm_ptdump_guest_state *st = m->private;
	struct kvm_pgtable_snapshot *snap = st->snap;

	st->parser_state.seq = m;

	return kvm_ptdump_show_common(m, &snap->pgtable, &st->parser_state);
}

static int kvm_ptdump_guest_open(struct inode *m, struct file *file)
{
	struct kvm *kvm = m->i_private;
	struct kvm_ptdump_guest_state *st;
	int ret;
	int (*kvm_ptdump_show)(struct seq_file *file, void *arg);

	if (is_protected_kvm_enabled())
		kvm_ptdump_show = kvm_ptdump_protected_guest_show;
	else
		kvm_ptdump_show = kvm_ptdump_guest_show;

	if (!kvm_get_kvm_safe(kvm))
		return -ENOENT;

	st = kvm_ptdump_parser_init(kvm, "Guest IPA", NULL);
	if (IS_ERR(st)) {
		ret = PTR_ERR(st);
		goto err;
	}

	ret = single_open(file, kvm_ptdump_show, st);
	if (!ret)
		return 0;

	if (is_protected_kvm_enabled())
		kvm_ptdump_put_snapshot(st->snap);
	kfree(st);
err:
	kvm_put_kvm(kvm);
	return ret;
}

static int kvm_ptdump_guest_close(struct inode *m, struct file *file)
{
	struct kvm *kvm = m->i_private;
	struct seq_file *seq = (struct seq_file *)file->private_data;
	struct kvm_ptdump_guest_state *st = seq->private;

	if (is_protected_kvm_enabled())
		kvm_ptdump_put_snapshot(st->snap);

	kfree(st);
	kvm_put_kvm(kvm);
	return single_release(m, file);
}

static const struct file_operations kvm_ptdump_guest_fops = {
	.open		= kvm_ptdump_guest_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= kvm_ptdump_guest_close,
};

static int kvm_pgtable_debugfs_show(struct seq_file *m, void *unused)
{
	const struct file *file = m->file;
	struct kvm_pgtable *pgtable = m->private;

	if (!strcmp(file_dentry(file)->d_iname, "ipa_range"))
		seq_printf(m, "%2u\n", pgtable->ia_bits);
	else if (!strcmp(file_dentry(file)->d_iname, "stage2_levels"))
		seq_printf(m, "%1d\n", pgtable->start_level);
	return 0;
}

static int kvm_pgtable_debugfs_open(struct inode *m, struct file *file)
{
	struct kvm *kvm = m->i_private;
	struct kvm_s2_mmu *mmu;
	struct kvm_pgtable *pgtable;
	struct kvm_pgtable_snapshot *snap;
	int ret = -EINVAL;
	pkvm_handle_t handle;

	if (kvm && !kvm_get_kvm_safe(kvm))
		return -ENOENT;

	if (is_protected_kvm_enabled()) {
		handle = kvm ? kvm->arch.pkvm.handle : 0;
		snap = kvm_ptdump_get_snapshot(handle, 0, 0);
		if (IS_ERR(snap))
			goto free_with_kvm_ref;
		pgtable = &snap->pgtable;
	} else {
		mmu = &kvm->arch.mmu;
		pgtable = mmu->pgt;
	}

	ret = single_open(file, kvm_pgtable_debugfs_show, pgtable);
	if (!ret)
		return 0;

	if (is_protected_kvm_enabled())
		kvm_ptdump_put_snapshot(snap);
free_with_kvm_ref:
	if (kvm)
		kvm_put_kvm(kvm);
	return ret;
}

static int kvm_pgtable_debugfs_close(struct inode *m, struct file *file)
{
	struct kvm *kvm = m->i_private;
	struct kvm_pgtable_snapshot *snap;
	struct seq_file *seq;
	struct kvm_pgtable *pgtable;

	if (is_protected_kvm_enabled()) {
		seq = (struct seq_file *)file->private_data;
		pgtable = seq->private;
		snap = container_of(pgtable, struct kvm_pgtable_snapshot,
				    pgtable);
		kvm_ptdump_put_snapshot(snap);
	}

	if (kvm)
		kvm_put_kvm(kvm);
	return single_release(m, file);
}

static const struct file_operations kvm_pgtable_debugfs_fops = {
	.open		= kvm_pgtable_debugfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= kvm_pgtable_debugfs_close,
};

void kvm_ptdump_guest_register(struct kvm *kvm)
{
	debugfs_create_file("stage2_page_tables", 0400, kvm->debugfs_dentry,
			    kvm, &kvm_ptdump_guest_fops);
	debugfs_create_file("ipa_range", 0400, kvm->debugfs_dentry, kvm,
			    &kvm_pgtable_debugfs_fops);
	debugfs_create_file("stage2_levels", 0400, kvm->debugfs_dentry,
			    kvm, &kvm_pgtable_debugfs_fops);
}

static int kvm_ptdump_host_open(struct inode *m, struct file *file)
{
	struct kvm_ptdump_guest_state *st;
	int ret;

	st = kvm_ptdump_parser_init(NULL, "Host IPA", m->i_private);
	if (IS_ERR(st))
		return PTR_ERR(st);

	ret = single_open(file, kvm_ptdump_protected_guest_show, st);
	if (!ret)
		return 0;

	kvm_ptdump_put_snapshot(st->snap);
	kfree(st);
	return ret;
}

static int kvm_ptdump_host_close(struct inode *m, struct file *file)
{
	struct seq_file *seq = (struct seq_file *)file->private_data;
	struct kvm_ptdump_guest_state *st = seq->private;

	kvm_ptdump_put_snapshot(st->snap);
	kfree(st);
	return single_release(m, file);
}

static const struct file_operations kvm_ptdump_host_fops = {
	.open		= kvm_ptdump_host_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= kvm_ptdump_host_close,
};

void kvm_ptdump_host_register(void)
{
	debugfs_create_file("host_stage2_page_tables", 0400, kvm_debugfs_dir,
			    (void *)host_s2_pgtable_pages(), &kvm_ptdump_host_fops);
	debugfs_create_file("ipa_range", 0400, kvm_debugfs_dir, NULL,
			    &kvm_pgtable_debugfs_fops);
	debugfs_create_file("stage2_levels", 0400, kvm_debugfs_dir,
			    NULL, &kvm_pgtable_debugfs_fops);
}
