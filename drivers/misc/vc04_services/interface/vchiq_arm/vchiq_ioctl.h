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

#ifndef VCHIQ_IOCTLS_H
#define VCHIQ_IOCTLS_H

#include <linux/ioctl.h>
#include "vchiq_if.h"

#define VCHIQ_IOC_MAGIC 0xc4
#define VCHIQ_INVALID_HANDLE -1

typedef struct {
   VCHIQ_SERVICE_PARAMS_T params;
   int is_open;
   int is_vchi;
   int handle;       /* OUT */
} VCHIQ_CREATE_SERVICE_T;

typedef struct {
   int handle;
   int count;
   const VCHIQ_ELEMENT_T *elements;
} VCHIQ_QUEUE_MESSAGE_T;

typedef struct {
   int handle;
   void *data;
   int size;
   void *userdata;
   VCHIQ_BULK_MODE_T mode;
} VCHIQ_QUEUE_BULK_TRANSFER_T;

typedef struct {
   VCHIQ_REASON_T reason;
   VCHIQ_HEADER_T *header;
   void *service_userdata;
   void *bulk_userdata;
} VCHIQ_COMPLETION_DATA_T;

typedef struct {
   int count;
   VCHIQ_COMPLETION_DATA_T *buf;
   int msgbufsize;
   int msgbufcount; /* IN/OUT */
   void **msgbufs;
} VCHIQ_AWAIT_COMPLETION_T;

typedef struct {
   int handle;
   int blocking;
   int bufsize;
   void *buf;
} VCHIQ_DEQUEUE_MESSAGE_T;

typedef struct {
   int config_size;
   VCHIQ_CONFIG_T *pconfig;
} VCHIQ_GET_CONFIG_T;

typedef struct {
   int handle;
   VCHIQ_SERVICE_OPTION_T option;
   int value;
} VCHIQ_SET_SERVICE_OPTION_T;

typedef struct {
   void     *virt_addr;
   size_t    num_bytes;
} VCHIQ_DUMP_MEM_T;

#define VCHIQ_IOC_CONNECT              _IO(VCHIQ_IOC_MAGIC,   0)
#define VCHIQ_IOC_SHUTDOWN             _IO(VCHIQ_IOC_MAGIC,   1)
#define VCHIQ_IOC_CREATE_SERVICE       _IOWR(VCHIQ_IOC_MAGIC, 2, VCHIQ_CREATE_SERVICE_T)
#define VCHIQ_IOC_REMOVE_SERVICE       _IO(VCHIQ_IOC_MAGIC,   3)
#define VCHIQ_IOC_QUEUE_MESSAGE        _IOW(VCHIQ_IOC_MAGIC,  4, VCHIQ_QUEUE_MESSAGE_T)
#define VCHIQ_IOC_QUEUE_BULK_TRANSMIT  _IOW(VCHIQ_IOC_MAGIC,  5, VCHIQ_QUEUE_BULK_TRANSFER_T)
#define VCHIQ_IOC_QUEUE_BULK_RECEIVE   _IOW(VCHIQ_IOC_MAGIC,  6, VCHIQ_QUEUE_BULK_TRANSFER_T)
#define VCHIQ_IOC_AWAIT_COMPLETION     _IOW(VCHIQ_IOC_MAGIC,  7, VCHIQ_AWAIT_COMPLETION_T)
#define VCHIQ_IOC_DEQUEUE_MESSAGE      _IOW(VCHIQ_IOC_MAGIC,  8, VCHIQ_DEQUEUE_MESSAGE_T)
#define VCHIQ_IOC_GET_CLIENT_ID        _IO(VCHIQ_IOC_MAGIC,   9)
#define VCHIQ_IOC_GET_CONFIG           _IOW(VCHIQ_IOC_MAGIC, 10, VCHIQ_GET_CONFIG_T)
#define VCHIQ_IOC_CLOSE_SERVICE        _IO(VCHIQ_IOC_MAGIC,  11)
#define VCHIQ_IOC_USE_SERVICE          _IO(VCHIQ_IOC_MAGIC,  12)
#define VCHIQ_IOC_RELEASE_SERVICE      _IO(VCHIQ_IOC_MAGIC,  13)
#define VCHIQ_IOC_SET_SERVICE_OPTION   _IOW(VCHIQ_IOC_MAGIC, 14, VCHIQ_SET_SERVICE_OPTION_T)
#define VCHIQ_IOC_DUMP_PHYS_MEM        _IOW(VCHIQ_IOC_MAGIC, 15, VCHIQ_DUMP_MEM_T)
#define VCHIQ_IOC_MAX                  15

#endif
