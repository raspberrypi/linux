/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARM64_KVM_HYP_NVHE_TRACE_H
#define __ARM64_KVM_HYP_NVHE_TRACE_H
#include <asm/kvm_hyptrace.h>

/* Internal struct that needs export for hyp-constants.c */
struct hyp_buffer_page {
	struct list_head	list;
	struct buffer_data_page	*page;
	unsigned long		write;
	unsigned long		entries;
	u32			id;
};

#ifdef CONFIG_TRACING
void *tracing_reserve_entry(unsigned long length);
void tracing_commit_entry(void);

int __pkvm_load_tracing(unsigned long desc_va, size_t desc_size);
void __pkvm_teardown_tracing(void);
int __pkvm_enable_tracing(bool enable);
int __pkvm_swap_reader_tracing(int cpu);
#else
static inline void *tracing_reserve_entry(unsigned long length) { return NULL; }
static inline void tracing_commit_entry(void) { }

static inline int __pkvm_load_tracing(unsigned long desc_va, size_t desc_size) { return -ENODEV; }
static inline void __pkvm_teardown_tracing(void) { }
static inline int __pkvm_enable_tracing(bool enable) { return -ENODEV; }
static inline int __pkvm_swap_reader_tracing(int cpu) { return -ENODEV; }
#endif
#endif
