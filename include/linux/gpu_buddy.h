/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __GPU_BUDDY_H__
#define __GPU_BUDDY_H__

#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/rbtree.h>

#define GPU_BUDDY_RANGE_ALLOCATION		BIT(0)
#define GPU_BUDDY_TOPDOWN_ALLOCATION		BIT(1)
#define GPU_BUDDY_CONTIGUOUS_ALLOCATION		BIT(2)
#define GPU_BUDDY_CLEAR_ALLOCATION		BIT(3)
#define GPU_BUDDY_CLEARED			BIT(4)
#define GPU_BUDDY_TRIM_DISABLE			BIT(5)

enum gpu_buddy_free_tree {
	GPU_BUDDY_CLEAR_TREE = 0,
	GPU_BUDDY_DIRTY_TREE,
	GPU_BUDDY_MAX_FREE_TREES,
};

#define for_each_free_tree(tree) \
	for ((tree) = 0; (tree) < GPU_BUDDY_MAX_FREE_TREES; (tree)++)

struct gpu_buddy_block {
#define GPU_BUDDY_HEADER_OFFSET GENMASK_ULL(63, 12)
#define GPU_BUDDY_HEADER_STATE  GENMASK_ULL(11, 10)
#define   GPU_BUDDY_ALLOCATED	   (1 << 10)
#define   GPU_BUDDY_FREE	   (2 << 10)
#define   GPU_BUDDY_SPLIT	   (3 << 10)
#define GPU_BUDDY_HEADER_CLEAR  GENMASK_ULL(9, 9)
/* Free to be used, if needed in the future */
#define GPU_BUDDY_HEADER_UNUSED GENMASK_ULL(8, 6)
#define GPU_BUDDY_HEADER_ORDER  GENMASK_ULL(5, 0)
	u64 header;

	struct gpu_buddy_block *left;
	struct gpu_buddy_block *right;
	struct gpu_buddy_block *parent;

	void *private; /* owned by creator */

	/*
	 * While the block is allocated by the user through gpu_buddy_alloc*,
	 * the user has ownership of the link, for example to maintain within
	 * a list, if so desired. As soon as the block is freed with
	 * gpu_buddy_free* ownership is given back to the mm.
	 */
	union {
		struct rb_node rb;
		struct list_head link;
	};

	struct list_head tmp_link;
};

/* Order-zero must be at least SZ_4K */
#define GPU_BUDDY_MAX_ORDER (63 - 12)

/*
 * Binary Buddy System.
 *
 * Locking should be handled by the user, a simple mutex around
 * gpu_buddy_alloc* and gpu_buddy_free* should suffice.
 */
struct gpu_buddy {
	/* Maintain a free list for each order. */
	struct rb_root **free_trees;

	/*
	 * Maintain explicit binary tree(s) to track the allocation of the
	 * address space. This gives us a simple way of finding a buddy block
	 * and performing the potentially recursive merge step when freeing a
	 * block.  Nodes are either allocated or free, in which case they will
	 * also exist on the respective free list.
	 */
	struct gpu_buddy_block **roots;

	/*
	 * Anything from here is public, and remains static for the lifetime of
	 * the mm. Everything above is considered do-not-touch.
	 */
	unsigned int n_roots;
	unsigned int max_order;

	/* Must be at least SZ_4K */
	u64 chunk_size;
	u64 size;
	u64 avail;
	u64 clear_avail;
};

static inline u64
gpu_buddy_block_offset(const struct gpu_buddy_block *block)
{
	return block->header & GPU_BUDDY_HEADER_OFFSET;
}

static inline unsigned int
gpu_buddy_block_order(struct gpu_buddy_block *block)
{
	return block->header & GPU_BUDDY_HEADER_ORDER;
}

static inline unsigned int
gpu_buddy_block_state(struct gpu_buddy_block *block)
{
	return block->header & GPU_BUDDY_HEADER_STATE;
}

static inline bool
gpu_buddy_block_is_allocated(struct gpu_buddy_block *block)
{
	return gpu_buddy_block_state(block) == GPU_BUDDY_ALLOCATED;
}

static inline bool
gpu_buddy_block_is_clear(struct gpu_buddy_block *block)
{
	return block->header & GPU_BUDDY_HEADER_CLEAR;
}

static inline bool
gpu_buddy_block_is_free(struct gpu_buddy_block *block)
{
	return gpu_buddy_block_state(block) == GPU_BUDDY_FREE;
}

static inline bool
gpu_buddy_block_is_split(struct gpu_buddy_block *block)
{
	return gpu_buddy_block_state(block) == GPU_BUDDY_SPLIT;
}

static inline u64
gpu_buddy_block_size(struct gpu_buddy *mm,
		     struct gpu_buddy_block *block)
{
	return mm->chunk_size << gpu_buddy_block_order(block);
}

int gpu_buddy_init(struct gpu_buddy *mm, u64 size, u64 chunk_size);

void gpu_buddy_fini(struct gpu_buddy *mm);

struct gpu_buddy_block *
gpu_get_buddy(struct gpu_buddy_block *block);

int gpu_buddy_alloc_blocks(struct gpu_buddy *mm,
			   u64 start, u64 end, u64 size,
			   u64 min_page_size,
			   struct list_head *blocks,
			   unsigned long flags);

int gpu_buddy_block_trim(struct gpu_buddy *mm,
			 u64 *start,
			 u64 new_size,
			 struct list_head *blocks);

void gpu_buddy_reset_clear(struct gpu_buddy *mm, bool is_clear);

void gpu_buddy_free_block(struct gpu_buddy *mm, struct gpu_buddy_block *block);

void gpu_buddy_free_list(struct gpu_buddy *mm,
			 struct list_head *objects,
			 unsigned int flags);

void gpu_buddy_print(struct gpu_buddy *mm);
void gpu_buddy_block_print(struct gpu_buddy *mm,
			   struct gpu_buddy_block *block);
#endif
