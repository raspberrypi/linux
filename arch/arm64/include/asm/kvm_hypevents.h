/* SPDX-License-Identifier: GPL-2.0 */

#if !defined(__ARM64_KVM_HYPEVENTS_H_) || defined(HYP_EVENT_MULTI_READ)
#define __ARM64_KVM_HYPEVENTS_H_

#ifdef __KVM_NVHE_HYPERVISOR__
#include <nvhe/trace/trace.h>
#endif

/*
 * Hypervisor events definitions.
 */

HYP_EVENT(hyp_enter,
	HE_PROTO(void),
	HE_STRUCT(
	),
	HE_ASSIGN(
	),
	HE_PRINTK(" ")
);

HYP_EVENT(hyp_exit,
	HE_PROTO(void),
	HE_STRUCT(
	),
	HE_ASSIGN(
	),
	HE_PRINTK(" ")
);

HYP_EVENT(host_hcall,
	HE_PROTO(unsigned int id, u8 invalid),
	HE_STRUCT(
		he_field(unsigned int, id)
		he_field(u8, invalid)
	),
	HE_ASSIGN(
		__entry->id = id;
		__entry->invalid = invalid;
	),
	HE_PRINTK("id=%u invalid=%u",
		  __entry->id, __entry->invalid)
);

HYP_EVENT(host_smc,
	HE_PROTO(u64 id, u8 forwarded),
	HE_STRUCT(
		he_field(u64, id)
		he_field(u8, forwarded)
	),
	HE_ASSIGN(
		__entry->id = id;
		__entry->forwarded = forwarded;
	),
	HE_PRINTK("id=%llu forwarded=%u",
		  __entry->id, __entry->forwarded)
);


HYP_EVENT(host_mem_abort,
	HE_PROTO(u64 esr, u64 addr),
	HE_STRUCT(
		he_field(u64, esr)
		he_field(u64, addr)
	),
	HE_ASSIGN(
		__entry->esr = esr;
		__entry->addr = addr;
	),
	HE_PRINTK("esr=0x%llx addr=0x%llx",
		  __entry->esr, __entry->addr)
);

HYP_EVENT(__hyp_printk,
	HE_PROTO(const char *fmt, u64 a, u64 b, u64 c, u64 d),
	HE_STRUCT(
		he_field(u8, fmt_id)
		he_field(u64, a)
		he_field(u64, b)
		he_field(u64, c)
		he_field(u64, d)
	),
	HE_ASSIGN(
		__entry->fmt_id = hyp_printk_fmt_to_id(fmt);
		__entry->a = a;
		__entry->b = b;
		__entry->c = c;
		__entry->d = d;
	),
	HE_PRINTK_UNKNOWN_FMT(hyp_printk_fmt_from_id(__entry->fmt_id),
		__entry->a, __entry->b, __entry->c, __entry->d)
);
#endif
