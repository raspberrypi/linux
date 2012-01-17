/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  osal

FILE DESCRIPTION
VideoCore OS Abstraction Layer - platform-specific types and defines
=============================================================================*/

#ifndef VCOS_PLATFORM_TYPES_H
#define VCOS_PLATFORM_TYPES_H

#include <stddef.h>
#include <linux/types.h>
#include <linux/bug.h>

#define VCOSPRE_ extern
#define VCOSPOST_

#if defined(__GNUC__) && (( __GNUC__ > 2 ) || (( __GNUC__ == 2 ) && ( __GNUC_MINOR__ >= 3 )))
#define VCOS_FORMAT_ATTR_(ARCHETYPE, STRING_INDEX, FIRST_TO_CHECK)  __attribute__ ((format (ARCHETYPE, STRING_INDEX, FIRST_TO_CHECK)))
#else
#define VCOS_FORMAT_ATTR_(ARCHETYPE, STRING_INDEX, FIRST_TO_CHECK)
#endif

#if !defined( __STDC_VERSION__ )
#define __STDC_VERSION__ 199901L
#endif

#if !defined( __STDC_VERSION )
#define __STDC_VERSION   __STDC_VERSION__
#endif

static inline void __vcos_bkpt( void ) { BUG(); }
#define VCOS_BKPT __vcos_bkpt()

#define VCOS_ASSERT_MSG(...) printk( KERN_ERR "vcos_assert: " __VA_ARGS__ )

#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIo64 "llo"
#define PRIu64 "llu"
#define PRIx64 "llx"

#endif
