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
#define GUNYAH_HYPERCALL_MSGQ_SEND		GUNYAH_HYPERCALL(0x801B)
#define GUNYAH_HYPERCALL_MSGQ_RECV		GUNYAH_HYPERCALL(0x801C)
#define GUNYAH_HYPERCALL_VCPU_RUN		GUNYAH_HYPERCALL(0x8065)
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

/**
 * gunyah_hypercall_msgq_send() - Send a buffer on a message queue
 * @capid: capability ID of the message queue to add message
 * @size: Size of @buff
 * @buff: Address of buffer to send
 * @tx_flags: See GUNYAH_HYPERCALL_MSGQ_TX_FLAGS_*
 * @ready: If the send was successful, ready is filled with true if more
 *         messages can be sent on the queue. If false, then the tx IRQ will
 *         be raised in future when send can succeed.
 */
enum gunyah_error gunyah_hypercall_msgq_send(u64 capid, size_t size, void *buff,
					     u64 tx_flags, bool *ready)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GUNYAH_HYPERCALL_MSGQ_SEND, capid, size,
			  (uintptr_t)buff, tx_flags, 0, &res);

	if (res.a0 == GUNYAH_ERROR_OK)
		*ready = !!res.a1;

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_msgq_send);

/**
 * gunyah_hypercall_msgq_recv() - Send a buffer on a message queue
 * @capid: capability ID of the message queue to add message
 * @buff: Address of buffer to copy received data into
 * @size: Size of @buff
 * @recv_size: If the receive was successful, recv_size is filled with the
 *             size of data received. Will be <= size.
 * @ready: If the receive was successful, ready is filled with true if more
 *         messages are ready to be received on the queue. If false, then the
 *         rx IRQ will be raised in future when recv can succeed.
 */
enum gunyah_error gunyah_hypercall_msgq_recv(u64 capid, void *buff, size_t size,
					     size_t *recv_size, bool *ready)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GUNYAH_HYPERCALL_MSGQ_RECV, capid, (uintptr_t)buff,
			  size, 0, &res);

	if (res.a0 == GUNYAH_ERROR_OK) {
		*recv_size = res.a1;
		*ready = !!res.a2;
	}

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_msgq_recv);

/**
 * gunyah_hypercall_vcpu_run() - Donate CPU time to a vcpu
 * @capid: capability ID of the vCPU to run
 * @resume_data: Array of 3 state-specific resume data
 * @resp: Filled reason why vCPU exited when return value is GUNYAH_ERROR_OK
 *
 * See also:
 * https://github.com/quic/gunyah-hypervisor/blob/develop/docs/api/gunyah_api.md#run-a-proxy-scheduled-vcpu-thread
 */
enum gunyah_error
gunyah_hypercall_vcpu_run(u64 capid, unsigned long *resume_data,
			  struct gunyah_hypercall_vcpu_run_resp *resp)
{
	struct arm_smccc_1_2_regs args = {
		.a0 = GUNYAH_HYPERCALL_VCPU_RUN,
		.a1 = capid,
		.a2 = resume_data[0],
		.a3 = resume_data[1],
		.a4 = resume_data[2],
		/* C language says this will be implictly zero. Gunyah requires 0, so be explicit */
		.a5 = 0,
	};
	struct arm_smccc_1_2_regs res;

	arm_smccc_1_2_hvc(&args, &res);
	if (res.a0 == GUNYAH_ERROR_OK) {
		resp->sized_state = res.a1;
		resp->state_data[0] = res.a2;
		resp->state_data[1] = res.a3;
		resp->state_data[2] = res.a4;
	}

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_vcpu_run);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gunyah Hypervisor Hypercalls");
