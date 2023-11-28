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
#include <kvm_ptdump.h>


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

	return kvm_pgtable_walk(pgtable, 0, BIT(pgtable->ia_bits), &walker);
}

static int kvm_ptdump_guest_show(struct seq_file *m, void *unused)
{
	struct kvm *kvm = m->private;
	struct kvm_s2_mmu *mmu = &kvm->arch.mmu;
	struct pg_state parser_state = {0};
	int ret;

	write_lock(&kvm->mmu_lock);
	ret = kvm_ptdump_show_common(m, mmu->pgt, &parser_state);
	write_unlock(&kvm->mmu_lock);

	return ret;
}

static int kvm_ptdump_guest_open(struct inode *m, struct file *file)
{
	struct kvm *kvm = m->i_private;
	int ret;

	if (is_protected_kvm_enabled())
		return -EPERM;

	if (!kvm_get_kvm_safe(kvm))
		return -ENOENT;

	ret = single_open(file, kvm_ptdump_guest_show, m->i_private);
	if (ret < 0)
		kvm_put_kvm(kvm);

	return ret;
}

static int kvm_ptdump_guest_close(struct inode *m, struct file *file)
{
	struct kvm *kvm = m->i_private;

	kvm_put_kvm(kvm);
	return single_release(m, file);
}

static const struct file_operations kvm_ptdump_guest_fops = {
	.open		= kvm_ptdump_guest_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= kvm_ptdump_guest_close,
};

void kvm_ptdump_guest_register(struct kvm *kvm)
{
	debugfs_create_file("stage2_page_tables", 0400, kvm->debugfs_dentry,
			    kvm, &kvm_ptdump_guest_fops);
}
