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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>

#include "vchiq_core.h"
#include "vchiq_ioctl.h"
#include "vchiq_arm.h"

#define DEVICE_NAME "vchiq"

/* Override the default prefix, which would be vchiq_arm (from the filename) */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX DEVICE_NAME "."

#define VCHIQ_MINOR 0

/* Some per-instance constants */
#define MAX_COMPLETIONS 16
#define MAX_SERVICES 64
#define MAX_ELEMENTS 8
#define MSG_QUEUE_SIZE 64

#define VCOS_LOG_CATEGORY (&vchiq_arm_log_category)

#define VCHIQ_ARM_VCSUSPEND_TASK_STACK 4096

#if VCOS_HAVE_TIMER
#define SUSPEND_TIMER_TIMEOUT_MS 100
static VCOS_TIMER_T      g_suspend_timer;
static void suspend_timer_callback(void *context);
#endif


typedef struct client_service_struct {
   VCHIQ_SERVICE_T *service;
   void *userdata;
   VCHIQ_INSTANCE_T instance;
   int handle;
   int is_vchi;
   volatile int dequeue_pending;
   volatile int message_available_pos;
   volatile int msg_insert;
   volatile int msg_remove;
   VCOS_EVENT_T insert_event;
   VCOS_EVENT_T remove_event;
   VCHIQ_HEADER_T *msg_queue[MSG_QUEUE_SIZE];
} USER_SERVICE_T;

struct vchiq_instance_struct {
   VCHIQ_STATE_T *state;
   VCHIQ_COMPLETION_DATA_T completions[MAX_COMPLETIONS];
   volatile int completion_insert;
   volatile int completion_remove;
   VCOS_EVENT_T insert_event;
   VCOS_EVENT_T remove_event;

   USER_SERVICE_T services[MAX_SERVICES];

   int connected;
   int closing;
   int pid;
   int mark;
};

typedef struct dump_context_struct
{
   char __user *buf;
   size_t actual;
   size_t space;
   loff_t offset;
} DUMP_CONTEXT_T;

VCOS_LOG_CAT_T vchiq_arm_log_category;

static struct cdev    vchiq_cdev;
static dev_t          vchiq_devid;
static VCHIQ_STATE_T g_state;
static struct class  *vchiq_class;
static struct device *vchiq_dev;

static const char *ioctl_names[] =
{
   "CONNECT",
   "SHUTDOWN",
   "CREATE_SERVICE",
   "REMOVE_SERVICE",
   "QUEUE_MESSAGE",
   "QUEUE_BULK_TRANSMIT",
   "QUEUE_BULK_RECEIVE",
   "AWAIT_COMPLETION",
   "DEQUEUE_MESSAGE",
   "GET_CLIENT_ID",
   "GET_CONFIG",
   "CLOSE_SERVICE",
   "USE_SERVICE",
   "RELEASE_SERVICE",
   "SET_SERVICE_OPTION",
   "DUMP_PHYS_MEM"
};

vcos_static_assert(vcos_countof(ioctl_names) == (VCHIQ_IOC_MAX + 1));

VCOS_LOG_LEVEL_T vchiq_default_arm_log_level = VCOS_LOG_ERROR;

static void
dump_phys_mem( void *virt_addr, uint32_t num_bytes );

/****************************************************************************
*
*   find_service_by_handle
*
***************************************************************************/

static inline USER_SERVICE_T *find_service_by_handle(
	VCHIQ_INSTANCE_T instance, int handle )
{
   USER_SERVICE_T *user_service;

   if (( handle >= 0 )
      && ( handle < MAX_SERVICES ))
   {
      user_service = &instance->services[ handle ];

      if ( user_service->service != NULL )
      {
         return user_service;
      }
   }

   return NULL;
}

/****************************************************************************
*
*   find_avail_service_handle
*
***************************************************************************/

static inline USER_SERVICE_T *find_avail_service_handle(
   VCHIQ_INSTANCE_T instance)
{
   int handle;

   for ( handle = 0; handle < MAX_SERVICES; handle++ )
   {
      if ( instance->services[handle].service == NULL )
      {
         instance->services[handle].instance = instance;
         instance->services[handle].handle = handle;

         return &instance->services[handle];
      }
   }
   return NULL;
}

/****************************************************************************
*
*   add_completion
*
***************************************************************************/

static VCHIQ_STATUS_T
add_completion(VCHIQ_INSTANCE_T instance, VCHIQ_REASON_T reason,
   VCHIQ_HEADER_T *header, USER_SERVICE_T *service, void *bulk_userdata)
{
   VCHIQ_COMPLETION_DATA_T *completion;
   DEBUG_INITIALISE(g_state.local)

   while (instance->completion_insert ==
          (instance->completion_remove + MAX_COMPLETIONS)) {
      /* Out of space - wait for the client */
      DEBUG_TRACE(SERVICE_CALLBACK_LINE);
      vcos_log_trace("add_completion - completion queue full");
      DEBUG_COUNT(COMPLETION_QUEUE_FULL_COUNT);
      if (vcos_event_wait(&instance->remove_event) != VCOS_SUCCESS) {
         vcos_log_info("service_callback interrupted");
         return VCHIQ_RETRY;
      } else if (instance->closing) {
         vcos_log_info("service_callback closing");
         return VCHIQ_ERROR;
      }
      DEBUG_TRACE(SERVICE_CALLBACK_LINE);
   }

   completion =
       &instance->
       completions[instance->completion_insert & (MAX_COMPLETIONS - 1)];

   completion->header = header;
   completion->reason = reason;
   completion->service_userdata = service;
   completion->bulk_userdata = bulk_userdata;

   /* A write barrier is needed here to ensure that the entire completion
      record is written out before the insert point. */
   vcos_wmb(&completion->bulk_userdata);

   if (reason == VCHIQ_MESSAGE_AVAILABLE)
      service->message_available_pos = instance->completion_insert;
   instance->completion_insert++;

   vcos_event_signal(&instance->insert_event);

   return VCHIQ_SUCCESS;
}

/****************************************************************************
*
*   service_callback
*
***************************************************************************/

