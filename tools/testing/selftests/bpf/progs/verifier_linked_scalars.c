// SPDX-License-Identifier: GPL-2.0

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include "bpf_misc.h"

SEC("socket")
__description("scalars: find linked scalars")
__failure
__msg("math between fp pointer and 2147483647 is not allowed")
__naked void scalars(void)
{
	asm volatile ("				\
	r0 = 0;					\
	r1 = 0x80000001 ll;			\
	r1 /= 1;				\
	r2 = r1;				\
	r4 = r1;				\
	w2 += 0x7FFFFFFF;			\
	w4 += 0;				\
	if r2 == 0 goto l1;			\
	exit;					\
l1:						\
	r4 >>= 63;				\
	r3 = 1;					\
	r3 -= r4;				\
	r3 *= 0x7FFFFFFF;			\
	r3 += r10;				\
	*(u8*)(r3 - 1) = r0;			\
	exit;					\
"	::: __clobber_all);
}

/*
 * Test that r += r (self-add, src_reg == dst_reg) clears the scalar ID
 * so that sync_linked_regs() does not propagate an incorrect delta.
 */
SEC("socket")
__failure
__msg("div by zero")
__naked void scalars_self_add_clears_id(void)
{
	asm volatile ("						\
	call %[bpf_get_prandom_u32];				\
	r6 = r0;		/* r6 unknown, id A */		\
	r7 = r6;		/* r7 linked to r6, id A */	\
	call %[bpf_get_prandom_u32];				\
	r8 = r0;		/* r8 unknown, id B */		\
	r9 = r8;		/* r9 linked to r8, id B */	\
	if r7 != 1 goto l_exit_%=;				\
	/* r7 == 1; sync propagates: r6 = 1 (known, id A) */	\
	r6 += r6;		/* r6 = 2; should clear id */	\
	if r7 == r9 goto l_exit_%=;				\
	/* Bug: r6 synced to r7(1)+delta(2)=3; Fix: r6 = 2 */	\
	if r6 == 3 goto l_exit_%=;				\
	r0 /= 0;						\
l_exit_%=:							\
	r0 = 0;							\
	exit;							\
"	:
	: __imm(bpf_get_prandom_u32)
	: __clobber_all);
}

/* Same as above but with alu32 such that w6 += w6 also clears id. */
SEC("socket")
__failure
__msg("div by zero")
__naked void scalars_self_add_alu32_clears_id(void)
{
	asm volatile ("						\
	call %[bpf_get_prandom_u32];				\
	w6 = w0;						\
	w7 = w6;						\
	call %[bpf_get_prandom_u32];				\
	w8 = w0;						\
	w9 = w8;						\
	if w7 != 1 goto l_exit_%=;				\
	w6 += w6;						\
	if w7 == w9 goto l_exit_%=;				\
	if w6 == 3 goto l_exit_%=;				\
	r0 /= 0;						\
l_exit_%=:							\
	r0 = 0;							\
	exit;							\
"	:
	: __imm(bpf_get_prandom_u32)
	: __clobber_all);
}

/*
 * Test that stale delta from a cleared BPF_ADD_CONST does not leak
 * through assign_scalar_id_before_mov() into a new id, causing
 * sync_linked_regs() to compute an incorrect offset.
 */
SEC("socket")
__failure
__msg("div by zero")
__naked void scalars_stale_delta_from_cleared_id(void)
{
	asm volatile ("						\
	call %[bpf_get_prandom_u32];				\
	r6 = r0;		/* r6 unknown, gets id A */	\
	r6 += 5;		/* id A|ADD_CONST, delta 5 */	\
	r6 ^= 0;		/* id cleared; delta stays 5 */	\
	r8 = r6;		/* new id B, stale delta 5 */	\
	r8 += 3;		/* id B|ADD_CONST, delta 3 */	\
	r9 = r6;		/* id B, stale delta 5 */	\
	if r9 != 10 goto l_exit_%=;				\
	/* Bug: r8 = 10+(3-5) = 8; Fix: r8 = 10+(3-0) = 13 */	\
	if r8 == 8 goto l_exit_%=;				\
	r0 /= 0;						\
l_exit_%=:							\
	r0 = 0;							\
	exit;							\
"	:
	: __imm(bpf_get_prandom_u32)
	: __clobber_all);
}

/* Same as above but with alu32. */
SEC("socket")
__failure
__msg("div by zero")
__naked void scalars_stale_delta_from_cleared_id_alu32(void)
{
	asm volatile ("						\
	call %[bpf_get_prandom_u32];				\
	w6 = w0;						\
	w6 += 5;						\
	w6 ^= 0;						\
	w8 = w6;						\
	w8 += 3;						\
	w9 = w6;						\
	if w9 != 10 goto l_exit_%=;				\
	if w8 == 8 goto l_exit_%=;				\
	r0 /= 0;						\
l_exit_%=:							\
	r0 = 0;							\
	exit;							\
"	:
	: __imm(bpf_get_prandom_u32)
	: __clobber_all);
}

char _license[] SEC("license") = "GPL";
