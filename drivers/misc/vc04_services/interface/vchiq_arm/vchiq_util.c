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

static inline int is_pow2(int i)
{
	return i && !(i & (i - 1));
}

int vchiu_queue_init(VCHIU_QUEUE_T *queue, int size)
{
	WARN_ON(!is_pow2(size));

	queue->size = size;
	queue->read = 0;
	queue->write = 0;

	sema_init(&queue->pop, 0);
	sema_init(&queue->push, 0);

	queue->storage = kzalloc(size * sizeof(VCHIQ_HEADER_T *), GFP_KERNEL);
	if (queue->storage == NULL) {
		vchiu_queue_delete(queue);
		return 0;
	}
	return 1;
}

void vchiu_queue_delete(VCHIU_QUEUE_T *queue)
{
	if (queue->storage != NULL)
		kfree(queue->storage);
}

int vchiu_queue_is_empty(VCHIU_QUEUE_T *queue)
{
	return queue->read == queue->write;
}

int vchiu_queue_is_full(VCHIU_QUEUE_T *queue)
{
	return queue->write == queue->read + queue->size;
}

void vchiu_queue_push(VCHIU_QUEUE_T *queue, VCHIQ_HEADER_T *header)
{
	while (queue->write == queue->read + queue->size) {
		if (down_interruptible(&queue->pop) != 0) {
			flush_signals(current);
		}
	}

	queue->storage[queue->write & (queue->size - 1)] = header;

	queue->write++;

	up(&queue->push);
}

VCHIQ_HEADER_T *vchiu_queue_peek(VCHIU_QUEUE_T *queue)
{
	while (queue->write == queue->read) {
		if (down_interruptible(&queue->push) != 0) {
			flush_signals(current);
		}
	}

	up(&queue->push); // We haven't removed anything from the queue.
	return queue->storage[queue->read & (queue->size - 1)];
}

VCHIQ_HEADER_T *vchiu_queue_pop(VCHIU_QUEUE_T *queue)
{
	VCHIQ_HEADER_T *header;

	while (queue->write == queue->read) {
		if (down_interruptible(&queue->push) != 0) {
			flush_signals(current);
		}
	}

	header = queue->storage[queue->read & (queue->size - 1)];

	queue->read++;

	up(&queue->pop);

	return header;
}
