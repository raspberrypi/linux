/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  vcos

FILE DESCRIPTION
VideoCore OS Abstraction Layer - Linux kernel (partial) implementation.
=============================================================================*/

/* Do not include this file directly - instead include it via vcos.h */

/** @file
  *
  * Linux kernel (partial) implementation of VCOS.
  *
  */

#ifndef VCOS_PLATFORM_H
#define VCOS_PLATFORM_H

#include <linux/types.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <asm/bitops.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/vmalloc.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/uaccess.h>
#include <linux/time.h>  /* for time_t */
#include <linux/slab.h>
#include <linux/vmalloc.h>

#define VCOS_HAVE_RTOS         1
#define VCOS_HAVE_SEMAPHORE    1
#define VCOS_HAVE_EVENT        1
#define VCOS_HAVE_QUEUE        0
#define VCOS_HAVE_LEGACY_ISR   0
#define VCOS_HAVE_TIMER        1
#define VCOS_HAVE_CANCELLATION_SAFE_TIMER 0
#define VCOS_HAVE_MEMPOOL      0
#define VCOS_HAVE_ISR          0
#define VCOS_HAVE_ATOMIC_FLAGS 1
#define VCOS_HAVE_BLOCK_POOL   0
#define VCOS_HAVE_ONCE         1
#define VCOS_HAVE_FILE         0
#define VCOS_HAVE_USER_BUF     0
#define VCOS_HAVE_CFG          1
#define VCOS_HAVE_SPINLOCK     0
#define VCOS_HAVE_CMD          1
#define VCOS_HAVE_EVENT_FLAGS  1

/* Exclude many VCOS classes which don't have predicates */
#define VCOS_TLS_H
#define VCOS_NAMED_MUTEX_H
#define VCOS_REENTRANT_MUTEX_H
#define VCOS_NAMED_SEMAPHORE_H
#define VCOS_QUICKSLOW_MUTEX_H
/*#define VCOS_INIT_H */
/*#define VCOS_MEM_H */
/*#define VCOS_STRING_H */

typedef struct semaphore      VCOS_SEMAPHORE_T;
typedef struct semaphore      VCOS_EVENT_T;
typedef struct mutex          VCOS_MUTEX_T;
typedef volatile int          VCOS_ONCE_T;

typedef unsigned int          VCOS_UNSIGNED;
typedef unsigned int          VCOS_OPTION;
typedef atomic_t              VCOS_ATOMIC_FLAGS_T;

typedef struct
{
    struct  timer_list      linux_timer;
    void                   *context;
    void                  (*expiration_routine)(void *context);

} VCOS_TIMER_T;

typedef struct VCOS_LLTHREAD_T
{
   struct task_struct *thread;             /**< The thread itself */
   VCOS_SEMAPHORE_T suspend;     /**< For support event groups and similar - a per thread semaphore */
} VCOS_LLTHREAD_T;

typedef enum
{
    VCOS_O_RDONLY   = 00000000,
    VCOS_O_WRONLY   = 00000001,
    VCOS_O_RDWR     = 00000002,
    VCOS_O_TRUNC    = 00001000,
} VCOS_FILE_FLAGS_T;

typedef struct file *VCOS_FILE_T;

#define VCOS_SUSPEND          -1
#define VCOS_NO_SUSPEND       0

#define VCOS_START 1
#define VCOS_NO_START 0

#define VCOS_THREAD_PRI_MIN   -20
#define VCOS_THREAD_PRI_MAX   19

#define VCOS_THREAD_PRI_INCREASE -1
#define VCOS_THREAD_PRI_HIGHEST  VCOS_THREAD_PRI_MIN
#define VCOS_THREAD_PRI_LOWEST   VCOS_THREAD_PRI_MAX
#define VCOS_THREAD_PRI_NORMAL ((VCOS_THREAD_PRI_MAX+VCOS_THREAD_PRI_MIN)/2)
#define VCOS_THREAD_PRI_ABOVE_NORMAL (VCOS_THREAD_PRI_NORMAL + VCOS_THREAD_PRI_INCREASE)
#define VCOS_THREAD_PRI_REALTIME VCOS_THREAD_PRI_HIGHEST

#define _VCOS_AFFINITY_DEFAULT 0
#define _VCOS_AFFINITY_CPU0 0
#define _VCOS_AFFINITY_CPU1 0
#define _VCOS_AFFINITY_MASK 0
#define VCOS_CAN_SET_STACK_ADDR  0

#define VCOS_TICKS_PER_SECOND HZ

#include "interface/vcos/generic/vcos_generic_event_flags.h"
#include "interface/vcos/generic/vcos_mem_from_malloc.h"
#include "interface/vcos/generic/vcos_joinable_thread_from_plain.h"

/***********************************************************
 *
 * Memory allcoation
 *
 ***********************************************************/

#define  _vcos_platform_malloc   vcos_platform_malloc
#define  _vcos_platform_free     vcos_platform_free

void *vcos_platform_malloc( VCOS_UNSIGNED required_size );
void  vcos_platform_free( void *ptr );

