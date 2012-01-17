/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver

FILE DESCRIPTION
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


