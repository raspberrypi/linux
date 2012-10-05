/*****************************************************************************
* Copyright 2001 - 2010 Broadcom Corporation.  All rights reserved.
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

#include "vchiq_connected.h"
#include "vchiq_core.h"
#include <linux/module.h>
#include <linux/mutex.h>

#define  MAX_CALLBACKS  10

static   int                        g_connected;
static   int                        g_num_deferred_callbacks;
static   VCHIQ_CONNECTED_CALLBACK_T g_deferred_callback[MAX_CALLBACKS];
static   int                        g_once_init;
static   struct mutex               g_connected_mutex;

/****************************************************************************
*
* Function to initialize our lock.
*
***************************************************************************/

static void connected_init(void)
{
	if (!g_once_init) {
		mutex_init(&g_connected_mutex);
		g_once_init = 1;
	}
}

/****************************************************************************
*
* This function is used to defer initialization until the vchiq stack is
* initialized. If the stack is already initialized, then the callback will
* be made immediately, otherwise it will be deferred until
* vchiq_call_connected_callbacks is called.
*
***************************************************************************/

void vchiq_add_connected_callback(VCHIQ_CONNECTED_CALLBACK_T callback)
{
	connected_init();

	if (mutex_lock_interruptible(&g_connected_mutex) != 0)
		return;

	if (g_connected)
		/* We're already connected. Call the callback immediately. */

		callback();
	else {
		if (g_num_deferred_callbacks >= MAX_CALLBACKS)
			vchiq_log_error(vchiq_core_log_level,
				"There already %d callback registered - "
				"please increase MAX_CALLBACKS",
				g_num_deferred_callbacks);
		else {
			g_deferred_callback[g_num_deferred_callbacks] =
				callback;
			g_num_deferred_callbacks++;
		}
	}
	mutex_unlock(&g_connected_mutex);
}

/****************************************************************************
*
* This function is called by the vchiq stack once it has been connected to
* the videocore and clients can start to use the stack.
*
***************************************************************************/

void vchiq_call_connected_callbacks(void)
{
	int i;

	connected_init();

	if (mutex_lock_interruptible(&g_connected_mutex) != 0)
		return;

	for (i = 0; i <  g_num_deferred_callbacks; i++)
		g_deferred_callback[i]();

	g_num_deferred_callbacks = 0;
	g_connected = 1;
	mutex_unlock(&g_connected_mutex);
}
EXPORT_SYMBOL(vchiq_add_connected_callback);
