/*=============================================================================
Copyright (c) 2010 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  vcos

FILE DESCRIPTION
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


