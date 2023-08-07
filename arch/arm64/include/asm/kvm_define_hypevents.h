/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/trace_events.h>

#include <asm/kvm_hyptrace.h>
#include <asm/kvm_hypevents_defs.h>

#ifndef HYP_EVENT_FILE
# undef __ARM64_KVM_HYPEVENTS_H_
# define  __HYP_EVENT_FILE <asm/kvm_hypevents.h>
#else
# define __HYP_EVENT_FILE __stringify(HYP_EVENT_FILE)
#endif

#define HYP_EVENT(__name, __proto, __struct, __assign, __printk)		\
	HYP_EVENT_FORMAT(__name, __struct);					\
	void hyp_event_trace_##__name(struct ht_iterator *iter)			\
	{									\
		struct trace_hyp_format_##__name __maybe_unused *__entry =	\
			(struct trace_hyp_format_##__name *)iter->ent;		\
		trace_seq_puts(&iter->seq, #__name);				\
		trace_seq_putc(&iter->seq, ' ');				\
		trace_seq_printf(&iter->seq, __printk);				\
		trace_seq_putc(&iter->seq, '\n');				\
	}
#define HYP_EVENT_MULTI_READ
#include __HYP_EVENT_FILE

#undef he_field
#define he_field(_type, _item)						\
	{								\
		.type = #_type, .name = #_item,				\
		.size = sizeof(_type), .align = __alignof__(_type),	\
		.is_signed = is_signed_type(_type),			\
	},
#undef HYP_EVENT
#define HYP_EVENT(__name, __proto, __struct, __assign, __printk)		\
	static struct trace_event_fields hyp_event_fields_##__name[] = {	\
		__struct							\
		{}								\
	};
#include __HYP_EVENT_FILE

#undef HYP_EVENT
#undef HE_PRINTK
#undef HE_PRINTK_UNKNOWN_FMT
#define __entry REC
#define HE_PRINTK(fmt, args...) "\"" fmt "\", " __stringify(args)
#define HE_PRINTK_UNKNOWN_FMT(fmt, args...) "Unknown"
#define HYP_EVENT(__name, __proto, __struct, __assign, __printk)	\
	static char hyp_event_print_fmt_##__name[] = __printk;		\
	static bool hyp_event_enabled_##__name;				\
	struct hyp_event __section("_hyp_events") hyp_event_##__name = {\
		.name		= #__name,				\
		.enabled	= &hyp_event_enabled_##__name,		\
		.fields		= hyp_event_fields_##__name,		\
		.print_fmt	= hyp_event_print_fmt_##__name,		\
		.trace_func	= hyp_event_trace_##__name,		\
	}
#include __HYP_EVENT_FILE

#undef HYP_EVENT_MULTI_READ
