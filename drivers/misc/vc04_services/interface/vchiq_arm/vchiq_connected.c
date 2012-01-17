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

#include "vcos.h"
#include "vchiq_connected.h"
#include <linux/module.h>

#define  MAX_CALLBACKS  10

static   int                        g_connected = 0;
static   int                        g_num_deferred_callbacks;
static   VCHIQ_CONNECTED_CALLBACK_T g_deferred_callback[ MAX_CALLBACKS ];
static   VCOS_ONCE_T                g_once_init;
static   VCOS_MUTEX_T               g_connected_mutex;

extern VCOS_LOG_CAT_T vchiq_core_log_category;
#define  VCOS_LOG_CATEGORY (&vchiq_core_log_category)

/****************************************************************************
*
* Function to initialize our lock.
*
***************************************************************************/

static void connected_init( void )
{
   vcos_mutex_create( &g_connected_mutex, "connected_mutex");
}

/****************************************************************************
*
* This function is used to defer initialization until the vchiq stack is
* initialized. If the stack is already initialized, then the callback will
* be made immediately, otherwise it will be deferred until
* vchiq_call_connected_callbacks is called.
*
***************************************************************************/

void vchiq_add_connected_callback( VCHIQ_CONNECTED_CALLBACK_T callback )
{
   vcos_once( &g_once_init, connected_init );

   vcos_mutex_lock( &g_connected_mutex );

   if ( g_connected )
   {
      // We're already connected. Call the callback immediately.

      callback();
   }
   else
   {
      if ( g_num_deferred_callbacks >= MAX_CALLBACKS )
      {
         vcos_log_error( "There already %d callback registered - please increase MAX_CALLBACKS",
                         g_num_deferred_callbacks );
      }
      else
      {
         g_deferred_callback[ g_num_deferred_callbacks ] = callback;
         g_num_deferred_callbacks++;
      }
   }
   vcos_mutex_unlock( &g_connected_mutex );
}

/****************************************************************************
*
* This function is called by the vchiq stack once it has been connected to
* the videocore and clients can start to use the stack.
*
***************************************************************************/

void vchiq_call_connected_callbacks( void )
{
   int   i;

   vcos_once( &g_once_init, connected_init );

   vcos_mutex_lock( &g_connected_mutex );
   for ( i = 0; i <  g_num_deferred_callbacks; i++ )\
   {
      g_deferred_callback[i]();
   }
   g_num_deferred_callbacks = 0;
   g_connected = 1;
   vcos_mutex_unlock( &g_connected_mutex );
}

EXPORT_SYMBOL( vchiq_add_connected_callback );
