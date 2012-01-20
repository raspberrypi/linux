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
VideoCore OS Abstraction Layer - Construct a latch from a semaphore
=============================================================================*/

/** FIXME: rename to vcos_mutex_from_sem.c
  */

typedef struct VCOS_MUTEX_T {
   VCOS_SEMAPHORE_T sem;
   struct VCOS_THREAD_T *owner;
} VCOS_MUTEX_T;

extern VCOS_STATUS_T vcos_generic_mutex_create(VCOS_MUTEX_T *latch, const char *name);
extern void vcos_generic_mutex_delete(VCOS_MUTEX_T *latch);
extern VCOS_STATUS_T vcos_generic_mutex_lock(VCOS_MUTEX_T *latch);
extern void vcos_generic_mutex_unlock(VCOS_MUTEX_T *latch);

#if defined(VCOS_INLINE_BODIES)

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_mutex_create(VCOS_MUTEX_T *latch, const char *name) {
   return vcos_generic_mutex_create(latch,name);
}

VCOS_INLINE_IMPL
void vcos_mutex_delete(VCOS_MUTEX_T *latch) {
   vcos_generic_mutex_delete(latch);
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_mutex_lock(VCOS_MUTEX_T *latch) {
   return vcos_generic_mutex_lock(latch);
}

VCOS_INLINE_IMPL
void vcos_mutex_unlock(VCOS_MUTEX_T *latch) {
   vcos_generic_mutex_unlock(latch);
}

#endif /* VCOS_INLINE_BODIES */

