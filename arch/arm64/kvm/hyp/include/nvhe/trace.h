/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARM64_KVM_HYP_NVHE_TRACE_H
#define __ARM64_KVM_HYP_NVHE_TRACE_H
#include <asm/kvm_hyptrace.h>
#include <asm/kvm_hypevents_defs.h>

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
#define HYP_EVENT(__name, __proto, __struct, __assign, __printk)		\
	HYP_EVENT_FORMAT(__name, __struct);					\
	extern atomic_t __name##_enabled;					\
	extern struct hyp_event_id hyp_event_id_##__name;			\
	static inline void trace_##__name(__proto)				\
	{									\
		size_t length = sizeof(struct trace_hyp_format_##__name);	\
		struct trace_hyp_format_##__name *__entry;			\
										\
		if (!atomic_read(&__name##_enabled))				\
			return;							\
		__entry = tracing_reserve_entry(length);			\
		if (!__entry)							\
			return;							\
		__entry->hdr.id = hyp_event_id_##__name.id;			\
		__assign							\
		tracing_commit_entry();						\
	}

int __pkvm_load_tracing(unsigned long desc_va, size_t desc_size);
void __pkvm_teardown_tracing(void);
int __pkvm_enable_tracing(bool enable);
int __pkvm_swap_reader_tracing(int cpu);
int __pkvm_enable_event(unsigned short id, bool enable);
#else
static inline void *tracing_reserve_entry(unsigned long length) { return NULL; }
static inline void tracing_commit_entry(void) { }
#define HYP_EVENT(__name, __proto, __struct, __assign, __printk)      \
	static inline void trace_##__name(__proto) {}

static inline int __pkvm_load_tracing(unsigned long desc_va, size_t desc_size) { return -ENODEV; }
static inline void __pkvm_teardown_tracing(void) { }
static inline int __pkvm_enable_tracing(bool enable) { return -ENODEV; }
static inline int __pkvm_swap_reader_tracing(int cpu) { return -ENODEV; }
static inline int __pkvm_enable_event(unsigned short id, bool enable)  { return -ENODEV; }
#endif
#endif
