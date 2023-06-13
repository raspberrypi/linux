/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ARM64_KVM_HYP_TRACE_H__
#define __ARM64_KVM_HYP_TRACE_H__

#include <linux/trace_seq.h>
#include <linux/workqueue.h>

struct ht_iterator {
	struct trace_buffer	*trace_buffer;
	int			cpu;
	struct hyp_entry_hdr	*ent;
	unsigned long		lost_events;
	int			ent_cpu;
	size_t			ent_size;
	u64			ts;
	void			*spare;
	size_t			copy_leftover;
	struct trace_seq	seq;
	struct delayed_work	poll_work;
};

#ifdef CONFIG_TRACING
int hyp_trace_init_tracefs(void);
#else
static inline int hyp_trace_init_tracefs(void) { return 0; }
#endif
#endif
