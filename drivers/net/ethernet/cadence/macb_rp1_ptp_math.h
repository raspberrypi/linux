/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RP1_PEROUT_MATH_H
#define RP1_PEROUT_MATH_H
#ifdef __KERNEL__
#include <linux/math64.h>
#define OUT_RATE_DEN BIT_ULL(26)
#else
typedef unsigned long long u64;
typedef unsigned int u32;
#define div64_u64(n, d) ((u64)(n) / (u64)(d))
#define OUT_RATE_DEN 67108864ULL
#endif

/* 50 MHz TSU increment in Q24 divided by four 200 MHz PIO cycles. */
#define OUT_SECOND 1000000000ULL
#define OUT_GUARD_CYCLES 60000000U
#define OUT_COMPACT_CHECKPOINT_CYCLES 5U
#define OUT_PREP_LEAD_NS 20000000ULL
#define OUT_UPDATE_GUARD_NS 100000ULL
#define OUT_COUNTER_SEED 0xffffffffU
#define OUT_FINE_OFFSET_CYCLES 10U
#define OUT_FINE_TICK_CYCLES 4U
#define OUT_MIN_LEAD_NS 800000000ULL
static inline u64 out_cycles_ns(u64 cycles, u32 rate)
{
	return div64_u64(cycles * rate + OUT_RATE_DEN / 2, OUT_RATE_DEN);
}

static inline u64 out_ns_cycles(u64 ns, u32 rate)
{
	return div64_u64(ns * OUT_RATE_DEN + rate / 2, rate);
}

/* Q16 cycle accumulation preserves sub-cycle phase across rate changes. */
static inline u64 out_ns_cycles_fp(u64 ns, u32 rate)
{
#ifdef __KERNEL__
	return mul_u64_u64_div_u64(ns, OUT_RATE_DEN << 16, rate);
#else
	return (u64)(((__uint128_t)ns * (OUT_RATE_DEN << 16)) / rate);
#endif
}

/* Inverse mapping for a Q16 PIO-cycle delta, rounded down to PHC ns. */
static inline u64 out_cycles_fp_ns(u64 cycles_fp, u32 rate)
{
#ifdef __KERNEL__
	return mul_u64_u64_div_u64(cycles_fp, rate, OUT_RATE_DEN << 16);
#else
	return (u64)(((__uint128_t)cycles_fp * rate) / (OUT_RATE_DEN << 16));
#endif
}

static inline u32 out_deadline_word(u64 cycles_fp)
{
	u64 fine_fp = cycles_fp - ((u64)OUT_FINE_OFFSET_CYCLES << 16);
	u64 ticks = (fine_fp + ((u64)OUT_FINE_TICK_CYCLES << 15)) /
		    ((u64)OUT_FINE_TICK_CYCLES << 16);
	return OUT_COUNTER_SEED - ticks;
}

static inline u64 out_next_grid(u64 earliest, u32 phase)
{
	u64 next = div64_u64(earliest, OUT_SECOND) * OUT_SECOND + phase;

	return next < earliest ? next + OUT_SECOND : next;
}
#endif
