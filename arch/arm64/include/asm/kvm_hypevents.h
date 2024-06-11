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

HYP_EVENT(host_ffa_call,
	HE_PROTO(u64 func_id, u64 res_a1, u64 res_a2, u64 res_a3, u64 res_a4, int handled, int err),
	HE_STRUCT(
		he_field(u64, func_id)
		he_field(u64, res_a1)
		he_field(u64, res_a2)
		he_field(u64, res_a3)
		he_field(u64, res_a4)
		he_field(int, handled)
		he_field(int, err)
	),
	HE_ASSIGN(
		__entry->func_id = func_id;
		__entry->res_a1 = res_a1;
		__entry->res_a2 = res_a2;
		__entry->res_a3 = res_a3;
		__entry->res_a4 = res_a4;
		__entry->handled = handled;
		__entry->err = err;
		),
	HE_PRINTK("ffa_func=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx a4=%llx handled=%d err=%d",
		  __entry->func_id, __entry->res_a1, __entry->res_a2,
		  __entry->res_a3, __entry->res_a4, __entry->handled, __entry->err)
);

HYP_EVENT(psci_mem_protect,
	HE_PROTO(u64 count, u64 was),
	HE_STRUCT(
		he_field(u64, count)
		he_field(u64, was)
	),
	HE_ASSIGN(
		__entry->count = count;
		__entry->was = was;
	),
	HE_PRINTK("count=%llu was=%llu", __entry->count, __entry->was)
);

HYP_EVENT(iommu_idmap,
	HE_PROTO(u64 from, u64 to, int prot),
	HE_STRUCT(
		he_field(u64, from)
		he_field(u64, to)
		he_field(int, prot)
	),
	HE_ASSIGN(
		__entry->from = from;
		__entry->to = to;
		__entry->prot = prot;
	),
	HE_PRINTK("from=0x%llx to=0x%llx prot=0x%x", __entry->from, __entry->to, __entry->prot)
);
#endif
