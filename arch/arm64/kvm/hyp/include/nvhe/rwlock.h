/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A stand-alone rwlock implementation for use by the non-VHE KVM
 * hypervisor code running at EL2. This is *not* a fair lock and is
 * likely to scale very badly under contention.
 *
 * Copyright (C) 2022 Google LLC
 * Author: Will Deacon <will@kernel.org>
 *
 * Heavily based on the implementation removed by 087133ac9076 which was:
 * Copyright (C) 2012 ARM Ltd.
 */

#ifndef __ARM64_KVM_NVHE_RWLOCK_H__
#define __ARM64_KVM_NVHE_RWLOCK_H__

#include <linux/bits.h>

typedef struct {
	u32	__val;
} hyp_rwlock_t;

#define __HYP_RWLOCK_INITIALIZER \
	{ .__val = 0 }

#define __HYP_RWLOCK_UNLOCKED \
	((hyp_rwlock_t) __HYP_RWLOCK_INITIALIZER)

#define DEFINE_HYP_RWLOCK(x)	hyp_rwlock_t x = __HYP_RWLOCK_UNLOCKED

#define hyp_rwlock_init(l)						\
do {									\
	*(l) = __HYP_RWLOCK_UNLOCKED;					\
} while (0)

#define __HYP_RWLOCK_WRITER_BIT	31

static inline void hyp_write_lock(hyp_rwlock_t *lock)
{
	u32 tmp;

	asm volatile(ARM64_LSE_ATOMIC_INSN(
	/* LL/SC */
	"	sevl\n"
	"1:	wfe\n"
	"2:	ldaxr	%w0, %1\n"
	"	cbnz	%w0, 1b\n"
	"	stxr	%w0, %w2, %1\n"
	"	cbnz	%w0, 2b\n"
	__nops(1),
	/* LSE atomics */
	"1:	mov	%w0, wzr\n"
	"2:	casa	%w0, %w2, %1\n"
	"	cbz	%w0, 3f\n"
	"	ldxr	%w0, %1\n"
	"	cbz	%w0, 2b\n"
	"	wfe\n"
	"	b	1b\n"
	"3:")
	: "=&r" (tmp), "+Q" (lock->__val)
	: "r" (BIT(__HYP_RWLOCK_WRITER_BIT))
	: "memory");
}

static inline void hyp_write_unlock(hyp_rwlock_t *lock)
{
	asm volatile(ARM64_LSE_ATOMIC_INSN(
	"	stlr	wzr, %0",
	"	swpl	wzr, wzr, %0")
	: "=Q" (lock->__val) :: "memory");
}

static inline void hyp_read_lock(hyp_rwlock_t *lock)
{
	u32 tmp, tmp2;

	asm volatile(
	"	sevl\n"
	ARM64_LSE_ATOMIC_INSN(
	/* LL/SC */
	"1:	wfe\n"
	"2:	ldaxr	%w0, %2\n"
	"	add	%w0, %w0, #1\n"
	"	tbnz	%w0, %3, 1b\n"
	"	stxr	%w1, %w0, %2\n"
	"	cbnz	%w1, 2b\n"
	__nops(1),
	/* LSE atomics */
	"1:	wfe\n"
	"2:	ldxr	%w0, %2\n"
	"	adds	%w1, %w0, #1\n"
	"	tbnz	%w1, %3, 1b\n"
	"	casa	%w0, %w1, %2\n"
	"	sbc	%w0, %w1, %w0\n"
	"	cbnz	%w0, 2b")
	: "=&r" (tmp), "=&r" (tmp2), "+Q" (lock->__val)
	: "i" (__HYP_RWLOCK_WRITER_BIT)
	: "cc", "memory");
}

static inline void hyp_read_unlock(hyp_rwlock_t *lock)
{
	u32 tmp, tmp2;

	asm volatile(ARM64_LSE_ATOMIC_INSN(
	/* LL/SC */
	"1:	ldxr	%w0, %2\n"
	"	sub	%w0, %w0, #1\n"
	"	stlxr	%w1, %w0, %2\n"
	"	cbnz	%w1, 1b",
	/* LSE atomics */
	"	movn	%w0, #0\n"
	"	staddl	%w0, %2\n"
	__nops(2))
	: "=&r" (tmp), "=&r" (tmp2), "+Q" (lock->__val)
	:
	: "memory");
}

#ifdef CONFIG_NVHE_EL2_DEBUG
static inline void hyp_assert_write_lock_held(hyp_rwlock_t *lock)
{
	BUG_ON(!(READ_ONCE(lock->__val) & BIT(__HYP_RWLOCK_WRITER_BIT)));
}
#else
static inline void hyp_assert_write_lock_held(hyp_rwlock_t *lock) { }
#endif

#endif	/* __ARM64_KVM_NVHE_RWLOCK_H__ */
