/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  vcos

FILE DESCRIPTION
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

