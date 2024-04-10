// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 - Google Inc
 * Author: Mostafa Saleh <smostafa@google.com>
 * Simple module for pKVM SMC filtering.
 */

#include <asm/kvm_pkvm_module.h>
#include <linux/arm-smccc.h>
#include <linux/bsearch.h>

#include "events.h"
#define HYP_EVENT_FILE ../../../../drivers/misc/pkvm-smc/pkvm/events.h
#include <define_events.h>

const struct pkvm_module_ops *pkvm_ops;
bool permissive;

#ifdef CONFIG_TRACING
extern char __hyp_event_ids_start[];
extern char __hyp_event_ids_end[];

void *tracing_reserve_entry(unsigned long length)
{
	return pkvm_ops->tracing_reserve_entry(length);
}

void tracing_commit_entry(void)
{
	pkvm_ops->tracing_commit_entry();
}
#endif

struct pkvm_smc_filter {
	u64 smc_id;
	bool (*cb)(struct user_pt_regs *regs); /* Forward unconditionally if NULL. */
};

static bool deny_smc(struct user_pt_regs *regs)
{
	trace_filtered_smc(regs->regs[0]);

	if (permissive)
		return false;

	regs->regs[0] = SMCCC_RET_NOT_SUPPORTED;
	return true;
}

/*
 * Must be sorted.
 * Allow SMCCCs that are known to be safe.
 * PSCI and FFA are already handled by the hypervisor.
 */
const struct pkvm_smc_filter allow_list[] = {
	/* Trusted OS Calls: Trusty Trusted OS (Yielding) */
	{0x32000014, NULL,}, /* SMC_SC_VIRTIO_GET_DESCR. */
	{0x32000015, NULL,}, /* SMC_SC_VIRTIO_START. */
	{0x32000016, NULL,}, /* SMC_SC_VIRTIO_STOP. */
	{0x32000017, NULL,}, /* SMC_SC_VDEV_RESET. */
	{0x32000018, NULL,}, /* SMC_SC_VDEV_KICK_VQ. */
	{0x32000019, NULL,}, /* SMC_NC_VDEV_KICK_VQ. */
	{0x3200001E, NULL,}, /* SMC_SC_CREATE_QL_TIPC_DEV. */
	{0x3200001F, NULL,}, /* SMC_SC_SHUTDOWN_QL_TIPC_DEV. */
	{0x32000020, NULL,}, /* SMC_SC_HANDLE_QL_TIPC_DEV_CMD. */
	{0x32000021, NULL,}, /* SMC_FC_HANDLE_QL_TIPC_DEV_CMD. */

	/* Trusted OS Calls: Trusty Secure Monitor (Yielding) */
	{0x3C000000, NULL,}, /* SMC_SC_RESTART_LAST. */
	{0x3C000001, NULL,}, /* SMC_SC_LOCKED_NOP. */
	{0x3C000002, NULL,}, /* SMC_SC_RESTART_FIQ. */
	{0x3C000003, NULL,}, /* SMC_SC_NOP. */
	{0x3C000004, NULL,}, /* SMC_SC_SCHED_SHARE_REGISTER. */
	{0x3C000005, NULL,}, /* SMC_SC_SCHED_SHARE_UNREGISTER. */

	/* Arm Architecture Calls. */
	{0x80000000, NULL}, /* SMCCC_VERSION. */
	{0x80000001, NULL}, /* SMCCC_ARCH_FEATURES. */
	{0x80000002, NULL}, /* SMCCC_ARCH_SOC_ID. */

	/* Standard Secure services: TRNG */
	{0x84000050, NULL,}, /* TRNG_VERSION. */
	{0x84000051, NULL,}, /* TRNG_FEATURES. */
	{0x84000052, NULL,}, /* TRNG_GET_UUID. */
	{0x84000053, NULL,}, /* TRNG_RND. */

	/* Trusted OS Calls: Trusty Secure Monitor (Fast) */
	{0xBC000001, NULL,}, /* SMC_FC_FIQ_EXIT. */
	{0xBC000002, NULL,}, /* SMC_FC_REQUEST_FIQ. */
	{0xBC000003, NULL,}, /* SMC_FC_GET_NEXT_IRQ. */
	{0xBC000007, NULL,}, /* SMC_FC_CPU_SUSPEND. */
	{0xBC000008, NULL,}, /* SMC_FC_CPU_RESUME. */
	{0xBC000009, NULL,}, /* SMC_FC_AARCH_SWITCH. */
	{0xBC00000A, NULL,}, /* SMC_FC_GET_VERSION_STR. */
	{0xBC00000B, NULL,}, /* SMC_FC_API_VERSION. */
	{0xBC00000C, NULL,}, /* SMC_FC_FIQ_RESUME. */
	{0xBC00000D, NULL,}, /* SMC_FC_GET_SMP_MAX_CPUS. */
};

static inline int match_smc(const void *key, const void *elt)
{
	u64 smc_id = ((struct pkvm_smc_filter *)key)->smc_id;
	u64 cur_id = ((struct pkvm_smc_filter *)elt)->smc_id;

	return smc_id - cur_id;
}

/*
 * Block all by default.
 * return false will allow the SMC to be forwarded.
 */
bool filter_smc(struct user_pt_regs *regs)
{
	u64 smc_id = regs->regs[0];
	/*
	 * Ignore bits that doesn't change the functionality:
	 * Bit[30]: 32/64 bit convention
	 * Bit[16]: SVE hint
	 */
	u64 mask = ~(ARM_SMCCC_1_3_SVE_HINT | BIT(ARM_SMCCC_CALL_CONV_SHIFT));
	struct pkvm_smc_filter pval = {smc_id & mask, NULL};
	struct pkvm_smc_filter *entry;

	/* alternatively, we can do 2 level binary search or switch case by service. */
	entry = (struct pkvm_smc_filter *)__inline_bsearch((void *)&pval, allow_list,
							   ARRAY_SIZE(allow_list),
							   sizeof(allow_list[0]),
							   match_smc);
	if (!entry)
		return deny_smc(regs);

	return entry->cb ? entry->cb(regs) : false;
}

int pkvm_smc_filter_hyp_init(const struct pkvm_module_ops *ops)
{
#ifdef CONFIG_TRACING
	ops->register_hyp_event_ids((unsigned long)__hyp_event_ids_start,
				    (unsigned long)__hyp_event_ids_end);
#endif
	pkvm_ops = ops;
	return ops->register_host_smc_handler(filter_smc);
}
