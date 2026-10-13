// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Christian Brauner <brauner@kernel.org> */
#include <linux/fs/super_types.h>
#include <linux/fs_context.h>
#include <linux/magic.h>

static const struct super_operations nullfs_super_operations = {
	.statfs	= simple_statfs,
};

static loff_t nullfs_dir_llseek(struct file *file, loff_t offset, int whence)
{
	/* an empty directory has two entries . and .. at offsets 0 and 1 */
	return generic_file_llseek_size(file, offset, whence, 2, 2);
}

static int nullfs_dir_readdir(struct file *file, struct dir_context *ctx)
{
	dir_emit_dots(file, ctx);
	return 0;
}

/* the one inode of nullfs is shared by every holder, so no locks on it */
static int nullfs_nolock(struct file *file, int cmd, struct file_lock *fl)
{
	return -ENOLCK;
}

/* what libfs gives an empty directory, plus the refusal of file locks */
static const struct file_operations nullfs_dir_operations = {
	.llseek		= nullfs_dir_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= nullfs_dir_readdir,
	.fsync		= noop_fsync,
	.lock		= nullfs_nolock,
	.flock		= nullfs_nolock,
	.fop_flags	= FOP_IMMUTABLE,
};

static int nullfs_fs_fill_super(struct super_block *s, struct fs_context *fc)
{
	struct inode *inode;

	s->s_maxbytes		= MAX_LFS_FILESIZE;
	s->s_blocksize		= PAGE_SIZE;
	s->s_blocksize_bits	= PAGE_SHIFT;
	s->s_magic		= NULL_FS_MAGIC;
	s->s_op			= &nullfs_super_operations;
	s->s_export_op		= NULL;
	s->s_xattr		= NULL;
	s->s_time_gran		= 1;
	s->s_d_flags		= 0;

	inode = new_inode(s);
	if (!inode)
		return -ENOMEM;

	/* nullfs is permanently empty... */
	make_empty_dir_inode(inode);
	inode->i_fop = &nullfs_dir_operations;
	simple_inode_init_ts(inode);
	inode->i_ino	= 1;
	/* ... and immutable, reading it leaves no trace either. */
	inode->i_flags |= S_IMMUTABLE | S_NOATIME;

	s->s_root = d_make_root(inode);
	if (!s->s_root)
		return -ENOMEM;

	return 0;
}

/*
 * For now this is a single global instance. If needed we can make it
 * mountable by userspace at which point we will need to make it
 * multi-instance.
 */
static int nullfs_fs_get_tree(struct fs_context *fc)
{
	return get_tree_single(fc, nullfs_fs_fill_super);
}

static const struct fs_context_operations nullfs_fs_context_ops = {
	.get_tree	= nullfs_fs_get_tree,
};

static int nullfs_init_fs_context(struct fs_context *fc)
{
	fc->ops		= &nullfs_fs_context_ops;
	fc->global	= true;
	fc->sb_flags	= SB_NOUSER;
	fc->s_iflags	= SB_I_NOEXEC | SB_I_NODEV;
	return 0;
}

struct file_system_type nullfs_fs_type = {
	.name			= "nullfs",
	.init_fs_context	= nullfs_init_fs_context,
	.kill_sb		= kill_anon_super,
};
