/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2014-2026 Christoph Hellwig.
 *
 * Support for exportfs-based layout grants for direct block device access.
 */
#ifndef LINUX_EXPORTFS_BLOCK_H
#define LINUX_EXPORTFS_BLOCK_H 1

#include <linux/types.h>

struct iattr;
struct inode;
struct iomap;
struct super_block;

struct exportfs_block_ops {
	/*
	 * Get the in-band device unique signature exposed to clients.
	 */
	int (*get_uuid)(struct super_block *sb, u8 *buf, u32 *len, u64 *offset);

	/*
	 * Map blocks for direct block access.
	 * If @write is %true, also allocate the blocks for the range if needed.
	 */
	int (*map_blocks)(struct inode *inode, loff_t offset, u64 len,
			struct iomap *iomap, bool write,
			u32 *device_generation);

	/*
	 * Commit blocks previously handed out by ->map_blocks and written to by
	 * the client.
	 */
	int (*commit_blocks)(struct inode *inode, struct iomap *iomaps,
			int nr_iomaps, struct iattr *iattr);
};

#endif /* LINUX_EXPORTFS_BLOCK_H */
