/*
 * Copyright (c) 2010-2011 Broadcom Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef VCHIQ_ARM_H
#define VCHIQ_ARM_H

#include "vchiq_core.h"


typedef struct vchiq_arm_state_struct {

   VCOS_THREAD_T lp_thread;            /* processes low priority messages (eg suspend) */
   VCOS_THREAD_T hp_thread;            /* processes high priority messages (eg resume) */

   VCOS_EVENT_T lp_evt;
   VCOS_EVENT_T hp_evt;

   VCOS_MUTEX_T use_count_mutex;
   VCOS_MUTEX_T suspend_resume_mutex;

   int suspend_pending;

   /* Global use count for videocore.
    * This is equal to the sum of the use counts for all services.  When this hits
    * zero the videocore suspend procedure will be initiated. */
   int videocore_use_count;

   /* Use count to track requests from videocore peer.
    * This use count is not associated with a service, so needs to be tracked separately
    * with the state.
    */
   int peer_use_count;

   /* Flag to indicate whether videocore is currently suspended */
   int videocore_suspended;

   /* Flag to indicate whether a notification is pending back to videocore that it's
    * "remote use request" has been actioned */
   int use_notify_pending;
} VCHIQ_ARM_STATE_T;


extern VCOS_LOG_CAT_T vchiq_arm_log_category;

extern int __init
vchiq_platform_vcos_init(void);

extern int __init
vchiq_platform_init(VCHIQ_STATE_T *state);

extern void __exit
vchiq_platform_exit(VCHIQ_STATE_T *state);

extern VCHIQ_STATE_T *
vchiq_get_state(void);

extern VCHIQ_STATUS_T
vchiq_arm_vcsuspend(VCHIQ_STATE_T *state);

extern VCHIQ_STATUS_T
vchiq_arm_vcresume(VCHIQ_STATE_T *state);

extern VCHIQ_STATUS_T
vchiq_arm_init_state(VCHIQ_STATE_T *state, VCHIQ_ARM_STATE_T *arm_state);

extern void
vchiq_check_resume(VCHIQ_STATE_T* state);

extern void
vchiq_check_suspend(VCHIQ_STATE_T* state);

extern VCHIQ_STATUS_T
vchiq_use_service(VCHIQ_SERVICE_HANDLE_T handle);

extern VCHIQ_STATUS_T
vchiq_release_service(VCHIQ_SERVICE_HANDLE_T handle);

extern VCHIQ_STATUS_T
vchiq_check_service(VCHIQ_SERVICE_T * service);

extern VCHIQ_STATUS_T
vchiq_platform_suspend(VCHIQ_STATE_T *state);

extern VCHIQ_STATUS_T
vchiq_platform_resume(VCHIQ_STATE_T *state);

extern int
vchiq_platform_videocore_wanted(VCHIQ_STATE_T* state);

extern int
vchiq_platform_use_suspend_timer(void);

extern void
vchiq_dump_platform_use_state(VCHIQ_STATE_T *state);

extern void
vchiq_dump_service_use_state(VCHIQ_STATE_T *state);

extern VCHIQ_ARM_STATE_T*
vchiq_platform_get_arm_state(VCHIQ_STATE_T *state);


#endif /* VCHIQ_ARM_H */
