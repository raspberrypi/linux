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


#ifndef VCOS_THREAD_MAP_H
#define VCOS_THREAD_MAP_H

#include <linux/string.h>

#include "vcos_platform.h"

static inline void vcos_thread_map_init(void)
{
   return;
}

static inline void vcos_thread_map_cleanup(void)
{
   return;
}

uint32_t vcos_add_thread(VCOS_THREAD_T *vcos_thread);

uint32_t vcos_remove_thread(struct task_struct *thread_id);

VCOS_THREAD_T *vcos_kthread_current(void);

#endif /*VCOS_THREAD_MAP_H */