static VCHIQ_STATUS_T
service_callback(VCHIQ_REASON_T reason, VCHIQ_HEADER_T *header,
   VCHIQ_SERVICE_HANDLE_T handle, void *bulk_userdata)
{
   /* How do we ensure the callback goes to the right client?
      The service_user data points to a USER_SERVICE_T record containing the
      original callback and the user state structure, which contains a circular
      buffer for completion records.
    */
   USER_SERVICE_T *service =
       (USER_SERVICE_T *) VCHIQ_GET_SERVICE_USERDATA(handle);
   VCHIQ_INSTANCE_T instance = service->instance;
   DEBUG_INITIALISE(g_state.local)

   DEBUG_TRACE(SERVICE_CALLBACK_LINE);
   vcos_log_trace
       ("service_callback - service %lx(%d), reason %d, header %lx, "
        "instance %lx, bulk_userdata %lx",
        (unsigned long)service, ((VCHIQ_SERVICE_T *) handle)->localport,
        reason, (unsigned long)header,
        (unsigned long)instance, (unsigned long)bulk_userdata);

   if (!instance || instance->closing) {
      return VCHIQ_SUCCESS;
   }

   if (header && service->is_vchi)
   {
      while (service->msg_insert == (service->msg_remove + MSG_QUEUE_SIZE))
      {
         DEBUG_TRACE(SERVICE_CALLBACK_LINE);
         DEBUG_COUNT(MSG_QUEUE_FULL_COUNT);
         vcos_log_trace("service_callback - msg queue full");
         /* If there is no MESSAGE_AVAILABLE in the completion queue, add one */
         if ((service->message_available_pos - instance->completion_remove) < 0)
         {
            VCHIQ_STATUS_T status;
            vcos_log_warn("Inserting extra MESSAGE_AVAILABLE");
            DEBUG_TRACE(SERVICE_CALLBACK_LINE);
            status = add_completion(instance, reason, NULL, service, bulk_userdata);
            if (status != VCHIQ_SUCCESS)
            {
               DEBUG_TRACE(SERVICE_CALLBACK_LINE);
               return status;
            }
         }
         
         DEBUG_TRACE(SERVICE_CALLBACK_LINE);
         if (vcos_event_wait(&service->remove_event) != VCOS_SUCCESS) {
            vcos_log_info("service_callback interrupted");
            DEBUG_TRACE(SERVICE_CALLBACK_LINE);
            return VCHIQ_RETRY;
         } else if (instance->closing) {
            vcos_log_info("service_callback closing");
            DEBUG_TRACE(SERVICE_CALLBACK_LINE);
            return VCHIQ_ERROR;
         }
         DEBUG_TRACE(SERVICE_CALLBACK_LINE);
      }

      service->msg_queue[service->msg_insert & (MSG_QUEUE_SIZE - 1)] =
         header;

      /* A write memory barrier is needed to ensure that the store of header
         is completed before the insertion point is updated */
      vcos_wmb(&service->msg_queue[service->msg_insert & (MSG_QUEUE_SIZE - 1)]);

      service->msg_insert++;
      vcos_event_signal(&service->insert_event);

      /* If there is a thread waiting in DEQUEUE_MESSAGE, or if
         there is a MESSAGE_AVAILABLE in the completion queue then
         bypass the completion queue. */
      if (((service->message_available_pos - instance->completion_remove) >= 0) ||
          service->dequeue_pending)
      {
         DEBUG_TRACE(SERVICE_CALLBACK_LINE);
         service->dequeue_pending = 0;
         return VCHIQ_SUCCESS;
      }

      header = NULL;
   }
   DEBUG_TRACE(SERVICE_CALLBACK_LINE);

   return add_completion(instance, reason, header, service, bulk_userdata);
}

/****************************************************************************
*
*   vchiq_ioctl
*
***************************************************************************/

