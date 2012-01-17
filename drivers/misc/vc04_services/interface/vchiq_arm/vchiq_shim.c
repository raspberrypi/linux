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

#include "interface/vchi/vchi.h"
#include "vchiq.h"
#include "vchiq_core.h"

#include "vchiq_util.h"

#include <stddef.h>

#if defined(__KERNEL__)
#include <linux/module.h>
#endif

#define vchiq_status_to_vchi(status) ((int32_t)status)

typedef struct {
   VCHIQ_SERVICE_HANDLE_T handle;

   VCHIU_QUEUE_T queue;

   VCHI_CALLBACK_T callback;
   void *callback_param;
} SHIM_SERVICE_T;

/* ----------------------------------------------------------------------
 * return pointer to the mphi message driver function table
 * -------------------------------------------------------------------- */
#ifdef WIN32
const VCHI_MESSAGE_DRIVER_T *
mphi_get_func_table( void )
{
   return NULL;
}
#endif

/* ----------------------------------------------------------------------
 * return pointer to the mphi message driver function table
 * -------------------------------------------------------------------- */
const VCHI_MESSAGE_DRIVER_T *
vchi_mphi_message_driver_func_table( void )
{
   return NULL;
}

/* ----------------------------------------------------------------------
 * return a pointer to the 'single' connection driver fops
 * -------------------------------------------------------------------- */
const VCHI_CONNECTION_API_T *
single_get_func_table( void )
{
   return NULL;
}

VCHI_CONNECTION_T * vchi_create_connection( const VCHI_CONNECTION_API_T * function_table,
                                            const VCHI_MESSAGE_DRIVER_T * low_level)
{
   vcos_unused(function_table);
   vcos_unused(low_level);
   return NULL;
}

