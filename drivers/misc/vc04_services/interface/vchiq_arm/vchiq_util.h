/*
 * Copyright (c) 2010-2011 Broadcom Corporation. All rights reserved.
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

#ifndef VCHIQ_UTIL_H
#define VCHIQ_UTIL_H

#include <linux/types.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/bitops.h>
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

#include "vchiq_if.h"

typedef struct {
	int size;
	int read;
	int write;

	struct semaphore pop;
	struct semaphore push;

	VCHIQ_HEADER_T **storage;
} VCHIU_QUEUE_T;

extern int  vchiu_queue_init(VCHIU_QUEUE_T *queue, int size);
extern void vchiu_queue_delete(VCHIU_QUEUE_T *queue);

extern int vchiu_queue_is_empty(VCHIU_QUEUE_T *queue);
extern int vchiu_queue_is_full(VCHIU_QUEUE_T *queue);

extern void vchiu_queue_push(VCHIU_QUEUE_T *queue, VCHIQ_HEADER_T *header);

extern VCHIQ_HEADER_T *vchiu_queue_peek(VCHIU_QUEUE_T *queue);
extern VCHIQ_HEADER_T *vchiu_queue_pop(VCHIU_QUEUE_T *queue);

#endif

