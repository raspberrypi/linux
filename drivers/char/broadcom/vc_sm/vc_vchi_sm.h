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

#ifndef __VC_VCHI_SM_H__INCLUDED__
#define __VC_VCHI_SM_H__INCLUDED__

#include "interface/vchi/vchi.h"

#include "vc_sm_defs.h"

/*
 * Forward declare.
 */
struct sm_instance;

/*
 * Initialize the shared memory service, opens up vchi connection to talk to it.
 */
struct sm_instance *vc_vchi_sm_init(VCHI_INSTANCE_T vchi_instance,
				    VCHI_CONNECTION_T **vchi_connections,
				    uint32_t num_connections);

/*
 * Terminates the shared memory service.
 */
int vc_vchi_sm_stop(struct sm_instance **handle);

/*
 * Ask the shared memory service to allocate some memory on videocre and
 * return the result of this allocation (which upon success will be a pointer
 * to some memory in videocore space).
 */
int vc_vchi_sm_alloc(struct sm_instance *handle, struct vc_sm_alloc_t *alloc,
		     struct vc_sm_alloc_result_t *alloc_result,
		     uint32_t *trans_id);

/*
 * Ask the shared memory service to free up some memory that was previously
 * allocated by the vc_vchi_sm_alloc function call.
 */
int vc_vchi_sm_free(struct sm_instance *handle,
		    struct vc_sm_free_t *free, uint32_t *trans_id);

/*
 * Ask the shared memory service to lock up some memory that was previously
 * allocated by the vc_vchi_sm_alloc function call.
 */
int vc_vchi_sm_lock(struct sm_instance *handle,
		    struct vc_sm_lock_unlock_t *lock_unlock,
		    struct vc_sm_lock_result_t *lock_result,
		    uint32_t *trans_id);

/*
 * Ask the shared memory service to unlock some memory that was previously
 * allocated by the vc_vchi_sm_alloc function call.
 */
int vc_vchi_sm_unlock(struct sm_instance *handle,
		      struct vc_sm_lock_unlock_t *lock_unlock,
		      uint32_t *trans_id, uint8_t wait_reply);

/*
 * Ask the shared memory service to resize some memory that was previously
 * allocated by the vc_vchi_sm_alloc function call.
 */
int vc_vchi_sm_resize(struct sm_instance *handle,
		      struct vc_sm_resize_t *resize, uint32_t *trans_id);

/*
 * Walk the allocated resources on the videocore side, the allocation will
 * show up in the log.  This is purely for debug/information and takes no
 * specific actions.
 */
int vc_vchi_sm_walk_alloc(struct sm_instance *handle);

/*
 * Clean up following a previously interrupted action which left the system
 * in a bad state of some sort.
 */
int vc_vchi_sm_clean_up(struct sm_instance *handle,
			struct vc_sm_action_clean_t *action_clean);

/*
 * Import a contiguous block of memory and wrap it in a GPU MEM_HANDLE_T.
 */
int vc_vchi_sm_import(struct sm_instance *handle, struct vc_sm_import *msg,
		      struct vc_sm_import_result *result,
		      uint32_t *cur_trans_id);

#endif /* __VC_VCHI_SM_H__INCLUDED__ */
