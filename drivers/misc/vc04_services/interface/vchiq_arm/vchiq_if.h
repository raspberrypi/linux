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

#ifndef VCHIQ_IF_H
#define VCHIQ_IF_H

#include "interface/vchi/vchi_mh.h"

#define VCHIQ_SLOT_SIZE          4096
#define VCHIQ_MAX_MSG_SIZE       (VCHIQ_SLOT_SIZE - sizeof(VCHIQ_HEADER_T))
#define VCHIQ_CHANNEL_SIZE       VCHIQ_MAX_MSG_SIZE /* For backwards compatibility */

#define VCHIQ_MAKE_FOURCC(x0, x1, x2, x3)     (((x0) << 24) | ((x1) << 16) | ((x2) << 8) | (x3))
#define VCHIQ_GET_SERVICE_USERDATA(service)   (service->userdata)
#define VCHIQ_GET_SERVICE_FOURCC(service)     (service->fourcc)

typedef enum {
   VCHIQ_SERVICE_OPENED,         // service, -, -
   VCHIQ_SERVICE_CLOSED,         // service, -, -
   VCHIQ_MESSAGE_AVAILABLE,      // service, header, -
   VCHIQ_BULK_TRANSMIT_DONE,     // service, -, bulk_userdata
   VCHIQ_BULK_RECEIVE_DONE,      // service, -, bulk_userdata
   VCHIQ_BULK_TRANSMIT_ABORTED,  // service, -, bulk_userdata
   VCHIQ_BULK_RECEIVE_ABORTED    // service, -, bulk_userdata
} VCHIQ_REASON_T;

typedef enum
{
   VCHIQ_ERROR   = -1,
   VCHIQ_SUCCESS = 0,
   VCHIQ_RETRY   = 1
} VCHIQ_STATUS_T;

typedef enum
{
   VCHIQ_BULK_MODE_CALLBACK,
   VCHIQ_BULK_MODE_BLOCKING,
   VCHIQ_BULK_MODE_NOCALLBACK
} VCHIQ_BULK_MODE_T;

typedef enum
{
   VCHIQ_SERVICE_OPTION_AUTOCLOSE,
   VCHIQ_SERVICE_OPTION_SLOT_QUOTA,
   VCHIQ_SERVICE_OPTION_MESSAGE_QUOTA
} VCHIQ_SERVICE_OPTION_T;

#ifdef __HIGHC__
/* Allow zero-sized arrays without warnings */
#pragma warning (push)
#pragma warning (disable : 4200)
#endif

typedef struct vchiq_header_struct {
   /* The message identifier - opaque to applications. */
   int msgid;

   /* Size of message data. */
   unsigned int size;      

   char data[0];           /* message */
} VCHIQ_HEADER_T;

#ifdef __HIGHC__
#pragma warning (pop)
#endif

typedef struct {
   const void *data;
   int size;
} VCHIQ_ELEMENT_T;

typedef const struct vchiq_service_base_struct *VCHIQ_SERVICE_HANDLE_T;

typedef VCHIQ_STATUS_T (*VCHIQ_CALLBACK_T)(VCHIQ_REASON_T, VCHIQ_HEADER_T *, VCHIQ_SERVICE_HANDLE_T, void *);

typedef struct vchiq_service_base_struct {
   int fourcc;
   VCHIQ_CALLBACK_T callback;
   void *userdata;
} VCHIQ_SERVICE_BASE_T;

typedef struct vchiq_service_params_struct {
   int fourcc;
   VCHIQ_CALLBACK_T callback;
   void *userdata;
   short version;       /* Increment for non-trivial changes */
   short version_min;   /* Update for incompatible changes */
} VCHIQ_SERVICE_PARAMS_T;

typedef struct vchiq_config_struct {
   int max_msg_size;
   int bulk_threshold; /* The message size aboce which it is better to use
                          a bulk transfer (<= max_msg_size) */
   int max_outstanding_bulks;
   int max_services;
   short version;      /* The version of VCHIQ */
   short version_min;  /* The minimum compatible version of VCHIQ */
} VCHIQ_CONFIG_T;

typedef struct vchiq_instance_struct *VCHIQ_INSTANCE_T;
typedef void (*VCHIQ_REMOTE_USE_CALLBACK_T)(void* cb_arg);


