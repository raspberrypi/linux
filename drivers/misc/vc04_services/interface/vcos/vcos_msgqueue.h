/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver

FILE DESCRIPTION
VCOS - packet-like messages, based loosely on those found in TRIPOS.
=============================================================================*/

#ifndef VCOS_MSGQUEUE_H
#define VCOS_MSGQUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"
#include "vcos_platform.h"

/**
 * \file
 *
 * Packet-like messages, based loosely on those found in TRIPOS and
 * derivatives thereof.
 *
 * A task can send a message *pointer* to another task, where it is
 * queued on a linked list and the task woken up. The receiving task
 * consumes all of the messages on its input queue, and optionally
 * sends back replies using the original message memory.
 *
 * A caller can wait for the reply to a specific message - any other
 * messages that arrive in the meantime are queued separately.
 *
 *
 * All messages have a standard common layout, but the payload area can
 * be used freely to extend this.
 */

/** Map the payload portion of a message to a structure pointer.
  */
#define VCOS_MSG_DATA(_msg) (void*)((_msg)->data)

/** Standard message ids - FIXME - these need to be done properly! */
#define VCOS_MSG_N_QUIT            1
#define VCOS_MSG_N_OPEN            2
#define VCOS_MSG_N_CLOSE           3
#define VCOS_MSG_N_PRIVATE         (1<<20)

#define VCOS_MSG_REPLY_BIT         (1<<31)

/** Make gnuc compiler be happy about pointer punning */
#ifdef __GNUC__
#define __VCOS_MAY_ALIAS __attribute__((__may_alias__))
#else
#define __VCOS_MAY_ALIAS
#endif

/** A single message queue.
  */
typedef struct VCOS_MSGQUEUE_T
{
   struct VCOS_MSG_T *head;            /**< head of linked list of messages waiting on this queue */
   struct VCOS_MSG_T *tail;            /**< tail of message queue */
   VCOS_SEMAPHORE_T sem;               /**< thread waits on this for new messages */
   VCOS_MUTEX_T lock;                  /**< locks the messages list */
} VCOS_MSGQUEUE_T;

/** A single message
  */
typedef struct VCOS_MSG_T
{
   uint32_t code;                      /**< message code */
   int error;                          /**< error status signalled back to caller */
   VCOS_MSGQUEUE_T *dst;               /**< destination queue */
   VCOS_MSGQUEUE_T *src;               /**< source; replies go back to here */
   struct VCOS_MSG_T *next;            /**< next in queue */
   VCOS_THREAD_T *src_thread;          /**< for debug */
   uint32_t data[25];                  /**< payload area */
} VCOS_MSG_T;
   
/** An endpoint
  */
typedef struct VCOS_MSG_ENDPOINT_T
{
   VCOS_MSGQUEUE_T primary;            /**< incoming messages */
   VCOS_MSGQUEUE_T secondary;          /**< this is used for waitspecific */
   char name[32];                      /**< name of this endpoint, for find() */
   struct VCOS_MSG_ENDPOINT_T *next;   /**< next in global list of endpoints */
} VCOS_MSG_ENDPOINT_T;
#define MSG_REPLY_BIT (1<<31)

/** Initalise the library. Normally called from vcos_init().
  */
extern VCOS_STATUS_T vcos_msgq_init(void);

/** Find a message queue by name and get a handle to it.
  *
  * @param name  the name of the queue to find
  *
  * @return The message queue, or NULL if not found.
  */
VCOSPRE_ VCOS_MSGQUEUE_T VCOSPOST_ *vcos_msgq_find(const char *name);

/** Wait for a message queue to come into existence. If it already exists,
  * return immediately, otherwise block.
  *
  * On the whole, if you find yourself using this, it is probably a sign
  * of poor design, since you should create all the server threads first,
  * and then the client threads. But it is sometimes useful.
  *
  * @param name  the name of the queue to find
  * @return The message queue
  */
VCOSPRE_ VCOS_MSGQUEUE_T VCOSPOST_ *vcos_msgq_wait(const char *name);

/** Send a message.
  */
VCOSPRE_ void VCOSPOST_ vcos_msg_send(VCOS_MSGQUEUE_T *dest, uint32_t code, VCOS_MSG_T *msg);

/** Send a message and wait for a reply.
  */
VCOSPRE_ void VCOSPOST_ vcos_msg_sendwait(VCOS_MSGQUEUE_T *queue, uint32_t code, VCOS_MSG_T *msg);

/** Wait for a message on this thread's endpoint.
  */
VCOSPRE_ VCOS_MSG_T * VCOSPOST_ vcos_msg_wait(void);

/** Wait for a specific message.
  */
VCOS_MSG_T * vcos_msg_wait_specific(VCOS_MSGQUEUE_T *queue, VCOS_MSG_T *msg);

/** Peek for a message on this thread's endpoint, if a message is not available, NULL is 
    returned. If a message is available it will be removed from the endpoint and returned.
  */
VCOSPRE_ VCOS_MSG_T * VCOSPOST_ vcos_msg_peek(void);

/** Send a reply to a message
  */
VCOSPRE_ void VCOSPOST_ vcos_msg_reply(VCOS_MSG_T *msg);

/** Create an endpoint. Each thread should need no more than one of these - if you 
  * find yourself needing a second one, you've done something wrong.
  */
VCOSPRE_ VCOS_STATUS_T VCOSPOST_ vcos_msgq_endpoint_create(VCOS_MSG_ENDPOINT_T *ep, const char *name);

/** Destroy an endpoint.
  */
VCOSPRE_ void  VCOSPOST_ vcos_msgq_endpoint_delete(VCOS_MSG_ENDPOINT_T *ep);

#ifdef __cplusplus
}
#endif
#endif


