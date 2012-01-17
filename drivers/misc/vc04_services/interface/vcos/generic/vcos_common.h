/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver

FILE DESCRIPTION
VideoCore OS Abstraction Layer - common postamble code
=============================================================================*/

/** \file
  *
  * Postamble code included by the platform-specific header files
  */

#define VCOS_THREAD_PRI_DEFAULT VCOS_THREAD_PRI_NORMAL

#if !defined(VCOS_THREAD_PRI_INCREASE)
#error Which way to thread priorities go?
#endif

#if VCOS_THREAD_PRI_INCREASE < 0
/* smaller numbers are higher priority */
#define VCOS_THREAD_PRI_LESS(x) ((x)<VCOS_THREAD_PRI_MAX?(x)+1:VCOS_THREAD_PRI_MAX)
#define VCOS_THREAD_PRI_MORE(x) ((x)>VCOS_THREAD_PRI_MIN?(x)-1:VCOS_THREAD_PRI_MIN)
#else
/* bigger numbers are lower priority */
#define VCOS_THREAD_PRI_MORE(x) ((x)<VCOS_THREAD_PRI_MAX?(x)+1:VCOS_THREAD_PRI_MAX)
#define VCOS_THREAD_PRI_LESS(x) ((x)>VCOS_THREAD_PRI_MIN?(x)-1:VCOS_THREAD_PRI_MIN)
#endif

/* Convenience for Brits: */
#define VCOS_APPLICATION_INITIALISE VCOS_APPLICATION_INITIALIZE

/*
 * Check for constant definitions
 */
#ifndef VCOS_TICKS_PER_SECOND
#error VCOS_TICKS_PER_SECOND not defined
#endif

#if !defined(VCOS_THREAD_PRI_MIN) || !defined(VCOS_THREAD_PRI_MAX)
#error Priority range not defined
#endif

#if !defined(VCOS_THREAD_PRI_HIGHEST) || !defined(VCOS_THREAD_PRI_LOWEST) || !defined(VCOS_THREAD_PRI_NORMAL)
#error Priority ordering not defined
#endif

#if !defined(VCOS_CAN_SET_STACK_ADDR)
#error Can stack addresses be set on this platform? Please set this macro to either 0 or 1.
#endif

#if (_VCOS_AFFINITY_CPU0|_VCOS_AFFINITY_CPU1) & (~_VCOS_AFFINITY_MASK) 
#error _VCOS_AFFINITY_CPUxxx values are not consistent with _VCOS_AFFINITY_MASK
#endif

/** Append to the end of a singly-linked queue, O(1). Works with
  * any structure where list has members 'head' and 'tail' and
  * item has a 'next' pointer.
  */
#define VCOS_QUEUE_APPEND_TAIL(list, item) {\
   (item)->next = NULL;\
   if (!(list)->head) {\
      (list)->head = (list)->tail = (item); \
   } else {\
      (list)->tail->next = (item); \
      (list)->tail = (item); \
   } \
}

#ifndef VCOS_HAVE_TIMER
VCOSPRE_ void VCOSPOST_ vcos_timer_init(void);
#endif

