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
VideoCore OS Abstraction Layer - thread reaping
=============================================================================*/

#ifndef VCOS_THREAD_REAPER_H
#define VCOS_THREAD_REAPER_H

#define VCOS_HAVE_THREAD_REAPER

/** Initialise the thread reaper.
  */
VCOS_STATUS_T vcos_thread_reaper_init(void);

/** Reap a thread. Arranges for the thread to be automatically
  * joined.
  *
  * @sa vcos_thread_join().
  *
  * @param thread           the thread to terminate
  * @param on_terminated    called after the thread has exited
  * @param cxt              pass back to the callback
  *
  */
void vcos_thread_reap(VCOS_THREAD_T *thread, void (*on_terminated)(void*), void *cxt);

#endif


