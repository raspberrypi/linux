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
