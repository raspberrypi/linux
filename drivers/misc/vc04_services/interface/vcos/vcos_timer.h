/*
 * Copyright (c) 2010-2011 Broadcom. All rights reserved.
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

/*=============================================================================
VideoCore OS Abstraction Layer - timer support
=============================================================================*/

#ifndef VCOS_TIMER_H
#define VCOS_TIMER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"
#include "vcos_platform.h"

/** \file vcos_timer.h
  *
  * Timers are single shot.
  *
  * Timer times are in milliseconds.
  *
  * \note that timer callback functions are called from an arbitrary thread
  * context. The expiration function should do its work as quickly as possible;
  * blocking should be avoided.
  *
  * \note On Windows, the separate function vcos_timer_init() must be called
  * as timer initialization from DllMain is not possible.
  */

/** Perform timer subsystem initialization. This function is not needed
  * on non-Windows platforms but is still present so that it can be
  * called. On Windows it is needed because vcos_init() gets called
  * from DLL initialization where it is not possible to create a
  * time queue (deadlock occurs if you try).
  *
  * @return VCOS_SUCCESS on success. VCOS_EEXIST if this has already been called
  * once. VCOS_ENOMEM if resource allocation failed.
  */
VCOSPRE_ VCOS_STATUS_T VCOSPOST_ vcos_timer_init(void);

/** Create a timer in a disabled state.
  *
  * The timer is initially disabled.
  *
  * @param timer     timer handle
  * @param name      name for timer
  * @param expiration_routine function to call when timer expires
  * @param context   context passed to expiration routine
  *
  */
VCOS_INLINE_DECL
VCOS_STATUS_T vcos_timer_create(VCOS_TIMER_T *timer,
                                const char *name,
                                void (*expiration_routine)(void *context),
                                void *context);



/** Start a timer running.
  *
  * Timer must be stopped.
  *
  * @param timer     timer handle
  * @param delay     Delay to wait for, in ms
  */
VCOS_INLINE_DECL
void vcos_timer_set(VCOS_TIMER_T *timer, VCOS_UNSIGNED delay);

/** Stop an already running timer.
  *
  * @param timer     timer handle
  */
VCOS_INLINE_DECL
void vcos_timer_cancel(VCOS_TIMER_T *timer);

/** Stop a timer and restart it.
  * @param timer     timer handle
  * @param delay     delay in ms
  */
VCOS_INLINE_DECL
void vcos_timer_reset(VCOS_TIMER_T *timer, VCOS_UNSIGNED delay);

VCOS_INLINE_DECL
void vcos_timer_delete(VCOS_TIMER_T *timer);

#ifdef __cplusplus
}
#endif
#endif
