/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver

FILE DESCRIPTION
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
