/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver

FILE DESCRIPTION
VideoCore OS Abstraction Layer - public header file
=============================================================================*/

#ifndef VCOS_SEMAPHORE_H
#define VCOS_SEMAPHORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"
#include "vcos_platform.h"

/**
 * \file vcos_semaphore.h
 *
 * \section sem Semaphores
 *
 * This provides counting semaphores. Semaphores are not re-entrant. On sensible
 * operating systems a semaphore can always be posted but can only be taken in 
 * thread (not interrupt) context. Under Nucleus, a LISR cannot post a semaphore,
 * although it would not be hard to lift this restriction.
 *
 * \subsection timeout Timeout
 *
 * On both Nucleus and ThreadX a semaphore can be taken with a timeout. This is
 * not supported by VCOS because it makes the non-timeout code considerably more
 * complicated (and hence slower). In the unlikely event that you need a timeout
 * with a semaphore, and you cannot simply redesign your code to avoid it, use
 * an event flag (vcos_event_flags.h).
 *
 * \subsection sem_nucleus Changes from Nucleus:
 *
 *  Semaphores are always "FIFO" - i.e. sleeping threads are woken in FIFO order. That's
 *  because:
 *  \arg there's no support for NU_PRIORITY in threadx (though it can be emulated, slowly)
 *  \arg we don't appear to actually consciously use it - for example, Dispmanx uses
 *  it, but all threads waiting are the same priority.
 *         
 */

/** 
  * \brief Create a semaphore.
  *
  * Create a semaphore.
  *
  * @param sem   Pointer to memory to be initialized
  * @param name  A name for this semaphore. The name may be truncated internally.
  * @param count The initial count for the semaphore.
  *
  * @return VCOS_SUCCESS if the semaphore was created.
  * 
  */
VCOS_INLINE_DECL
VCOS_STATUS_T vcos_semaphore_create(VCOS_SEMAPHORE_T *sem, const char *name, VCOS_UNSIGNED count);

/**
  * \brief Wait on a semaphore.
  *
  * There is no timeout option on a semaphore, as adding this will slow down
  * implementations on some platforms. If you need that kind of behaviour, use
  * an event group.
  *
  * On most platforms this always returns VCOS_SUCCESS, and so would ideally be
  * a void function, however some platforms allow a wait to be interrupted so
  * it remains non-void.
  *
  * @param sem Semaphore to wait on
  * @return VCOS_SUCCESS - semaphore was taken.
  *         VCOS_EAGAIN  - could not take semaphore
  *
  */
VCOS_INLINE_DECL
VCOS_STATUS_T vcos_semaphore_wait(VCOS_SEMAPHORE_T *sem);

/**
  * \brief Try to wait for a semaphore.
  *
  * Try to obtain the semaphore. If it is already taken, return VCOS_TIMEOUT.
  * @param sem Semaphore to wait on
  * @return VCOS_SUCCESS - semaphore was taken.
  *         VCOS_EAGAIN - could not take semaphore
  */
VCOS_INLINE_DECL
VCOS_STATUS_T vcos_semaphore_trywait(VCOS_SEMAPHORE_T *sem);

/**
  * \brief Post a semaphore.
  *
  * @param sem Semaphore to wait on
  */
VCOS_INLINE_DECL
VCOS_STATUS_T vcos_semaphore_post(VCOS_SEMAPHORE_T *sem);

/**
  * \brief Delete a semaphore, releasing any resources consumed by it.
  *
  * @param sem Semaphore to wait on
  */
VCOS_INLINE_DECL
void vcos_semaphore_delete(VCOS_SEMAPHORE_T *sem);

#ifdef __cplusplus
}
#endif
#endif

