/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __KVM_NVHE_HYP_ALLOC_MGT__
#define __KVM_NVHE_HYP_ALLOC_MGT__
#include <asm/kvm_host.h>

struct hyp_mgt_allocator_ops {
	int (*refill)(struct kvm_hyp_memcache *host_mc);
	int (*reclaimable)(void);
	void (*reclaim)(struct kvm_hyp_memcache *host_mc, int target);
};

int hyp_alloc_mgt_refill(unsigned long id, struct kvm_hyp_memcache *host_mc);
int hyp_alloc_mgt_reclaimable(void);
void hyp_alloc_mgt_reclaim(struct kvm_hyp_memcache *host_mc, int target);

#endif /* __KVM_NVHE_HYP_ALLOC_MGT__ */
