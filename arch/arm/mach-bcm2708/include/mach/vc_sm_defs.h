/*****************************************************************************
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
*****************************************************************************/

#ifndef __VC_SM_DEFS_H__INCLUDED__
#define __VC_SM_DEFS_H__INCLUDED__

/* FourCC code used for VCHI connection */
#define VC_SM_SERVER_NAME MAKE_FOURCC("SMEM")

/* Maximum message length */
#define VC_SM_MAX_MSG_LEN (sizeof(VC_SM_MSG_UNION_T) + \
	sizeof(VC_SM_MSG_HDR_T))
#define VC_SM_MAX_RSP_LEN (sizeof(VC_SM_MSG_UNION_T))

/* Resource name maximum size */
#define VC_SM_RESOURCE_NAME 32

/* All message types supported for HOST->VC direction */
typedef enum {
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
	VC_SM_MSG_TYPE_MAX
} VC_SM_MSG_TYPE;

/* Type of memory to be allocated */
typedef enum {
	VC_SM_ALLOC_CACHED,
	VC_SM_ALLOC_NON_CACHED,

} VC_SM_ALLOC_TYPE_T;

/* Message header for all messages in HOST->VC direction */
typedef struct {
	int32_t type;
	uint32_t trans_id;
	uint8_t body[0];

} VC_SM_MSG_HDR_T;

/* Request to allocate memory (HOST->VC) */
typedef struct {
	/* type of memory to allocate */
	VC_SM_ALLOC_TYPE_T type;
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

} VC_SM_ALLOC_T;

/* Result of a requested memory allocation (VC->HOST) */
typedef struct {
	/* Transaction identifier */
	uint32_t trans_id;

	/* Resource handle */
	uint32_t res_handle;
	/* Pointer to resource buffer */
	void *res_mem;
	/* Resource base size (bytes) */
	uint32_t res_base_size;
	/* Resource number */
	uint32_t res_num;

} VC_SM_ALLOC_RESULT_T;

/* Request to free a previously allocated memory (HOST->VC) */
typedef struct {
	/* Resource handle (returned from alloc) */
	uint32_t res_handle;
	/* Resource buffer (returned from alloc) */
	void *res_mem;

} VC_SM_FREE_T;

/* Request to lock a previously allocated memory (HOST->VC) */
typedef struct {
	/* Resource handle (returned from alloc) */
	uint32_t res_handle;
	/* Resource buffer (returned from alloc) */
	void *res_mem;

} VC_SM_LOCK_UNLOCK_T;

/* Request to resize a previously allocated memory (HOST->VC) */
typedef struct {
	/* Resource handle (returned from alloc) */
	uint32_t res_handle;
	/* Resource buffer (returned from alloc) */
	void *res_mem;
	/* Resource *new* size requested (bytes) */
	uint32_t res_new_size;

} VC_SM_RESIZE_T;

/* Result of a requested memory lock (VC->HOST) */
typedef struct {
	/* Transaction identifier */
	uint32_t trans_id;

	/* Resource handle */
	uint32_t res_handle;
	/* Pointer to resource buffer */
	void *res_mem;
	/* Pointer to former resource buffer if the memory
	 * was reallocated */
	void *res_old_mem;

} VC_SM_LOCK_RESULT_T;

/* Generic result for a request (VC->HOST) */
typedef struct {
	/* Transaction identifier */
	uint32_t trans_id;

	int32_t success;

} VC_SM_RESULT_T;

/* Request to revert a previously applied action (HOST->VC) */
typedef struct {
	/* Action of interest */
	VC_SM_MSG_TYPE res_action;
	/* Transaction identifier for the action of interest */
	uint32_t action_trans_id;

} VC_SM_ACTION_CLEAN_T;

/* Request to remove all data associated with a given allocator (HOST->VC) */
typedef struct {
	/* Allocator identifier */
	uint32_t allocator;

} VC_SM_FREE_ALL_T;

/* Union of ALL messages */
typedef union {
	VC_SM_ALLOC_T alloc;
	VC_SM_ALLOC_RESULT_T alloc_result;
	VC_SM_FREE_T free;
	VC_SM_ACTION_CLEAN_T action_clean;
	VC_SM_RESIZE_T resize;
	VC_SM_LOCK_RESULT_T lock_result;
	VC_SM_RESULT_T result;
	VC_SM_FREE_ALL_T free_all;

} VC_SM_MSG_UNION_T;

#endif /* __VC_SM_DEFS_H__INCLUDED__ */
