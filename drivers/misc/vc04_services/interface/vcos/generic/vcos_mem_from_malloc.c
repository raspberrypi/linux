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
VideoCore OS Abstraction Layer - memory alloc implementation
=============================================================================*/

#include "interface/vcos/vcos.h"

#ifndef _vcos_platform_malloc
#include <stdlib.h>
#define _vcos_platform_malloc malloc
#define _vcos_platform_free   free
#endif

typedef struct malloc_header_s {
   uint32_t guardword;
   uint32_t size;
   const char *description;
   void *ptr;
} MALLOC_HEADER_T;


#define MIN_ALIGN sizeof(MALLOC_HEADER_T)

#define GUARDWORDHEAP  0xa55a5aa5

void *vcos_generic_mem_alloc_aligned(VCOS_UNSIGNED size, VCOS_UNSIGNED align, const char *desc)
{
   int local_align = align == 0 ? 1 : align;
   int required_size = size + local_align + sizeof(MALLOC_HEADER_T);
   void *ptr = _vcos_platform_malloc(required_size);
   void *ret = (void *)VCOS_ALIGN_UP(((char *)ptr)+sizeof(MALLOC_HEADER_T), local_align);
   MALLOC_HEADER_T *h = ((MALLOC_HEADER_T *)ret)-1;

   h->size = size;
   h->description = desc;
   h->guardword = GUARDWORDHEAP;
   h->ptr = ptr;

   return ret;
}

void *vcos_generic_mem_alloc(VCOS_UNSIGNED size, const char *desc)
{
   return vcos_generic_mem_alloc_aligned(size,MIN_ALIGN,desc);
}

void *vcos_generic_mem_calloc(VCOS_UNSIGNED count, VCOS_UNSIGNED sz, const char *desc)
{
   uint32_t size = count*sz;
   void *ptr = vcos_generic_mem_alloc_aligned(size,MIN_ALIGN,desc);
   if (ptr)
   {
      memset(ptr, 0, size);
   }
   return ptr;
}

void vcos_generic_mem_free(void *ptr)
{
   MALLOC_HEADER_T *h;
   if (! ptr) return;

   h = ((MALLOC_HEADER_T *)ptr)-1;
   vcos_assert(h->guardword == GUARDWORDHEAP);
   _vcos_platform_free(h->ptr);
}

