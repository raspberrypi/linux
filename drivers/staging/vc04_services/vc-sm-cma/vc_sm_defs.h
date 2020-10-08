/* SPDX-License-Identifier: GPL-2.0 */

/*
 * VideoCore Shared Memory CMA allocator
 *
 * Copyright: 2018, Raspberry Pi (Trading) Ltd
 *
 * Based on vc_sm_defs.h from the vmcs_sm driver Copyright Broadcom Corporation.
 * All IPC messages are copied across to this file, even if the vc-sm-cma
 * driver is not currently using them.
 *
 ****************************************************************************
 */

#ifndef __VC_SM_DEFS_H__INCLUDED__
#define __VC_SM_DEFS_H__INCLUDED__

/* Maximum message length */
#define VC_SM_MAX_MSG_LEN (sizeof(union vc_sm_msg_union_t) + \
	sizeof(struct vc_sm_msg_hdr_t))
#define VC_SM_MAX_RSP_LEN (sizeof(union vc_sm_msg_union_t))

/* Resource name maximum size */
#define VC_SM_RESOURCE_NAME 32

/*
 * Version to be reported to the VPU
 * VPU assumes 0 (aka 1) which does not require the released callback, nor
 * expect the client to handle VC_MEM_REQUESTS.
 * Version 2 requires the released callback, and must support VC_MEM_REQUESTS.
 */
#define VC_SM_PROTOCOL_VERSION	2

enum vc_sm_msg_type {
	/* Message types supported for HOST->VC direction */

	/* Allocate shared memory block */
	VC_SM_MSG_TYPE_ALLOC,
	/* Lock allocated shared memory block */
	VC_SM_MSG_TYPE_LOCK,
	/* Unlock allocated shared memory block */
	VC_SM_MSG_TYPE_UNLOCK,
	/* Unlock allocated shared memory block, do not answer command */
	VC_SM_MSG_TYPE_UNLOCK_NOANS,
	/* Free shared memory block */
	VC_SM_MSG_TYPE_FREE,
	/* Resize a shared memory block */
	VC_SM_MSG_TYPE_RESIZE,
	/* Walk the allocated shared memory block(s) */
	VC_SM_MSG_TYPE_WALK_ALLOC,

	/* A previously applied action will need to be reverted */
	VC_SM_MSG_TYPE_ACTION_CLEAN,

	/*
	 * Import a physical address and wrap into a MEM_HANDLE_T.
	 * Release with VC_SM_MSG_TYPE_FREE.
	 */
	VC_SM_MSG_TYPE_IMPORT,
	/*
	 *Tells VC the protocol version supported by this client.
	 * 2 supports the async/cmd messages from the VPU for final release
	 * of memory, and for VC allocations.
	 */
	VC_SM_MSG_TYPE_CLIENT_VERSION,
	/* Response to VC request for memory */
	VC_SM_MSG_TYPE_VC_MEM_REQUEST_REPLY,

	/*
	 * Asynchronous/cmd messages supported for VC->HOST direction.
	 * Signalled by setting the top bit in vc_sm_result_t trans_id.
	 */

	/*
	 * VC has finished with an imported memory allocation.
	 * Release any Linux reference counts on the underlying block.
	 */
	VC_SM_MSG_TYPE_RELEASED,
	/* VC request for memory */
	VC_SM_MSG_TYPE_VC_MEM_REQUEST,

	VC_SM_MSG_TYPE_MAX
};

/* Type of memory to be allocated */
enum vc_sm_alloc_type_t {
	VC_SM_ALLOC_CACHED,
	VC_SM_ALLOC_NON_CACHED,
};

/* Message header for all messages in HOST->VC direction */
struct vc_sm_msg_hdr_t {
	u32 type;
	u32 trans_id;
	u8 body[0];

};

/* Request to allocate memory (HOST->VC) */
struct vc_sm_alloc_t {
	/* type of memory to allocate */
	enum vc_sm_alloc_type_t type;
	/* byte amount of data to allocate per unit */
	u32 base_unit;
	/* number of unit to allocate */
	u32 num_unit;
	/* alignment to be applied on allocation */
	u32 alignment;
	/* identity of who allocated this block */
	u32 allocator;
	/* resource name (for easier tracking on vc side) */
	char name[VC_SM_RESOURCE_NAME];

};

/* Result of a requested memory allocation (VC->HOST) */
struct vc_sm_alloc_result_t {
	/* Transaction identifier */
	u32 trans_id;

	/* Resource handle */
	u32 res_handle;
	/* Pointer to resource buffer */
	u32 res_mem;
	/* Resource base size (bytes) */
	u32 res_base_size;
	/* Resource number */
	u32 res_num;

};

