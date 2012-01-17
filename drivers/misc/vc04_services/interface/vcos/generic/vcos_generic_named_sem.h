/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver

FILE DESCRIPTION
VideoCore OS Abstraction Layer - named semaphores
=============================================================================*/

#ifndef VCOS_GENERIC_NAMED_SEM_H
#define VCOS_GENERIC_NAMED_SEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"

/**
 * \file
 *
 * Generic support for named semaphores, using regular ones. This is only
 * suitable for emulating them on an embedded MMUless system, since there is
 * no support for opening semaphores across process boundaries.
 *
 */

#define VCOS_NAMED_SEMAPHORE_NAMELEN   64

/* In theory we could use the name facility provided within Nucleus. However, this
 * is hard to do as semaphores are constantly being created and destroyed; we
 * would need to stop everything while allocating the memory for the semaphore
 * list and then walking it. So keep our own list.
 */
typedef struct VCOS_NAMED_SEMAPHORE_T
{
   struct VCOS_NAMED_SEMAPHORE_IMPL_T *actual; /**< There are 'n' named semaphores per 1 actual semaphore  */
   VCOS_SEMAPHORE_T *sem;                      /**< Pointer to actual underlying semaphore */
} VCOS_NAMED_SEMAPHORE_T;

VCOSPRE_ VCOS_STATUS_T VCOSPOST_
vcos_generic_named_semaphore_create(VCOS_NAMED_SEMAPHORE_T *sem, const char *name, VCOS_UNSIGNED count);

VCOSPRE_ void VCOSPOST_ vcos_named_semaphore_delete(VCOS_NAMED_SEMAPHORE_T *sem);

VCOSPRE_ VCOS_STATUS_T VCOSPOST_ _vcos_named_semaphore_init(void);
VCOSPRE_ void VCOSPOST_ _vcos_named_semaphore_deinit(void);

#if defined(VCOS_INLINE_BODIES)

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_named_semaphore_create(VCOS_NAMED_SEMAPHORE_T *sem, const char *name, VCOS_UNSIGNED count) {
   return vcos_generic_named_semaphore_create(sem, name, count);
}

VCOS_INLINE_IMPL
void vcos_named_semaphore_wait(VCOS_NAMED_SEMAPHORE_T *sem) {
   vcos_semaphore_wait(sem->sem);
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_named_semaphore_trywait(VCOS_NAMED_SEMAPHORE_T *sem) {
   return vcos_semaphore_trywait(sem->sem);
}

VCOS_INLINE_IMPL
void vcos_named_semaphore_post(VCOS_NAMED_SEMAPHORE_T *sem) {
   vcos_semaphore_post(sem->sem);
}


#endif

#ifdef __cplusplus
}
#endif
#endif