/***********************************************************
 * Name: vchi_msg_peek
 *
 * Arguments:  const VCHI_SERVICE_HANDLE_T handle,
 *             void **data,
 *             uint32_t *msg_size,
 *             VCHI_FLAGS_T flags
 *
 * Description: Routine to return a pointer to the current message (to allow in place processing)
 *              The message can be removed using vchi_msg_remove when you're finished
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t vchi_msg_peek( VCHI_SERVICE_HANDLE_T handle,
                       void **data,
                       uint32_t *msg_size,
                       VCHI_FLAGS_T flags )
{
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;
   VCHIQ_HEADER_T *header;

   vcos_assert(flags == VCHI_FLAGS_NONE || flags == VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE);

   if (flags == VCHI_FLAGS_NONE)
      if (vchiu_queue_is_empty(&service->queue))
         return -1;

   header = vchiu_queue_peek(&service->queue);

   *data = header->data;
   *msg_size = header->size;

   return 0;
}

/***********************************************************
 * Name: vchi_msg_remove
 *
 * Arguments:  const VCHI_SERVICE_HANDLE_T handle,
 *
 * Description: Routine to remove a message (after it has been read with vchi_msg_peek)
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t vchi_msg_remove( VCHI_SERVICE_HANDLE_T handle )
{
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;
   VCHIQ_HEADER_T *header;

   header = vchiu_queue_pop(&service->queue);

   vchiq_release_message(service->handle, header);

   return 0;
}

/***********************************************************
 * Name: vchi_msg_queue
 *
 * Arguments:  VCHI_SERVICE_HANDLE_T handle,
 *             const void *data,
 *             uint32_t data_size,
 *             VCHI_FLAGS_T flags,
 *             void *msg_handle,
 *
 * Description: Thin wrapper to queue a message onto a connection
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t vchi_msg_queue( VCHI_SERVICE_HANDLE_T handle,
                        const void * data,
                        uint32_t data_size,
                        VCHI_FLAGS_T flags,
                        void * msg_handle )
{
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;
   VCHIQ_ELEMENT_T element = {data, data_size};
   VCHIQ_STATUS_T status;

   vcos_unused(msg_handle);

   vcos_assert(flags == VCHI_FLAGS_BLOCK_UNTIL_QUEUED);

   status = vchiq_queue_message(service->handle, &element, 1);

   // On some platforms, like linux kernel, vchiq_queue_message() may return
   // VCHIQ_RETRY, so we need to implment a retry mechanism since this
   // function is supposed to block until queued
   while ( status == VCHIQ_RETRY )
   {
      vcos_sleep( 1 );
      status = vchiq_queue_message(service->handle, &element, 1);
   }

   return vchiq_status_to_vchi(status);
}

/***********************************************************
 * Name: vchi_bulk_queue_receive
 *
 * Arguments:  VCHI_BULK_HANDLE_T handle,
 *             void *data_dst,
 *             const uint32_t data_size,
 *             VCHI_FLAGS_T flags
 *             void *bulk_handle
 *
 * Description: Routine to setup a rcv buffer
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t vchi_bulk_queue_receive( VCHI_SERVICE_HANDLE_T handle,
                                 void * data_dst,
                                 uint32_t data_size,
                                 VCHI_FLAGS_T flags,
                                 void * bulk_handle )
{
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;
   VCHIQ_BULK_MODE_T mode;
   VCHIQ_STATUS_T status;

   switch ((int)flags) {
   case VCHI_FLAGS_CALLBACK_WHEN_OP_COMPLETE | VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
      vcos_assert(service->callback);
      mode = VCHIQ_BULK_MODE_CALLBACK;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE:
      mode = VCHIQ_BULK_MODE_BLOCKING;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
   case VCHI_FLAGS_NONE:
      mode = VCHIQ_BULK_MODE_NOCALLBACK;
      break;
   default:
      vcos_assert(0);
      return vchiq_status_to_vchi(VCHIQ_ERROR);
   }

   status = vchiq_bulk_receive(service->handle, data_dst, data_size,
      bulk_handle, mode);

   // On some platforms, like linux kernel, vchiq_bulk_receive() may return
   // VCHIQ_RETRY, so we need to implment a retry mechanism since this
   // function is supposed to block until queued
   while ( status == VCHIQ_RETRY )
   {
      vcos_sleep( 1 );
      status = vchiq_bulk_receive(service->handle, data_dst, data_size,
         bulk_handle, mode);
   }

   return vchiq_status_to_vchi(status);
}

/***********************************************************
 * Name: vchi_bulk_queue_receive_reloc
 *
 * Arguments:  VCHI_BULK_HANDLE_T handle,
 *             VCHI_MEM_HANDLE_T h
 *             uint32_t offset
 *             const uint32_t data_size,
 *             VCHI_FLAGS_T flags
 *             void *bulk_handle
 *
 * Description: Routine to setup a relocatable rcv buffer
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t vchi_bulk_queue_receive_reloc( const VCHI_SERVICE_HANDLE_T handle,
                                       VCHI_MEM_HANDLE_T h,
                                       uint32_t offset,
                                       uint32_t data_size,
                                       const VCHI_FLAGS_T flags,
                                       void * const bulk_handle )
{
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;
   VCHIQ_BULK_MODE_T mode;
   VCHIQ_STATUS_T status;

   switch ((int)flags) {
   case VCHI_FLAGS_CALLBACK_WHEN_OP_COMPLETE | VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
      vcos_assert(service->callback);
      mode = VCHIQ_BULK_MODE_CALLBACK;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE:
      mode = VCHIQ_BULK_MODE_BLOCKING;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
   case VCHI_FLAGS_NONE:
      mode = VCHIQ_BULK_MODE_NOCALLBACK;
      break;
   default:
      vcos_assert(0);
      return vchiq_status_to_vchi(VCHIQ_ERROR);
   }

   status = vchiq_bulk_receive_handle(service->handle, h, (void*)offset,
      data_size, bulk_handle, mode);

   // On some platforms, like linux kernel, vchiq_bulk_receive_handle() may
   // return VCHIQ_RETRY, so we need to implment a retry mechanism since
   // this function is supposed to block until queued
   while ( status == VCHIQ_RETRY )
   {
      vcos_sleep( 1 );
      status = vchiq_bulk_receive_handle(service->handle, h, (void*)offset,
         data_size, bulk_handle, mode);
   }

   return vchiq_status_to_vchi(status);
}

/***********************************************************
 * Name: vchi_bulk_queue_transmit
 *
 * Arguments:  VCHI_BULK_HANDLE_T handle,
 *             const void *data_src,
 *             uint32_t data_size,
 *             VCHI_FLAGS_T flags,
 *             void *bulk_handle
 *
 * Description: Routine to transmit some data
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t vchi_bulk_queue_transmit( VCHI_SERVICE_HANDLE_T handle,
                                  const void * data_src,
                                  uint32_t data_size,
                                  VCHI_FLAGS_T flags,
                                  void * bulk_handle )
{
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;
   VCHIQ_BULK_MODE_T mode;
   VCHIQ_STATUS_T status;

   switch ((int)flags) {
   case VCHI_FLAGS_CALLBACK_WHEN_OP_COMPLETE | VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
      vcos_assert(service->callback);
      mode = VCHIQ_BULK_MODE_CALLBACK;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_DATA_READ:
   case VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE:
      mode = VCHIQ_BULK_MODE_BLOCKING;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
   case VCHI_FLAGS_NONE:
      mode = VCHIQ_BULK_MODE_NOCALLBACK;
      break;
   default:
      vcos_assert(0);
      return vchiq_status_to_vchi(VCHIQ_ERROR);
   }

   status = vchiq_bulk_transmit(service->handle, data_src, data_size,
      bulk_handle, mode);

   // On some platforms, like linux kernel, vchiq_bulk_transmit() may return
   // VCHIQ_RETRY, so we need to implment a retry mechanism since this
   // function is supposed to block until queued
   while ( status == VCHIQ_RETRY )
   {
      vcos_sleep( 1 );
      status = vchiq_bulk_transmit(service->handle, data_src, data_size,
         bulk_handle, mode);
   }

   return vchiq_status_to_vchi(status);
}

/***********************************************************
 * Name: vchi_bulk_queue_transmit_reloc
 *
 * Arguments:  VCHI_BULK_HANDLE_T handle,
 *             VCHI_MEM_HANDLE_T h_src,
 *             uint32_t offset,
 *             uint32_t data_size,
 *             VCHI_FLAGS_T flags,
 *             void *bulk_handle
 *
 * Description: Routine to transmit some data from a relocatable buffer
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/

int32_t vchi_bulk_queue_transmit_reloc( VCHI_SERVICE_HANDLE_T handle,
                                        VCHI_MEM_HANDLE_T h_src,
                                        uint32_t offset,
                                        uint32_t data_size,
                                        VCHI_FLAGS_T flags,
                                        void * const bulk_handle )
{
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;
   VCHIQ_BULK_MODE_T mode;
   VCHIQ_STATUS_T status;

   switch ((int)flags) {
   case VCHI_FLAGS_CALLBACK_WHEN_OP_COMPLETE | VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
      vcos_assert(service->callback);
      mode = VCHIQ_BULK_MODE_CALLBACK;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_DATA_READ:
   case VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE:
      mode = VCHIQ_BULK_MODE_BLOCKING;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
   case VCHI_FLAGS_NONE:
      mode = VCHIQ_BULK_MODE_NOCALLBACK;
      break;
   default:
      vcos_assert(0);
      return vchiq_status_to_vchi(VCHIQ_ERROR);
   }

   status = vchiq_bulk_transmit_handle(service->handle, h_src, (void*)offset,
      data_size, bulk_handle, mode);

   // On some platforms, like linux kernel, vchiq_bulk_transmit_handle() may
   // return VCHIQ_RETRY, so we need to implment a retry mechanism since this
   // function is supposed to block until queued
   while ( status == VCHIQ_RETRY )
   {
      vcos_sleep( 1 );
      status = vchiq_bulk_transmit_handle(service->handle, h_src, (void*)offset,
         data_size, bulk_handle, mode);
   }

   return vchiq_status_to_vchi(status);
}

/***********************************************************
 * Name: vchi_msg_dequeue
 *
 * Arguments:  VCHI_SERVICE_HANDLE_T handle,
 *             void *data,
 *             uint32_t max_data_size_to_read,
 *             uint32_t *actual_msg_size
 *             VCHI_FLAGS_T flags
 *
 * Description: Routine to dequeue a message into the supplied buffer
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t vchi_msg_dequeue( VCHI_SERVICE_HANDLE_T handle,
                          void *data,
                          uint32_t max_data_size_to_read,
                          uint32_t *actual_msg_size,
                          VCHI_FLAGS_T flags )
{
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;
   VCHIQ_HEADER_T *header;

   vcos_assert(flags == VCHI_FLAGS_NONE || flags == VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE);

   if (flags == VCHI_FLAGS_NONE)
      if (vchiu_queue_is_empty(&service->queue))
         return -1;

   header = vchiu_queue_pop(&service->queue);

   memcpy(data, header->data, header->size < max_data_size_to_read ? header->size : max_data_size_to_read);

   *actual_msg_size = header->size;

   vchiq_release_message(service->handle, header);

   return 0;
}

/***********************************************************
 * Name: vchi_msg_queuev
 *
 * Arguments:  VCHI_SERVICE_HANDLE_T handle,
 *             const void *data,
 *             uint32_t data_size,
 *             VCHI_FLAGS_T flags,
 *             void *msg_handle
 *
 * Description: Thin wrapper to queue a message onto a connection
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/

vcos_static_assert(sizeof(VCHI_MSG_VECTOR_T) == sizeof(VCHIQ_ELEMENT_T));
vcos_static_assert(offsetof(VCHI_MSG_VECTOR_T, vec_base) == offsetof(VCHIQ_ELEMENT_T, data));
vcos_static_assert(offsetof(VCHI_MSG_VECTOR_T, vec_len) == offsetof(VCHIQ_ELEMENT_T, size));

int32_t vchi_msg_queuev( VCHI_SERVICE_HANDLE_T handle,
                         VCHI_MSG_VECTOR_T * vector,
                         uint32_t count,
                         VCHI_FLAGS_T flags,
                         void *msg_handle )
{
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;

   vcos_unused(msg_handle);

   vcos_assert(flags == VCHI_FLAGS_BLOCK_UNTIL_QUEUED);

   return vchiq_status_to_vchi(vchiq_queue_message(service->handle, (const VCHIQ_ELEMENT_T *)vector, count));
}

#ifdef USE_MEMMGR

/***********************************************************
 * Name: vchi_msg_queuev_ex
 *
 * Arguments:  VCHI_SERVICE_HANDLE_T handle,
 *             VCHI_MSG_VECTOR_EX_T *vector
 *             uint32_t count
 *             VCHI_FLAGS_T flags,
 *             void *msg_handle
 *
 * Description: Thin wrapper to queue an array of messages onto a connection
 * Supports resolving MEM_HANDLE's at last possible moment to avoid deadlocks.
 *
 * Currently just a shim, so deadlocks are still possible!
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t vchi_msg_queuev_ex( const VCHI_SERVICE_HANDLE_T handle,
                            VCHI_MSG_VECTOR_EX_T * const vector,
                            const uint32_t count,
                            const VCHI_FLAGS_T flags,
                            void * const msg_handle )
{
   int32_t success = -1;
   // For now, we don't actually support sending anything other than
   // a pointer, so handles have to be patched up; this is likely
   // to cause deadlocks. This code is not designed to be either
   // pretty, efficient, or deadlock-free.

   #define max_vecs 16
   VCHI_MSG_VECTOR_T copy[max_vecs];
   const uint8_t *orig[max_vecs];

   int i;
   vcos_unused(msg_handle);

   if (count > sizeof(copy)/sizeof(copy[0]))
   {
      vcos_assert(0);
      return -1;
   }

   for (i=0; i<count; i++)
   {
      VCHI_MSG_VECTOR_EX_T *v = vector+i;

      switch (vector[i].type)
      {
      case VCHI_VEC_POINTER:
         copy[i].vec_base = v->u.ptr.vec_base;
         copy[i].vec_len =  v->u.ptr.vec_len;
         break;
      case VCHI_VEC_HANDLE:
         vcos_assert(v->u.handle.offset+v->u.handle.vec_len <= mem_get_size(v->u.handle.handle));
         copy[i].vec_base = (uint8_t*)mem_lock(v->u.handle.handle) + v->u.handle.offset;
         orig[i] = copy[i].vec_base;
         copy[i].vec_len = v->u.handle.vec_len;
         break;
      case VCHI_VEC_LIST:
         vcos_assert(0); // FIXME: implement this
         break;
      default:
         vcos_assert(0);
      }
   }
   success = vchi_msg_queuev( handle,
                              copy,
                              count,
                              flags &~ VCHI_FLAGS_INTERNAL,
                              msg_handle );
   if (vcos_verify(success == 0))
   {
      // now we need to patch up the vectors if any have been only partially consumed, and
      // unlock memory handles.
   
      for (i=0; i<count; i++)
      {
         VCHI_MSG_VECTOR_EX_T *v = vector+i;

         switch (vector[i].type)
         {
         case VCHI_VEC_POINTER:
            if (flags & VCHI_FLAGS_ALLOW_PARTIAL)
            {
               v->u.ptr.vec_base = copy[i].vec_base;
               v->u.ptr.vec_len  = copy[i].vec_len;
            }
            break;
         case VCHI_VEC_HANDLE:
            mem_unlock(v->u.handle.handle);
            if (flags & VCHI_FLAGS_ALLOW_PARTIAL)
            {
               const uint8_t *old = orig[i];
               uint32_t change = (const uint8_t*)copy[i].vec_base-old;
               v->u.handle.offset += change;
               v->u.handle.vec_len -= change;
            }
            break;
         default:
            vcos_assert(0);
         }
      }
   }

   return vchiq_status_to_vchi(success);
}

#endif

/***********************************************************
 * Name: vchi_held_msg_release
 *
 * Arguments:  VCHI_HELD_MSG_T *message
 *
 * Description: Routine to release a held message (after it has been read with vchi_msg_hold)
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t vchi_held_msg_release( VCHI_HELD_MSG_T *message )
{
   vchiq_release_message((VCHIQ_SERVICE_HANDLE_T)message->service, (VCHIQ_HEADER_T *)message->message);

   return 0;
}

/***********************************************************
 * Name: vchi_msg_hold
 *
 * Arguments:  VCHI_SERVICE_HANDLE_T handle,
 *             void **data,
 *             uint32_t *msg_size,
 *             VCHI_FLAGS_T flags,
 *             VCHI_HELD_MSG_T *message_handle
 *
 * Description: Routine to return a pointer to the current message (to allow in place processing)
 *              The message is dequeued - don't forget to release the message using
 *              vchi_held_msg_release when you're finished
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t vchi_msg_hold( VCHI_SERVICE_HANDLE_T handle,
                       void **data,
                       uint32_t *msg_size,
                       VCHI_FLAGS_T flags,
                       VCHI_HELD_MSG_T *message_handle )
{
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;
   VCHIQ_HEADER_T *header;

   vcos_assert(flags == VCHI_FLAGS_NONE || flags == VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE);

   if (flags == VCHI_FLAGS_NONE)
      if (vchiu_queue_is_empty(&service->queue))
         return -1;

   header = vchiu_queue_pop(&service->queue);

   *data = header->data;
   *msg_size = header->size;

   message_handle->service = (struct opaque_vchi_service_t *)service->handle;
   message_handle->message = header;

   return 0;
}

/***********************************************************
 * Name: vchi_initialise
 *
 * Arguments: VCHI_INSTANCE_T *instance_handle
 *            VCHI_CONNECTION_T **connections
 *            const uint32_t num_connections
 *
 * Description: Initialises the hardware but does not transmit anything
 *              When run as a Host App this will be called twice hence the need
 *              to malloc the state information
 *
 * Returns: 0 if successful, failure otherwise
 *
 ***********************************************************/

