/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Atomics-based checking refcount implementation.
 * Copyright (C) 2023 Google LLC
 * Author: Will Deacon <will@kernel.org>
 */
#ifndef __ARM64_KVM_NVHE_REFCOUNT_H__
#define __ARM64_KVM_NVHE_REFCOUNT_H__

#include <asm/lse.h>

static inline s16 __ll_sc_refcount_fetch_add_16(u16 *refcount, s16 addend)
{
	u16 new;
	u32 flag;

	asm volatile(
	"	prfm	pstl1strm, %[refcount]\n"
	"1:	ldxrh	%w[new], %[refcount]\n"
	"	add	%w[new], %w[new], %w[addend]\n"
	"	stxrh	%w[flag], %w[new], %[refcount]\n"
	"	cbnz	%w[flag], 1b"
	: [refcount] "+Q" (*refcount),
	  [new] "=&r" (new),
	  [flag] "=&r" (flag)
	: [addend] "Ir" (addend));

	return new;
}

#ifdef CONFIG_ARM64_LSE_ATOMICS

static inline s16 __lse_refcount_fetch_add_16(u16 *refcount, s16 addend)
{
	s16 old;

	asm volatile(__LSE_PREAMBLE
	"	ldaddh	%w[addend], %w[old], %[refcount]"
	: [refcount] "+Q" (*refcount),
	  [old] "=r" (old)
	: [addend] "r" (addend));

	return old + addend;
}

#endif /* CONFIG_ARM64_LSE_ATOMICS */

static inline u64 __hyp_refcount_fetch_add(void *refcount, const size_t size,
					   const s64 addend)
{
	s64 new;

	switch (size) {
	case 2:
		new = __lse_ll_sc_body(refcount_fetch_add_16, refcount, addend);
		break;
	default:
		BUILD_BUG_ON_MSG(1, "Unsupported refcount size");
		unreachable();
	}

	BUG_ON(new < 0);
	return new;
}


#define hyp_refcount_inc(r)	__hyp_refcount_fetch_add(&(r), sizeof(r), 1)
#define hyp_refcount_dec(r)	__hyp_refcount_fetch_add(&(r), sizeof(r), -1)
#define hyp_refcount_get(r)	READ_ONCE(r)
#define hyp_refcount_set(r, v)	do {			\
	typeof(r) *__rp = &(r);				\
	WARN_ON(hyp_refcount_get(*__rp));		\
	WRITE_ONCE(*__rp, v);				\
} while (0)

#endif /* __ARM64_KVM_NVHE_REFCOUNT_H__ */
