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

#ifndef VCHI_CFG_INTERNAL_H_
#define VCHI_CFG_INTERNAL_H_

/****************************************************************************************
 * Control optimisation attempts.
 ***************************************************************************************/

// Don't use lots of short-term locks - use great long ones, reducing the overall locks-per-second
#define VCHI_COARSE_LOCKING

// Avoid lock then unlock on exit from blocking queue operations (msg tx, bulk rx/tx)
// (only relevant if VCHI_COARSE_LOCKING)
#define VCHI_ELIDE_BLOCK_EXIT_LOCK

// Avoid lock on non-blocking peek
// (only relevant if VCHI_COARSE_LOCKING)
#define VCHI_AVOID_PEEK_LOCK

// Use one slot-handler thread per connection, rather than 1 thread dealing with all connections in rotation.
#define VCHI_MULTIPLE_HANDLER_THREADS

// Put free descriptors onto the head of the free queue, rather than the tail, so that we don't thrash
// our way through the pool of descriptors.
#define VCHI_PUSH_FREE_DESCRIPTORS_ONTO_HEAD

// Don't issue a MSG_AVAILABLE callback for every single message. Possibly only safe if VCHI_COARSE_LOCKING.
#define VCHI_FEWER_MSG_AVAILABLE_CALLBACKS

// Don't use message descriptors for TX messages that don't need them
#define VCHI_MINIMISE_TX_MSG_DESCRIPTORS

// Nano-locks for multiqueue
//#define VCHI_MQUEUE_NANOLOCKS

// Lock-free(er) dequeuing
//#define VCHI_RX_NANOLOCKS

#endif /*VCHI_CFG_INTERNAL_H_*/
