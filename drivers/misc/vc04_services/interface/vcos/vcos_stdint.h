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

#ifndef VCOS_STDINT_H
#define VCOS_STDINT_H

/* Attempt to provide the types defined in stdint.h.
 *
 * Ideally this would either call out to a platform-specific
 * header file (e.g. stdint.h) or define the types on a
 * per-architecture/compiler basis. But for now we just
 * use #ifdefs.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __SYMBIAN32__

typedef signed   char      int8_t;
typedef unsigned char      uint8_t;

typedef signed   short     int16_t;
typedef unsigned short     uint16_t;

typedef int16_t            int_least16_t;

typedef signed   long      int32_t;
typedef unsigned long      uint32_t;

typedef signed long long   int64_t;
typedef unsigned long long uint64_t;

typedef int32_t            intptr_t;
typedef uint32_t           uintptr_t;

typedef int64_t            intmax_t;
typedef uint64_t           uintmax_t;

#define INT8_MIN SCHAR_MIN
#define INT8_MAX SCHAR_MAX
#define UINT8_MAX UCHAR_MAX
#define INT16_MIN SHRT_MIN
#define INT16_MAX SHRT_MAX
#define UINT16_MAX USHRT_MAX
#define INT32_MIN LONG_MIN
#define INT32_MAX LONG_MAX
#define UINT32_MAX ULONG_MAX
#define INT64_MIN LLONG_MIN
#define INT64_MAX LLONG_MAX
#define UINT64_MAX ULLONG_MAX

#define INTPTR_MIN INT32_MIN
#define INTPTR_MAX INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#define INTMAX_MIN INT64_MIN
#define INTMAX_MAX INT64_MAX
#define INT_LEAST16_MAX INT16_MAX
#define INT_LEAST16_MAX INT16_MAX

/*{{{ C99 types  - THIS WHOLE SECTION IS INCOMPATIBLE WITH C99. IT SHOULD RESIDE IN A STDINT.H SINCE THIS FILE GETS USED ON HOST SIDE */

#elif defined( __STDC__ ) && __STDC_VERSION__ >= 199901L

#include <stdint.h>

#elif defined( __GNUC__ )

#include <stdint.h>

#elif defined(_MSC_VER)                     /* Visual C define equivalent types */

#include <stddef.h> /* Avoids intptr_t being defined in vadefs.h */

typedef          __int8    int8_t;
typedef unsigned __int8    uint8_t;

typedef          __int16   int16_t;
typedef unsigned __int16   uint16_t;

typedef          __int32   int32_t;
typedef unsigned __int32   uint32_t;

typedef          __int64   int64_t;
typedef unsigned __int64   uint64_t;
typedef uint32_t           uintptr_t;
typedef int64_t            intmax_t;
typedef uint64_t           uintmax_t;
typedef int16_t            int_least16_t;

#elif defined (VCMODS_LCC)
#include <limits.h>

typedef signed   char      int8_t;
typedef unsigned char      uint8_t;

typedef signed   short     int16_t;
typedef unsigned short     uint16_t;

typedef signed   long      int32_t;
typedef unsigned long      uint32_t;

typedef signed   long      int64_t; /*!!!! PFCD, this means code using 64bit numbers will be broken on the VCE */
typedef unsigned long      uint64_t; /* !!!! PFCD */

typedef int32_t            intptr_t;
typedef uint32_t           uintptr_t;
typedef int64_t            intmax_t;
typedef uint64_t           uintmax_t;
typedef int16_t            int_least16_t;

#define INT8_MIN SCHAR_MIN
#define INT8_MAX SCHAR_MAX
#define UINT8_MAX UCHAR_MAX
#define INT16_MIN SHRT_MIN
#define INT16_MAX SHRT_MAX
#define UINT16_MAX USHRT_MAX
#define INT32_MIN LONG_MIN
#define INT32_MAX LONG_MAX
#define UINT32_MAX ULONG_MAX
#define INT64_MIN LONG_MIN /* !!!! PFCD */
#define INT64_MAX LONG_MAX /* !!!! PFCD */
#define UINT64_MAX ULONG_MAX /* !!!! PFCD */

#define INTPTR_MIN INT32_MIN
#define INTPTR_MAX INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#define INTMAX_MIN INT64_MIN
#define INTMAX_MIN INT64_MIN
#define INT_LEAST16_MAX INT16_MAX
#define INT_LEAST16_MAX INT16_MAX

#elif defined(__VIDEOCORE__)

typedef signed   char      int8_t;
typedef unsigned char      uint8_t;

typedef signed   short     int16_t;
typedef unsigned short     uint16_t;

typedef signed   long      int32_t;
typedef unsigned long      uint32_t;

typedef signed   long long int64_t;
typedef unsigned long long uint64_t;

typedef int32_t            intptr_t;
typedef uint32_t           uintptr_t;
typedef int64_t            intmax_t;
typedef uint64_t           uintmax_t;
typedef int16_t            int_least16_t;

#define INT8_MIN SCHAR_MIN
#define INT8_MAX SCHAR_MAX
#define UINT8_MAX UCHAR_MAX
#define INT16_MIN SHRT_MIN
#define INT16_MAX SHRT_MAX
#define UINT16_MAX USHRT_MAX
#define INT32_MIN LONG_MIN
#define INT32_MAX LONG_MAX
#define UINT32_MAX ULONG_MAX
#define INT64_MIN LLONG_MIN
#define INT64_MAX LLONG_MAX
#define UINT64_MAX ULLONG_MAX

#define INTPTR_MIN INT32_MIN
#define INTPTR_MAX INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#define INTMAX_MIN INT64_MIN
#define INTMAX_MAX INT64_MAX
#define INT_LEAST16_MAX INT16_MAX
#define INT_LEAST16_MAX INT16_MAX

#elif defined (__HIGHC__) && defined(_I386)

#include <stdint.h>

#else
#error Unknown platform
#endif

#ifdef __cplusplus
}
#endif
#endif /* VCOS_STDINT_H */