int32_t vchi_initialise( VCHI_INSTANCE_T *instance_handle )
{
   VCHIQ_INSTANCE_T instance;
   VCHIQ_STATUS_T status;

   status = vchiq_initialise(&instance);

   *instance_handle = (VCHI_INSTANCE_T)instance;

   return vchiq_status_to_vchi(status);
}

/***********************************************************
 * Name: vchi_connect
 *
 * Arguments: VCHI_CONNECTION_T **connections
 *            const uint32_t num_connections
 *            VCHI_INSTANCE_T instance_handle )
 *
 * Description: Starts the command service on each connection,
 *              causing INIT messages to be pinged back and forth
 *
 * Returns: 0 if successful, failure otherwise
 *
 ***********************************************************/
int32_t vchi_connect( VCHI_CONNECTION_T **connections,
                      const uint32_t num_connections,
                      VCHI_INSTANCE_T instance_handle )
{
   VCHIQ_INSTANCE_T instance = (VCHIQ_INSTANCE_T)instance_handle;

   vcos_unused(connections);
   vcos_unused(num_connections);

   return vchiq_connect(instance);
}


/***********************************************************
 * Name: vchi_disconnect
 *
 * Arguments: VCHI_INSTANCE_T instance_handle
 *
 * Description: Stops the command service on each connection,
 *              causing DE-INIT messages to be pinged back and forth
 *
 * Returns: 0 if successful, failure otherwise
 *
 ***********************************************************/
