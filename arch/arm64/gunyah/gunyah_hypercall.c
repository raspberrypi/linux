// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/arm-smccc.h>
#include <linux/module.h>
#include <linux/gunyah.h>
#include <linux/uuid.h>

/* {c1d58fcd-a453-5fdb-9265-ce36673d5f14} */
static const uuid_t GUNYAH_UUID = UUID_INIT(0xc1d58fcd, 0xa453, 0x5fdb, 0x92,
					    0x65, 0xce, 0x36, 0x67, 0x3d, 0x5f,
					    0x14);

bool arch_is_gunyah_guest(void)
{
	struct arm_smccc_res res;
	uuid_t uuid;
	u32 *up;

	arm_smccc_1_1_invoke(ARM_SMCCC_VENDOR_HYP_CALL_UID_FUNC_ID, &res);

	up = (u32 *)&uuid.b[0];
	up[0] = lower_32_bits(res.a0);
	up[1] = lower_32_bits(res.a1);
	up[2] = lower_32_bits(res.a2);
	up[3] = lower_32_bits(res.a3);

	return uuid_equal(&uuid, &GUNYAH_UUID);
}
EXPORT_SYMBOL_GPL(arch_is_gunyah_guest);

#define GUNYAH_HYPERCALL(fn)                                      \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64, \
			   ARM_SMCCC_OWNER_VENDOR_HYP, fn)

/* clang-format off */
#define GUNYAH_HYPERCALL_HYP_IDENTIFY		GUNYAH_HYPERCALL(0x8000)
/* clang-format on */

/**
 * gunyah_hypercall_hyp_identify() - Returns build information and feature flags
 *                               supported by Gunyah.
 * @hyp_identity: filled by the hypercall with the API info and feature flags.
 */
void gunyah_hypercall_hyp_identify(
	struct gunyah_hypercall_hyp_identify_resp *hyp_identity)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GUNYAH_HYPERCALL_HYP_IDENTIFY, &res);

	hyp_identity->api_info = res.a0;
	hyp_identity->flags[0] = res.a1;
	hyp_identity->flags[1] = res.a2;
	hyp_identity->flags[2] = res.a3;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_hyp_identify);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gunyah Hypervisor Hypercalls");
