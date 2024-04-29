// SPDX-License-Identifier: GPL-2.0-only
/*
 * Allocator abstraction for the hypervisor.
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <nvhe/alloc.h>
#include <nvhe/alloc_mgt.h>
#include <nvhe/iommu.h>

static struct hyp_mgt_allocator_ops *registered_allocators[] = {
	[HYP_ALLOC_MGT_HEAP_ID] = &hyp_alloc_ops,
	[HYP_ALLOC_MGT_IOMMU_ID] = &kvm_iommu_allocator_ops,
};

#define MAX_ALLOC_ID		(ARRAY_SIZE(registered_allocators))

int hyp_alloc_mgt_refill(unsigned long id, struct kvm_hyp_memcache *host_mc)
{
	struct hyp_mgt_allocator_ops *ops;

	if (id > MAX_ALLOC_ID)
		return -EINVAL;

	id = array_index_nospec(id, MAX_ALLOC_ID);

	ops = registered_allocators[id];

	return ops->refill ? ops->refill(host_mc) : 0;
}

int hyp_alloc_mgt_reclaimable(void)
{
	struct hyp_mgt_allocator_ops *ops;
	int reclaimable = 0;
	int i;

	for (i = 0 ; i < MAX_ALLOC_ID ; ++i) {
		ops = registered_allocators[i];
		if (ops->reclaimable)
			reclaimable += ops->reclaimable();
	}
	return reclaimable;
}

void hyp_alloc_mgt_reclaim(struct kvm_hyp_memcache *host_mc, int target)
{
	struct hyp_mgt_allocator_ops *ops;
	int i;

	for (i = 0 ; (i < MAX_ALLOC_ID) && (host_mc->nr_pages < target) ; ++i) {
		ops = registered_allocators[i];
		/* Not fair but OK for now. */
		if (ops->reclaim)
			ops->reclaim(host_mc, target);
	}
}
