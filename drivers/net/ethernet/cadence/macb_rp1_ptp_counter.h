/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MACB_RP1_PTP_COUNTER_H
#define MACB_RP1_PTP_COUNTER_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdint.h>
typedef long long s64;
#endif
#include "macb_rp1_ptp_math.h"

#define RP1_S64_MAX ((s64)(~0ULL >> 1))
#define RP1_S64_MIN (-RP1_S64_MAX - 1)

#define RP1_COUNTER_LOOP_CYCLES 2
#define RP1_COUNTER_EDGE_CYCLES 4
#define RP1_COUNTER_MAX_Y_DELTA 0x7fffffffU
#define RP1_COUNTER_MAX_RATE_CHANGES 128
#define RP1_COUNTER_MIN_TSU_RATE (18U << 24)
#define RP1_COUNTER_MAX_TSU_RATE (23U << 24)

/* Return the oldest valid cyclic-DMA sequence and number of unread words. */
static inline int rp1_counter_ring_window(u64 produced, u64 consumed,
					  u32 capacity, u64 *first,
					  u32 *available)
{
	if (!capacity || !first || !available || produced < consumed)
		return -EINVAL;
	if (produced - consumed > capacity) {
		*first = produced - capacity;
		*available = capacity;
		return -EOVERFLOW;
	}
	*first = consumed;
	*available = produced - consumed;
	return 0;
}

struct rp1_counter_rate_change {
	u64 before_ns;
	u64 after_ns;
	u32 old_rate;
	u32 new_rate;
};

static inline int rp1_counter_signed_delta(u64 sample, u64 anchor, s64 *delta)
{
	u64 magnitude;

	if (sample >= anchor) {
		magnitude = sample - anchor;
		if (magnitude > (u64)RP1_S64_MAX)
			return -ERANGE;
		*delta = magnitude;
	} else {
		magnitude = anchor - sample;
		if (magnitude > (u64)RP1_S64_MAX)
			return -ERANGE;
		*delta = -(s64)magnitude;
	}

	return 0;
}

/*
 * Convert a bounded interval between two synchronously started counter SMs
 * into PIO cycles. Callers must select an anchor no more than half a 32-bit
 * Y-counter span before the sample. The sequence correction accounts for
 * four PIO instructions spent capturing each edge; the wrap correction
 * accounts for the extra instruction on each Y wrap. This is only a PIO-cycle
 * delta. Mapping cycles to PHC nanoseconds and electrical bias are separate.
 */
static inline int rp1_counter_cycle_delta(u32 anchor_y, u64 anchor_sequence,
					  u64 anchor_wraps, u32 sample_y,
					  u64 sample_sequence, u64 sample_wraps,
					  u64 *cycles)
{
	u32 y_delta = anchor_y - sample_y;
	s64 sequence_delta, wrap_delta, edge_cycles, total;
	int ret;

	if (!cycles || y_delta > RP1_COUNTER_MAX_Y_DELTA)
		return -ERANGE;
	ret = rp1_counter_signed_delta(sample_sequence, anchor_sequence,
				       &sequence_delta);
	if (ret)
		return ret;
	ret = rp1_counter_signed_delta(sample_wraps, anchor_wraps, &wrap_delta);
	if (ret)
		return ret;
	if (sequence_delta > RP1_S64_MAX / RP1_COUNTER_EDGE_CYCLES ||
	    sequence_delta < RP1_S64_MIN / RP1_COUNTER_EDGE_CYCLES)
		return -ERANGE;
	edge_cycles = sequence_delta * RP1_COUNTER_EDGE_CYCLES;
	total = (s64)y_delta * RP1_COUNTER_LOOP_CYCLES;
	if ((edge_cycles > 0 && total > RP1_S64_MAX - edge_cycles) ||
	    (edge_cycles < 0 && total < RP1_S64_MIN - edge_cycles))
		return -ERANGE;
	total += edge_cycles;
	if ((wrap_delta > 0 && total > RP1_S64_MAX - wrap_delta) ||
	    (wrap_delta < 0 && total < RP1_S64_MIN - wrap_delta))
		return -ERANGE;
	total += wrap_delta;
	if (total < 0)
		return -ERANGE;
	*cycles = total;
	return 0;
}

/*
 * Return signed cycles from an anchor sample to another lane's sample.
 * Positive means the sample followed the anchor; negative means it preceded
 * it. Trying both orders handles an event that lands within the four-cycle
 * capture path of an anchor edge while retaining the half-range stale check.
 */
