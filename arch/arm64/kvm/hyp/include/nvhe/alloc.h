/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __KVM_NVHE_ALLOC__
#define __KVM_NVHE_ALLOC__
#include <linux/types.h>

#include <asm/kvm_host.h>

/**
 * hyp_alloc() - Allocate memory from the heap allocator
 *
 * @size:	Allocation size in bytes.
 *
 * Return: A pointer to the allocated memory on success, else NULL.
 */
void *hyp_alloc(size_t size);

/**
 * hyp_alloc_account() - Allocate memory from the heap allocator and account
 *
 * Similar to hyp_alloc(). But on success, the allocated memory will be
 * accounted against the vm (@host_kvm) protected_hyp_mem counter. This allows
 * the host to know about detailed footprint of that vm.
 *
 * @size:	Allocation size in bytes.
 * @host_kvm:	Pointer (in the hyp VA space) to the host KVM struct.
 *
 * Return: A pointer to the allocated memory on success, else NULL.
 */
void *hyp_alloc_account(size_t size, struct kvm *host_kvm);

/**
 * hyp_alloc_errno() - Read the errno on allocation error
 *
 * Get the return code from an allocation failure.
 *
 * Return: -ENOMEM if the allocator needs a refill from the host, -E2BIG if
 * there is no VA space left else 0.
 */
int hyp_alloc_errno(void);

/**
 * hyp_free() - Free memory allocated with hyp_alloc()
 *
 * @addr:	Address returned by the original hyp_alloc().
 *
 * The use of any other address than one returned by hyp_alloc() will cause a
 * hypervisor panic.
 */
void hyp_free(void *addr);

/**
 * hyp_free_account() - Free memory allocated with hyp_alloc_account()
 *
 * Similar to hyp_free, but for memory allocated with hyp_alloc_account().
 *
 * @addr:	Address returned by the original hyp_alloc_account().
 * @host_kvm:	pointer (in the hyp VA space) to the host KVM struct.
 *
 * The use of any other address than one returned by hyp_alloc() will cause a
 * hypervisor panic.
 */
void hyp_free_account(void *addr, struct kvm *host_kvm);

int hyp_alloc_init(size_t size);
int hyp_alloc_refill(struct kvm_hyp_memcache *host_mc);
int hyp_alloc_reclaimable(void);
void hyp_alloc_reclaim(struct kvm_hyp_memcache *host_mc, int target);
u8 hyp_alloc_missing_donations(void);

extern struct hyp_mgt_allocator_ops hyp_alloc_ops;
#endif