static long
vchiq_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
   VCHIQ_INSTANCE_T instance = file->private_data;
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;
   long ret = 0;
   int i, rc;
   DEBUG_INITIALISE(g_state.local)

   vcos_log_trace("vchiq_ioctl - instance %x, cmd %s, arg %lx",
      (unsigned int)instance,
      ((_IOC_TYPE(cmd) == VCHIQ_IOC_MAGIC) && (_IOC_NR(cmd) <= VCHIQ_IOC_MAX)) ?
      ioctl_names[_IOC_NR(cmd)] : "<invalid>", arg);

   switch (cmd) {
   case VCHIQ_IOC_SHUTDOWN:
      if (!instance->connected)
         break;

      /* Remove all services */
      for (i = 0; i < MAX_SERVICES; i++) {
         USER_SERVICE_T *service = &instance->services[i];
         if (service->service != NULL) {
            status = vchiq_remove_service(&service->service->base);
            if (status != VCHIQ_SUCCESS)
               break;
            service->service = NULL;
         }
      }

      if (status == VCHIQ_SUCCESS) {
         /* Wake the completion thread and ask it to exit */
         instance->closing = 1;
         vcos_event_signal(&instance->insert_event);
      }

      break;

   case VCHIQ_IOC_CONNECT:
      if (instance->connected) {
         ret = -EINVAL;
         break;
      }
      if ((rc=vcos_mutex_lock(&instance->state->mutex)) != VCOS_SUCCESS) {
         vcos_log_error("vchiq: connect: could not lock mutex for state %d: %d",
                        instance->state->id, rc);
         ret = -EINTR;
         break;
      }
      status = vchiq_connect_internal(instance->state, instance);
      vcos_mutex_unlock(&instance->state->mutex);

      if (status == VCHIQ_SUCCESS)
         instance->connected = 1;
      else
         vcos_log_error("vchiq: could not connect: %d", status);
      break;

   case VCHIQ_IOC_CREATE_SERVICE:
      {
         VCHIQ_CREATE_SERVICE_T args;
         VCHIQ_SERVICE_T *service = NULL;
         USER_SERVICE_T *user_service = NULL;
         void *userdata;
         int srvstate;

         if (copy_from_user
             (&args, (const void __user *)arg,
              sizeof(args)) != 0) {
            ret = -EFAULT;
            break;
         }

         for (i = 0; i < MAX_SERVICES; i++) {
            if (instance->services[i].service == NULL) {
               user_service = &instance->services[i];
               break;
            }
         }

         if (!user_service) {
            ret = -EMFILE;
            break;
         }

         if (args.is_open) {
            if (instance->connected)
               srvstate = VCHIQ_SRVSTATE_OPENING;
            else {
               ret = -ENOTCONN;
               break;
            }
         } else {
            srvstate =
                instance->connected ?
                VCHIQ_SRVSTATE_LISTENING :
                VCHIQ_SRVSTATE_HIDDEN;
         }

         vcos_mutex_lock(&instance->state->mutex);

         userdata = args.params.userdata;
         args.params.callback = service_callback;
         args.params.userdata = user_service;
         service =
             vchiq_add_service_internal(instance->state,
                         &args.params, srvstate,
                         instance);

         vcos_mutex_unlock(&instance->state->mutex);

         if (service != NULL) {
            user_service->service = service;
            user_service->userdata = userdata;
            user_service->instance = instance;
            user_service->handle = i;
            user_service->is_vchi = args.is_vchi;
            user_service->dequeue_pending = 0;
            user_service->message_available_pos = instance->completion_remove - 1;
            user_service->msg_insert = 0;
            user_service->msg_remove = 0;
            vcos_event_create(&user_service->insert_event, "insert_event");
            vcos_event_create(&user_service->remove_event, "remove_event");

            if (args.is_open) {
               status =
                   vchiq_open_service_internal
                   (service, instance->pid);
               if (status != VCHIQ_SUCCESS) {
                  vchiq_remove_service
                      (&service->base);
                  ret =
                      (status ==
                       VCHIQ_RETRY) ? -EINTR :
                      -EIO;
                  user_service->service = NULL;
                  user_service->instance = NULL;
                  vcos_event_delete(&user_service->insert_event);
                  vcos_event_delete(&user_service->remove_event);
                  break;
               }
            }

            if (copy_to_user((void __user *)
                   &(((VCHIQ_CREATE_SERVICE_T __user
                       *) arg)->handle),
                   (const void *)&user_service->
                   handle,
                   sizeof(user_service->
                     handle)) != 0)
               ret = -EFAULT;
         } else {
            ret = -EEXIST;
         }
      }
      break;

   case VCHIQ_IOC_CLOSE_SERVICE:
      {
         USER_SERVICE_T *user_service;
         int handle = (int)arg;

         user_service = find_service_by_handle(instance, handle);
         if (user_service != NULL)
         {
            int is_server = (user_service->service->public_fourcc != VCHIQ_FOURCC_INVALID);

            status =
                vchiq_close_service(&user_service->service->base);
            if ((status == VCHIQ_SUCCESS) && !is_server)
            {
               vcos_event_delete(&user_service->insert_event);
               vcos_event_delete(&user_service->remove_event);
               user_service->service = NULL;
            }
         } else
            ret = -EINVAL;
      }
      break;

   case VCHIQ_IOC_REMOVE_SERVICE:
      {
         USER_SERVICE_T *user_service;
         int handle = (int)arg;

         user_service = find_service_by_handle(instance, handle);
         if (user_service != NULL)
         {
            status =
                vchiq_remove_service(&user_service->service->base);
            if (status == VCHIQ_SUCCESS)
            {
               vcos_event_delete(&user_service->insert_event);
               vcos_event_delete(&user_service->remove_event);
               user_service->service = NULL;
            }
         } else
            ret = -EINVAL;
      }
      break;

   case VCHIQ_IOC_USE_SERVICE:
   case VCHIQ_IOC_RELEASE_SERVICE:
      {
         USER_SERVICE_T *user_service;
         int handle = (int)arg;

         user_service = find_service_by_handle(instance, handle);
         if (user_service != NULL)
         {
            status = (cmd == VCHIQ_IOC_USE_SERVICE) ? vchiq_use_service(&user_service->service->base) : vchiq_release_service(&user_service->service->base);
            if (status != VCHIQ_SUCCESS)
            {
               ret = -EINVAL; /* ??? */
            }
         }
      }
      break;

   case VCHIQ_IOC_QUEUE_MESSAGE:
      {
         VCHIQ_QUEUE_MESSAGE_T args;
         USER_SERVICE_T *user_service;

         if (copy_from_user
             (&args, (const void __user *)arg,
              sizeof(args)) != 0) {
            ret = -EFAULT;
            break;
         }
         user_service = find_service_by_handle(instance, args.handle);
         if ((user_service != NULL) && (args.count <= MAX_ELEMENTS))
         {
            /* Copy elements into kernel space */
            VCHIQ_ELEMENT_T elements[MAX_ELEMENTS];
            if (copy_from_user
                (elements, args.elements,
                 args.count * sizeof(VCHIQ_ELEMENT_T)) == 0)
               status =
                  vchiq_queue_message
                  (&user_service->service->base,
                   elements, args.count);
            else
               ret = -EFAULT;
         } else {
            ret = -EINVAL;
         }
      }
      break;

   case VCHIQ_IOC_QUEUE_BULK_TRANSMIT:
   case VCHIQ_IOC_QUEUE_BULK_RECEIVE:
      {
         VCHIQ_QUEUE_BULK_TRANSFER_T args;
         USER_SERVICE_T *user_service;
         VCHIQ_BULK_DIR_T dir =
           (cmd == VCHIQ_IOC_QUEUE_BULK_TRANSMIT) ?
           VCHIQ_BULK_TRANSMIT : VCHIQ_BULK_RECEIVE;

         if (copy_from_user
             (&args, (const void __user *)arg,
              sizeof(args)) != 0) {
            ret = -EFAULT;
            break;
         }
         user_service = find_service_by_handle(instance, args.handle);
         if (user_service != NULL)
         {
            status =
               vchiq_bulk_transfer
               ((VCHIQ_SERVICE_T *)user_service->service,
                VCHI_MEM_HANDLE_INVALID,
                args.data, args.size,
                args.userdata, args.mode,
                dir);
         } else {
            ret = -EINVAL;
         }
      }
      break;

   case VCHIQ_IOC_AWAIT_COMPLETION:
      {
         VCHIQ_AWAIT_COMPLETION_T args;

         DEBUG_TRACE(AWAIT_COMPLETION_LINE);
         if (!instance->connected) {
            ret = -ENOTCONN;
            break;
         }

         if (copy_from_user
             (&args, (const void __user *)arg,
              sizeof(args)) != 0) {
            ret = -EFAULT;
            break;
         }
         DEBUG_TRACE(AWAIT_COMPLETION_LINE);
         while ((instance->completion_remove ==
            instance->completion_insert)
                && !instance->closing) {
            DEBUG_TRACE(AWAIT_COMPLETION_LINE);
            if (vcos_event_wait(&instance->insert_event) !=
                VCOS_SUCCESS) {
               DEBUG_TRACE(AWAIT_COMPLETION_LINE);
               vcos_log_info
                   ("AWAIT_COMPLETION interrupted");
               ret = -EINTR;
               break;
            }
         }
         DEBUG_TRACE(AWAIT_COMPLETION_LINE);

         /* A read memory barrier is needed to stop prefetch of a stale
            completion record */
         vcos_rmb();

         if (ret == 0) {
            int msgbufcount = args.msgbufcount;
            for (ret = 0; ret < args.count; ret++) {
               VCHIQ_COMPLETION_DATA_T *completion;
               USER_SERVICE_T *service;
               VCHIQ_HEADER_T *header;
               if (instance->completion_remove ==
                   instance->completion_insert)
                  break;
               completion =
                   &instance->
                   completions
                   [instance->completion_remove &
                    (MAX_COMPLETIONS - 1)];

               service = (USER_SERVICE_T *)completion->service_userdata;
               completion->service_userdata = service->userdata;

               header = completion->header;
               if (header)
               {
                  void __user *msgbuf;
                  int msglen;

                  msglen = header->size + sizeof(VCHIQ_HEADER_T);
                  /* This must be a VCHIQ-style service */
                  if (args.msgbufsize < msglen)
                  {
                     vcos_log_error("header %x: msgbufsize %x < msglen %x",
                        (unsigned int)header, args.msgbufsize, msglen);
                     vcos_assert(0);
                     if (ret == 0)
                        ret = -EMSGSIZE;
                     break;
                  }
                  if (msgbufcount <= 0)
                  {
                     /* Stall here for lack of a buffer for the message */
                     break;
                  }
                  /* Get the pointer from user space */
                  msgbufcount--;
                  if (copy_from_user(&msgbuf,
                     (const void __user *)&args.msgbufs[msgbufcount],
                     sizeof(msgbuf)) != 0)
                  {
                     if (ret == 0)
                        ret = -EFAULT;
                     break;
                  }

                  /* Copy the message to user space */
                  if (copy_to_user(msgbuf, header, msglen) != 0)
                  {
                     if (ret == 0)
                        ret = -EFAULT;
                     break;
                  }

                  /* Now it has been copied, the message can be released. */
                  vchiq_release_message(&service->service->base, header);

                  /* The completion must point to the msgbuf */
                  completion->header = msgbuf;
               }

               if (copy_to_user
                   ((void __user *)((size_t) args.buf +
                          ret *
                          sizeof
                          (VCHIQ_COMPLETION_DATA_T)),
                    completion,
                    sizeof(VCHIQ_COMPLETION_DATA_T)) !=
                   0) {
                  if (ret == 0)
                     ret = -EFAULT;
                  break;
               }
               instance->completion_remove++;
            }

            if (msgbufcount != args.msgbufcount)
            {
               if (copy_to_user((void __user *)
                  &((VCHIQ_AWAIT_COMPLETION_T *)arg)->msgbufcount,
                  &msgbufcount, sizeof(msgbufcount)) != 0)
               {
                  ret = -EFAULT;
                  break;
               }
            }
         }

         if (ret != 0)
            vcos_event_signal(&instance->remove_event);
         DEBUG_TRACE(AWAIT_COMPLETION_LINE);
      }
      break;

   case VCHIQ_IOC_DEQUEUE_MESSAGE:
      {
         VCHIQ_DEQUEUE_MESSAGE_T args;
         USER_SERVICE_T *user_service;
         VCHIQ_HEADER_T *header;

         DEBUG_TRACE(DEQUEUE_MESSAGE_LINE);
         if (copy_from_user
             (&args, (const void __user *)arg,
              sizeof(args)) != 0) {
            ret = -EFAULT;
            break;
         }
         user_service = &instance->services[args.handle];
         if ((args.handle < 0) || (args.handle >= MAX_SERVICES) ||
            (user_service->service == NULL) ||
            (user_service->is_vchi == 0)) {
            ret = -EINVAL;
            break;
         }
         if (user_service->msg_remove == user_service->msg_insert)
         {
            if (!args.blocking)
            {
               DEBUG_TRACE(DEQUEUE_MESSAGE_LINE);
               ret = -EWOULDBLOCK;
               break;
            }
            user_service->dequeue_pending = 1;
            do {
               DEBUG_TRACE(DEQUEUE_MESSAGE_LINE);
               if (vcos_event_wait(&user_service->insert_event) !=
                   VCOS_SUCCESS) {
                  vcos_log_info("DEQUEUE_MESSAGE interrupted");
                  ret = -EINTR;
                  break;
               }
            }
            while (user_service->msg_remove == user_service->msg_insert);
         }

         /* A read memory barrier is needed to stop prefetch of a stale
            header value */
         vcos_rmb();

         header = user_service->msg_queue[user_service->msg_remove &
            (MSG_QUEUE_SIZE - 1)];
         if (header == NULL)
            ret = -ENOTCONN;
         else if (header->size <= args.bufsize)
         {
            /* Copy to user space if msgbuf is not NULL */
            if ((args.buf == NULL) ||
               (copy_to_user((void __user *)args.buf, header->data,
               header->size) == 0))
            {
               ret = header->size;
               vchiq_release_message(&user_service->service->base,
                  header);
               user_service->msg_remove++;
               vcos_event_signal(&user_service->remove_event);
            }
            else
               ret = -EFAULT;
         }
         else
         {
            vcos_log_error("header %x: bufsize %x < size %x",
               (unsigned int)header, args.bufsize, header->size);
            vcos_assert(0);
            ret = -EMSGSIZE;
         }
         DEBUG_TRACE(DEQUEUE_MESSAGE_LINE);
      }
      break;

   case VCHIQ_IOC_GET_CLIENT_ID:
      {
         USER_SERVICE_T *user_service;
         int handle = (int)arg;

         user_service = find_service_by_handle(instance, handle);
         if (user_service != NULL)
            ret = vchiq_get_client_id(&user_service->service->base);
         else
            ret = 0;
      }
      break;

   case VCHIQ_IOC_GET_CONFIG:
      {
         VCHIQ_GET_CONFIG_T args;
         VCHIQ_CONFIG_T config;

         if (copy_from_user
             (&args, (const void __user *)arg,
              sizeof(args)) != 0) {
            ret = -EFAULT;
            break;
         }
         if (args.config_size > sizeof(config))
         {
            ret = -EINVAL;
            break;
         }
         status = vchiq_get_config(instance, args.config_size, &config);
         if (status == VCHIQ_SUCCESS)
         {
            if (copy_to_user((void __user *)args.pconfig,
                   &config, args.config_size) != 0)
            {
               ret = -EFAULT;
               break;
            }
         }
      }
      break;

   case VCHIQ_IOC_SET_SERVICE_OPTION:
      {
         VCHIQ_SET_SERVICE_OPTION_T args;
         USER_SERVICE_T *user_service;

         if (copy_from_user(
            &args, (const void __user *)arg,
            sizeof(args)) != 0)
         {
            ret = -EFAULT;
            break;
         }

         user_service = find_service_by_handle(instance, args.handle);
         if (user_service != NULL)
         {
            status = vchiq_set_service_option(
               &user_service->service->base,
               args.option, args.value);
         }
         else
         {
            ret = -EINVAL;
         }
      }
      break;

   case VCHIQ_IOC_DUMP_PHYS_MEM:
      {
         VCHIQ_DUMP_MEM_T  args;

         if (copy_from_user
             (&args, (const void __user *)arg,
              sizeof(args)) != 0) {
            ret = -EFAULT;
            break;
         }
         dump_phys_mem( args.virt_addr, args.num_bytes );
      }
      break;


   default:
      ret = -ENOTTY;
      break;
   }

   if (ret == 0) {
      if (status == VCHIQ_ERROR)
         ret = -EIO;
      else if (status == VCHIQ_RETRY)
         ret = -EINTR;
   }

   if ((ret < 0) && (ret != -EINTR) && (ret != -EWOULDBLOCK))
      vcos_log_warn("  ioctl instance %lx, cmd %s -> status %d, %ld",
         (unsigned long)instance,
         (_IOC_NR(cmd) <= VCHIQ_IOC_MAX) ? ioctl_names[_IOC_NR(cmd)] :
         "<invalid>", status, ret);
   else
      vcos_log_trace("  ioctl instance %lx, cmd %s -> status %d, %ld",
         (unsigned long)instance,
         (_IOC_NR(cmd) <= VCHIQ_IOC_MAX) ? ioctl_names[_IOC_NR(cmd)] :
         "<invalid>", status, ret);

   return ret;
}

