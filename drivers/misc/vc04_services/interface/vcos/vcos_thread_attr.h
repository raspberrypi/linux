/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

FILE DESCRIPTION
VideoCore OS Abstraction Layer - thread attributes
=============================================================================*/

#ifndef VCOS_THREAD_ATTR_H
#define VCOS_THREAD_ATTR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 *
 * Attributes for thread creation.
 *
 */

/** Initialize thread attribute struct. This call does not allocate memory,
  * and so cannot fail.
  *
  */
VCOSPRE_ void VCOSPOST_ vcos_thread_attr_init(VCOS_THREAD_ATTR_T *attrs);

/** Set the stack address and size. If not set, a stack will be allocated automatically.
  *
  * This can only be set on some platforms. It will always be possible to set the stack
  * address on VideoCore, but on host platforms, support may well not be available.
  */
#if VCOS_CAN_SET_STACK_ADDR
VCOS_INLINE_DECL
void vcos_thread_attr_setstack(VCOS_THREAD_ATTR_T *attrs, void *addr, VCOS_UNSIGNED sz);
#endif

/** Set the stack size. If not set, a default size will be used. Attempting to call this after having
  * set the stack location with vcos_thread_attr_setstack() will result in undefined behaviour.
  */
VCOS_INLINE_DECL
void vcos_thread_attr_setstacksize(VCOS_THREAD_ATTR_T *attrs, VCOS_UNSIGNED sz);

/** Set the task priority. If not set, a default value will be used.
  */
VCOS_INLINE_DECL
void vcos_thread_attr_setpriority(VCOS_THREAD_ATTR_T *attrs, VCOS_UNSIGNED pri);

/** Set the task cpu affinity. If not set, the default will be used.
  */
VCOS_INLINE_DECL
void vcos_thread_attr_setaffinity(VCOS_THREAD_ATTR_T *attrs, VCOS_UNSIGNED aff);

/** Set the timeslice. If not set the default will be used.
  */
VCOS_INLINE_DECL
void vcos_thread_attr_settimeslice(VCOS_THREAD_ATTR_T *attrs, VCOS_UNSIGNED ts);

/** The thread entry function takes (argc,argv), as per Nucleus, with
  * argc being 0. This may be withdrawn in a future release and should not
  * be used in new code.
  */
VCOS_INLINE_DECL
void _vcos_thread_attr_setlegacyapi(VCOS_THREAD_ATTR_T *attrs, VCOS_UNSIGNED legacy);

VCOS_INLINE_DECL
void vcos_thread_attr_setautostart(VCOS_THREAD_ATTR_T *attrs, VCOS_UNSIGNED autostart);

#ifdef __cplusplus
}
#endif
#endif
