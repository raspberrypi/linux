/*****************************************************************************
* Copyright 2009 - 2010 Broadcom Corporation.  All rights reserved.
*
* Unless you and Broadcom execute a separate written software license
* agreement governing use of this software, this software is licensed to you
* under the terms of the GNU General Public License version 2, available at
* http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
*
* Notwithstanding the above, under no circumstances may you combine this
* software in any way with any other Broadcom software provided under a
* license other than the GPL, without Broadcom's express prior written
* consent.
*****************************************************************************/

/** Support to allow VCOS thread-related functions to be called from
  * threads that were not created by VCOS.
  */

#include <linux/semaphore.h>
#include <linux/vmalloc.h>
#include <linux/list.h>
#include <linux/sched.h>

#include "vcos_thread_map.h"
#include "interface/vcos/vcos_logging.h"

/*
 * Store the vcos_thread pointer at the end of
 * current kthread stack, right after the thread_info
 * structure.
 *
 * I belive we should be safe here to steal these 4 bytes
 * from the stack, as long as the vcos thread does not use up
 * all the stack available
 *
 * NOTE: This scheme will not work on architectures with stack growing up
 */

/* Shout, if we are not being compiled for ARM kernel */

#ifndef CONFIG_ARM
#error " **** The vcos kthread implementation may not work for non-ARM kernel ****"
#endif

static inline void *to_current_vcos_thread(void)
{
   unsigned long *vcos_data;

   vcos_data = (unsigned long *)((char *)current_thread_info() + sizeof(struct thread_info));

   return (void *)vcos_data;
}


static inline void *to_vcos_thread(struct task_struct *tsk)
{
   unsigned long *vcos_data;

   vcos_data = (unsigned long *)((char *)tsk->stack + sizeof(struct thread_info));

   return (void *)vcos_data;
}

/**
   @fn uint32_t vcos_add_thread(THREAD_MAP_T *vcos_thread);
*/
uint32_t vcos_add_thread(VCOS_THREAD_T *vcos_thread)
{
   VCOS_THREAD_T **vcos_thread_storage = (VCOS_THREAD_T **)to_current_vcos_thread();

   *vcos_thread_storage = vcos_thread;

   return(0);
}


/**
   @fn uint32_t vcos_remove_thread(struct task_struct * thread_id);
*/
uint32_t vcos_remove_thread(struct task_struct *thread_id)
{
   /* Remove thread_id -> VCOS_THREAD_T relationship */
   VCOS_THREAD_T **vcos_thread_storage;

   /*
    * We want to be able to build vcos as a loadable module, which
    * means that we can't call get_task_struct. So we assert if we're
    * ever called with thread_id != current.
    */

   BUG_ON( thread_id != current );

   vcos_thread_storage = (VCOS_THREAD_T **)to_vcos_thread(thread_id);

   *(unsigned long *)vcos_thread_storage = 0xCAFEBABE;

   return(0);
}


VCOS_THREAD_T *vcos_kthread_current(void)
{
   VCOS_THREAD_T **vcos_thread_storage = (VCOS_THREAD_T **)to_current_vcos_thread();

   /* If we find this, either the thread is already dead or stack pages of a
    * dead vcos thread are re-allocated to this one.
    *
    * Since there's no way to differentiate between these 2 cases, we just dump
    * the current task name to the log.
    *
    * If the current thread is created using VCOS API, you should *never* see this
    * print.
    * 
    * If its a non-VCOS thread, just let it go ...
    *
    * To debug VCOS, uncomment printk's under the "if" condition below
    *
    */
   if (*vcos_thread_storage == (void *)0xCAFEBABE)
   {
     #if 0
      printk(KERN_DEBUG"****************************************************\n");
      printk(KERN_DEBUG"%s : You have a problem, if \"%s\" is a VCOS thread\n",__func__, current->comm);
      printk(KERN_DEBUG"****************************************************\n");
     #endif
   }

   return *vcos_thread_storage;
}
