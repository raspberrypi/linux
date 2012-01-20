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

#ifndef VCOS_STRING_H
#define VCOS_STRING_H

/**
  * \file
  *
  * String functions.
  *
  */

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"
#include "vcos_platform.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

/** Case insensitive string comparison.
  *
  */

VCOS_INLINE_DECL
int vcos_strcasecmp(const char *s1, const char *s2);

VCOS_INLINE_DECL
int vcos_strncasecmp(const char *s1, const char *s2, size_t n);

VCOSPRE_ int VCOSPOST_ vcos_vsnprintf( char *buf, size_t buflen, const char *fmt, va_list ap );

VCOSPRE_ int VCOSPOST_ vcos_snprintf(char *buf, size_t buflen, const char *fmt, ...);

VCOS_STATIC_INLINE
int vcos_strlen(const char *s) { return (int)strlen(s); }

VCOS_STATIC_INLINE
int vcos_strcmp(const char *s1, const char *s2) { return strcmp(s1,s2); }

VCOS_STATIC_INLINE
int vcos_strncmp(const char *cs, const char *ct, size_t count) { return strncmp(cs, ct, count); }

VCOS_STATIC_INLINE
char *vcos_strcpy(char *dst, const char *src) { return strcpy(dst, src); }

VCOS_STATIC_INLINE
char *vcos_strncpy(char *dst, const char *src, size_t count) { return strncpy(dst, src, count); }

VCOS_STATIC_INLINE
void *vcos_memcpy(void *dst, const void *src, size_t n) {  memcpy(dst, src, n);  return dst;  }

VCOS_STATIC_INLINE
void *vcos_memset(void *p, int c, size_t n) { return memset(p, c, n); }

#ifdef __cplusplus
}
#endif
#endif
