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
VideoCore OS Abstraction Layer - public header file
=============================================================================*/

#ifndef VCOS_EVENT_FLAGS_H
#define VCOS_EVENT_FLAGS_H


#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"
#include "vcos_platform.h"

#define VCOS_EVENT_FLAGS_SUSPEND    VCOS_SUSPEND
#define VCOS_EVENT_FLAGS_NO_SUSPEND VCOS_NO_SUSPEND
typedef VCOS_OPTION VCOS_EVENTGROUP_OPERATION_T;

/**
 * \file vcos_event_flags.h
 *
 * Defines event flags API.
 *
 * Similar to Nucleus event groups.
 *
 * These have the same semantics as Nucleus event groups and ThreadX event
 * flags. As such, they are quite complex internally; if speed is important
 * they might not be your best choice.
 *
 */

/**
 * Create an event flags instance.
 *
 * @param flags   Pointer to event flags instance, filled in on return.
 * @param name    Name for the event flags, used for debug.
 *
 * @return VCOS_SUCCESS if succeeded.
 */

VCOS_INLINE_DECL
VCOS_STATUS_T vcos_event_flags_create(VCOS_EVENT_FLAGS_T *flags, const char *name);

/**
  * Set some events.
  *
  * @param flags   Instance to set flags on
  * @param events  Bitmask of the flags to actually set
  * @param op      How the flags should be set. VCOS_OR will OR in the flags; VCOS_AND
  *                will AND them in, possibly clearing existing flags.
  */
VCOS_INLINE_DECL
void vcos_event_flags_set(VCOS_EVENT_FLAGS_T *flags,
                          VCOS_UNSIGNED events,
                          VCOS_OPTION op);

/**
 * Retrieve some events.
 *
 * Waits until the specified events have been set.
 *
 * @param flags            Instance to wait on
 * @param requested_events The bitmask to wait for
 * @param op               VCOS_OR - get any; VCOS_AND - get all.
 * @param ms_suspend       How long to wait, in milliseconds
 * @param retrieved_events the events actually retrieved.
 *
 * @return VCOS_SUCCESS if events were retrieved. VCOS_EAGAIN if the
 * timeout expired.
 */
VCOS_INLINE_DECL
VCOS_STATUS_T vcos_event_flags_get(VCOS_EVENT_FLAGS_T *flags,
                                                     VCOS_UNSIGNED requested_events,
                                                     VCOS_OPTION op,
                                                     VCOS_UNSIGNED ms_suspend,
                                                     VCOS_UNSIGNED *retrieved_events);


/**
 * Delete an event flags instance.
 */
VCOS_INLINE_DECL
void vcos_event_flags_delete(VCOS_EVENT_FLAGS_T *);

#ifdef __cplusplus
}
#endif

#endif

