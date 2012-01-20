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
Create the vcos_malloc API from the regular system malloc/free
=============================================================================*/

/**
  * \file
  *
  * Create the vcos malloc API from a regular system malloc/free library.
  *
  * The API lets callers specify an alignment.
  *
  * Under VideoCore this is not needed, as we can simply use the rtos_malloc routines.
  * But on host platforms that won't be the case.
  *
  */

VCOSPRE_ void * VCOSPOST_  vcos_generic_mem_alloc(VCOS_UNSIGNED sz, const char *desc);
VCOSPRE_  void * VCOSPOST_ vcos_generic_mem_calloc(VCOS_UNSIGNED count, VCOS_UNSIGNED sz, const char *descr);
VCOSPRE_  void VCOSPOST_   vcos_generic_mem_free(void *ptr);
VCOSPRE_  void * VCOSPOST_ vcos_generic_mem_alloc_aligned(VCOS_UNSIGNED sz, VCOS_UNSIGNED align, const char *desc);

#ifdef VCOS_INLINE_BODIES

VCOS_INLINE_IMPL
void *vcos_malloc(VCOS_UNSIGNED size, const char *description) {
   return vcos_generic_mem_alloc(size, description);
}

VCOS_INLINE_IMPL
void *vcos_calloc(VCOS_UNSIGNED num, VCOS_UNSIGNED size, const char *description) {
   return vcos_generic_mem_calloc(num, size, description);
}

VCOS_INLINE_IMPL
void vcos_free(void *ptr) {
   vcos_generic_mem_free(ptr);
}

VCOS_INLINE_IMPL
void * vcos_malloc_aligned(VCOS_UNSIGNED size, VCOS_UNSIGNED align, const char *description) {
   return vcos_generic_mem_alloc_aligned(size, align, description);
}


#endif /* VCOS_INLINE_BODIES */