/****************************************************************************
*
*   vchiq_open
*
***************************************************************************/

static int
vchiq_open(struct inode *inode, struct file *file)
{
   int dev = iminor(inode) & 0x0f;
   vcos_log_info("vchiq_open");
   switch (dev) {
   case VCHIQ_MINOR:
      {
         VCHIQ_STATE_T *state = vchiq_get_state();
         VCHIQ_INSTANCE_T instance;

         if (!state)
         {
            vcos_log_error( "vchiq has no connection to VideoCore");
            return -ENOTCONN;
         }

         instance = kzalloc(sizeof(*instance), GFP_KERNEL);
         if (!instance)
            return -ENOMEM;

         instance->state = state;
         instance->pid = current->tgid;
         vcos_event_create(&instance->insert_event, DEVICE_NAME);
         vcos_event_create(&instance->remove_event, DEVICE_NAME);

         file->private_data = instance;
      }
      break;

   default:
      vcos_log_error("Unknown minor device: %d", dev);
      return -ENXIO;
   }

   return 0;
}

/****************************************************************************
*
*   vchiq_release
*
***************************************************************************/

static int
vchiq_release(struct inode *inode, struct file *file)
{
   int dev = iminor(inode) & 0x0f;
   int ret = 0;
   switch (dev) {
   case VCHIQ_MINOR:
      {
         VCHIQ_INSTANCE_T instance = file->private_data;
         int i;

         vcos_log_info("vchiq_release: instance=%lx",
                  (unsigned long)instance);

         instance->closing = 1;

         /* Wake the slot handler if the completion queue is full */
         vcos_event_signal(&instance->remove_event);

         /* Mark all services for termination... */

         for (i = 0; i < MAX_SERVICES; i++) {
            USER_SERVICE_T *user_service =
                &instance->services[i];
            if (user_service->service != NULL)
            {
               /* Wake the slot handler if the msg queue is full */
               vcos_event_signal(&user_service->remove_event);

               if ((user_service->service->srvstate != VCHIQ_SRVSTATE_CLOSEWAIT) &&
                  (user_service->service->srvstate != VCHIQ_SRVSTATE_LISTENING))
               {
                  vchiq_terminate_service_internal(user_service->service);
               }
            }
         }

         /* ...and wait for them to die */

         for (i = 0; i < MAX_SERVICES; i++) {
            USER_SERVICE_T *user_service =
                &instance->services[i];
            if (user_service->service != NULL)
            {
               /* Wait in this non-portable fashion because interruptible
                  calls will not block in this context. */
               while ((user_service->service->srvstate != VCHIQ_SRVSTATE_CLOSEWAIT) &&
                  (user_service->service->srvstate != VCHIQ_SRVSTATE_LISTENING))
               {
                  down(&user_service->service->remove_event);
               }

               vchiq_free_service_internal
                      (user_service->service);
            }
         }

         vcos_event_delete(&instance->insert_event);
         vcos_event_delete(&instance->remove_event);

         kfree(instance);
         file->private_data = NULL;
      }
      break;

   default:
      vcos_log_error("Unknown minor device: %d", dev);
      ret = -ENXIO;
   }

   return ret;
}

