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
