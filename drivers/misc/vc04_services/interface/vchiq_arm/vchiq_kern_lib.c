/*****************************************************************************
* Copyright 2001 - 2011 Broadcom Corporation.  All rights reserved.
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

/* ---- Include Files ---------------------------------------------------- */

#include <linux/kernel.h>
#include <linux/module.h>

#include "vchiq_core.h"
#include "vchiq_arm.h"
#include "interface/vcos/vcos_logging.h"

/* ---- Public Variables ------------------------------------------------- */

extern VCOS_LOG_CAT_T vchiq_core_log_category;
#define  VCOS_LOG_CATEGORY (&vchiq_core_log_category)

/* ---- Private Constants and Types -------------------------------------- */

struct vchiq_instance_struct {
   VCHIQ_STATE_T *state;

   int connected;
};

/****************************************************************************
*
*   vchiq_initialise
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_initialise( VCHIQ_INSTANCE_T *instanceOut )
{
   VCHIQ_STATUS_T status = VCHIQ_ERROR;
   VCHIQ_STATE_T *state;
   VCHIQ_INSTANCE_T instance = NULL;

   vcos_log_trace( "%s called", __func__ );

   state = vchiq_get_state();
   if (!state)
   {
      printk( KERN_ERR "%s: videocore not initialized\n", __func__ );
      goto failed;
   }

   instance = kzalloc( sizeof(*instance), GFP_KERNEL );
   if( !instance )
   {
      printk( KERN_ERR "%s: error allocating vchiq instance\n", __func__ );
      goto failed;
   }

   instance->connected = 0;
   instance->state = state;

   *instanceOut = instance;
   
   status = VCHIQ_SUCCESS;

failed:
   vcos_log_trace( "%s(%p): returning %d", __func__, instance, status );

   return status;
}

/****************************************************************************
*
*   vchiq_shutdown
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_shutdown( VCHIQ_INSTANCE_T instance )
{
   VCHIQ_STATUS_T status;
   VCHIQ_STATE_T *state = instance->state;

   vcos_log_trace( "%s(%p) called", __func__, instance );

   vcos_mutex_lock(&state->mutex);

   /* Remove all services */
   status = vchiq_shutdown_internal(state, instance);

   vcos_mutex_unlock(&state->mutex);

   if (status == VCHIQ_SUCCESS)
      kfree(instance);

   vcos_log_trace( "%s(%p): returning %d", __func__, instance, status );

   return status;
}

/****************************************************************************
*
*   vchiq_is_connected
*
***************************************************************************/

int vchiq_is_connected(VCHIQ_INSTANCE_T instance)
{
   return instance->connected;
}

/****************************************************************************
*
*   vchiq_connect
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_connect(VCHIQ_INSTANCE_T instance)
{
   VCHIQ_STATUS_T status;
   VCHIQ_STATE_T *state = instance->state;

   vcos_log_trace( "%s(%p) called", __func__, instance );

   if (vcos_mutex_lock(&state->mutex) != VCOS_SUCCESS) {
      vcos_log_trace( "%s: call to vcos_mutex_lock failed", __func__ );
      status = VCHIQ_RETRY;
      goto failed;
   }
   status = vchiq_connect_internal(state, instance);

   if (status == VCHIQ_SUCCESS)
      instance->connected = 1;

   vcos_mutex_unlock(&state->mutex);

failed:
   vcos_log_trace( "%s(%p): returning %d", __func__, instance, status );

   return status;
}

/****************************************************************************
*
*   vchiq_add_service
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_add_service(
   VCHIQ_INSTANCE_T        instance,
   int                     fourcc,
   VCHIQ_CALLBACK_T        callback,
   void                   *userdata,
   VCHIQ_SERVICE_HANDLE_T *pservice)
{
   VCHIQ_SERVICE_PARAMS_T params;

   params.fourcc        = fourcc;
   params.callback      = callback;
   params.userdata      = userdata;
   params.version       = 0;
   params.version_min   = 0;

   return vchiq_add_service_params(instance, &params, pservice);
}

/****************************************************************************
*
*   vchiq_open_service
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_open_service(
   VCHIQ_INSTANCE_T        instance,
   int                     fourcc,
   VCHIQ_CALLBACK_T        callback,
   void                   *userdata,
   VCHIQ_SERVICE_HANDLE_T *pservice)
{
   VCHIQ_SERVICE_PARAMS_T params;

   params.fourcc        = fourcc;
   params.callback      = callback;
   params.userdata      = userdata;
   params.version       = 0;
   params.version_min   = 0;

   return vchiq_open_service_params(instance, &params, pservice);
}

/****************************************************************************
*
*   vchiq_add_service_params
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_add_service_params(
   VCHIQ_INSTANCE_T              instance,
   const VCHIQ_SERVICE_PARAMS_T *params,
   VCHIQ_SERVICE_HANDLE_T       *pservice)
{
   VCHIQ_STATUS_T status;
   VCHIQ_STATE_T *state = instance->state;
   VCHIQ_SERVICE_T *service;
   int srvstate;

   vcos_log_trace( "%s(%p) called", __func__, instance );

   *pservice = NULL;

   srvstate = vchiq_is_connected( instance )
      ? VCHIQ_SRVSTATE_LISTENING
      : VCHIQ_SRVSTATE_HIDDEN;

   vcos_mutex_lock(&state->mutex);

   service = vchiq_add_service_internal(
      state,
      params,
      srvstate,
      instance);

   vcos_mutex_unlock(&state->mutex);

   if ( service  )
   {
      *pservice = &service->base;
      status = VCHIQ_SUCCESS;
   }
   else
   {
      status = VCHIQ_ERROR;
   }

   vcos_log_trace( "%s(%p): returning %d", __func__, instance, status );

   return status;
}

/****************************************************************************
*
*   vchiq_open_service_params
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_open_service_params(
   VCHIQ_INSTANCE_T              instance,
   const VCHIQ_SERVICE_PARAMS_T *params,
   VCHIQ_SERVICE_HANDLE_T       *pservice)
{
   VCHIQ_STATUS_T   status = VCHIQ_ERROR;
   VCHIQ_STATE_T   *state = instance->state;
   VCHIQ_SERVICE_T *service;

   vcos_log_trace( "%s(%p) called", __func__, instance );

   *pservice = NULL;

   if (!vchiq_is_connected(instance))
      goto failed;

   vcos_mutex_lock(&state->mutex);

   service = vchiq_add_service_internal(state,
      params,
      VCHIQ_SRVSTATE_OPENING,
      instance);

   vcos_mutex_unlock(&state->mutex);

   if ( service  )
   {
      status = vchiq_open_service_internal(service, current->pid);
      if ( status == VCHIQ_SUCCESS )
         *pservice = &service->base;
      else
         vchiq_remove_service(&service->base);
   }

failed:
   vcos_log_trace( "%s(%p): returning %d", __func__, instance, status );

   return status;
}

EXPORT_SYMBOL(vchiq_initialise);
EXPORT_SYMBOL(vchiq_shutdown);
EXPORT_SYMBOL(vchiq_connect);
EXPORT_SYMBOL(vchiq_add_service);
EXPORT_SYMBOL(vchiq_open_service);
EXPORT_SYMBOL(vchiq_add_service_params);
EXPORT_SYMBOL(vchiq_open_service_params);
