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
VideoCore OS Abstraction Layer - memory support
=============================================================================*/

#ifndef VCOS_MEM_H
#define VCOS_MEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"
#include "vcos_platform.h"

/** \file
  *
  * Memory allocation api (malloc/free equivalents) is for benefit of host
  * applications. VideoCore code should use rtos_XXX functions.
  *
  */


/** Allocate memory
  *
  * @param size Size of memory to allocate
  * @param description Description, to aid in debugging. May be ignored internally on some platforms.
  */
VCOS_INLINE_DECL
void *vcos_malloc(VCOS_UNSIGNED size, const char *description);

void *vcos_kmalloc(VCOS_UNSIGNED size, const char *description);
void *vcos_kcalloc(VCOS_UNSIGNED num, VCOS_UNSIGNED size, const char *description);

/** Allocate cleared memory
  *
  * @param num Number of items to allocate.
  * @param size Size of each item in bytes.
  * @param description Description, to aid in debugging. May be ignored internally on some platforms.
  */
VCOS_INLINE_DECL
void *vcos_calloc(VCOS_UNSIGNED num, VCOS_UNSIGNED size, const char *description);

/** Free memory
  *
  * Free memory that has been allocated.
  */
VCOS_INLINE_DECL
void vcos_free(void *ptr);

void vcos_kfree(void *ptr);

/** Allocate aligned memory
  *
  * Allocate memory aligned on the specified boundary.
  *
  * @param size Size of memory to allocate
  * @param description Description, to aid in debugging. May be ignored internally on some platforms.
  */
VCOS_INLINE_DECL
void *vcos_malloc_aligned(VCOS_UNSIGNED size, VCOS_UNSIGNED align, const char *description);

/** Return the amount of free heap memory
  *
  */
VCOS_INLINE_DECL
unsigned long vcos_get_free_mem(void);

#ifdef __cplusplus
}
#endif

#endif


