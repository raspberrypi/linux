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

#ifndef __VC_VCHI_SM_H__INCLUDED__
#define __VC_VCHI_SM_H__INCLUDED__

#include "interface/vchi/vchi.h"

#include "vc_sm_defs.h"

/* Forward declare.
*/
typedef struct sm_instance *VC_VCHI_SM_HANDLE_T;

/* Initialize the shared memory service, opens up vchi connection to talk to it.
*/
VC_VCHI_SM_HANDLE_T vc_vchi_sm_init(VCHI_INSTANCE_T vchi_instance,
				    VCHI_CONNECTION_T **vchi_connections,
				    uint32_t num_connections);

/* Terminates the shared memory service.
*/
int vc_vchi_sm_stop(VC_VCHI_SM_HANDLE_T *handle);

/* Ask the shared memory service to allocate some memory on videocre and
** return the result of this allocation (which upon success will be a pointer
** to some memory in videocore space).
*/
int vc_vchi_sm_alloc(VC_VCHI_SM_HANDLE_T handle,
		     VC_SM_ALLOC_T *alloc,
		     VC_SM_ALLOC_RESULT_T *alloc_result, uint32_t *trans_id);

/* Ask the shared memory service to free up some memory that was previously
** allocated by the vc_vchi_sm_alloc function call.
*/
int vc_vchi_sm_free(VC_VCHI_SM_HANDLE_T handle,
		    VC_SM_FREE_T *free, uint32_t *trans_id);

/* Ask the shared memory service to lock up some memory that was previously
** allocated by the vc_vchi_sm_alloc function call.
*/
int vc_vchi_sm_lock(VC_VCHI_SM_HANDLE_T handle,
		    VC_SM_LOCK_UNLOCK_T *lock_unlock,
		    VC_SM_LOCK_RESULT_T *lock_result, uint32_t *trans_id);

/* Ask the shared memory service to unlock some memory that was previously
** allocated by the vc_vchi_sm_alloc function call.
*/
int vc_vchi_sm_unlock(VC_VCHI_SM_HANDLE_T handle,
		      VC_SM_LOCK_UNLOCK_T *lock_unlock,
		      uint32_t *trans_id, uint8_t wait_reply);

/* Ask the shared memory service to resize some memory that was previously
** allocated by the vc_vchi_sm_alloc function call.
*/
int vc_vchi_sm_resize(VC_VCHI_SM_HANDLE_T handle,
		      VC_SM_RESIZE_T *resize, uint32_t *trans_id);

/* Walk the allocated resources on the videocore side, the allocation will
** show up in the log.  This is purely for debug/information and takes no
** specific actions.
*/
int vc_vchi_sm_walk_alloc(VC_VCHI_SM_HANDLE_T handle);

/* Clean up following a previously interrupted action which left the system
** in a bad state of some sort.
*/
int vc_vchi_sm_clean_up(VC_VCHI_SM_HANDLE_T handle,
			VC_SM_ACTION_CLEAN_T *action_clean);

#endif /* __VC_VCHI_SM_H__INCLUDED__ */
