/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver

FILE DESCRIPTION
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