int32_t vchi_disconnect( VCHI_INSTANCE_T instance_handle )
{
   VCHIQ_INSTANCE_T instance = (VCHIQ_INSTANCE_T)instance_handle;
   return vchiq_status_to_vchi(vchiq_shutdown(instance));
}


/***********************************************************
 * Name: vchi_service_open
 * Name: vchi_service_create
 *
 * Arguments: VCHI_INSTANCE_T *instance_handle
 *            SERVICE_CREATION_T *setup,
 *            VCHI_SERVICE_HANDLE_T *handle
 *
 * Description: Routine to open a service
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/

static VCHIQ_STATUS_T shim_callback(VCHIQ_REASON_T reason, VCHIQ_HEADER_T *header, VCHIQ_SERVICE_HANDLE_T handle, void *bulk_user)
{
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)VCHIQ_GET_SERVICE_USERDATA(handle);

   switch (reason) {
   case VCHIQ_MESSAGE_AVAILABLE:
      vchiu_queue_push(&service->queue, header);

      if (service->callback)
         service->callback(service->callback_param, VCHI_CALLBACK_MSG_AVAILABLE, NULL);
      break;
   case VCHIQ_BULK_TRANSMIT_DONE:
      if (service->callback)
         service->callback(service->callback_param, VCHI_CALLBACK_BULK_SENT, bulk_user);
      break;
   case VCHIQ_BULK_RECEIVE_DONE:
      if (service->callback)
         service->callback(service->callback_param, VCHI_CALLBACK_BULK_RECEIVED, bulk_user);
      break;
   case VCHIQ_SERVICE_CLOSED:
      if (service->callback)
         service->callback(service->callback_param, VCHI_CALLBACK_SERVICE_CLOSED, NULL);
      break;
   case VCHIQ_SERVICE_OPENED:
      /* No equivalent VCHI reason */
      break;
   case VCHIQ_BULK_TRANSMIT_ABORTED:
      if (service->callback)
         service->callback(service->callback_param, VCHI_CALLBACK_BULK_TRANSMIT_ABORTED, bulk_user);
      break;
   case VCHIQ_BULK_RECEIVE_ABORTED:
      if (service->callback)
         service->callback(service->callback_param, VCHI_CALLBACK_BULK_RECEIVE_ABORTED, bulk_user);
      break;
   default:
      vcos_assert(0);
      break;
   }

   return VCHIQ_SUCCESS;
}

