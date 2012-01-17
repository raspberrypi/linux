// #############################################################################
// START #######################################################################
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

#include "interface/vcos/vcos.h"
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/freezer.h>
#include <linux/string.h>
#include <linux/slab.h>

/***************************************************************************** 
* 
*        vcos_semaphore_wait_freezable
*  
*****************************************************************************/

VCOS_STATUS_T vcos_semaphore_wait_freezable(VCOS_SEMAPHORE_T *sem)
{
    int rval, sig_pended = 0;
    unsigned long flags;
    struct task_struct *task = current;

    while (1) {
       rval = down_interruptible((struct semaphore *)sem);
       if (rval == 0) { /* down now */
          break;
       } else {
          if (freezing(current)) {
             try_to_freeze();
          } else {
             spin_lock_irqsave(&task->sighand->siglock, flags);
             if (test_tsk_thread_flag(task, TIF_SIGPENDING)) {
                clear_tsk_thread_flag(task, TIF_SIGPENDING);
                sig_pended = 1;
             }
             spin_unlock_irqrestore(&task->sighand->siglock, flags);
          }
       }
    }

    if (sig_pended) {
       spin_lock_irqsave(&task->sighand->siglock, flags);
       set_tsk_thread_flag(task, TIF_SIGPENDING);
       spin_unlock_irqrestore(&task->sighand->siglock, flags);
    }

    return 0;
}

EXPORT_SYMBOL( vcos_semaphore_wait_freezable );

/***************************************************************************** 
* 
*  vcos_kmalloc
*  
*  We really need to convert malloc to do kmalloc or vmalloc based on the
*  size, but for now we'll add a separate function.
*  
*****************************************************************************/

void *vcos_kmalloc(VCOS_UNSIGNED size, const char *description)
{
   (void)description;

   return kmalloc( size, GFP_KERNEL );
}

/***************************************************************************** 
* 
*  vcos_kmalloc
*  
*  We really need to convert malloc to do kmalloc or vmalloc based on the
*  size, but for now we'll add a separate function.
*  
*****************************************************************************/

void *vcos_kcalloc(VCOS_UNSIGNED num, VCOS_UNSIGNED size, const char *description)
{
   (void)description;

   return kzalloc( num * size, GFP_KERNEL );
}

/***************************************************************************** 
* 
*  vcos_kfree
*  
*****************************************************************************/

void vcos_kfree(void *ptr)
{
   kfree( ptr );
}

EXPORT_SYMBOL( vcos_kmalloc );
EXPORT_SYMBOL( vcos_kcalloc );
EXPORT_SYMBOL( vcos_kfree );

// END #########################################################################
// #############################################################################