/****************************************************************************
*
*   vchiq_dump
*
***************************************************************************/

void
vchiq_dump(void *dump_context, const char *str, int len)
{
   DUMP_CONTEXT_T *context = (DUMP_CONTEXT_T *)dump_context;

   if ((context->actual >= 0) && (context->actual < context->space))
   {
      int copy_bytes;
      if (context->offset > 0)
      {
         int skip_bytes = vcos_min(len, context->offset);
         str += skip_bytes;
         len -= skip_bytes;
         context->offset -= skip_bytes;
         if (context->offset > 0)
            return;
      }
      copy_bytes = vcos_min(len, context->space - context->actual);
      if (copy_bytes == 0)
         return;
      if (copy_to_user(context->buf + context->actual, str, copy_bytes))
         context->actual = -EFAULT;
      context->actual += copy_bytes;
      len -= copy_bytes;

      /* If tne terminating NUL is included in the length, then it marks
       * the end of a line and should be replaced with a carriage return.
       */
      if ((len == 0) && (str[copy_bytes - 1] == '\0'))
      {
         char cr = '\n';
         if (copy_to_user(context->buf + context->actual - 1, &cr, 1))
         {
	    context->actual = -EFAULT;
         }
      }
   }
}

/****************************************************************************
*
*   vchiq_dump_platform_instance_state
*
***************************************************************************/

void
vchiq_dump_platform_instances(void *dump_context)
{
   VCHIQ_STATE_T *state = vchiq_get_state();
   char buf[80];
   int len;
   int i;

   /* There is no list of instances, so instead scan all services,
      marking those that have been dumped. */

   for (i = 0; i < state->unused_service; i++)
   {
      VCHIQ_SERVICE_T *service = state->services[i];
      VCHIQ_INSTANCE_T instance;

      if (service
         && ((instance = service->instance) != NULL)
         && (service->base.callback == service_callback))
         instance->mark = 0;
   }

   for (i = 0; i < state->unused_service; i++)
   {
      VCHIQ_SERVICE_T *service = state->services[i];
      VCHIQ_INSTANCE_T instance;

      if (service
         && ((instance = service->instance) != NULL)
         && (service->base.callback == service_callback))
      {
         if (!instance->mark)
         {
            len = vcos_snprintf(buf, sizeof(buf),
               "Instance %x: pid %d,%s completions %d/%d",
               (unsigned int)instance, instance->pid,
               instance->connected ? " connected," : "",
               instance->completion_insert - instance->completion_remove,
               MAX_COMPLETIONS);

            vchiq_dump(dump_context, buf, len + 1);

            instance->mark = 1;
         }
      }
   }
}

/****************************************************************************
*
*   vchiq_dump_platform_service_state
*
***************************************************************************/

void
vchiq_dump_platform_service_state(void *dump_context, VCHIQ_SERVICE_T *service)
{
   USER_SERVICE_T *user_service = (USER_SERVICE_T *)service->base.userdata;
   char buf[80];
   int len;

   len = vcos_snprintf(buf, sizeof(buf), "  instance %x",
      service->instance);

   if ((service->base.callback == service_callback) && user_service->is_vchi)
   {
      len += vcos_snprintf(buf + len, sizeof(buf) - len,
         ", %d/%d messages",
         user_service->msg_insert - user_service->msg_remove,
         MSG_QUEUE_SIZE);

      if (user_service->dequeue_pending)
         len += vcos_snprintf(buf + len, sizeof(buf) - len,
            " (dequeue pending)");
   }

   vchiq_dump(dump_context, buf, len + 1);
}

/****************************************************************************
*
*   dump_user_mem
*
***************************************************************************/