static SHIM_SERVICE_T *service_alloc(VCHIQ_INSTANCE_T instance,
                                     SERVICE_CREATION_T *setup)
{
   SHIM_SERVICE_T *service = vcos_calloc(1, sizeof(SHIM_SERVICE_T), "vchiq_shim");

   vcos_unused(instance);

   if (service)
   {
      if (vchiu_queue_init(&service->queue, 64))
      {
         service->callback = setup->callback;
         service->callback_param = setup->callback_param;
      }
      else
      {
         vcos_free(service);
         service = NULL;
      }
   }

   return service;
}

static void service_free(SHIM_SERVICE_T *service)
{
   if (service)
   {
      vchiu_queue_delete(&service->queue);
      vcos_free((void*)service);
   }
}

int32_t vchi_service_open( VCHI_INSTANCE_T instance_handle,
                           SERVICE_CREATION_T *setup,
                           VCHI_SERVICE_HANDLE_T *handle)
{
   VCHIQ_INSTANCE_T instance = (VCHIQ_INSTANCE_T)instance_handle;
   SHIM_SERVICE_T *service = service_alloc(instance, setup);
   if (service)
   {
      VCHIQ_STATUS_T status = vchiq_open_service(instance, setup->service_id, shim_callback, service, &service->handle);
      if (status != VCHIQ_SUCCESS)
      {
         service_free(service);
         service = NULL;
      }
   }

   *handle = (VCHI_SERVICE_HANDLE_T)service;

   return (service != NULL) ? 0 : -1;
}