static inline int rp1_counter_signed_cycle_delta(u32 anchor_y,
						 u64 anchor_sequence,
						 u64 anchor_wraps,
						 u32 sample_y,
						 u64 sample_sequence,
						 u64 sample_wraps,
						 s64 *cycles)
{
	u64 magnitude;
	int first, second;

	if (!cycles)
		return -EINVAL;
	first = rp1_counter_cycle_delta(anchor_y, anchor_sequence, anchor_wraps,
					sample_y, sample_sequence, sample_wraps,
					&magnitude);
	if (!first) {
		if (magnitude > (u64)RP1_S64_MAX)
			return -ERANGE;
		*cycles = magnitude;
		return 0;
	}
	second = rp1_counter_cycle_delta(sample_y, sample_sequence, sample_wraps,
					 anchor_y, anchor_sequence, anchor_wraps,
					 &magnitude);
	if (!second) {
		if (magnitude > (u64)RP1_S64_MAX)
			return -ERANGE;
		*cycles = -(s64)magnitude;
		return 0;
	}
	return first == -ERANGE ? second : first;
}

/*
 * Map a cycle delta ending at anchor_ns back into the PHC. Rate-change times
 * are represented by the midpoint of the driver's before/after PHC reads;
 * uncertainty accumulates the full bracket width for every crossed change.
 * oldest_ns is the PHC history floor. Events older than it are rejected.
 */
static inline int rp1_counter_cycles_to_phc(u64 anchor_ns, u32 anchor_rate,
					    u64 cycles, u64 oldest_ns,
					    const struct rp1_counter_rate_change *changes,
					    unsigned int change_count, u64 *event_ns,
					    u64 *uncertainty_ns)
{
	u64 current_ns = anchor_ns;
	u64 remaining_fp, uncertainty = 0;
	u32 rate = anchor_rate;
	unsigned int i;

	if (!event_ns || !uncertainty_ns ||
	    anchor_rate < RP1_COUNTER_MIN_TSU_RATE ||
	    anchor_rate >= RP1_COUNTER_MAX_TSU_RATE ||
	    anchor_ns < oldest_ns || cycles > (~0ULL >> 16) ||
	    (change_count && !changes))
		return -ERANGE;
	if (change_count > RP1_COUNTER_MAX_RATE_CHANGES)
		return -E2BIG;

	for (i = 0; i < change_count; i++) {
		const struct rp1_counter_rate_change *change = &changes[i];

		if (change->old_rate < RP1_COUNTER_MIN_TSU_RATE ||
		    change->old_rate >= RP1_COUNTER_MAX_TSU_RATE ||
		    change->new_rate < RP1_COUNTER_MIN_TSU_RATE ||
		    change->new_rate >= RP1_COUNTER_MAX_TSU_RATE ||
		    change->before_ns > change->after_ns ||
		    change->after_ns > anchor_ns ||
		    (i && change->before_ns < changes[i - 1].after_ns))
			return -EINVAL;
		if (i && change->old_rate != changes[i - 1].new_rate)
			return -ESTALE;
	}
	if (change_count && changes[change_count - 1].new_rate != anchor_rate)
		return -ESTALE;

	remaining_fp = cycles << 16;
	for (i = change_count; i > 0; i--) {
		const struct rp1_counter_rate_change *change = &changes[i - 1];
		u64 midpoint = change->before_ns +
			(change->after_ns - change->before_ns) / 2;
		u64 segment_fp, delta_ns;

		if (midpoint > current_ns)
			return -ESTALE;
		if (midpoint == current_ns) {
			rate = change->old_rate;
			current_ns = midpoint;
			continue;
		}
		segment_fp = out_ns_cycles_fp(current_ns - midpoint, rate);
		if (remaining_fp <= segment_fp) {
			delta_ns = out_cycles_fp_ns(remaining_fp, rate);
			if (delta_ns > current_ns - oldest_ns)
				return -ESTALE;
			*event_ns = current_ns - delta_ns;
			if (change->old_rate != change->new_rate &&
			    *event_ns >= change->before_ns &&
			    *event_ns <= change->after_ns) {
				u64 width = change->after_ns - change->before_ns;

				if (uncertainty > ~0ULL - width)
					return -ERANGE;
				uncertainty += width;
			}
			*uncertainty_ns = uncertainty;
			return 0;
		}

		remaining_fp -= segment_fp;
		if (change->old_rate != change->new_rate) {
			u64 width = change->after_ns - change->before_ns;

			if (uncertainty > ~0ULL - width)
				return -ERANGE;
			uncertainty += width;
		}
		current_ns = midpoint;
		rate = change->old_rate;
	}

	{
		u64 delta_ns = out_cycles_fp_ns(remaining_fp, rate);

		if (delta_ns > current_ns - oldest_ns)
			return -ESTALE;
		*event_ns = current_ns - delta_ns;
	}
	*uncertainty_ns = uncertainty;
	return 0;
}

/*
 * Map a cycle delta forward from a PHC anchor. This is used for an input edge
 * newer than the latest output-marker anchor, so it can be reported without
 * waiting for the following PPS marker. Rate-change records must begin at or
 * after anchor_ns and be ordered. The caller supplies the retained PHC window
 * so a delayed DMA sample cannot silently extrapolate beyond it.
 */
