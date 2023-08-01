/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARM64_KVM_HYPTRACE_H_
#define __ARM64_KVM_HYPTRACE_H_
#include <asm/kvm_hyp.h>

#include <linux/ring_buffer.h>
#include <linux/trace_seq.h>
#include <linux/workqueue.h>

struct ht_iterator {
	struct hyp_trace_buffer	*hyp_buffer;
	int			cpu;
	struct hyp_entry_hdr	*ent;
	unsigned long		lost_events;
	int			ent_cpu;
	size_t			ent_size;
	u64			ts;
	void			*spare;
	size_t			copy_leftover;
	struct trace_seq        seq;
	struct delayed_work     poll_work;
};

struct hyp_trace_desc {
	struct kvm_nvhe_clock_data	clock_data;
	struct trace_page_desc		page_desc;

};
#endif