int32_t vchi_service_create( VCHI_INSTANCE_T instance_handle,
                             SERVICE_CREATION_T *setup,
                             VCHI_SERVICE_HANDLE_T *handle )
{
   VCHIQ_INSTANCE_T instance = (VCHIQ_INSTANCE_T)instance_handle;
   SHIM_SERVICE_T *service = service_alloc(instance, setup);
   if (service)
   {
      VCHIQ_STATUS_T status = vchiq_add_service(instance, setup->service_id, shim_callback, service, &service->handle);
      if (status != VCHIQ_SUCCESS)
      {
         service_free(service);
         service = NULL;
      }
   }

   *handle = (VCHI_SERVICE_HANDLE_T)service;

   return (service != NULL) ? 0 : -1;
}

int32_t vchi_service_close( const VCHI_SERVICE_HANDLE_T handle )
{
   vcos_unused(handle);

   // YTI??
   return 0;
}

/* ----------------------------------------------------------------------
 * read a uint32_t from buffer.
 * network format is defined to be little endian
 * -------------------------------------------------------------------- */
uint32_t
vchi_readbuf_uint32( const void *_ptr )
{
   const unsigned char *ptr = _ptr;
   return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
}

/* ----------------------------------------------------------------------
 * write a uint32_t to buffer.
 * network format is defined to be little endian
 * -------------------------------------------------------------------- */