static void
dump_phys_mem( void *virt_addr, uint32_t num_bytes )
{
   int            rc;
   uint8_t       *end_virt_addr = virt_addr + num_bytes;
   int            num_pages;
   int            offset;
   int            end_offset;
   int            page_idx;
   int            prev_idx;
   struct page   *page;
   struct page  **pages;
   uint8_t       *kmapped_virt_ptr;

   // Align virtAddr and endVirtAddr to 16 byte boundaries.

   virt_addr = (void *)((unsigned long)virt_addr & ~0x0fuL );
   end_virt_addr = (void *)(( (unsigned long)end_virt_addr + 15uL ) & ~0x0fuL);

   offset = (int)(long)virt_addr & ( PAGE_SIZE - 1 );
   end_offset = (int)(long)end_virt_addr & ( PAGE_SIZE - 1 );

   num_pages = (offset + num_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

   if (( pages = kmalloc( sizeof( struct page *) * num_pages, GFP_KERNEL )) == NULL )
   {
      printk( KERN_ERR "Unable to allocation memory for %d pages\n", num_pages );
      return;
   }

   down_read( &current->mm->mmap_sem );
   rc = get_user_pages( current,                   /* task */
                        current->mm,               /* mm */
                        (unsigned long)virt_addr,  /* start */
                        num_pages,                 /* len */
                        0,                         /* write */
                        0,                         /* force */
                        pages,                     /* pages (array of pointers to page) */
                        NULL );                    /* vmas */
   up_read( &current->mm->mmap_sem );

   prev_idx = -1;
   page = NULL;

   while ( offset < end_offset ) {

      int page_offset = offset % PAGE_SIZE;
      page_idx = offset / PAGE_SIZE;

      if ( page_idx != prev_idx ) {

         if (page != NULL) {
            kunmap( page );
         }
         page = pages[page_idx];
         kmapped_virt_ptr = kmap( page );

         prev_idx = page_idx;
      }

      vcos_log_dump_mem_impl( &vchiq_arm_log_category, "ph",
                              (uint32_t)(unsigned long)&kmapped_virt_ptr[page_offset],
                              &kmapped_virt_ptr[page_offset], 16 );

      offset += 16;
   }
   if (page != NULL) {
      kunmap( page );
   }

   for ( page_idx = 0; page_idx < num_pages; page_idx++ ) {
      page_cache_release( pages[page_idx] );
   }
   kfree( pages );
}

/****************************************************************************
*
*   vchiq_read
*
***************************************************************************/

static ssize_t
vchiq_read(struct file * file, char __user * buf,
   size_t count, loff_t *ppos)
{
   DUMP_CONTEXT_T context;
   context.buf = buf;
   context.actual = 0;
   context.space = count;
   context.offset = *ppos;

   vchiq_dump_state(&context, &g_state);

   if (context.actual >= 0)
      *ppos += context.actual;

   return context.actual;
}

VCHIQ_STATE_T *
vchiq_get_state(void)
{

   if (g_state.remote == NULL)
   {
      printk( "%s: g_state.remote == NULL\n", __func__ );
   }
   else
   {
      if ( g_state.remote->initialised != 1)
      {
         printk( "%s: g_state.remote->initialised != 1 (%d)\n", __func__, g_state.remote->initialised );
      }
   }

   return ((g_state.remote != NULL) &&
      (g_state.remote->initialised == 1)) ? &g_state : NULL;
}

static const struct file_operations
vchiq_fops = {
   .owner = THIS_MODULE,
   .unlocked_ioctl = vchiq_ioctl,
   .open = vchiq_open,
   .release = vchiq_release,
   .read = vchiq_read
};

/*
 * Autosuspend related functionality
 */

static int vchiq_videocore_wanted(VCHIQ_STATE_T* state)
{
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state(state);
   if(!arm_state)
   { // autosuspend not supported - always return wanted
      return 1;
   }
   else if(!arm_state->videocore_use_count)
   { // usage count zero - check for override
      return vchiq_platform_videocore_wanted(state);
   }
   else
   { // non-zero usage count - videocore still required
      return 1;
   }
}


/* Called by the lp thread */
static void *
lp_func(void *v)
{
   VCHIQ_STATE_T *state = (VCHIQ_STATE_T *) v;
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state(state);

   while (1) {
      vcos_event_wait(&arm_state->lp_evt);

      vcos_mutex_lock(&arm_state->use_count_mutex);
      if (!vchiq_videocore_wanted(state))
      {
         arm_state->suspend_pending = 1;
      }
      vcos_mutex_unlock(&arm_state->use_count_mutex);

      vchiq_arm_vcsuspend(state);
   }
   return NULL;
}
/* Called by the hp thread */
static void *
hp_func(void *v)
{
   VCHIQ_STATE_T *state = (VCHIQ_STATE_T *) v;
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state(state);
   int send_pending;

   while (1) {
      vcos_event_wait(&arm_state->hp_evt);

      send_pending = 0;

      vcos_mutex_lock(&arm_state->use_count_mutex);
      if (vchiq_videocore_wanted(state))
      {
         vchiq_arm_vcresume(state);
      }
      if(arm_state->use_notify_pending)
      {
         send_pending = 1;
         arm_state->use_notify_pending = 0;
      }
      vcos_mutex_unlock(&arm_state->use_count_mutex);
      if(send_pending)
      {
         vcos_log_info( "%s sending VCHIQ_MSG_REMOTE_USE_ACTIVE", __func__);
         if ( vchiq_send_remote_use_active(state) != VCHIQ_SUCCESS)
         {
            BUG();  /* vc should be resumed, so shouldn't be a problem sending message */
         }
      }
   }
   return NULL;
}

VCHIQ_STATUS_T
vchiq_arm_init_state(VCHIQ_STATE_T* state, VCHIQ_ARM_STATE_T *arm_state)
{
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;
   VCOS_THREAD_ATTR_T attrs;
   char threadname[10];

   if(arm_state)
   {
      vcos_mutex_create(&arm_state->use_count_mutex, "v.use_count_mutex");
      vcos_mutex_create(&arm_state->suspend_resume_mutex, "v.susp_res_mutex");

      vcos_event_create(&arm_state->lp_evt, "LP_EVT");
      vcos_event_create(&arm_state->hp_evt, "HP_EVT");

      vcos_thread_attr_init(&attrs);
      vcos_thread_attr_setstacksize(&attrs, VCHIQ_ARM_VCSUSPEND_TASK_STACK);
      vcos_thread_attr_setpriority(&attrs, VCOS_THREAD_PRI_LOWEST);
      vcos_snprintf(threadname, sizeof(threadname), "VCHIQl-%d", state->id);
      if(vcos_thread_create(&arm_state->lp_thread, threadname, &attrs, lp_func, state) != VCOS_SUCCESS)
      {
         vcos_log_error("vchiq: FATAL: couldn't create thread %s", threadname);
         status = VCHIQ_ERROR;
      }
      else
      {
         vcos_thread_attr_init(&attrs);
         vcos_thread_attr_setstacksize(&attrs, VCHIQ_ARM_VCSUSPEND_TASK_STACK);
         vcos_thread_attr_setpriority(&attrs, VCOS_THREAD_PRI_HIGHEST);
         vcos_snprintf(threadname, sizeof(threadname), "VCHIQh-%d", state->id);

         if(vcos_thread_create(&arm_state->hp_thread, threadname, &attrs, hp_func, state) != VCOS_SUCCESS)
         {
            vcos_log_error("vchiq: FATAL: couldn't create thread %s", threadname);
            status = VCHIQ_ERROR;
         }
      }
   }
   return status;
}


VCHIQ_STATUS_T
vchiq_arm_vcsuspend(VCHIQ_STATE_T *state)
{
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state(state);

   if (state->conn_state != VCHIQ_CONNSTATE_CONNECTED)
      return VCHIQ_ERROR;

   if(arm_state->suspend_pending)
   {
      vcos_mutex_lock(&arm_state->suspend_resume_mutex);
      if(arm_state->videocore_suspended)
      {
         vcos_log_info("%s - already suspended", __func__);
      }
      else
      {
         vcos_log_info("%s - suspending", __func__);

         status = vchiq_platform_suspend(state);
         arm_state->videocore_suspended = (status == VCHIQ_SUCCESS) ? 1 : 0;

         vcos_mutex_unlock(&arm_state->suspend_resume_mutex);

         vcos_mutex_lock(&arm_state->use_count_mutex);
         if(!arm_state->suspend_pending)
         { /* Something has changed the suspend_pending state while we were suspending.
            Run the HP task to check if we need to resume */
            vcos_log_info( "%s trigger HP task to check resume", __func__);
            vcos_event_signal(&arm_state->hp_evt);
         }
         arm_state->suspend_pending = 0;
         vcos_mutex_unlock(&arm_state->use_count_mutex);
      }
   }
   else
   {
      vchiq_check_resume(state);
   }
   return status;
}


VCHIQ_STATUS_T
vchiq_arm_vcresume(VCHIQ_STATE_T *state)
{
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state(state);
   vcos_mutex_lock(&arm_state->suspend_resume_mutex);

   status = vchiq_platform_resume(state);
   arm_state->videocore_suspended = (status == VCHIQ_RETRY) ? 1 : 0;

   vcos_mutex_unlock(&arm_state->suspend_resume_mutex);

   return status;
}

void
vchiq_check_resume(VCHIQ_STATE_T* state)
{
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state(state);
   vcos_mutex_lock(&arm_state->use_count_mutex);

   if (arm_state->videocore_suspended && vchiq_videocore_wanted(state))
   { /* signal high priority task to resume vc */
      vcos_event_signal(&arm_state->hp_evt);
   }

   vcos_mutex_unlock(&arm_state->use_count_mutex);
}

void
vchiq_check_suspend(VCHIQ_STATE_T* state)
{
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state(state);

   vcos_mutex_lock(&arm_state->use_count_mutex);

   if (!arm_state->videocore_suspended && !vchiq_videocore_wanted(state))
   { /* signal low priority task to suspend vc */
      vcos_event_signal(&arm_state->lp_evt);
   }

   vcos_mutex_unlock(&arm_state->use_count_mutex);
}



static VCHIQ_STATUS_T
vchiq_use_internal(VCHIQ_STATE_T *state, VCHIQ_SERVICE_T *service, int block_while_resume)
{
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state(state);
   VCHIQ_STATUS_T ret = VCHIQ_SUCCESS;
   char entity[10];
   int* entity_uc;

   if(arm_state)
   {
      vcos_mutex_lock(&arm_state->use_count_mutex);

      if (service)
      {
         sprintf(entity, "%c%c%c%c:%03d",VCHIQ_FOURCC_AS_4CHARS(service->base.fourcc), service->client_id);
         entity_uc = &service->service_use_count;
      }
      else
      {
         sprintf(entity, "PEER:   ");
         entity_uc = &arm_state->peer_use_count;
      }

      if (!arm_state->videocore_suspended && !vchiq_videocore_wanted(state))
      {
#if VCOS_HAVE_TIMER
         if (vchiq_platform_use_suspend_timer())
         {
            vcos_log_trace( "%s %s - cancel suspend timer", __func__, entity);
         }
         vcos_timer_cancel(&g_suspend_timer);
#endif
      }

      arm_state->videocore_use_count++;
      (*entity_uc)++;
      arm_state->suspend_pending = 0;

      if (arm_state->videocore_suspended && vchiq_videocore_wanted(state))
      {
         vcos_log_info( "%s %s count %d, state count %d", __func__, entity, *entity_uc, arm_state->videocore_use_count);
         if(block_while_resume)
         {
            ret = vchiq_arm_vcresume(state);
         }
         else
         {
            vcos_log_info( "%s trigger HP task to do resume", __func__); /* triggering is done below */
         }
      }
      else
      {
         vcos_log_trace( "%s %s count %d, state count %d", __func__, entity, *entity_uc, arm_state->videocore_use_count);
      }
      if(!block_while_resume)
      {
         arm_state->use_notify_pending = 1;
         vcos_event_signal(&arm_state->hp_evt); /* hp task will check if we need to resume and also send use notify */
      }

      if (ret == VCHIQ_RETRY)
      { /* if we're told to retry, decrement the counters.  VCHIQ_ERROR probably means we're already resumed. */
         (*entity_uc)--;
         arm_state->videocore_use_count--;
      }

      vcos_mutex_unlock(&arm_state->use_count_mutex);
   }
   return ret;
}

static VCHIQ_STATUS_T
vchiq_release_internal(VCHIQ_STATE_T *state, VCHIQ_SERVICE_T *service)
{
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state(state);
   VCHIQ_STATUS_T ret = VCHIQ_SUCCESS;
   char entity[10];
   int* entity_uc;

   if(arm_state)
   {
      vcos_mutex_lock(&arm_state->use_count_mutex);

      if (service)
      {
         sprintf(entity, "%c%c%c%c:%03d",VCHIQ_FOURCC_AS_4CHARS(service->base.fourcc), service->client_id);
         entity_uc = &service->service_use_count;
      }
      else
      {
         sprintf(entity, "PEER:   ");
         entity_uc = &arm_state->peer_use_count;
      }

      if (*entity_uc && arm_state->videocore_use_count)
      {
         arm_state->videocore_use_count--;
         (*entity_uc)--;

         if (!vchiq_videocore_wanted(state))
         {
#if VCOS_HAVE_TIMER
            if (vchiq_platform_use_suspend_timer())
            {
               vcos_log_trace( "%s %s count %d, state count %d - starting suspend timer", __func__, entity, *entity_uc, arm_state->videocore_use_count);
               vcos_timer_cancel(&g_suspend_timer);
               vcos_timer_set(&g_suspend_timer, SUSPEND_TIMER_TIMEOUT_MS);
            }
            else
#endif
            {
               vcos_log_info( "%s %s count %d, state count %d - suspend pending", __func__, entity, *entity_uc, arm_state->videocore_use_count);
               vcos_event_signal(&arm_state->lp_evt); /* kick the lp thread to do the suspend */
            }
         }
         else
         {
            vcos_log_trace( "%s %s count %d, state count %d", __func__, entity, *entity_uc, arm_state->videocore_use_count);
         }
      }
      else
      {
         vcos_log_error( "%s %s ERROR releasing service; count %d, state count %d", __func__, entity, *entity_uc, arm_state->videocore_use_count);
         ret = VCHIQ_ERROR;
      }

      vcos_mutex_unlock(&arm_state->use_count_mutex);
   }
   return ret;
}

VCHIQ_STATUS_T
vchiq_on_remote_use(VCHIQ_STATE_T *state)
{
   vcos_log_info("%s state %p", __func__, state);
   return state ? vchiq_use_internal(state, NULL, 0) : VCHIQ_ERROR;
}

VCHIQ_STATUS_T
vchiq_on_remote_release(VCHIQ_STATE_T *state)
{
   vcos_log_info("%s state %p", __func__, state);
   return state ? vchiq_release_internal(state, NULL) : VCHIQ_ERROR;
}

VCHIQ_STATUS_T
vchiq_use_service_internal(VCHIQ_SERVICE_T *service)
{
   VCHIQ_STATE_T* state = NULL;

   if (service)
   {
      state = service->state;
   }

   if (!service || !state)
   {
      return VCHIQ_ERROR;
   }
   return vchiq_use_internal(state, service, 1);
}

VCHIQ_STATUS_T
vchiq_release_service_internal(VCHIQ_SERVICE_T *service)
{
   VCHIQ_STATE_T* state = NULL;

   if (service)
   {
      state = service->state;
   }

   if (!service || !state)
   {
      return VCHIQ_ERROR;
   }
   return vchiq_release_internal(state, service);
}


#if VCOS_HAVE_TIMER
static void suspend_timer_callback(void* context)
{
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state((VCHIQ_STATE_T*)context);
   vcos_log_info( "%s - suspend pending", __func__);
   vcos_event_signal(&arm_state->lp_evt);
}
#endif

VCHIQ_STATUS_T
vchiq_use_service(VCHIQ_SERVICE_HANDLE_T handle)
{
   VCHIQ_STATUS_T ret = VCHIQ_ERROR;
   VCHIQ_SERVICE_T *service = (VCHIQ_SERVICE_T *) handle;
   if (service)
   {
      ret = vchiq_use_service_internal(service);
   }
   return ret;
}

VCHIQ_STATUS_T
vchiq_release_service(VCHIQ_SERVICE_HANDLE_T handle)
{
   VCHIQ_STATUS_T ret = VCHIQ_ERROR;
   VCHIQ_SERVICE_T *service = (VCHIQ_SERVICE_T *) handle;
   if (service)
   {
      ret = vchiq_release_service_internal(service);
   }
   return ret;
}

void
vchiq_dump_service_use_state(VCHIQ_STATE_T *state)
{
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state(state);
   int i;
   if(arm_state)
   {
      vcos_mutex_lock(&arm_state->suspend_resume_mutex);
      if (arm_state->videocore_suspended)
      {
         vcos_log_warn("--VIDEOCORE SUSPENDED--");
      }
      else
      {
         vcos_log_warn("--VIDEOCORE AWAKE--");
      }
      for (i = 0; i < state->unused_service; i++) {
         VCHIQ_SERVICE_T *service_ptr = state->services[i];
         if (service_ptr && (service_ptr->srvstate != VCHIQ_SRVSTATE_FREE))
         {
            if (service_ptr->service_use_count)
               vcos_log_error("----- %c%c%c%c:%d service count %d <-- preventing suspend", VCHIQ_FOURCC_AS_4CHARS(service_ptr->base.fourcc), service_ptr->client_id, service_ptr->service_use_count);
            else
               vcos_log_warn("----- %c%c%c%c:%d service count 0", VCHIQ_FOURCC_AS_4CHARS(service_ptr->base.fourcc), service_ptr->client_id);
         }
      }
      vcos_log_warn("----- PEER use count count %d", arm_state->peer_use_count);
      vcos_log_warn("--- Overall vchiq instance use count %d", arm_state->videocore_use_count);

      vchiq_dump_platform_use_state(state);

      vcos_mutex_unlock(&arm_state->suspend_resume_mutex);
   }
}

VCHIQ_STATUS_T
vchiq_check_service(VCHIQ_SERVICE_T * service)
{
   VCHIQ_ARM_STATE_T *arm_state = vchiq_platform_get_arm_state(service->state);
   VCHIQ_STATUS_T ret = VCHIQ_ERROR;
   /* on 2835 vchiq does not have an arm_state */
   if (!arm_state)
      return VCHIQ_SUCCESS;
   if (service && arm_state)
   {
      vcos_mutex_lock(&arm_state->use_count_mutex);
      if (!service->service_use_count)
      {
         vcos_log_error( "%s ERROR - %c%c%c%c:%d service count %d, state count %d, videocore_suspended %d", __func__,VCHIQ_FOURCC_AS_4CHARS(service->base.fourcc), service->client_id, service->service_use_count, arm_state->videocore_use_count, arm_state->videocore_suspended);
         vchiq_dump_service_use_state(service->state);
         vcos_assert(0); // vcos_assert should kill the calling thread, so a user thread shouldn't be able to kill the kernel.
      }
      else
      {
         ret = VCHIQ_SUCCESS;
      }
      vcos_mutex_unlock(&arm_state->use_count_mutex);
   }
   return ret;
}

/* stub functions */
void vchiq_on_remote_use_active(VCHIQ_STATE_T *state)
{
   vcos_unused(state);
}


/****************************************************************************
*
*   vchiq_init - called when the module is loaded.
*
***************************************************************************/

static int __init
vchiq_init(void)
{
   int err;
   void *ptr_err;

   err = vchiq_platform_vcos_init();
   if (err != 0)
      goto failed_platform_vcos_init;

   vcos_log_set_level(VCOS_LOG_CATEGORY, vchiq_default_arm_log_level);
   vcos_log_register("vchiq_arm", VCOS_LOG_CATEGORY);

   if ((err =
        alloc_chrdev_region(&vchiq_devid, VCHIQ_MINOR, 1,
             DEVICE_NAME)) != 0) {
      vcos_log_error("Unable to allocate device number");
      goto failed_alloc_chrdev;
   }
   cdev_init(&vchiq_cdev, &vchiq_fops);
   vchiq_cdev.owner = THIS_MODULE;
   if ((err = cdev_add(&vchiq_cdev, vchiq_devid, 1)) != 0) {
      vcos_log_error("Unable to register device");
      goto failed_cdev_add;
   }

   /* create sysfs entries */
   vchiq_class = class_create(THIS_MODULE, DEVICE_NAME);
   if (IS_ERR(ptr_err = vchiq_class))
      goto failed_class_create;

   vchiq_dev = device_create(vchiq_class, NULL,
      vchiq_devid, NULL, "vchiq");
   if (IS_ERR(ptr_err = vchiq_dev))
      goto failed_device_create;

   err = vchiq_platform_init(&g_state);
   if (err != 0)
      goto failed_platform_init;

#if VCOS_HAVE_TIMER
   vcos_timer_create( &g_suspend_timer, "suspend_timer", suspend_timer_callback, (void*)(&g_state));
#endif

   vcos_log_error("vchiq: initialised - version %d (min %d), device %d.%d",
      VCHIQ_VERSION, VCHIQ_VERSION_MIN,
      MAJOR(vchiq_devid), MINOR(vchiq_devid));

   return 0;

failed_platform_init:
   device_destroy(vchiq_class, vchiq_devid);
failed_device_create:
   class_destroy(vchiq_class);
failed_class_create:
   cdev_del(&vchiq_cdev);
   err = PTR_ERR(ptr_err);
failed_cdev_add:
   unregister_chrdev_region(vchiq_devid, 1);
failed_alloc_chrdev:
failed_platform_vcos_init:
   printk(KERN_WARNING "could not load vchiq\n");
   return err;
}
/****************************************************************************
*
*   vchiq_exit - called when the module is unloaded.
*
***************************************************************************/

static void __exit
vchiq_exit(void)
{
   vchiq_platform_exit(&g_state);
   device_destroy(vchiq_class, vchiq_devid);
   class_destroy(vchiq_class);
   cdev_del(&vchiq_cdev);
   unregister_chrdev_region(vchiq_devid, 1);
   vcos_log_unregister(VCOS_LOG_CATEGORY);
}

module_init(vchiq_init);
module_exit(vchiq_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Broadcom Corporation");