/* Request to free a previously allocated memory (HOST->VC) */
struct vc_sm_free_t {
	/* Resource handle (returned from alloc) */
	u32 res_handle;
	/* Resource buffer (returned from alloc) */
	u32 res_mem;

};

/* Request to lock a previously allocated memory (HOST->VC) */
struct vc_sm_lock_unlock_t {
	/* Resource handle (returned from alloc) */
	u32 res_handle;
	/* Resource buffer (returned from alloc) */
	u32 res_mem;

};

/* Request to resize a previously allocated memory (HOST->VC) */
struct vc_sm_resize_t {
	/* Resource handle (returned from alloc) */
	u32 res_handle;
	/* Resource buffer (returned from alloc) */
	u32 res_mem;
	/* Resource *new* size requested (bytes) */
	u32 res_new_size;

};

/* Result of a requested memory lock (VC->HOST) */
struct vc_sm_lock_result_t {
	/* Transaction identifier */
	u32 trans_id;

	/* Resource handle */
	u32 res_handle;
	/* Pointer to resource buffer */
	u32 res_mem;
	/*
	 * Pointer to former resource buffer if the memory
	 * was reallocated
	 */
	u32 res_old_mem;

};

/* Generic result for a request (VC->HOST) */
struct vc_sm_result_t {
	/* Transaction identifier */
	u32 trans_id;

	s32 success;

};

/* Request to revert a previously applied action (HOST->VC) */
struct vc_sm_action_clean_t {
	/* Action of interest */
	enum vc_sm_msg_type res_action;
	/* Transaction identifier for the action of interest */
	u32 action_trans_id;

};

/* Request to remove all data associated with a given allocator (HOST->VC) */
struct vc_sm_free_all_t {
	/* Allocator identifier */
	u32 allocator;
};

/* Request to import memory (HOST->VC) */
struct vc_sm_import {
	/* type of memory to allocate */
	enum vc_sm_alloc_type_t type;
	/* pointer to the VC (ie physical) address of the allocated memory */
	u32 addr;
	/* size of buffer */
	u32 size;
	/* opaque handle returned in RELEASED messages */
	u32 kernel_id;
	/* Allocator identifier */
	u32 allocator;
	/* resource name (for easier tracking on vc side) */
	char     name[VC_SM_RESOURCE_NAME];
};

/* Result of a requested memory import (VC->HOST) */
struct vc_sm_import_result {
	/* Transaction identifier */
	u32 trans_id;

	/* Resource handle */
	u32 res_handle;
};

/* Notification that VC has finished with an allocation (VC->HOST) */
struct vc_sm_released {
	/* cmd type / trans_id */
	u32 cmd;

	/* pointer to the VC (ie physical) address of the allocated memory */
	u32 addr;
	/* size of buffer */
	u32 size;
	/* opaque handle returned in RELEASED messages */
	u32 kernel_id;
	u32 vc_handle;
};

/*
 * Client informing VC as to the protocol version it supports.
 * >=2 requires the released callback, and supports VC asking for memory.
 * Failure means that the firmware doesn't support this call, and therefore the
 * client should either fail, or NOT rely on getting the released callback.
 */
struct vc_sm_version {
	u32 version;
};

/* Request FROM VideoCore for some memory */
struct vc_sm_vc_mem_request {
	/* cmd type */
	u32 cmd;

	/* trans_id (from VPU) */
	u32 trans_id;
	/* size of buffer */
	u32 size;
	/* alignment of buffer */
	u32 align;
	/* resource name (for easier tracking) */
	char     name[VC_SM_RESOURCE_NAME];
	/* VPU handle for the resource */
	u32 vc_handle;
};

/* Response from the kernel to provide the VPU with some memory */
struct vc_sm_vc_mem_request_result {
	/* Transaction identifier for the VPU */
	u32 trans_id;
	/* pointer to the physical address of the allocated memory */
	u32 addr;
	/* opaque handle returned in RELEASED messages */
	u32 kernel_id;
};

/* Union of ALL messages */
union vc_sm_msg_union_t {
	struct vc_sm_alloc_t alloc;
	struct vc_sm_alloc_result_t alloc_result;
	struct vc_sm_free_t free;
	struct vc_sm_lock_unlock_t lock_unlock;
	struct vc_sm_action_clean_t action_clean;
	struct vc_sm_resize_t resize;
	struct vc_sm_lock_result_t lock_result;
	struct vc_sm_result_t result;
	struct vc_sm_free_all_t free_all;
	struct vc_sm_import import;
	struct vc_sm_import_result import_result;
	struct vc_sm_version version;
	struct vc_sm_released released;
	struct vc_sm_vc_mem_request vc_request;
	struct vc_sm_vc_mem_request_result vc_request_result;
};

#endif /* __VC_SM_DEFS_H__INCLUDED__ */