void
vchi_writebuf_uint32( void *_ptr, uint32_t value )
{
   unsigned char *ptr = _ptr;
   ptr[0] = (unsigned char)((value >> 0)  & 0xFF);
   ptr[1] = (unsigned char)((value >> 8)  & 0xFF);
   ptr[2] = (unsigned char)((value >> 16) & 0xFF);
   ptr[3] = (unsigned char)((value >> 24) & 0xFF);
}

/* ----------------------------------------------------------------------
 * read a uint16_t from buffer.
 * network format is defined to be little endian
 * -------------------------------------------------------------------- */
uint16_t
vchi_readbuf_uint16( const void *_ptr )
{
   const unsigned char *ptr = _ptr;
   return ptr[0] | (ptr[1] << 8);
}

/* ----------------------------------------------------------------------
 * write a uint16_t into the buffer.
 * network format is defined to be little endian
 * -------------------------------------------------------------------- */
void
vchi_writebuf_uint16( void *_ptr, uint16_t value )
{
   unsigned char *ptr = _ptr;
   ptr[0] = (value >> 0)  & 0xFF;
   ptr[1] = (value >> 8)  & 0xFF;
}

/***********************************************************
 * Name: vchi_service_use
 *
 * Arguments: const VCHI_SERVICE_HANDLE_T handle
 *
 * Description: Routine to increment refcount on a service
 *
 * Returns: void
 *
 ***********************************************************/
int32_t vchi_service_use( const VCHI_SERVICE_HANDLE_T handle )
{
   int32_t ret = -1;
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;
   if(service)
   {
      ret = vchiq_status_to_vchi(vchiq_use_service(service->handle));
   }
   return ret;
}

/***********************************************************
 * Name: vchi_service_release
 *
 * Arguments: const VCHI_SERVICE_HANDLE_T handle
 *
 * Description: Routine to decrement refcount on a service
 *
 * Returns: void
 *
 ***********************************************************/
int32_t vchi_service_release( const VCHI_SERVICE_HANDLE_T handle )
{
   int32_t ret = -1;
   SHIM_SERVICE_T *service = (SHIM_SERVICE_T *)handle;
   if(service)
   {
      ret = vchiq_status_to_vchi(vchiq_release_service(service->handle));
   }
   return ret;
}

#if defined(__KERNEL__)
EXPORT_SYMBOL(vchi_initialise);
EXPORT_SYMBOL(vchi_connect);
EXPORT_SYMBOL(vchi_bulk_queue_transmit);
EXPORT_SYMBOL(vchi_msg_dequeue);
EXPORT_SYMBOL(vchi_msg_queue);
EXPORT_SYMBOL(vchi_msg_queuev);
EXPORT_SYMBOL(vchi_service_close);
EXPORT_SYMBOL(vchi_service_open);
EXPORT_SYMBOL(vchi_service_create);
EXPORT_SYMBOL(vchi_service_use);
EXPORT_SYMBOL(vchi_service_release);
#endif
