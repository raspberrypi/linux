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

#include "vchiq_util.h"

#if !defined(__KERNEL__)
#include <stdlib.h>
#endif

static __inline int is_pow2(int i)
{
  return i && !(i & (i - 1));
}

int vchiu_queue_init(VCHIU_QUEUE_T *queue, int size)
{
   vcos_assert(is_pow2(size));

   queue->size = size;
   queue->read = 0;
   queue->write = 0;

   vcos_event_create(&queue->pop, "vchiu");
   vcos_event_create(&queue->push, "vchiu");

   queue->storage = vcos_malloc(size * sizeof(VCHIQ_HEADER_T *), VCOS_FUNCTION);
   if (queue->storage == NULL)
   {
      vchiu_queue_delete(queue);
      return 0;
   }
   return 1;
}

void vchiu_queue_delete(VCHIU_QUEUE_T *queue)
{
   vcos_event_delete(&queue->pop);
   vcos_event_delete(&queue->push);
   if (queue->storage != NULL)
      vcos_free(queue->storage);
}

int vchiu_queue_is_empty(VCHIU_QUEUE_T *queue)
{
   return queue->read == queue->write;
}

void vchiu_queue_push(VCHIU_QUEUE_T *queue, VCHIQ_HEADER_T *header)
{
   while (queue->write == queue->read + queue->size)
      vcos_event_wait(&queue->pop);

   queue->storage[queue->write & (queue->size - 1)] = header;

   queue->write++;

   vcos_event_signal(&queue->push);
}

VCHIQ_HEADER_T *vchiu_queue_peek(VCHIU_QUEUE_T *queue)
{
   while (queue->write == queue->read)
      vcos_event_wait(&queue->push);

   return queue->storage[queue->read & (queue->size - 1)];
}

VCHIQ_HEADER_T *vchiu_queue_pop(VCHIU_QUEUE_T *queue)
{
   VCHIQ_HEADER_T *header;

   while (queue->write == queue->read)
      vcos_event_wait(&queue->push);

   header = queue->storage[queue->read & (queue->size - 1)];

   queue->read++;

   vcos_event_signal(&queue->pop);

   return header;
}