#if defined(VCOS_INLINE_BODIES)

#undef VCOS_ASSERT_LOGGING_DISABLE
#define VCOS_ASSERT_LOGGING_DISABLE 1

/***********************************************************
 *
 * Counted Semaphores
 *
 ***********************************************************/

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_semaphore_wait(VCOS_SEMAPHORE_T *sem) {
   int ret = down_interruptible(sem);
   if ( ret == 0 )
      /* Success */
      return VCOS_SUCCESS;
   else if ( ret == -EINTR )
      /* Interrupted */
      return VCOS_EINTR;
   else
      /* Default (timeout) */
      return VCOS_EAGAIN;
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_semaphore_trywait(VCOS_SEMAPHORE_T *sem) {
   if (down_trylock(sem) != 0)
      return VCOS_EAGAIN;
   return VCOS_SUCCESS;
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_semaphore_create(VCOS_SEMAPHORE_T *sem,
                                    const char *name,
                                    VCOS_UNSIGNED initial_count) {
   sema_init(sem, initial_count);
   return VCOS_SUCCESS;
}

VCOS_INLINE_IMPL
void vcos_semaphore_delete(VCOS_SEMAPHORE_T *sem) {
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_semaphore_post(VCOS_SEMAPHORE_T *sem) {
   up(sem);
   return VCOS_SUCCESS;
}

/***********************************************************
 *
 * Threads
 *
 ***********************************************************/

#include "vcos_thread_map.h"

VCOS_INLINE_IMPL
VCOS_LLTHREAD_T *vcos_llthread_current(void) {
        return &vcos_kthread_current()->thread;
}

VCOS_INLINE_IMPL
void vcos_llthread_resume(VCOS_LLTHREAD_T *thread) {
   vcos_assert(0);
}

VCOS_INLINE_IMPL
void vcos_sleep(uint32_t ms) {
   msleep(ms);
}

VCOS_INLINE_IMPL
void vcos_thread_set_priority(VCOS_THREAD_T *thread, VCOS_UNSIGNED p) {
   /* not implemented */
}
VCOS_INLINE_IMPL
VCOS_UNSIGNED vcos_thread_get_priority(VCOS_THREAD_T *thread) {
   /* not implemented */
   return 0;
}

/***********************************************************
 *
 * Miscellaneous
 *
 ***********************************************************/

VCOS_INLINE_IMPL
int vcos_strcasecmp(const char *s1, const char *s2) {
   return strcasecmp(s1,s2);
}


/***********************************************************
 *
 * Mutexes
 *
 ***********************************************************/

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_mutex_create(VCOS_MUTEX_T *m, const char *name) {
   mutex_init(m);
   return VCOS_SUCCESS;
}

VCOS_INLINE_IMPL
void vcos_mutex_delete(VCOS_MUTEX_T *m) {
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_mutex_lock(VCOS_MUTEX_T *m) {
   int ret = mutex_lock_interruptible(m);
   if ( ret == 0 )
      /* Success */
      return VCOS_SUCCESS;
   else if ( ret == -EINTR )
      /* Interrupted */
      return VCOS_EINTR;
   else
      /* Default */
      return VCOS_EAGAIN;
}

VCOS_INLINE_IMPL
void vcos_mutex_unlock(VCOS_MUTEX_T *m) {
   mutex_unlock(m);
}

VCOS_INLINE_IMPL
int vcos_mutex_is_locked(VCOS_MUTEX_T *m) {
   if (mutex_trylock(m) != 0)
      return 1; /* it was locked */
   mutex_unlock(m);
   /* it wasn't locked */
   return 0;
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_mutex_trylock(VCOS_MUTEX_T *m) {
   if (mutex_trylock(m) == 0)
      return VCOS_SUCCESS;
   else
      return VCOS_EAGAIN;
}

/* For supporting event groups - per thread semaphore */
VCOS_INLINE_IMPL
void _vcos_thread_sem_wait(void) {
   VCOS_THREAD_T *t = vcos_thread_current();
   vcos_semaphore_wait(&t->suspend);
}

VCOS_INLINE_IMPL
void _vcos_thread_sem_post(VCOS_THREAD_T *target) {
   vcos_semaphore_post(&target->suspend);
}

/***********************************************************
 *
 * Events
 *
 ***********************************************************/

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_event_create(VCOS_EVENT_T *event, const char *debug_name)
{
   sema_init(event, 0);
   return VCOS_SUCCESS;
}

VCOS_INLINE_IMPL
void vcos_event_signal(VCOS_EVENT_T *event)
{
   up(event);
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_event_wait(VCOS_EVENT_T *event)
{
   int ret = down_interruptible(event);
   if ( ret == -EINTR )
      /* Interrupted */
      return VCOS_EINTR;
   else if (ret != 0)
      /* Default (timeout) */
      return VCOS_EAGAIN;
   /* Emulate a maximum count of 1 by removing any extra upness */
   while (down_trylock(event) == 0) continue;
   return VCOS_SUCCESS;
}

VCOS_INLINE_DECL
VCOS_STATUS_T vcos_event_try(VCOS_EVENT_T *event)
{
   return (down_trylock(event) == 0) ? VCOS_SUCCESS : VCOS_EAGAIN;
}

VCOS_INLINE_IMPL
void vcos_event_delete(VCOS_EVENT_T *event)
{
}

/***********************************************************
 *
 * Timers
 *
 ***********************************************************/

VCOS_INLINE_DECL
void vcos_timer_linux_func(unsigned long data)
{
    VCOS_TIMER_T    *vcos_timer = (VCOS_TIMER_T *)data;

    vcos_timer->expiration_routine( vcos_timer->context );
}

VCOS_INLINE_DECL
VCOS_STATUS_T vcos_timer_create(VCOS_TIMER_T *timer,
                                const char *name,
                                void (*expiration_routine)(void *context),
                                void *context) {
	init_timer(&timer->linux_timer);
	timer->linux_timer.data = (unsigned long)timer;
	timer->linux_timer.function = vcos_timer_linux_func;

    timer->context = context;
    timer->expiration_routine = expiration_routine;

    return VCOS_SUCCESS;
}

VCOS_INLINE_IMPL
void vcos_timer_set(VCOS_TIMER_T *timer, VCOS_UNSIGNED delay_ms) {
	timer->linux_timer.expires = jiffies + msecs_to_jiffies(delay_ms);
       add_timer(&timer->linux_timer);
}

VCOS_INLINE_IMPL
void vcos_timer_cancel(VCOS_TIMER_T *timer) {
     del_timer(&timer->linux_timer);
}

VCOS_INLINE_IMPL
void vcos_timer_reset(VCOS_TIMER_T *timer, VCOS_UNSIGNED delay_ms) {
    del_timer_sync(&timer->linux_timer);
    timer->linux_timer.expires = jiffies + msecs_to_jiffies(delay_ms);
    add_timer(&timer->linux_timer);
}

VCOS_INLINE_IMPL
void vcos_timer_delete(VCOS_TIMER_T *timer) {
    timer->context = NULL;
    timer->expiration_routine = NULL;
    timer->linux_timer.function = NULL;
    timer->linux_timer.data = 0;
    return;
}

VCOS_INLINE_IMPL
VCOS_UNSIGNED vcos_process_id_current(void) {
   return (VCOS_UNSIGNED)current->pid;
}


VCOS_INLINE_IMPL
int vcos_in_interrupt(void) {
   return in_interrupt();
}

/***********************************************************
 *
 * Atomic flags
 *
 ***********************************************************/

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_atomic_flags_create(VCOS_ATOMIC_FLAGS_T *atomic_flags)
{
   atomic_set(atomic_flags, 0);
   return VCOS_SUCCESS;
}

VCOS_INLINE_IMPL
void vcos_atomic_flags_or(VCOS_ATOMIC_FLAGS_T *atomic_flags, uint32_t flags)
{
   uint32_t value;
   do {
      value = atomic_read(atomic_flags);
   } while (atomic_cmpxchg(atomic_flags, value, value | flags) != value);
}

VCOS_INLINE_IMPL
uint32_t vcos_atomic_flags_get_and_clear(VCOS_ATOMIC_FLAGS_T *atomic_flags)
{
   return atomic_xchg(atomic_flags, 0);
}

VCOS_INLINE_IMPL
void vcos_atomic_flags_delete(VCOS_ATOMIC_FLAGS_T *atomic_flags)
{
}

#undef VCOS_ASSERT_LOGGING_DISABLE
#define VCOS_ASSERT_LOGGING_DISABLE 0

#endif /* VCOS_INLINE_BODIES */

VCOS_INLINE_DECL void _vcos_thread_sem_wait(void);
VCOS_INLINE_DECL void _vcos_thread_sem_post(VCOS_THREAD_T *);

/***********************************************************
 *
 * Misc
 *
 ***********************************************************/
VCOS_INLINE_DECL char *vcos_strdup(const char *str);

/***********************************************************
 *
 * Logging
 *
 ***********************************************************/

VCOSPRE_ const char * VCOSPOST_ _vcos_log_level(void);
#define _VCOS_LOG_LEVEL() _vcos_log_level()

#define  vcos_log_platform_init()               _vcos_log_platform_init()
#define  vcos_log_platform_register(category)   _vcos_log_platform_register(category)
#define  vcos_log_platform_unregister(category) _vcos_log_platform_unregister(category)

struct VCOS_LOG_CAT_T;  /* Forward declaration since vcos_logging.h hasn't been included yet */

void _vcos_log_platform_init(void);
void _vcos_log_platform_register(struct VCOS_LOG_CAT_T *category);
void _vcos_log_platform_unregister(struct VCOS_LOG_CAT_T *category);

/***********************************************************
 *
 * Memory barriers
 *
 ***********************************************************/

#define vcos_wmb(x) wmb()
#define vcos_rmb() rmb()

#include "interface/vcos/generic/vcos_common.h"
/*#include "interface/vcos/generic/vcos_generic_quickslow_mutex.h" */

#endif /* VCOS_PLATFORM_H */