extern VCHIQ_STATUS_T vchiq_initialise(VCHIQ_INSTANCE_T *pinstance);
extern VCHIQ_STATUS_T vchiq_shutdown(VCHIQ_INSTANCE_T instance);
extern VCHIQ_STATUS_T vchiq_connect(VCHIQ_INSTANCE_T instance);
extern VCHIQ_STATUS_T vchiq_add_service(VCHIQ_INSTANCE_T instance, int fourcc, VCHIQ_CALLBACK_T callback, void *userdata, VCHIQ_SERVICE_HANDLE_T *pservice);
extern VCHIQ_STATUS_T vchiq_open_service(VCHIQ_INSTANCE_T instance, int fourcc, VCHIQ_CALLBACK_T callback, void *userdata, VCHIQ_SERVICE_HANDLE_T *pservice);
extern VCHIQ_STATUS_T vchiq_add_service_params(VCHIQ_INSTANCE_T instance,
   const VCHIQ_SERVICE_PARAMS_T *params,
   VCHIQ_SERVICE_HANDLE_T *pservice);
extern VCHIQ_STATUS_T vchiq_open_service_params(VCHIQ_INSTANCE_T instance,
   const VCHIQ_SERVICE_PARAMS_T *params,
   VCHIQ_SERVICE_HANDLE_T *pservice);
extern VCHIQ_STATUS_T vchiq_close_service(VCHIQ_SERVICE_HANDLE_T service);
extern VCHIQ_STATUS_T vchiq_remove_service(VCHIQ_SERVICE_HANDLE_T service);
extern VCHIQ_STATUS_T vchiq_use_service(VCHIQ_SERVICE_HANDLE_T service);
extern VCHIQ_STATUS_T vchiq_release_service(VCHIQ_SERVICE_HANDLE_T service);

extern VCHIQ_STATUS_T vchiq_queue_message(VCHIQ_SERVICE_HANDLE_T service, const VCHIQ_ELEMENT_T *elements, int count);
extern void           vchiq_release_message(VCHIQ_SERVICE_HANDLE_T service, VCHIQ_HEADER_T *header);
extern VCHIQ_STATUS_T vchiq_queue_bulk_transmit(VCHIQ_SERVICE_HANDLE_T service, const void *data, int size, void *userdata);
extern VCHIQ_STATUS_T vchiq_queue_bulk_receive(VCHIQ_SERVICE_HANDLE_T service, void *data, int size, void *userdata);
extern VCHIQ_STATUS_T vchiq_queue_bulk_transmit_handle(VCHIQ_SERVICE_HANDLE_T service, VCHI_MEM_HANDLE_T handle, const void *offset, int size, void *userdata);
extern VCHIQ_STATUS_T vchiq_queue_bulk_receive_handle(VCHIQ_SERVICE_HANDLE_T service, VCHI_MEM_HANDLE_T handle, void *offset, int size, void *userdata);
extern VCHIQ_STATUS_T vchiq_bulk_transmit(VCHIQ_SERVICE_HANDLE_T service, const void *data, int size, void *userdata, VCHIQ_BULK_MODE_T mode);
extern VCHIQ_STATUS_T vchiq_bulk_receive(VCHIQ_SERVICE_HANDLE_T service, void *data, int size, void *userdata, VCHIQ_BULK_MODE_T mode);
extern VCHIQ_STATUS_T vchiq_bulk_transmit_handle(VCHIQ_SERVICE_HANDLE_T service, VCHI_MEM_HANDLE_T handle, const void *offset, int size, void *userdata, VCHIQ_BULK_MODE_T mode);
extern VCHIQ_STATUS_T vchiq_bulk_receive_handle(VCHIQ_SERVICE_HANDLE_T service, VCHI_MEM_HANDLE_T handle, void *offset, int size, void *userdata, VCHIQ_BULK_MODE_T mode);
extern int            vchiq_get_client_id(VCHIQ_SERVICE_HANDLE_T service);
extern VCHIQ_STATUS_T vchiq_get_config(VCHIQ_INSTANCE_T instance, int config_size, VCHIQ_CONFIG_T *pconfig);
extern VCHIQ_STATUS_T vchiq_set_service_option(VCHIQ_SERVICE_HANDLE_T service, VCHIQ_SERVICE_OPTION_T option, int value);

extern VCHIQ_STATUS_T vchiq_remote_use(VCHIQ_INSTANCE_T instance, VCHIQ_REMOTE_USE_CALLBACK_T callback, void* cb_arg);
extern VCHIQ_STATUS_T vchiq_remote_release(VCHIQ_INSTANCE_T instance);

extern VCHIQ_STATUS_T vchiq_dump_phys_mem( VCHIQ_SERVICE_HANDLE_T service, void *ptr, size_t num_bytes );

#endif /* VCHIQ_IF_H */