static inline int rp1_counter_cycles_to_phc_forward(u64 anchor_ns,
						    u32 anchor_rate,
						    u64 cycles,
						    u64 oldest_ns,
						    u64 latest_ns,
						    const struct rp1_counter_rate_change *changes,
						    unsigned int change_count,
						    u64 *event_ns,
						    u64 *uncertainty_ns)
{
	u64 current_ns = anchor_ns;
	u64 remaining_fp, uncertainty = 0;
	u32 rate = anchor_rate;
	unsigned int i;

	if (!event_ns || !uncertainty_ns ||
	    anchor_rate < RP1_COUNTER_MIN_TSU_RATE ||
	    anchor_rate >= RP1_COUNTER_MAX_TSU_RATE ||
	    anchor_ns < oldest_ns || anchor_ns > latest_ns ||
	    cycles > (~0ULL >> 16) || (change_count && !changes))
		return -ERANGE;
	if (change_count > RP1_COUNTER_MAX_RATE_CHANGES)
		return -E2BIG;
	remaining_fp = cycles << 16;
	for (i = 0; i < change_count; i++) {
		const struct rp1_counter_rate_change *change = &changes[i];
		u64 midpoint, segment_fp, delta_ns;

		if (change->old_rate < RP1_COUNTER_MIN_TSU_RATE ||
		    change->old_rate >= RP1_COUNTER_MAX_TSU_RATE ||
		    change->new_rate < RP1_COUNTER_MIN_TSU_RATE ||
		    change->new_rate >= RP1_COUNTER_MAX_TSU_RATE ||
		    change->before_ns < anchor_ns ||
		    change->before_ns > change->after_ns ||
		    change->after_ns > latest_ns ||
		    (i && change->before_ns < changes[i - 1].after_ns))
			return -EINVAL;
		if (change->old_rate != rate)
			return -ESTALE;
		midpoint = change->before_ns +
			(change->after_ns - change->before_ns) / 2;
		segment_fp = out_ns_cycles_fp(midpoint - current_ns, rate);
		if (remaining_fp <= segment_fp) {
			delta_ns = out_cycles_fp_ns(remaining_fp, rate);
			if (delta_ns > latest_ns - current_ns)
				return -ESTALE;
			*event_ns = current_ns + delta_ns;
			if (change->old_rate != change->new_rate &&
			    *event_ns >= change->before_ns &&
			    *event_ns <= change->after_ns) {
				u64 width = change->after_ns - change->before_ns;

				if (uncertainty > ~0ULL - width)
					return -ERANGE;
				uncertainty += width;
			}
			*uncertainty_ns = uncertainty;
			return 0;
		}
		remaining_fp -= segment_fp;
		if (change->old_rate != change->new_rate) {
			u64 width = change->after_ns - change->before_ns;

			if (uncertainty > ~0ULL - width)
				return -ERANGE;
			uncertainty += width;
		}
		current_ns = midpoint;
		rate = change->new_rate;
	}
	{
		u64 delta_ns = out_cycles_fp_ns(remaining_fp, rate);

		if (delta_ns > latest_ns - current_ns)
			return -ESTALE;
		*event_ns = current_ns + delta_ns;
	}
	*uncertainty_ns = uncertainty;
	return 0;
}

/* Map one counter-lane sample relative to a PHC-stamped counter anchor. */
static inline int rp1_counter_sample_to_phc(u64 anchor_ns,
					    u32 anchor_rate, u32 anchor_y,
					    u64 anchor_sequence,
					    u64 anchor_wraps,
					    u32 sample_y,
					    u64 sample_sequence,
					    u64 sample_wraps,
					    u64 oldest_ns,
					    u64 latest_ns,
					    const struct rp1_counter_rate_change *changes,
					    unsigned int change_count,
					    u64 *event_ns,
					    u64 *uncertainty_ns)
{
	s64 cycle_delta;
	int ret;

	if (anchor_ns > latest_ns)
		return -ERANGE;
	ret = rp1_counter_signed_cycle_delta(anchor_y, anchor_sequence,
					     anchor_wraps, sample_y,
					     sample_sequence, sample_wraps,
					     &cycle_delta);
	if (ret)
		return ret;
	if (cycle_delta >= 0)
		return rp1_counter_cycles_to_phc_forward(anchor_ns, anchor_rate,
							 cycle_delta, oldest_ns,
							 latest_ns, changes,
							 change_count, event_ns,
							 uncertainty_ns);
	return rp1_counter_cycles_to_phc(anchor_ns, anchor_rate,
					  (u64)-cycle_delta, oldest_ns,
					  changes, change_count, event_ns,
					  uncertainty_ns);
}

#endif
