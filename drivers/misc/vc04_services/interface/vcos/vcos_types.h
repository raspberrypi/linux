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
VideoCore OS Abstraction Layer - basic types
=============================================================================*/

#ifndef VCOS_TYPES_H
#define VCOS_TYPES_H

#define VCOS_VERSION   1

#include "vcos_platform_types.h"

#if !defined(VCOSPRE_) || !defined(VCOSPOST_)
#error VCOSPRE_ and VCOSPOST_ not defined!
#endif

/* Redefine these here; this means that existing header files can carry on
 * using the VCHPOST/VCHPRE macros rather than having huge changes, which
 * could cause nasty merge problems.
 */
#ifndef VCHPOST_
#define VCHPOST_ VCOSPOST_
#endif
#ifndef VCHPRE_
#define VCHPRE_  VCOSPRE_
#endif

/** Entry function for a lowlevel thread.
  *
  * Returns void for consistency with Nucleus/ThreadX.
  */
typedef void (*VCOS_LLTHREAD_ENTRY_FN_T)(void *);

/** Thread entry point. Returns a void* for consistency
  * with pthreads.
  */
typedef void *(*VCOS_THREAD_ENTRY_FN_T)(void*);


/* Error return codes - chosen to be similar to errno values */
typedef enum
{
   VCOS_SUCCESS,
   VCOS_EAGAIN,
   VCOS_ENOENT,
   VCOS_ENOSPC,
   VCOS_EINVAL,
   VCOS_EACCESS,
   VCOS_ENOMEM,
   VCOS_ENOSYS,
   VCOS_EEXIST,
   VCOS_ENXIO,
   VCOS_EINTR
} VCOS_STATUS_T;

/* Some compilers (MetaWare) won't inline with -g turned on, which then results
 * in a lot of code bloat. To overcome this, inline functions are forward declared
 * with the prefix VCOS_INLINE_DECL, and implemented with the prefix VCOS_INLINE_IMPL.
 *
 * That then means that in a release build, "static inline" can be used in the obvious
 * way, but in a debug build the implementations can be skipped in all but one file,
 * by using VCOS_INLINE_BODIES.
 *
 * VCOS_INLINE_DECL - put this at the start of an inline forward declaration of a VCOS
 * function.
 *
 * VCOS_INLINE_IMPL - put this at the start of an inlined implementation of a VCOS
 * function.
 *
 */

/* VCOS_EXPORT - it turns out that in some circumstances we need the implementation of
 * a function even if it is usually inlined.
 *
 * In particular, if we have a codec that is usually provided in object form, if it
 * was built for a debug build it will be full of calls to vcos_XXX(). If this is used
 * in a *release* build, then there won't be any of these calls around in the main image
 * as they will all have been inlined. The problem also exists for vcos functions called
 * from assembler.
 *
 * VCOS_EXPORT ensures that the named function will be emitted as a regular (not static-inline)
 * function inside vcos_<platform>.c so that it can be linked against. Doing this for every
 * VCOS function would be a bit code-bloat-tastic, so it is only done for those that need it.
 *
 */

#ifdef __cplusplus
#define _VCOS_INLINE inline
#else
#define _VCOS_INLINE __inline
#endif

#if defined(NDEBUG)

#ifdef __GNUC__
# define VCOS_INLINE_DECL extern __inline__
# define VCOS_INLINE_IMPL static __inline__
#else
# define VCOS_INLINE_DECL static _VCOS_INLINE   /* declare a func */
# define VCOS_INLINE_IMPL static _VCOS_INLINE   /* implement a func inline */
#endif

# if defined(VCOS_WANT_IMPL)
#  define VCOS_EXPORT
# else
#  define VCOS_EXPORT VCOS_INLINE_IMPL
# endif /* VCOS_WANT_IMPL */

#define VCOS_INLINE_BODIES

#else /* NDEBUG */

#if !defined(VCOS_INLINE_DECL)
   #define VCOS_INLINE_DECL extern
#endif
#if !defined(VCOS_INLINE_IMPL)
   #define VCOS_INLINE_IMPL
#endif
#define VCOS_EXPORT VCOS_INLINE_IMPL
#endif

#define VCOS_STATIC_INLINE static _VCOS_INLINE

#if defined(__HIGHC__) || defined(__HIGHC_ANSI__)
#define _VCOS_METAWARE
#endif

/** It seems that __FUNCTION__ isn't standard!
  */
#if __STDC_VERSION__ < 199901L
# if __GNUC__ >= 2 || defined(__VIDEOCORE__)
#  define VCOS_FUNCTION __FUNCTION__
# else
#  define VCOS_FUNCTION "<unknown>"
# endif
#else
# define VCOS_FUNCTION __func__
#endif

#define _VCOS_MS_PER_TICK (1000/VCOS_TICKS_PER_SECOND)

/* Convert a number of milliseconds to a tick count. Internal use only - fails to
 * convert VCOS_SUSPEND correctly.
 */
#define _VCOS_MS_TO_TICKS(ms) (((ms)+_VCOS_MS_PER_TICK-1)/_VCOS_MS_PER_TICK)

#define VCOS_TICKS_TO_MS(ticks) ((ticks) * _VCOS_MS_PER_TICK)

/** VCOS version of DATESTR, from pcdisk.h. Used by the hostreq service.
 */ 
typedef struct vcos_datestr
{
   uint8_t       cmsec;              /**< Centesimal mili second */
   uint16_t      date;               /**< Date */
   uint16_t      time;               /**< Time */

} VCOS_DATESTR;

/* Compile-time assert - declares invalid array length if condition
 * not met, or array of length one if OK.
 */
#define VCOS_CASSERT(e) extern char vcos_compile_time_check[1/(e)]

#define vcos_min(x,y) ((x) < (y) ? (x) : (y))
#define vcos_max(x,y) ((x) > (y) ? (x) : (y))

/** Return the count of an array. FIXME: under gcc we could make
 * this report an error for pointers using __builtin_types_compatible().
 */
#define vcos_countof(x) (sizeof((x)) / sizeof((x)[0]))

/* for backward compatibility */
#define countof(x) (sizeof((x)) / sizeof((x)[0]))

#define VCOS_ALIGN_DOWN(p,n) (((ptrdiff_t)(p)) & ~((n)-1))
#define VCOS_ALIGN_UP(p,n) VCOS_ALIGN_DOWN((ptrdiff_t)(p)+(n)-1,(n))

/** bool_t is not a POSIX type so cannot rely on it. Define it here.
  * It's not even defined in stdbool.h.
  */
typedef int32_t vcos_bool_t;
typedef int32_t vcos_fourcc_t;

#define VCOS_FALSE   0
#define VCOS_TRUE    (!VCOS_FALSE)

/** Mark unused arguments to keep compilers quiet */
#define vcos_unused(x) (void)(x)

/** For backward compatibility */
typedef vcos_fourcc_t fourcc_t;
typedef vcos_fourcc_t FOURCC_T;

#endif
