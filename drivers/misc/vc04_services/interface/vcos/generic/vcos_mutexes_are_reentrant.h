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
VideoCore OS Abstraction Layer - reentrant mutexes mapped directly to regular ones
=============================================================================*/

#ifndef VCOS_GENERIC_REENTRANT_MUTEX_H
#define VCOS_GENERIC_REENTRANT_MUTEX_H

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"
#include "interface/vcos/vcos_mutex.h"

/**
 * \file
 *
 * Reentrant Mutexes directly using the native re-entrant mutex.
 *
 */

typedef VCOS_MUTEX_T VCOS_REENTRANT_MUTEX_T;

/* Inline forwarding functions */

#if defined(VCOS_INLINE_BODIES)

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_reentrant_mutex_create(VCOS_REENTRANT_MUTEX_T *m, const char *name) {
   return vcos_mutex_create(m,name);
}

VCOS_INLINE_IMPL
void vcos_reentrant_mutex_delete(VCOS_REENTRANT_MUTEX_T *m) {
   vcos_mutex_delete(m);
}

VCOS_INLINE_IMPL
void vcos_reentrant_mutex_lock(VCOS_REENTRANT_MUTEX_T *m) {
   vcos_mutex_lock(m);
}

VCOS_INLINE_IMPL
void vcos_reentrant_mutex_unlock(VCOS_REENTRANT_MUTEX_T *m) {
   vcos_mutex_unlock(m);
}

VCOS_INLINE_IMPL
int vcos_reentrant_mutex_is_locked(VCOS_REENTRANT_MUTEX_T *m) {
   return vcos_mutex_is_locked(m);
}

#endif

#ifdef __cplusplus
}
#endif
#endif



