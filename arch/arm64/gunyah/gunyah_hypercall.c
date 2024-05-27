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
#define GUNYAH_HYPERCALL_BELL_SEND		GUNYAH_HYPERCALL(0x8012)
#define GUNYAH_HYPERCALL_BELL_SET_MASK		GUNYAH_HYPERCALL(0x8015)
#define GUNYAH_HYPERCALL_MSGQ_SEND		GUNYAH_HYPERCALL(0x801B)
#define GUNYAH_HYPERCALL_MSGQ_RECV		GUNYAH_HYPERCALL(0x801C)
#define GUNYAH_HYPERCALL_ADDRSPACE_MAP		GUNYAH_HYPERCALL(0x802B)
#define GUNYAH_HYPERCALL_ADDRSPACE_UNMAP	GUNYAH_HYPERCALL(0x802C)
#define GUNYAH_HYPERCALL_MEMEXTENT_DONATE	GUNYAH_HYPERCALL(0x8061)
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
 * gunyah_hypercall_bell_send() - Assert a gunyah doorbell
 * @capid: capability ID of the doorbell
 * @new_flags: bits to set on the doorbell
 * @old_flags: Filled with the bits set before the send call if return value is GUNYAH_ERROR_OK
 */
enum gunyah_error gunyah_hypercall_bell_send(u64 capid, u64 new_flags, u64 *old_flags)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GUNYAH_HYPERCALL_BELL_SEND, capid, new_flags, 0, &res);

	if (res.a0 == GUNYAH_ERROR_OK && old_flags)
		*old_flags = res.a1;

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_bell_send);

/**
 * gunyah_hypercall_bell_set_mask() - Set masks on a Gunyah doorbell
 * @capid: capability ID of the doorbell
 * @enable_mask: which bits trigger the receiver interrupt
 * @ack_mask: which bits are automatically acknowledged when the receiver
 *            interrupt is ack'd
 */
enum gunyah_error gunyah_hypercall_bell_set_mask(u64 capid, u64 enable_mask, u64 ack_mask)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GUNYAH_HYPERCALL_BELL_SET_MASK, capid, enable_mask, ack_mask, 0, &res);

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_bell_set_mask);

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
 * gunyah_hypercall_addrspace_map() - Add memory to an address space from a memory extent
 * @capid: Address space capability ID
 * @extent_capid: Memory extent capability ID
 * @vbase: location in address space
 * @extent_attrs: Attributes for the memory
 * @flags: Flags for address space mapping
 * @offset: Offset into memory extent (physical address of memory)
 * @size: Size of memory to map; must be page-aligned
 */
enum gunyah_error gunyah_hypercall_addrspace_map(u64 capid, u64 extent_capid, u64 vbase,
					u32 extent_attrs, u32 flags, u64 offset, u64 size)
{
	struct arm_smccc_1_2_regs args = {
		.a0 = GUNYAH_HYPERCALL_ADDRSPACE_MAP,
		.a1 = capid,
		.a2 = extent_capid,
		.a3 = vbase,
		.a4 = extent_attrs,
		.a5 = flags,
		.a6 = offset,
		.a7 = size,
		/* C language says this will be implictly zero. Gunyah requires 0, so be explicit */
		.a8 = 0,
	};
	struct arm_smccc_1_2_regs res;

	arm_smccc_1_2_hvc(&args, &res);

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_addrspace_map);

/**
 * gunyah_hypercall_addrspace_unmap() - Remove memory from an address space
 * @capid: Address space capability ID
 * @extent_capid: Memory extent capability ID
 * @vbase: location in address space
 * @flags: Flags for address space mapping
 * @offset: Offset into memory extent (physical address of memory)
 * @size: Size of memory to map; must be page-aligned
 */
enum gunyah_error gunyah_hypercall_addrspace_unmap(u64 capid, u64 extent_capid, u64 vbase,
					u32 flags, u64 offset, u64 size)
{
	struct arm_smccc_1_2_regs args = {
		.a0 = GUNYAH_HYPERCALL_ADDRSPACE_UNMAP,
		.a1 = capid,
		.a2 = extent_capid,
		.a3 = vbase,
		.a4 = flags,
		.a5 = offset,
		.a6 = size,
		/* C language says this will be implictly zero. Gunyah requires 0, so be explicit */
		.a7 = 0,
	};
	struct arm_smccc_1_2_regs res;

	arm_smccc_1_2_hvc(&args, &res);

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_addrspace_unmap);

/**
 * gunyah_hypercall_memextent_donate() - Donate memory from one memory extent to another
 * @options: donate options
 * @from_capid: Memory extent capability ID to donate from
 * @to_capid: Memory extent capability ID to donate to
 * @offset: Offset into memory extent (physical address of memory)
 * @size: Size of memory to donate; must be page-aligned
 */
enum gunyah_error gunyah_hypercall_memextent_donate(u32 options, u64 from_capid, u64 to_capid,
					    u64 offset, u64 size)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GUNYAH_HYPERCALL_MEMEXTENT_DONATE, options, from_capid, to_capid,
				offset, size, 0, &res);

	return res.a0;
}
EXPORT_SYMBOL_GPL(gunyah_hypercall_memextent_donate);

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
