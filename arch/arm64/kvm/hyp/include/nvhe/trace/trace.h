/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARM64_KVM_HYP_NVHE_TRACE_H
#define __ARM64_KVM_HYP_NVHE_TRACE_H
#include <asm/kvm_hyptrace.h>
#include <asm/kvm_hypevents_defs.h>

#ifdef CONFIG_TRACING
void *tracing_reserve_entry(unsigned long length);
void tracing_commit_entry(void);
int register_hyp_event_ids(unsigned long start, unsigned long end);

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

extern char __hyp_printk_fmts_start[];

static inline u8 hyp_printk_fmt_to_id(const char *fmt)
{
	return (fmt - __hyp_printk_fmts_start) / sizeof(struct hyp_printk_fmt);
}

#define __trace_hyp_printk(__fmt, a, b, c, d)		\
do {							\
	static struct hyp_printk_fmt __used		\
			__section(".hyp.printk_fmts")	\
			ht_fmt = {			\
				.fmt = __fmt		\
	};						\
	trace___hyp_printk(ht_fmt.fmt, a, b, c, d);	\
} while (0)

#define __trace_hyp_printk_0(fmt, arg)		\
	__trace_hyp_printk(fmt, 0, 0, 0, 0)
#define __trace_hyp_printk_1(fmt, a)		\
	__trace_hyp_printk(fmt, a, 0, 0, 0)
#define __trace_hyp_printk_2(fmt, a, b)		\
	__trace_hyp_printk(fmt, a, b, 0, 0)
#define __trace_hyp_printk_3(fmt, a, b, c)	\
	__trace_hyp_printk(fmt, a, b, c, 0)
#define __trace_hyp_printk_4(fmt, a, b, c, d) \
	__trace_hyp_printk(fmt, a, b, c, d)

#define __trace_hyp_printk_N(fmt, ...) \
	CONCATENATE(__trace_hyp_printk_, COUNT_ARGS(__VA_ARGS__))(fmt, ##__VA_ARGS__)

#define trace_hyp_printk(fmt, ...) \
	__trace_hyp_printk_N(fmt, __VA_ARGS__)
#else
static inline void *tracing_reserve_entry(unsigned long length) { return NULL; }
static inline void tracing_commit_entry(void) { }
static inline int register_hyp_event_ids(unsigned long start, unsigned long end)
{
	return -ENODEV;
}

#define HYP_EVENT(__name, __proto, __struct, __assign, __printk)      \
	static inline void trace_##__name(__proto) {}

static inline int __pkvm_load_tracing(unsigned long desc_va, size_t desc_size) { return -ENODEV; }
static inline void __pkvm_teardown_tracing(void) { }
static inline int __pkvm_enable_tracing(bool enable) { return -ENODEV; }
static inline int __pkvm_swap_reader_tracing(int cpu) { return -ENODEV; }
static inline int __pkvm_enable_event(unsigned short id, bool enable)  { return -ENODEV; }
#define trace_hyp_printk(fmt, ...)
#endif
#endif
