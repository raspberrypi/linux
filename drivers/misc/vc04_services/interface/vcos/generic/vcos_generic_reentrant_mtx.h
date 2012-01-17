/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver

FILE DESCRIPTION
VideoCore OS Abstraction Layer - reentrant mutexes created from regular ones.
=============================================================================*/

#ifndef VCOS_GENERIC_REENTRANT_MUTEX_H
#define VCOS_GENERIC_REENTRANT_MUTEX_H

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"

/**
 * \file
 *
 * Reentrant Mutexes from regular ones.
 *
 */

typedef struct VCOS_REENTRANT_MUTEX_T
{
   VCOS_MUTEX_T mutex;
   VCOS_THREAD_T *owner;
   unsigned count;
} VCOS_REENTRANT_MUTEX_T;

/* Extern definitions of functions that do the actual work */

VCOSPRE_ VCOS_STATUS_T VCOSPOST_ vcos_generic_reentrant_mutex_create(VCOS_REENTRANT_MUTEX_T *m, const char *name);

VCOSPRE_ void VCOSPOST_ vcos_generic_reentrant_mutex_delete(VCOS_REENTRANT_MUTEX_T *m);

VCOSPRE_ void VCOSPOST_ vcos_generic_reentrant_mutex_lock(VCOS_REENTRANT_MUTEX_T *m);

VCOSPRE_ void VCOSPOST_ vcos_generic_reentrant_mutex_unlock(VCOS_REENTRANT_MUTEX_T *m);

/* Inline forwarding functions */

#if defined(VCOS_INLINE_BODIES)

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_reentrant_mutex_create(VCOS_REENTRANT_MUTEX_T *m, const char *name) {
   return vcos_generic_reentrant_mutex_create(m,name);
}

VCOS_INLINE_IMPL
void vcos_reentrant_mutex_delete(VCOS_REENTRANT_MUTEX_T *m) {
   vcos_generic_reentrant_mutex_delete(m);
}

VCOS_INLINE_IMPL
void vcos_reentrant_mutex_lock(VCOS_REENTRANT_MUTEX_T *m) {
   vcos_generic_reentrant_mutex_lock(m);
}

VCOS_INLINE_IMPL
void vcos_reentrant_mutex_unlock(VCOS_REENTRANT_MUTEX_T *m) {
   vcos_generic_reentrant_mutex_unlock(m);
}
#endif

#ifdef __cplusplus
}
#endif
#endif


