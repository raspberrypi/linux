/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  vcos

FILE DESCRIPTION
VideoCore OS Abstraction Layer - pthreads types
=============================================================================*/

#define  VCOS_INLINE_BODIES
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/version.h>

#if defined( CONFIG_BCM_KNLLOG_SUPPORT )
#include <linux/broadcom/knllog.h>
#endif
#include "interface/vcos/vcos.h"
#ifdef HAVE_VCOS_VERSION
#include "interface/vcos/vcos_build_info.h"
#endif

VCOS_CFG_ENTRY_T  vcos_cfg_dir;
VCOS_CFG_ENTRY_T  vcos_logging_cfg_dir;
VCOS_CFG_ENTRY_T  vcos_version_cfg;

#ifndef VCOS_DEFAULT_STACK_SIZE
#define VCOS_DEFAULT_STACK_SIZE 4096
#endif

static VCOS_THREAD_ATTR_T default_attrs = {
   0,
   VCOS_DEFAULT_STACK_SIZE,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
static DEFINE_SEMAPHORE(lock);
#else
static DECLARE_MUTEX(lock);
#endif

typedef void (*LEGACY_ENTRY_FN_T)(int, void *);

/** Wrapper function around the real thread function. Posts the semaphore
  * when completed.
  */
static int vcos_thread_wrapper(void *arg)
{
   void *ret;
   VCOS_THREAD_T *thread = arg;

   vcos_assert(thread->magic == VCOS_THREAD_MAGIC);

   thread->thread.thread = current;

   vcos_add_thread(thread);

#ifdef VCOS_WANT_TLS_EMULATION
   vcos_tls_thread_register(&thread->_tls);
#endif

   if (thread->legacy)
   {
      LEGACY_ENTRY_FN_T fn = (LEGACY_ENTRY_FN_T)thread->entry;
      fn(0,thread->arg);
      ret = 0;
   }
   else
   {
      ret = thread->entry(thread->arg);
   }

   thread->exit_data = ret;

   vcos_remove_thread(current);

   /* For join and cleanup */
   vcos_semaphore_post(&thread->wait);

   return 0;
}

VCOS_STATUS_T vcos_thread_create(VCOS_THREAD_T *thread,
                                 const char *name,
                                 VCOS_THREAD_ATTR_T *attrs,
                                 VCOS_THREAD_ENTRY_FN_T entry,
                                 void *arg)
{
   VCOS_STATUS_T st;
   struct task_struct *kthread;

   memset(thread, 0, sizeof(*thread));
   thread->magic     = VCOS_THREAD_MAGIC;
   strlcpy( thread->name, name, sizeof( thread->name ));
   thread->legacy    = attrs ? attrs->legacy : 0;
   thread->entry = entry;
   thread->arg = arg;

   if (!name)
   {
      vcos_assert(0);
      return VCOS_EINVAL;
   }

   st = vcos_semaphore_create(&thread->wait, NULL, 0);
   if (st != VCOS_SUCCESS)
   {
      return st;
   }

   st = vcos_semaphore_create(&thread->suspend, NULL, 0);
   if (st != VCOS_SUCCESS)
   {
      return st;
   }

   /*required for event groups */
   vcos_timer_create(&thread->_timer.timer, thread->name, NULL, NULL);

   kthread = kthread_create((int (*)(void *))vcos_thread_wrapper, (void*)thread, name);
   vcos_assert(kthread != NULL);
   set_user_nice(kthread, attrs->ta_priority);
   thread->thread.thread = kthread;
   wake_up_process(kthread);
   return VCOS_SUCCESS;
}

void vcos_thread_join(VCOS_THREAD_T *thread,
                             void **pData)
{
   vcos_assert(thread);
   vcos_assert(thread->magic == VCOS_THREAD_MAGIC);

   thread->joined = 1;

   vcos_semaphore_wait(&thread->wait);

   if (pData)
   {
      *pData = thread->exit_data;
   }

   /* Clean up */
   if (thread->stack)
      vcos_free(thread->stack);

   vcos_semaphore_delete(&thread->wait);
   vcos_semaphore_delete(&thread->suspend);

}

uint32_t vcos_getmicrosecs( void )
{
   struct timeval tv;
/*XXX FIX ME! switch to ktime_get_ts to use MONOTONIC clock */
   do_gettimeofday(&tv);
   return (tv.tv_sec*1000000) + tv.tv_usec;
}

VCOS_STATUS_T vcos_timer_init(void)
{
    return VCOS_SUCCESS;
}

static const char *log_prefix[] =
{
   "",            /* VCOS_LOG_UNINITIALIZED */
   "",            /* VCOS_LOG_NEVER */
   KERN_ERR,      /* VCOS_LOG_ERROR */
   KERN_WARNING,  /* VCOS_LOG_WARN */
   KERN_INFO,     /* VCOS_LOG_INFO */
   KERN_INFO      /* VCOS_LOG_TRACE */
};

void vcos_vlog_default_impl(const VCOS_LOG_CAT_T *cat, VCOS_LOG_LEVEL_T _level, const char *fmt, va_list args)
{
   char *newline = strchr( fmt, '\n' );
   const char  *prefix;
   const char  *real_fmt;

   preempt_disable();
   {
       if ( *fmt == '<' )
       {
           prefix = fmt;
           real_fmt= &fmt[3];
       }
       else
       {
          prefix = log_prefix[_level];
          real_fmt = fmt;
       }
#if defined( CONFIG_BCM_KNLLOG_SUPPORT )
       knllog_ventry( "vcos", real_fmt, args );
#endif
       printk( "%.3svcos: [%d]: ", prefix, current->pid );
       vprintk( real_fmt, args );

       if ( newline == NULL )
       {
          printk("\n");
       }
   }
   preempt_enable();
}


const char * _vcos_log_level(void)
{
   return NULL;
}

/*****************************************************************************
*
*    Displays the version information in /proc/vcos/version
*
*****************************************************************************/

#ifdef HAVE_VCOS_VERSION

static void show_version( VCOS_CFG_BUF_T buf, void *data )
{
   static const char* copyright = "Copyright (c) 2011 Broadcom";

   vcos_cfg_buf_printf( buf, "Built %s %s on %s\n%s\nversion %s\n",
                        vcos_get_build_date(),
                        vcos_get_build_time(),
                        vcos_get_build_hostname(),
                        copyright,
                        vcos_get_build_version() );
}

#endif

/*****************************************************************************
*
*    Initialises vcos
*
*****************************************************************************/

VCOS_STATUS_T vcos_init(void)
{
   if ( vcos_cfg_mkdir( &vcos_cfg_dir, NULL, "vcos" ) != VCOS_SUCCESS )
   {
      printk( KERN_ERR "%s: Unable to create vcos cfg entry\n", __func__ );
   }
   vcos_logging_init();

#ifdef HAVE_VCOS_VERSION
   if ( vcos_cfg_create_entry( &vcos_version_cfg, &vcos_cfg_dir, "version",
                               show_version, NULL, NULL ) != VCOS_SUCCESS )
   {
      printk( KERN_ERR "%s: Unable to create vcos cfg entry 'version'\n", __func__ );
   }
#endif

   return VCOS_SUCCESS;
}

/*****************************************************************************
*
*    Deinitializes vcos
*
*****************************************************************************/

void vcos_deinit(void)
{
#ifdef HAVE_VCOS_VERSION
   vcos_cfg_remove_entry( &vcos_version_cfg );
#endif
   vcos_cfg_remove_entry( &vcos_cfg_dir );
}

void vcos_global_lock(void)
{
   down(&lock);
}

void vcos_global_unlock(void)
{
   up(&lock);
}

/* vcos_thread_exit() doesn't really stop this thread here
 *
 * At the moment, call to do_exit() will leak task_struct for
 * current thread, so we let the vcos_thread_wrapper() do the
 * cleanup and exit job, and we return w/o actually stopping the thread.
 *
 * ToDo: Kernel v2.6.31 onwards, it is considered safe to call do_exit()
 * from kthread, the implementation of which is combined in 2 patches
 * with commit-ids "63706172" and "cdd140bd" in oss Linux kernel tree
 */

void vcos_thread_exit(void *arg)
{
   VCOS_THREAD_T *thread = vcos_thread_current();

   vcos_assert(thread);
   vcos_assert(thread->magic == VCOS_THREAD_MAGIC);

   thread->exit_data = arg;
}

void vcos_thread_attr_init(VCOS_THREAD_ATTR_T *attrs)
{
   *attrs = default_attrs;
}

void _vcos_task_timer_set(void (*pfn)(void *), void *cxt, VCOS_UNSIGNED ms)
{
   VCOS_THREAD_T *self = vcos_thread_current();
   vcos_assert(self);
   vcos_assert(self->_timer.pfn == NULL);

   vcos_timer_create( &self->_timer.timer, "TaskTimer", pfn, cxt );
   vcos_timer_set(&self->_timer.timer, ms);
}

void _vcos_task_timer_cancel(void)
{
   VCOS_THREAD_T *self = vcos_thread_current();
   if (self->_timer.timer.linux_timer.function)
   {
      vcos_timer_cancel(&self->_timer.timer);
      vcos_timer_delete(&self->_timer.timer);
   }
}

int vcos_vsnprintf( char *buf, size_t buflen, const char *fmt, va_list ap )
{
   return vsnprintf( buf, buflen, fmt, ap );
}

int vcos_snprintf(char *buf, size_t buflen, const char *fmt, ...)
{
   int ret;
   va_list ap;
   va_start(ap,fmt);
   ret = vsnprintf(buf, buflen, fmt, ap);
   va_end(ap);
   return ret;
}

int vcos_llthread_running(VCOS_LLTHREAD_T *t) {
   vcos_assert(0);   /* this function only exists as a nasty hack for the video codecs! */
   return 1;
}

static int vcos_verify_bkpts = 1;

int vcos_verify_bkpts_enabled(void)
{
   return vcos_verify_bkpts;
}

/*****************************************************************************
*
*    _vcos_log_platform_init is called from vcos_logging_init
*
*****************************************************************************/

void _vcos_log_platform_init(void)
{
   if ( vcos_cfg_mkdir( &vcos_logging_cfg_dir, &vcos_cfg_dir, "logging" ) != VCOS_SUCCESS )
   {
      printk( KERN_ERR "%s: Unable to create logging cfg entry\n", __func__ );
   }
}

/*****************************************************************************
*
*    Called to display the contents of a logging category.
*
*****************************************************************************/

static void logging_show_category( VCOS_CFG_BUF_T buf, void *data )
{
   VCOS_LOG_CAT_T *category = data;

   vcos_cfg_buf_printf( buf, "%s\n", vcos_log_level_to_string( category->level ));
}

/*****************************************************************************
*
*    Called to parse content for a logging category.
*
*****************************************************************************/

static void logging_parse_category( VCOS_CFG_BUF_T buf, void *data )
{
   VCOS_LOG_CAT_T *category = data;
   const char *str = vcos_cfg_buf_get_str( buf );
   VCOS_LOG_LEVEL_T  level;

   if ( vcos_string_to_log_level( str, &level ) == VCOS_SUCCESS )
   {
      category->level = level;
   }
   else
   {
      printk( KERN_ERR "%s: Unrecognized logging level: '%s'\n",
              __func__, str );
   }
}

/*****************************************************************************
*
*    _vcos_log_platform_register is called from vcos_log_register whenever
*    a new category is registered.
*
*****************************************************************************/

void _vcos_log_platform_register(VCOS_LOG_CAT_T *category)
{
   VCOS_CFG_ENTRY_T  entry;

   if ( vcos_cfg_create_entry( &entry, &vcos_logging_cfg_dir, category->name,
                               logging_show_category, logging_parse_category,
                               category ) != VCOS_SUCCESS )
   {
      printk( KERN_ERR "%s: Unable to create cfg entry for logging category '%s'\n",
              __func__, category->name );
      category->platform_data = NULL;
   }
   else
   {
      category->platform_data = entry;
   }
}

/*****************************************************************************
*
*    _vcos_log_platform_unregister is called from vcos_log_unregister whenever
*    a new category is unregistered.
*
*****************************************************************************/

void _vcos_log_platform_unregister(VCOS_LOG_CAT_T *category)
{
   VCOS_CFG_ENTRY_T  entry;

   entry = category->platform_data;
   if ( entry != NULL )
   {
      if ( vcos_cfg_remove_entry( &entry ) != VCOS_SUCCESS )
      {
         printk( KERN_ERR "%s: Unable to remove cfg entry for logging category '%s'\n",
                 __func__, category->name );
      }
   }
}

/*****************************************************************************
*
*    Allocate memory.
*
*****************************************************************************/

void *vcos_platform_malloc( VCOS_UNSIGNED required_size )
{
   if ( required_size >= ( 2 * PAGE_SIZE ))
   {
      /* For larger allocations, use vmalloc, whose underlying allocator
       * returns pages
       */

      return vmalloc( required_size );
   }

   /* For smaller allocation, use kmalloc */

   return kmalloc( required_size, GFP_KERNEL );
}

/*****************************************************************************
*
*    Free previously allocated memory
*
*****************************************************************************/

void  vcos_platform_free( void *ptr )
{
   if (((unsigned long)ptr >= VMALLOC_START )
   &&  ((unsigned long)ptr < VMALLOC_END ))
   {
      vfree( ptr );
   }
   else
   {
      kfree( ptr );
   }
}

/*****************************************************************************
*
*    Execute a routine exactly once.
*
*****************************************************************************/

VCOS_STATUS_T vcos_once(VCOS_ONCE_T *once_control,
                        void (*init_routine)(void))
{
   /* In order to be thread-safe we need to re-test *once_control
    * inside the lock. The outer test is basically an optimization
    * so that once it is initialized we don't need to waste time
    * trying to acquire the lock.
    */

   if ( *once_control == 0 )
   {
       vcos_global_lock();
       if ( *once_control == 0 )
       {
           init_routine();
           *once_control = 1;
       }
       vcos_global_unlock();
   }

   return VCOS_SUCCESS;
}

/*****************************************************************************
*
*    String duplication routine.
*
*****************************************************************************/

char *vcos_strdup(const char *str)
{
    return kstrdup(str, GFP_KERNEL);
}


/* Export functions for modules to use */
EXPORT_SYMBOL( vcos_init );

EXPORT_SYMBOL( vcos_semaphore_trywait );
EXPORT_SYMBOL( vcos_semaphore_post );
EXPORT_SYMBOL( vcos_semaphore_create );
EXPORT_SYMBOL( vcos_semaphore_wait );
EXPORT_SYMBOL( vcos_semaphore_delete );

EXPORT_SYMBOL( vcos_log_impl );
EXPORT_SYMBOL( vcos_vlog_impl );
EXPORT_SYMBOL( vcos_vlog_default_impl );
EXPORT_SYMBOL( vcos_log_get_default_category );
EXPORT_SYMBOL( vcos_log_register );
EXPORT_SYMBOL( vcos_log_unregister );
EXPORT_SYMBOL( vcos_logging_init );
EXPORT_SYMBOL( vcos_log_level_to_string );
EXPORT_SYMBOL( vcos_string_to_log_level );
EXPORT_SYMBOL( vcos_log_dump_mem_impl );

EXPORT_SYMBOL( vcos_event_create );
EXPORT_SYMBOL( vcos_event_delete );
EXPORT_SYMBOL( vcos_event_flags_set );
EXPORT_SYMBOL( vcos_event_signal );
EXPORT_SYMBOL( vcos_event_wait );
EXPORT_SYMBOL( vcos_event_try );

EXPORT_SYMBOL( vcos_getmicrosecs );

EXPORT_SYMBOL( vcos_strcasecmp );
EXPORT_SYMBOL( vcos_snprintf );
EXPORT_SYMBOL( vcos_vsnprintf );

EXPORT_SYMBOL( vcos_thread_current );
EXPORT_SYMBOL( vcos_thread_join );
EXPORT_SYMBOL( vcos_thread_create );
EXPORT_SYMBOL( vcos_thread_set_priority );
EXPORT_SYMBOL( vcos_thread_exit );
EXPORT_SYMBOL( vcos_once );

EXPORT_SYMBOL( vcos_thread_attr_init );
EXPORT_SYMBOL( vcos_thread_attr_setpriority );
EXPORT_SYMBOL( vcos_thread_attr_settimeslice );
EXPORT_SYMBOL( vcos_thread_attr_setstacksize );
EXPORT_SYMBOL( _vcos_thread_attr_setlegacyapi );

EXPORT_SYMBOL( vcos_event_flags_create );
EXPORT_SYMBOL( vcos_event_flags_delete );
EXPORT_SYMBOL( vcos_event_flags_get );

EXPORT_SYMBOL( vcos_sleep );

EXPORT_SYMBOL( vcos_calloc );
EXPORT_SYMBOL( vcos_malloc );
EXPORT_SYMBOL( vcos_malloc_aligned );
EXPORT_SYMBOL( vcos_free );

EXPORT_SYMBOL( vcos_mutex_create );
EXPORT_SYMBOL( vcos_mutex_delete );
EXPORT_SYMBOL( vcos_mutex_lock );
EXPORT_SYMBOL( vcos_mutex_unlock );
EXPORT_SYMBOL( vcos_mutex_trylock );

EXPORT_SYMBOL( vcos_timer_cancel );
EXPORT_SYMBOL( vcos_timer_create );
EXPORT_SYMBOL( vcos_timer_delete );
EXPORT_SYMBOL( vcos_timer_set );

EXPORT_SYMBOL( vcos_atomic_flags_create );
EXPORT_SYMBOL( vcos_atomic_flags_delete );
EXPORT_SYMBOL( vcos_atomic_flags_or );
EXPORT_SYMBOL( vcos_atomic_flags_get_and_clear );

EXPORT_SYMBOL( vcos_verify_bkpts_enabled );

EXPORT_SYMBOL( vcos_strdup );
