/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARM64_KVM_HYPTRACE_H_
#define __ARM64_KVM_HYPTRACE_H_
#include <asm/kvm_hyp.h>

#include <linux/ring_buffer.h>

/*
 * Host donations to the hypervisor to store the struct hyp_buffer_page.
 */
struct hyp_buffer_pages_backing {
	unsigned long start;
	size_t size;
};

struct hyp_trace_desc {
	struct hyp_buffer_pages_backing	backing;
	struct kvm_nvhe_clock_data	clock_data;
	struct trace_page_desc		page_desc;

};
#endif
