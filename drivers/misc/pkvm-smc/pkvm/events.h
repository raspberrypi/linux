/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(__PKVM_SMC_FILTER_HYPEVENTS_H_) || defined(HYP_EVENT_MULTI_READ)
#define __PKVM_SMC_FILTER_HYPEVENTS_H_

#ifdef __KVM_NVHE_HYPERVISOR__
#include <trace.h>
#endif

HYP_EVENT(filtered_smc,
	HE_PROTO(u64 smc_id),
	HE_STRUCT(
		he_field(u64, smc_id)
	),
	HE_ASSIGN(
		__entry->smc_id = smc_id;
	),
	HE_PRINTK("smc_id = 0x%08llx", __entry->smc_id)
);
#endif
