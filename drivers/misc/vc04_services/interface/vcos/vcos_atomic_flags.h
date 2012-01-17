/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver (just for consistency with the rest of vcos ;)

FILE DESCRIPTION
VideoCore OS Abstraction Layer - public header file
=============================================================================*/

#ifndef VCOS_ATOMIC_FLAGS_H
#define VCOS_ATOMIC_FLAGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"
#include "vcos_platform.h"

/**
 * \file vcos_atomic_flags.h
 *
 * Defines atomic flags API.
 *
 * 32 flags. Atomic "or" and "get and clear" operations
 */

/**
 * Create an atomic flags instance.
 *
 * @param atomic_flags Pointer to atomic flags instance, filled in on return
 *
 * @return VCOS_SUCCESS if succeeded.
 */
VCOS_INLINE_DECL
VCOS_STATUS_T vcos_atomic_flags_create(VCOS_ATOMIC_FLAGS_T *atomic_flags);

/**
 * Atomically set the specified flags.
 *
 * @param atomic_flags Instance to set flags on
 * @param flags        Mask of flags to set
 */
VCOS_INLINE_DECL
void vcos_atomic_flags_or(VCOS_ATOMIC_FLAGS_T *atomic_flags, uint32_t flags);

/**
 * Retrieve the current flags and then clear them. The entire operation is
 * atomic.
 *
 * @param atomic_flags Instance to get/clear flags from/on
 *
 * @return Mask of flags which were set (and we cleared)
 */
VCOS_INLINE_DECL
uint32_t vcos_atomic_flags_get_and_clear(VCOS_ATOMIC_FLAGS_T *atomic_flags);

/**
 * Delete an atomic flags instance.
 *
 * @param atomic_flags Instance to delete
 */
VCOS_INLINE_DECL
void vcos_atomic_flags_delete(VCOS_ATOMIC_FLAGS_T *atomic_flags);

#ifdef __cplusplus
}
#endif

#endif
