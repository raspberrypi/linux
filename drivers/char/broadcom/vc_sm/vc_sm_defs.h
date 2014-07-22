/*
 ****************************************************************************
 * Copyright 2011 Broadcom Corporation.  All rights reserved.
 *
 * Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2, available at
 * http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
 *
 * Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a
 * license other than the GPL, without Broadcom's express prior written
 * consent.
 ****************************************************************************
 */

#ifndef __VC_SM_DEFS_H__INCLUDED__
#define __VC_SM_DEFS_H__INCLUDED__

/* FourCC code used for VCHI connection */
#define VC_SM_SERVER_NAME MAKE_FOURCC("SMEM")

/* Maximum message length */
#define VC_SM_MAX_MSG_LEN (sizeof(union vc_sm_msg_union_t) + \
	sizeof(struct vc_sm_msg_hdr_t))
#define VC_SM_MAX_RSP_LEN (sizeof(union vc_sm_msg_union_t))

/* Resource name maximum size */
#define VC_SM_RESOURCE_NAME 32

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

	/* Message types supported for VC->HOST direction */

	/*
	 * VC has finished with an imported memory allocation.
	 * Release any Linux reference counts on the underlying block.
	 */
	VC_SM_MSG_TYPE_RELEASED,

	VC_SM_MSG_TYPE_MAX
};

/* Type of memory to be allocated */
enum vc_sm_alloc_type_t {
	VC_SM_ALLOC_CACHED,
	VC_SM_ALLOC_NON_CACHED,
};

/* Message header for all messages in HOST->VC direction */
struct vc_sm_msg_hdr_t {
	int32_t type;
	uint32_t trans_id;
	uint8_t body[0];

};

/* Request to allocate memory (HOST->VC) */
struct vc_sm_alloc_t {
	/* type of memory to allocate */
	enum vc_sm_alloc_type_t type;
	/* byte amount of data to allocate per unit */
	uint32_t base_unit;
	/* number of unit to allocate */
	uint32_t num_unit;
	/* alignement to be applied on allocation */
	uint32_t alignement;
	/* identity of who allocated this block */
	uint32_t allocator;
	/* resource name (for easier tracking on vc side) */
	char name[VC_SM_RESOURCE_NAME];

};

/* Result of a requested memory allocation (VC->HOST) */
struct vc_sm_alloc_result_t {
	/* Transaction identifier */
	uint32_t trans_id;

	/* Resource handle */
	uint32_t res_handle;
	/* Pointer to resource buffer */
	uint32_t res_mem;
	/* Resource base size (bytes) */
	uint32_t res_base_size;
	/* Resource number */
	uint32_t res_num;

};

/* Request to free a previously allocated memory (HOST->VC) */
struct vc_sm_free_t {
	/* Resource handle (returned from alloc) */
	uint32_t res_handle;
	/* Resource buffer (returned from alloc) */
	uint32_t res_mem;

};

/* Request to lock a previously allocated memory (HOST->VC) */
struct vc_sm_lock_unlock_t {
	/* Resource handle (returned from alloc) */
	uint32_t res_handle;
	/* Resource buffer (returned from alloc) */
	uint32_t res_mem;

};

/* Request to resize a previously allocated memory (HOST->VC) */
struct vc_sm_resize_t {
	/* Resource handle (returned from alloc) */
	uint32_t res_handle;
	/* Resource buffer (returned from alloc) */
	uint32_t res_mem;
	/* Resource *new* size requested (bytes) */
	uint32_t res_new_size;

};

/* Result of a requested memory lock (VC->HOST) */
struct vc_sm_lock_result_t {
	/* Transaction identifier */
	uint32_t trans_id;

	/* Resource handle */
	uint32_t res_handle;
	/* Pointer to resource buffer */
	uint32_t res_mem;
	/*
	 * Pointer to former resource buffer if the memory
	 * was reallocated
	 */
	uint32_t res_old_mem;

};

/* Generic result for a request (VC->HOST) */
struct vc_sm_result_t {
	/* Transaction identifier */
	uint32_t trans_id;

	int32_t success;

};

/* Request to revert a previously applied action (HOST->VC) */
struct vc_sm_action_clean_t {
	/* Action of interest */
	enum vc_sm_msg_type res_action;
	/* Transaction identifier for the action of interest */
	uint32_t action_trans_id;

};

/* Request to remove all data associated with a given allocator (HOST->VC) */
struct vc_sm_free_all_t {
	/* Allocator identifier */
	uint32_t allocator;
};

/* Request to import memory (HOST->VC) */
struct vc_sm_import {
	/* type of memory to allocate */
	enum vc_sm_alloc_type_t type;
	/* pointer to the VC (ie physical) address of the allocated memory */
	uint32_t addr;
	/* size of buffer */
	uint32_t size;
	/* opaque handle returned in RELEASED messages */
	int32_t  kernel_id;
	/* Allocator identifier */
	uint32_t allocator;
	/* resource name (for easier tracking on vc side) */
	char     name[VC_SM_RESOURCE_NAME];
};

/* Result of a requested memory import (VC->HOST) */
struct vc_sm_import_result {
	/* Transaction identifier */
	uint32_t trans_id;

	/* Resource handle */
	uint32_t res_handle;
};

/* Notification that VC has finished with an allocation (VC->HOST) */
struct vc_sm_released {
	/* pointer to the VC (ie physical) address of the allocated memory */
	uint32_t addr;
	/* size of buffer */
	uint32_t size;
	/* opaque handle returned in RELEASED messages */
	int32_t  kernel_id;
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
	struct vc_sm_released released;
};

#endif /* __VC_SM_DEFS_H__INCLUDED__ */
