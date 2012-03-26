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

#include "vchiq_core.h"

#define VCHIQ_SLOT_HANDLER_STACK 8192

#define SLOT_INFO_FROM_INDEX(state, index) (state->slot_info + (index))
#define SLOT_DATA_FROM_INDEX(state, index) (state->slot_data + (index))
#define SLOT_INDEX_FROM_DATA(state, data) (((unsigned int)((char *)data - (char *)state->slot_data)) / VCHIQ_SLOT_SIZE)
#define SLOT_INDEX_FROM_INFO(state, info) ((unsigned int)(info - state->slot_info))
#define SLOT_QUEUE_INDEX_FROM_POS(pos) ((int)((unsigned int)(pos) / VCHIQ_SLOT_SIZE))

#define VCOS_LOG_CATEGORY (&vchiq_core_log_category)

#define BULK_INDEX(x) (x & (VCHIQ_NUM_SERVICE_BULKS - 1))


/* Used to check use counts allow vchiq use. */
extern VCHIQ_STATUS_T vchiq_check_service(VCHIQ_SERVICE_T * service);


typedef struct bulk_waiter_struct
{
   VCOS_EVENT_T event;
   int actual;
} BULK_WAITER_T;

typedef struct vchiq_open_payload_struct{
   int fourcc;
   int client_id;
   short version;
   short version_min;
} VCHIQ_OPEN_PAYLOAD_T;

vcos_static_assert(sizeof(VCHIQ_HEADER_T) == 8);   /* we require this for consistency between endpoints */
vcos_static_assert(IS_POW2(sizeof(VCHIQ_HEADER_T)));
vcos_static_assert(IS_POW2(VCHIQ_NUM_CURRENT_BULKS));
vcos_static_assert(IS_POW2(VCHIQ_NUM_SERVICE_BULKS));

VCOS_LOG_CAT_T vchiq_core_log_category;
VCOS_LOG_CAT_T vchiq_core_msg_log_category;
VCOS_LOG_LEVEL_T vchiq_default_core_log_level = VCOS_LOG_WARN;
VCOS_LOG_LEVEL_T vchiq_default_core_msg_log_level = VCOS_LOG_WARN;

static const char *const srvstate_names[] =
{
   "FREE",
   "HIDDEN",
   "LISTENING",
   "OPENING",
   "OPEN",
   "CLOSESENT",
   "CLOSING",
   "CLOSEWAIT"
};

static const char *const reason_names[] =
{
   "SERVICE_OPENED",
   "SERVICE_CLOSED",
   "MESSAGE_AVAILABLE",
   "BULK_TRANSMIT_DONE",
   "BULK_RECEIVE_DONE",
   "BULK_TRANSMIT_ABORTED",
   "BULK_RECEIVE_ABORTED"
};

static const char *const conn_state_names[] =
{
   "DISCONNECTED",
   "CONNECTED",
   "PAUSING",
   "PAUSE_SENT",
   "PAUSED",
   "RESUMING"
};

static const char *msg_type_str( unsigned int msg_type )
{
   switch (msg_type) {
   case VCHIQ_MSG_PADDING:       return "PADDING";
   case VCHIQ_MSG_CONNECT:       return "CONNECT";
   case VCHIQ_MSG_OPEN:          return "OPEN";
   case VCHIQ_MSG_OPENACK:       return "OPENACK";
   case VCHIQ_MSG_CLOSE:         return "CLOSE";
   case VCHIQ_MSG_DATA:          return "DATA";
   case VCHIQ_MSG_BULK_RX:       return "BULK_RX";
   case VCHIQ_MSG_BULK_TX:       return "BULK_TX";
   case VCHIQ_MSG_BULK_RX_DONE:  return "BULK_RX_DONE";
   case VCHIQ_MSG_BULK_TX_DONE:  return "BULK_TX_DONE";
   case VCHIQ_MSG_PAUSE:         return "PAUSE";
   case VCHIQ_MSG_RESUME:        return "RESUME";
   }
   return "???";
}

static inline void
vchiq_set_service_state(VCHIQ_SERVICE_T *service, int newstate)
{
   vcos_log_info("%d: srv:%d %s->%s", service->state->id, service->localport,
      srvstate_names[service->srvstate],
      srvstate_names[newstate]);
   service->srvstate = newstate;
}

static inline int
is_valid_service(VCHIQ_SERVICE_T *service)
{
   return ((service != NULL) &&
      (service->srvstate != VCHIQ_SRVSTATE_FREE));
}

static inline VCHIQ_STATUS_T
make_service_callback(VCHIQ_SERVICE_T *service, VCHIQ_REASON_T reason,
   VCHIQ_HEADER_T *header, void *bulk_userdata)
{
   vcos_log_trace("%d: callback:%d (%s, %x, %x)", service->state->id,
      service->localport, reason_names[reason],
      (unsigned int)header, (unsigned int)bulk_userdata);
   return service->base.callback(reason, header, &service->base, bulk_userdata);
}

static inline void
vchiq_set_conn_state(VCHIQ_STATE_T *state, VCHIQ_CONNSTATE_T newstate)
{
   vcos_log_info("%d: %s->%s", state->id,
      conn_state_names[state->conn_state],
      conn_state_names[newstate]);
   state->conn_state = newstate;
}

static inline void
remote_event_create(REMOTE_EVENT_T *event)
{
   event->armed = 0;
   /* Don't clear the 'fired' flag because it may already have been set by the other side */
   vcos_event_create(event->event, "vchiq");
}

static inline void
remote_event_destroy(REMOTE_EVENT_T *event)
{
   vcos_event_delete(event->event);
}

static inline int
remote_event_wait(REMOTE_EVENT_T *event)
{
   if (!event->fired)
   {
      event->armed = 1;
      if (event->fired) /* Also ensures the write has completed */
         event->armed = 0;
      else if (vcos_event_wait(event->event) != VCOS_SUCCESS)
         return 0;
   }

   event->fired = 0;
   return 1;
}

static inline void
remote_event_signal_local(REMOTE_EVENT_T *event)
{
   event->armed = 0;
   vcos_event_signal(event->event);
}

static inline void
remote_event_poll(REMOTE_EVENT_T *event)
{
   if (event->armed)
      remote_event_signal_local(event);
}

void
remote_event_pollall(VCHIQ_STATE_T *state)
{
   remote_event_poll(&state->local->trigger);
   remote_event_poll(&state->local->recycle);
}

/* Round up message sizes so that any space at the end of a slot is always big
   enough for a header. This relies on header size being a power of two, which
   has been verified earlier by a static assertion. */

static inline unsigned int
calc_stride(unsigned int size)
{
   /* Allow room for the header */
   size += sizeof(VCHIQ_HEADER_T);

   /* Round up */
   return (size + sizeof(VCHIQ_HEADER_T) - 1) & ~(sizeof(VCHIQ_HEADER_T) - 1);
}

static VCHIQ_SERVICE_T *
get_listening_service(VCHIQ_STATE_T *state, int fourcc)
{
   int i;

   vcos_assert(fourcc != VCHIQ_FOURCC_INVALID);

   for (i = 0; i < state->unused_service; i++)
   {
      VCHIQ_SERVICE_T *service = state->services[i];
      if (service &&
         (service->public_fourcc == fourcc) &&
         ((service->srvstate == VCHIQ_SRVSTATE_LISTENING) ||
         ((service->srvstate == VCHIQ_SRVSTATE_OPEN) &&
         (service->remoteport == VCHIQ_PORT_FREE))))
         return service;
   }

   return NULL;
}

static VCHIQ_SERVICE_T *
get_connected_service(VCHIQ_STATE_T *state, unsigned int port)
{
   int i;
   for (i = 0; i < state->unused_service; i++) {
      VCHIQ_SERVICE_T *service = state->services[i];
      if (service && (service->srvstate == VCHIQ_SRVSTATE_OPEN)
         && (service->remoteport == port)) {
         return service;
      }
   }
   return NULL;
}

static inline void
request_poll(VCHIQ_STATE_T *state, VCHIQ_SERVICE_T *service, int poll_type)
{
   if (service)
   {
      vcos_atomic_flags_or(&service->poll_flags, (1 << poll_type));
      vcos_atomic_flags_or(&state->poll_services[service->localport>>5],
         (1 <<(service->localport & 0x1f)));
   }

   state->poll_needed = 1;
   vcos_wmb(&state->poll_needed);

   /* ... and ensure the slot handler runs. */
   remote_event_signal_local(&state->local->trigger);
}

/* Called from queue_message, by the slot handler and application threads,
   with slot_mutex held */
static VCHIQ_HEADER_T *
reserve_space(VCHIQ_STATE_T *state, int space, int is_blocking)
{
   VCHIQ_SHARED_STATE_T *local = state->local;
   int tx_pos = state->local_tx_pos;
   int slot_space = VCHIQ_SLOT_SIZE - (tx_pos & VCHIQ_SLOT_MASK);

   if (space > slot_space) {
      VCHIQ_HEADER_T *header;
      /* Fill the remaining space with padding */
      vcos_assert(state->tx_data != NULL);
      header = (VCHIQ_HEADER_T *) (state->tx_data + (tx_pos & VCHIQ_SLOT_MASK));
      header->msgid = VCHIQ_MSGID_PADDING;
      header->size = slot_space - sizeof(VCHIQ_HEADER_T);

      tx_pos += slot_space;
   }

   /* If necessary, get the next slot. */
   if ((tx_pos & VCHIQ_SLOT_MASK) == 0)
   {
      int slot_index;

      /* If there is no free slot... */
      if (tx_pos == (state->slot_queue_available * VCHIQ_SLOT_SIZE))
      {
         /* ...wait for one. */
         VCHIQ_STATS_INC(state, slot_stalls);

         /* But first, flush through the last slot. */
         local->tx_pos = tx_pos;
         remote_event_signal(&state->remote->trigger);

         do {
            if (!is_blocking ||
               (vcos_event_wait(&state->slot_available_event) != VCOS_SUCCESS))
            {
               return NULL; /* No space available now */
            }
         }
         while (tx_pos == (state->slot_queue_available * VCHIQ_SLOT_SIZE));
      }

      slot_index = local->slot_queue[SLOT_QUEUE_INDEX_FROM_POS(tx_pos) & VCHIQ_SLOT_QUEUE_MASK];
      state->tx_data = (char *)SLOT_DATA_FROM_INDEX(state, slot_index);
   }

   state->local_tx_pos = tx_pos + space;

   return (VCHIQ_HEADER_T *)(state->tx_data + (tx_pos & VCHIQ_SLOT_MASK));
}

/* Called with slot_mutex held */
static void
process_free_queue(VCHIQ_STATE_T *state)
{
   VCHIQ_SHARED_STATE_T *local = state->local;
   BITSET_T service_found[BITSET_SIZE(VCHIQ_MAX_SERVICES)];
   int slot_queue_available;

   /* Use a read memory barrier to ensure that any state that may have
      been modified by another thread is not masked by stale prefetched
      values. */
   vcos_rmb();

   /* Find slots which have been freed by the other side, and return them to
      the available queue. */
   slot_queue_available = state->slot_queue_available;

   while (slot_queue_available != local->slot_queue_recycle)
   {
      unsigned int pos;
      int slot_index = local->slot_queue[slot_queue_available++ & VCHIQ_SLOT_QUEUE_MASK];
      char *data = (char *)SLOT_DATA_FROM_INDEX(state, slot_index);

      vcos_log_trace("%d: pfq %d=%x %x %x", state->id, slot_index,
         (unsigned int)data, local->slot_queue_recycle,
         slot_queue_available);

      /* Initialise the bitmask for services which have used this slot */
      BITSET_ZERO(service_found);

      pos = 0;

      while (pos < VCHIQ_SLOT_SIZE)
      {
         VCHIQ_HEADER_T *header = (VCHIQ_HEADER_T *)(data + pos);
         int msgid = header->msgid;
         if (VCHIQ_MSG_TYPE(msgid) == VCHIQ_MSG_DATA)
         {
            int port = VCHIQ_MSG_SRCPORT(msgid);
            VCHIQ_SERVICE_QUOTA_T *service_quota =
               &state->service_quotas[port];
            int count;
            count = service_quota->message_use_count;
            if (count > 0)
            {
               service_quota->message_use_count = count - 1;
               if (count == service_quota->message_quota)
               {
                  /* Signal the service that it has dropped below its quota */
                  vcos_event_signal(&service_quota->quota_event);
               }
            }
            else
            {
               vcos_log_error("service %d message_use_count=%d (header %x,"
                              " msgid %x, header->msgid %x, header->size %x)",
                  port, service_quota->message_use_count,
                  (unsigned int)header, msgid, header->msgid,
                  header->size);
               vcos_assert(0);
            }
            if (!BITSET_IS_SET(service_found, port))
            {
               /* Set the found bit for this service */
               BITSET_SET(service_found, port);

               count = service_quota->slot_use_count;
               if (count > 0)
               {
                  service_quota->slot_use_count = count - 1;
                  /* Signal the service in case it has dropped below its quota */
                  vcos_event_signal(&service_quota->quota_event);
                  vcos_log_trace("%d: pfq:%d %x@%x - slot_use->%d",
                     state->id, port,
                     header->size, (unsigned int)header,
                     service_quota->slot_use_count);
               }
               else
               {
                  vcos_log_error("service %d slot_use_count=%d (header %x,"
                                 " msgid %x, header->msgid %x, header->size %x)",
                     port, service_quota->slot_use_count,
                     (unsigned int)header, msgid, header->msgid,
                     header->size);
                  vcos_assert(0);
               }
            }
         }

         pos += calc_stride(header->size);
         if (pos > VCHIQ_SLOT_SIZE)
         {
            vcos_log_error("pfq - pos %x: header %x, msgid %x, header->msgid %x, header->size %x",
               pos, (unsigned int)header, msgid, header->msgid, header->size);
            vcos_assert(0);
         }
      }
   }

   if (slot_queue_available != state->slot_queue_available)
   {
      state->slot_queue_available = slot_queue_available;
      vcos_wmb(&state->slot_queue_available);
      vcos_event_signal(&state->slot_available_event);
   }
}

/* Called by the slot handler and application threads */
static VCHIQ_STATUS_T
queue_message(VCHIQ_STATE_T *state, VCHIQ_SERVICE_T *service,
   int msgid, const VCHIQ_ELEMENT_T *elements,
   int count, int size, int is_blocking)
{
   VCHIQ_SHARED_STATE_T *local;
   VCHIQ_SERVICE_QUOTA_T *service_quota = NULL;
   VCHIQ_HEADER_T *header;

   unsigned int stride;

   local = state->local;

   stride = calc_stride(size);

   vcos_assert(stride <= VCHIQ_SLOT_SIZE);

   /* On platforms where vcos_mutex_lock cannot fail, the return will never
      be taken and the compiler may optimise out that code. Let Coverity
      know this is intentional.
   */
   /* coverity[constant_expression_result] */
   if ((VCHIQ_MSG_TYPE(msgid) != VCHIQ_MSG_RESUME) &&
      (vcos_mutex_lock(&state->slot_mutex) != VCOS_SUCCESS))
      return VCHIQ_RETRY;

   if (service)
   {
      int tx_end_index = SLOT_QUEUE_INDEX_FROM_POS(state->local_tx_pos + stride - 1);

      if (service->srvstate != VCHIQ_SRVSTATE_OPEN)
      {
         /* The service has been closed, probably while waiting for the mutex */
         vcos_mutex_unlock(&state->slot_mutex);
         return VCHIQ_ERROR;
      }

      service_quota = &state->service_quotas[service->localport];

      /* ...ensure it doesn't use more than its quota of messages or slots */
      while ((service_quota->message_use_count == service_quota->message_quota) ||
         ((tx_end_index != service_quota->previous_tx_index) &&
         (service_quota->slot_use_count == service_quota->slot_quota)))
      {
         vcos_log_trace("%d: qm:%d %s,%x - quota stall (msg %d, slot %d)",
            state->id, service->localport,
            msg_type_str(VCHIQ_MSG_TYPE(msgid)), size,
            service_quota->message_use_count, service_quota->slot_use_count);
         VCHIQ_SERVICE_STATS_INC(service, quota_stalls);
         vcos_mutex_unlock(&state->slot_mutex);
         if (vcos_event_wait(&service_quota->quota_event) != VCOS_SUCCESS)
            return VCHIQ_RETRY;
         if (vcos_mutex_lock(&state->slot_mutex) != VCOS_SUCCESS)
            return VCHIQ_RETRY;
         tx_end_index = SLOT_QUEUE_INDEX_FROM_POS(state->local_tx_pos + stride - 1);
      }
   }

   header = reserve_space(state, stride, is_blocking);

   if (!header) {
      if (service)
         VCHIQ_SERVICE_STATS_INC(service, slot_stalls);
      vcos_mutex_unlock(&state->slot_mutex);
      return VCHIQ_RETRY;
   }

   if (service) {
      int i, pos;
      int tx_end_index;

      vcos_log_info("%d: qm %s@%x,%x (%d->%d)", state->id,
         msg_type_str(VCHIQ_MSG_TYPE(msgid)),
         (unsigned int)header, size,
         VCHIQ_MSG_SRCPORT(msgid),
         VCHIQ_MSG_DSTPORT(msgid));

      for (i = 0, pos = 0; i < (unsigned int)count;
         pos += elements[i++].size)
         if (elements[i].size) {
            if (vchiq_copy_from_user
               (header->data + pos, elements[i].data,
               (size_t) elements[i].size) !=
               VCHIQ_SUCCESS) {
               vcos_mutex_unlock(&state->slot_mutex);
               VCHIQ_SERVICE_STATS_INC(service, error_count);
               return VCHIQ_ERROR;
            }
            if (i == 0) {
               vcos_log_dump_mem( &vchiq_core_msg_log_category,
                              "Sent", 0, header->data + pos,
                              vcos_min( 64, elements[0].size ));
            }
         }

      /* If this transmission can't fit in the last slot used by this service... */
      tx_end_index = SLOT_QUEUE_INDEX_FROM_POS(state->local_tx_pos - 1);
      if (tx_end_index != service_quota->previous_tx_index)
      {
         service_quota->slot_use_count++;
         vcos_log_trace("%d: qm:%d %s,%x - slot_use->%d",
            state->id, service->localport,
            msg_type_str(VCHIQ_MSG_TYPE(msgid)), size,
            service_quota->slot_use_count);
      }

      service_quota->previous_tx_index = tx_end_index;
      service_quota->message_use_count++;
      VCHIQ_SERVICE_STATS_INC(service, ctrl_tx_count);
      VCHIQ_SERVICE_STATS_ADD(service, ctrl_tx_bytes, size);
   } else {
      vcos_log_info("%d: qm %s@%x,%x (%d->%d)", state->id,
         msg_type_str(VCHIQ_MSG_TYPE(msgid)),
         (unsigned int)header, size,
         VCHIQ_MSG_SRCPORT(msgid),
         VCHIQ_MSG_DSTPORT(msgid));
      if (size != 0)
      { 
         vcos_assert((count == 1) && (size == elements[0].size));
         memcpy(header->data, elements[0].data, elements[0].size);
      }
      VCHIQ_STATS_INC(state, ctrl_tx_count);
   }

   header->msgid = msgid;
   header->size = size;

   if (vcos_is_log_enabled( &vchiq_core_msg_log_category, VCOS_LOG_INFO))
   {
      int svc_fourcc;

      svc_fourcc = service
         ? service->base.fourcc
         : VCHIQ_MAKE_FOURCC('?','?','?','?');

      vcos_log_impl( &vchiq_core_msg_log_category,
         VCOS_LOG_INFO,
         "Sent Msg %s(%u) to %c%c%c%c s:%u d:%d len:%d",
         msg_type_str(VCHIQ_MSG_TYPE(msgid)),
         VCHIQ_MSG_TYPE(msgid),
         VCHIQ_FOURCC_AS_4CHARS(svc_fourcc),
         VCHIQ_MSG_SRCPORT(msgid),
         VCHIQ_MSG_DSTPORT(msgid),
         size );
   }

   /* Make the new tx_pos visible to the peer. */
   local->tx_pos = state->local_tx_pos;
   vcos_wmb(&local->tx_pos);

   if (VCHIQ_MSG_TYPE(msgid) != VCHIQ_MSG_PAUSE)
      vcos_mutex_unlock(&state->slot_mutex);

   remote_event_signal(&state->remote->trigger);

   return VCHIQ_SUCCESS;
}

static inline void
claim_slot(VCHIQ_SLOT_INFO_T *slot)
{
   slot->use_count++;
}

static void
release_slot(VCHIQ_STATE_T *state, VCHIQ_SLOT_INFO_T *slot_info)
{
   int release_count;
   vcos_mutex_lock(&state->recycle_mutex);

   release_count = slot_info->release_count;
   slot_info->release_count = ++release_count;

   if (release_count == slot_info->use_count)
   {
      int slot_queue_recycle;
      /* Add to the freed queue */

      /* A read barrier is necessary here to prevent speculative fetches of
         remote->slot_queue_recycle from overtaking the mutex. */
      vcos_rmb();

      slot_queue_recycle = state->remote->slot_queue_recycle;
      state->remote->slot_queue[slot_queue_recycle & VCHIQ_SLOT_QUEUE_MASK] =
         SLOT_INDEX_FROM_INFO(state, slot_info);
      state->remote->slot_queue_recycle = slot_queue_recycle + 1;
      vcos_log_info("%d: release_slot %d - recycle->%x",
         state->id, SLOT_INDEX_FROM_INFO(state, slot_info),
         state->remote->slot_queue_recycle);

      /* A write barrier is necessary, but remote_event_signal contains one. */
      remote_event_signal(&state->remote->recycle);
   }

   vcos_mutex_unlock(&state->recycle_mutex);
}

/* Called by the slot handler - don't hold the bulk mutex */
static VCHIQ_STATUS_T
notify_bulks(VCHIQ_SERVICE_T *service, VCHIQ_BULK_QUEUE_T *queue)
{
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;

   vcos_log_trace("%d: nb:%d %cx - p=%x rn=%x r=%x",
      service->state->id, service->localport,
      (queue == &service->bulk_tx) ? 't' : 'r',
      queue->process, queue->remote_notify, queue->remove);

   if (service->state->is_master)
   {
      while (queue->remote_notify != queue->process)
      {
         VCHIQ_BULK_T *bulk = &queue->bulks[BULK_INDEX(queue->remote_notify)];
         int msgtype = (bulk->dir == VCHIQ_BULK_TRANSMIT) ?
            VCHIQ_MSG_BULK_RX_DONE : VCHIQ_MSG_BULK_TX_DONE;
         int msgid = VCHIQ_MAKE_MSG(msgtype, service->localport, service->remoteport);
         VCHIQ_ELEMENT_T element = { &bulk->actual, 4 };
         /* Only reply to non-dummy bulk requests */
         if (bulk->remote_data)
         {
            status = queue_message(service->state, NULL, msgid, &element, 1, 4, 0);
            if (status != VCHIQ_SUCCESS)
               break;
         }
         queue->remote_notify++;
      }
   }
   else
   {
      queue->remote_notify = queue->process;
   }

   if (status == VCHIQ_SUCCESS)
   {
      while (queue->remove != queue->remote_notify)
      {
         VCHIQ_BULK_T *bulk = &queue->bulks[BULK_INDEX(queue->remove)];

         /* Only generate callbacks for non-dummy bulk requests */
         if (bulk->data)
         {
            if (bulk->actual != VCHIQ_BULK_ACTUAL_ABORTED)
            {
               if (bulk->dir == VCHIQ_BULK_TRANSMIT)
               {
                  VCHIQ_SERVICE_STATS_INC(service, bulk_tx_count);
                  VCHIQ_SERVICE_STATS_ADD(service, bulk_tx_bytes, bulk->actual);
               }
               else
               {
                  VCHIQ_SERVICE_STATS_INC(service, bulk_rx_count);
                  VCHIQ_SERVICE_STATS_ADD(service, bulk_rx_bytes, bulk->actual);
               }
            }
            else
            {
               VCHIQ_SERVICE_STATS_INC(service, bulk_aborted_count);
            }
            if (bulk->mode == VCHIQ_BULK_MODE_BLOCKING)
            {
               BULK_WAITER_T *waiter = (BULK_WAITER_T *)bulk->userdata;
               if (waiter)
               {
                  waiter->actual = bulk->actual;
                  vcos_event_signal(&waiter->event);
               }
            }
            else if (bulk->mode == VCHIQ_BULK_MODE_CALLBACK)
            {
               VCHIQ_REASON_T reason = (bulk->dir == VCHIQ_BULK_TRANSMIT) ?
                  ((bulk->actual == VCHIQ_BULK_ACTUAL_ABORTED) ?
                     VCHIQ_BULK_TRANSMIT_ABORTED : VCHIQ_BULK_TRANSMIT_DONE) :
                  ((bulk->actual == VCHIQ_BULK_ACTUAL_ABORTED) ?
                     VCHIQ_BULK_RECEIVE_ABORTED : VCHIQ_BULK_RECEIVE_DONE);
               status = make_service_callback(service, reason,
                  NULL, bulk->userdata);
               if (status == VCHIQ_RETRY)
                  break;
            }
         }

         queue->remove++;
         vcos_event_signal(&service->bulk_remove_event);
      }
   }

   if (status != VCHIQ_SUCCESS)
      request_poll(service->state, service, (queue == &service->bulk_tx) ?
         VCHIQ_POLL_TXNOTIFY : VCHIQ_POLL_RXNOTIFY);

   return status;
}

/* Called by the slot handler thread */
static void
poll_services(VCHIQ_STATE_T *state)
{
   int group, i;

   for (group = 0; group < BITSET_SIZE(state->unused_service); group++)
   {
      uint32_t flags;
      flags = vcos_atomic_flags_get_and_clear(&state->poll_services[group]);
      for (i = 0; flags; i++)
      {
         if (flags & (1 << i))
         {
            VCHIQ_SERVICE_T *service = state->services[(group<<5) + i];
            uint32_t service_flags =
               vcos_atomic_flags_get_and_clear(&service->poll_flags);
            if (service_flags & (1 << VCHIQ_POLL_TERMINATE))
            {
               vcos_log_info("%d: ps - terminate %d<->%d", state->id, service->localport, service->remoteport);
               if (vchiq_close_service_internal(service, 0/*!close_recvd*/) != VCHIQ_SUCCESS)
                  request_poll(state, service, VCHIQ_POLL_TERMINATE);
            }
            if (service_flags & (1 << VCHIQ_POLL_TXNOTIFY))
               notify_bulks(service, &service->bulk_tx);
            if (service_flags & (1 << VCHIQ_POLL_RXNOTIFY))
               notify_bulks(service, &service->bulk_rx);
            flags &= ~(1 << i);
         }
      }
   }
}

/* Called by the slot handler or application threads, holding the bulk mutex. */
static int
resolve_bulks(VCHIQ_SERVICE_T *service, VCHIQ_BULK_QUEUE_T *queue)
{
   VCHIQ_STATE_T *state = service->state;
   int resolved = 0;

   while ((queue->process != queue->local_insert) &&
      (queue->process != queue->remote_insert))
   {
      VCHIQ_BULK_T *bulk = &queue->bulks[BULK_INDEX(queue->process)];

      vcos_log_trace("%d: rb:%d %cx - li=%x ri=%x p=%x",
         state->id, service->localport,
         (queue == &service->bulk_tx) ? 't' : 'r',
         queue->local_insert, queue->remote_insert,
         queue->process);

      vcos_assert((int)(queue->local_insert - queue->process) > 0);
      vcos_assert((int)(queue->remote_insert - queue->process) > 0);
      vchiq_transfer_bulk(bulk);

      if (vcos_is_log_enabled( &vchiq_core_msg_log_category, VCOS_LOG_INFO))
      {
         const char *header = (queue == &service->bulk_tx) ?
            "Send Bulk to" : "Recv Bulk from";
         if (bulk->actual != VCHIQ_BULK_ACTUAL_ABORTED)
            vcos_log_impl( &vchiq_core_msg_log_category,
               VCOS_LOG_INFO,
               "%s %c%c%c%c d:%d len:%d %x<->%x",
               header,
               VCHIQ_FOURCC_AS_4CHARS(service->base.fourcc),
               service->remoteport,
               bulk->size,
               (unsigned int)bulk->data,
               (unsigned int)bulk->remote_data );
         else
            vcos_log_impl( &vchiq_core_msg_log_category,
               VCOS_LOG_INFO,
               "%s %c%c%c%c d:%d ABORTED - tx len:%d, rx len:%d %x<->%x",
               header,
               VCHIQ_FOURCC_AS_4CHARS(service->base.fourcc),
               service->remoteport,
               bulk->size,
               bulk->remote_size,
               (unsigned int)bulk->data,
               (unsigned int)bulk->remote_data );
      }

      vchiq_complete_bulk(bulk);
      queue->process++;
      resolved++;
   }
   return resolved;
}

/* Called with the bulk_mutex held */
static void
abort_outstanding_bulks(VCHIQ_SERVICE_T *service, VCHIQ_BULK_QUEUE_T *queue)
{
   int is_tx = (queue == &service->bulk_tx);
   vcos_log_trace("%d: aob:%d %cx - li=%x ri=%x p=%x",
      service->state->id, service->localport, is_tx ? 't' : 'r',
      queue->local_insert, queue->remote_insert, queue->process);

   vcos_assert((int)(queue->local_insert - queue->process) >= 0);
   vcos_assert((int)(queue->remote_insert - queue->process) >= 0);

   while ((queue->process != queue->local_insert) ||
      (queue->process != queue->remote_insert))
   {
      VCHIQ_BULK_T *bulk = &queue->bulks[BULK_INDEX(queue->process)];

      if (queue->process == queue->remote_insert)
      {
         /* fabricate a matching dummy bulk */
         bulk->remote_data = NULL;
         bulk->remote_size = 0;
         queue->remote_insert++;
      }

      if (queue->process != queue->local_insert)
      {
         vchiq_complete_bulk(bulk);

         if (vcos_is_log_enabled( &vchiq_core_msg_log_category, VCOS_LOG_INFO))
         {
            vcos_log_impl( &vchiq_core_msg_log_category,
               VCOS_LOG_INFO,
               "%s %c%c%c%c d:%d ABORTED - tx len:%d, rx len:%d",
               is_tx ? "Send Bulk to" : "Recv Bulk from",
               VCHIQ_FOURCC_AS_4CHARS(service->base.fourcc),
               service->remoteport,
               bulk->size,
               bulk->remote_size );
         }
      }
      else
      {
         /* fabricate a matching dummy bulk */
         bulk->data = NULL;
         bulk->size = 0;
         bulk->actual = VCHIQ_BULK_ACTUAL_ABORTED;
         bulk->dir = is_tx ? VCHIQ_BULK_TRANSMIT : VCHIQ_BULK_RECEIVE;
         queue->local_insert++;
      }

      queue->process++;
   }
}

static void
pause_bulks(VCHIQ_STATE_T *state)
{
   int i;

   /* Block bulk transfers from all services */
   for (i = 0; i < state->unused_service; i++)
   {
      VCHIQ_SERVICE_T *service = state->services[i];
      if (!service || (service->srvstate != VCHIQ_SRVSTATE_OPEN))
         continue;

      vcos_log_trace("locking bulk_mutex for service %d", i);
      vcos_mutex_lock(&service->bulk_mutex);
   }
}

static void
resume_bulks(VCHIQ_STATE_T *state)
{
   int i;

   /* Poll all services in case any bulk transfers have been
      deferred */
   for (i = 0; i < state->unused_service; i++)
   {
      VCHIQ_SERVICE_T *service = state->services[i];
      if (!service || (service->srvstate != VCHIQ_SRVSTATE_OPEN))
         continue;

      if (resolve_bulks(service, &service->bulk_tx))
         request_poll(state, service, VCHIQ_POLL_TXNOTIFY);
      if (resolve_bulks(service, &service->bulk_rx))
         request_poll(state, service, VCHIQ_POLL_RXNOTIFY);
      vcos_log_trace("unlocking bulk_mutex for service %d", i);
      vcos_mutex_unlock(&service->bulk_mutex);
   }
}

/* Called by the slot handler thread */
static void
parse_rx_slots(VCHIQ_STATE_T *state)
{
   VCHIQ_SHARED_STATE_T *remote = state->remote;
   int tx_pos;
   DEBUG_INITIALISE(state->local)

   tx_pos = remote->tx_pos;

   while (state->rx_pos != tx_pos) {
      VCHIQ_SERVICE_T *service = NULL;
      VCHIQ_HEADER_T *header;
      int msgid, size;
      int type;
      unsigned int localport, remoteport;

      DEBUG_TRACE(PARSE_LINE);
      if (!state->rx_data)
      {
         int rx_index;
         vcos_assert((state->rx_pos & VCHIQ_SLOT_MASK) == 0);
         rx_index = remote->slot_queue[SLOT_QUEUE_INDEX_FROM_POS(state->rx_pos) & VCHIQ_SLOT_QUEUE_MASK];
         state->rx_data = (char *)SLOT_DATA_FROM_INDEX(state, rx_index);
         state->rx_info = SLOT_INFO_FROM_INDEX(state, rx_index);

         /* Initialise use_count to one, and increment release_count at the end
            of the slot to avoid releasing the slot prematurely. */
         state->rx_info->use_count = 1;
         state->rx_info->release_count = 0;
      }

      header = (VCHIQ_HEADER_T *)(state->rx_data + (state->rx_pos & VCHIQ_SLOT_MASK));
      DEBUG_VALUE(PARSE_HEADER, (int)header);
      msgid = header->msgid;
      DEBUG_VALUE(PARSE_MSGID, msgid);
      size = header->size;
      type = VCHIQ_MSG_TYPE(msgid);
      localport = VCHIQ_MSG_DSTPORT(msgid);
      remoteport = VCHIQ_MSG_SRCPORT(msgid);

      if (type != VCHIQ_MSG_DATA)
      {
         VCHIQ_STATS_INC(state, ctrl_rx_count);
      }

      switch (type)
      {
      case VCHIQ_MSG_OPENACK:
      case VCHIQ_MSG_CLOSE:
      case VCHIQ_MSG_DATA:
      case VCHIQ_MSG_BULK_RX:
      case VCHIQ_MSG_BULK_TX:
      case VCHIQ_MSG_BULK_RX_DONE:
      case VCHIQ_MSG_BULK_TX_DONE:
         if (localport <= VCHIQ_PORT_MAX)
         {
            service = state->services[localport];
            if (service && (service->srvstate == VCHIQ_SRVSTATE_FREE))
               service = NULL;
         }
         if (!service)
         {
            vcos_log_error(
               "%d: prs %s@%x (%d->%d) - invalid/closed service %d",
               state->id, msg_type_str(type), (unsigned int)header,
               remoteport, localport, localport);
            goto skip_message;
         }
      default:
         break;
      }

      if ( vcos_is_log_enabled( &vchiq_core_msg_log_category, VCOS_LOG_INFO))
      {
         int svc_fourcc;

         svc_fourcc = service
            ? service->base.fourcc
            : VCHIQ_MAKE_FOURCC('?','?','?','?');
         vcos_log_impl( &vchiq_core_msg_log_category,
            VCOS_LOG_INFO,
            "Rcvd Msg %s(%u) from %c%c%c%c s:%d d:%d len:%d",
            msg_type_str(type), type,
            VCHIQ_FOURCC_AS_4CHARS(svc_fourcc),
            remoteport, localport, size );
         if (size > 0) {
            vcos_log_dump_mem( &vchiq_core_msg_log_category,
                           "Rcvd", 0, header->data,
                           vcos_min( 64, size ));
         }
      }

      if (((unsigned int)header & VCHIQ_SLOT_MASK) + calc_stride(size) > VCHIQ_SLOT_SIZE)
      {
         vcos_log_error("header %x (msgid %x) - size %x too big for slot",
            (unsigned int)header, (unsigned int)msgid, (unsigned int)size);
         vcos_assert(0);
      }

      switch (type) {
      case VCHIQ_MSG_OPEN:
         vcos_assert(VCHIQ_MSG_DSTPORT(msgid) == 0);
         if (vcos_verify(size == sizeof(VCHIQ_OPEN_PAYLOAD_T))) {
            const VCHIQ_OPEN_PAYLOAD_T *payload = (VCHIQ_OPEN_PAYLOAD_T *)header->data;
            unsigned int fourcc;

            fourcc = payload->fourcc;
            vcos_log_info("%d: prs OPEN@%x (%d->'%c%c%c%c')",
               state->id, (unsigned int)header,
               localport,
               VCHIQ_FOURCC_AS_4CHARS(fourcc));

            service = get_listening_service(state, fourcc);

            if (service)
            {
               /* A matching service exists */
               short version = payload->version;
               short version_min = payload->version_min;
               if ((service->version < version_min) ||
                  (version < service->version_min))
               {
                  /* Version mismatch */
                  vcos_log_error("%d: service %d (%c%c%c%c) version mismatch -"
                     " local (%d, min %d) vs. remote (%d, min %d)",
                     state->id, service->localport,
                     VCHIQ_FOURCC_AS_4CHARS(fourcc),
                     service->version, service->version_min,
                     version, version_min);
                  goto fail_open;
               }
               if (service->srvstate == VCHIQ_SRVSTATE_LISTENING)
               {
                  /* Acknowledge the OPEN */
                  if (queue_message(state, NULL,
                     VCHIQ_MAKE_MSG(VCHIQ_MSG_OPENACK, service->localport, remoteport),
                     NULL, 0, 0, 0) == VCHIQ_RETRY)
                     return;  /* Bail out if not ready */

                  /* The service is now open */
                  vchiq_set_service_state(service, VCHIQ_SRVSTATE_OPEN);
               }

               service->remoteport = remoteport;
               service->client_id = ((int *)header->data)[1];
               if (make_service_callback(service, VCHIQ_SERVICE_OPENED,
                  NULL, NULL) == VCHIQ_RETRY)
               {
                  /* Bail out if not ready */
                  service->remoteport = VCHIQ_PORT_FREE;
                  return;
               }

               /* Break out, and skip the failure handling */
               break;
            }
         }
      fail_open:
         /* No available service, or an invalid request - send a CLOSE */
         if (queue_message(state, NULL,
            VCHIQ_MAKE_MSG(VCHIQ_MSG_CLOSE, 0, VCHIQ_MSG_SRCPORT(msgid)),
            NULL, 0, 0, 0) == VCHIQ_RETRY)
            return;  /* Bail out if not ready */
         break;
      case VCHIQ_MSG_OPENACK:
         {
            vcos_log_info("%d: prs OPENACK@%x (%d->%d)",
               state->id, (unsigned int)header,
               remoteport, localport);
            if (service->srvstate == VCHIQ_SRVSTATE_OPENING) {
               service->remoteport = remoteport;
               vchiq_set_service_state(service,
                        VCHIQ_SRVSTATE_OPEN);
               vcos_event_signal(&service->remove_event);
            }
         }
         break;
      case VCHIQ_MSG_CLOSE:
         {
            vcos_assert(size == 0); /* There should be no data */

            vcos_log_info("%d: prs CLOSE@%x (%d->%d)",
               state->id, (unsigned int)header,
               remoteport, localport);

            if ((service->remoteport != remoteport) &&
               VCHIQ_PORT_IS_VALID(service->remoteport)) {
               /* This could be from a client which hadn't yet received
                  the OPENACK - look for the connected service */
               service = get_connected_service(state, remoteport);
               if (!service)
                  break;
            }

            if (vchiq_close_service_internal(service,
               1/*close_recvd*/) == VCHIQ_RETRY)
               return;  /* Bail out if not ready */

            if (vcos_is_log_enabled( &vchiq_core_msg_log_category, VCOS_LOG_INFO))
            {
               vcos_log_impl( &vchiq_core_msg_log_category,
                           VCOS_LOG_INFO,
                           "Close Service %c%c%c%c s:%u d:%d",
                           VCHIQ_FOURCC_AS_4CHARS(service->base.fourcc),
                           service->localport,
                           service->remoteport );
            }
         }
         break;
      case VCHIQ_MSG_DATA:
         {
            vcos_log_trace("%d: prs DATA@%x,%x (%d->%d)",
               state->id, (unsigned int)header, size,
               remoteport, localport);

            if ((service->remoteport == remoteport)
               && (service->srvstate ==
               VCHIQ_SRVSTATE_OPEN)) {
               header->msgid = msgid | VCHIQ_MSGID_CLAIMED;
               claim_slot(state->rx_info);
               DEBUG_TRACE(PARSE_LINE);
               if (make_service_callback(service,
                  VCHIQ_MESSAGE_AVAILABLE, header,
                  NULL) == VCHIQ_RETRY)
               {
                  DEBUG_TRACE(PARSE_LINE);
                  return;  /* Bail out if not ready */
               }
               VCHIQ_SERVICE_STATS_INC(service, ctrl_rx_count);
               VCHIQ_SERVICE_STATS_ADD(service, ctrl_rx_bytes, size);
            }
            else
            {
               VCHIQ_STATS_INC(state, error_count);
            }
         }
         break;
      case VCHIQ_MSG_CONNECT:
         vcos_log_info("%d: prs CONNECT@%x",
            state->id, (unsigned int)header);
         vcos_event_signal(&state->connect);
         break;
      case VCHIQ_MSG_BULK_RX:
      case VCHIQ_MSG_BULK_TX:
         {
            VCHIQ_BULK_QUEUE_T *queue;
            vcos_assert(state->is_master);
            queue = (type == VCHIQ_MSG_BULK_RX) ?
               &service->bulk_tx : &service->bulk_rx;
            if ((service->remoteport == remoteport)
               && (service->srvstate ==
               VCHIQ_SRVSTATE_OPEN))
            {
               VCHIQ_BULK_T *bulk;
               int resolved;

               vcos_assert(queue->remote_insert < queue->remove +
                  VCHIQ_NUM_SERVICE_BULKS);
               bulk = &queue->bulks[BULK_INDEX(queue->remote_insert)];
               bulk->remote_data = (void *)((int *)header->data)[0];
               bulk->remote_size = ((int *)header->data)[1];

               vcos_log_info("%d: prs %s@%x (%d->%d) %x@%x",
                  state->id, msg_type_str(type),
                  (unsigned int)header,
                  remoteport, localport,
                  bulk->remote_size,
                  (unsigned int)bulk->remote_data);

               queue->remote_insert++;

               if (state->conn_state != VCHIQ_CONNSTATE_CONNECTED)
                  break;

               DEBUG_TRACE(PARSE_LINE);
               if (vcos_mutex_lock(&service->bulk_mutex) != VCOS_SUCCESS)
               {
                  DEBUG_TRACE(PARSE_LINE);
                  return;
               }
               DEBUG_TRACE(PARSE_LINE);
               resolved = resolve_bulks(service, queue);
               vcos_mutex_unlock(&service->bulk_mutex);
               if (resolved)
                  notify_bulks(service, queue);
            }
         }
         break;
      case VCHIQ_MSG_BULK_RX_DONE:
      case VCHIQ_MSG_BULK_TX_DONE:
         {
            vcos_assert(!state->is_master);
            if ((service->remoteport == remoteport)
               && (service->srvstate !=
               VCHIQ_SRVSTATE_FREE)) {
               VCHIQ_BULK_QUEUE_T *queue;
               VCHIQ_BULK_T *bulk;

               queue = (type == VCHIQ_MSG_BULK_RX_DONE) ?
                  &service->bulk_rx : &service->bulk_tx;

               bulk = &queue->bulks[BULK_INDEX(queue->process)];
               bulk->actual = *(int *)header->data;

               vcos_log_info("%d: prs %s@%x (%d->%d) %x@%x",
                  state->id, msg_type_str(type),
                  (unsigned int)header,
                  remoteport, localport,
                  bulk->actual, (unsigned int)bulk->data);

               vcos_log_trace("%d: prs:%d %cx li=%x ri=%x p=%x",
                  state->id, localport,
                  (type == VCHIQ_MSG_BULK_RX_DONE) ? 'r' : 't',
                  queue->local_insert,
                  queue->remote_insert, queue->process);

               DEBUG_TRACE(PARSE_LINE);
               if (vcos_mutex_lock(&service->bulk_mutex) != VCOS_SUCCESS)
               {
                  DEBUG_TRACE(PARSE_LINE);
                  return;
               }
               DEBUG_TRACE(PARSE_LINE);
               vcos_assert(queue->process != queue->local_insert);
               vchiq_complete_bulk(bulk);
               queue->process++;
               vcos_mutex_unlock(&service->bulk_mutex);
               DEBUG_TRACE(PARSE_LINE);
               notify_bulks(service, queue);
               DEBUG_TRACE(PARSE_LINE);
            }
         }
         break;
      case VCHIQ_MSG_PADDING:
         vcos_log_trace("%d: prs PADDING@%x,%x",
            state->id, (unsigned int)header, size);
         break;
      case VCHIQ_MSG_PAUSE:
         /* If initiated, signal the application thread */
         vcos_log_trace("%d: prs PAUSE@%x,%x",
            state->id, (unsigned int)header, size);
         if (state->conn_state != VCHIQ_CONNSTATE_PAUSE_SENT)
         {
            /* Send a PAUSE in response */
            if (queue_message(state, NULL,
               VCHIQ_MAKE_MSG(VCHIQ_MSG_PAUSE, 0, 0),
               NULL, 0, 0, 0) == VCHIQ_RETRY)
               return;  /* Bail out if not ready */
            if (state->is_master)
               pause_bulks(state);
         }
         /* At this point slot_mutex is held */
         vchiq_set_conn_state(state, VCHIQ_CONNSTATE_PAUSED);
         vchiq_platform_paused(state);
         break;
      case VCHIQ_MSG_RESUME:
         vcos_log_trace("%d: prs RESUME@%x,%x",
            state->id, (unsigned int)header, size);
         /* Release the slot mutex */
         vcos_mutex_unlock(&state->slot_mutex);
         if (state->is_master)
            resume_bulks(state);
         vchiq_set_conn_state(state, VCHIQ_CONNSTATE_CONNECTED);
         vchiq_platform_resumed(state);
         break;

      case VCHIQ_MSG_REMOTE_USE:
         vchiq_on_remote_use(state);
         break;
      case VCHIQ_MSG_REMOTE_RELEASE:
         vchiq_on_remote_release(state);
         break;
      case VCHIQ_MSG_REMOTE_USE_ACTIVE:
         vchiq_on_remote_use_active(state);
         break;

      default:
         vcos_log_error("%d: prs invalid msgid %x@%x,%x",
            state->id, msgid, (unsigned int)header, size);
         vcos_assert(0);
         break;
      }

   skip_message:
      state->rx_pos += calc_stride(size);

      DEBUG_TRACE(PARSE_LINE);
      /* Perform some housekeeping when the end of the slot is reached. */
      if ((state->rx_pos & VCHIQ_SLOT_MASK) == 0)
      {
         /* Remove the extra reference count. */
         release_slot(state, state->rx_info);
         state->rx_data = NULL;
      }
   }
}

/* Called by the slot handler thread */
static void *
slot_handler_func(void *v)
{
   VCHIQ_STATE_T *state = (VCHIQ_STATE_T *) v;
   VCHIQ_SHARED_STATE_T *local = state->local;
   DEBUG_INITIALISE(local)

   while (1) {
      DEBUG_COUNT(SLOT_HANDLER_COUNT);
      DEBUG_TRACE(SLOT_HANDLER_LINE);
      remote_event_wait(&local->trigger);

      vcos_rmb();

      DEBUG_TRACE(SLOT_HANDLER_LINE);
      if (state->poll_needed)
      {
         state->poll_needed = 0;

         /* Handle service polling and other rare conditions here out
            of the mainline code */
         switch (state->conn_state)
         {
         case VCHIQ_CONNSTATE_CONNECTED:
            /* Poll the services as requested */
            poll_services(state);
            break;

         case VCHIQ_CONNSTATE_PAUSING:
            if (queue_message(state, NULL,
               VCHIQ_MAKE_MSG(VCHIQ_MSG_PAUSE, 0, 0), NULL, 0, 0, 0)
               != VCHIQ_RETRY)
            {
               if (state->is_master)
                  pause_bulks(state);
               vchiq_set_conn_state(state, VCHIQ_CONNSTATE_PAUSE_SENT);
            }
            else
            {
               state->poll_needed = 1; /* Retry later */
            }
            break;

         case VCHIQ_CONNSTATE_RESUMING:
            if (queue_message(state, NULL,
               VCHIQ_MAKE_MSG(VCHIQ_MSG_RESUME, 0, 0), NULL, 0, 0, 0)
               != VCHIQ_RETRY)
            {
               if (state->is_master)
                  resume_bulks(state);
               vchiq_set_conn_state(state, VCHIQ_CONNSTATE_CONNECTED);
               vchiq_platform_resumed(state);
            }
            else
            {
               /* This should really be impossible, since the PAUSE should
                  have flushed through outstanding messages. */
               vcos_log_error("Failed to send RESUME message");
               vcos_demand(0);
            }
            break;
         default:
            break;
         }
      }

      DEBUG_TRACE(SLOT_HANDLER_LINE);
      parse_rx_slots(state);
   }
   return NULL;
}


/* Called by the recycle thread */
static void *
recycle_func(void *v)
{
   VCHIQ_STATE_T *state = (VCHIQ_STATE_T *) v;
   VCHIQ_SHARED_STATE_T *local = state->local;

   while (1) {
      remote_event_wait(&local->recycle);

      vcos_mutex_lock(&state->slot_mutex);

      process_free_queue(state);

      vcos_mutex_unlock(&state->slot_mutex);
   }
   return NULL;
}


static void
init_bulk_queue(VCHIQ_BULK_QUEUE_T *queue)
{
   queue->local_insert = 0;
   queue->remote_insert = 0;
   queue->process = 0;
   queue->remote_notify = 0;
   queue->remove = 0;
}

VCHIQ_SLOT_ZERO_T *
vchiq_init_slots(void *mem_base, int mem_size)
{
   int mem_align = (VCHIQ_SLOT_SIZE - (int)mem_base) & VCHIQ_SLOT_MASK;
   VCHIQ_SLOT_ZERO_T *slot_zero = (VCHIQ_SLOT_ZERO_T *)((char *)mem_base + mem_align);
   int num_slots = (mem_size - mem_align)/VCHIQ_SLOT_SIZE;
   int first_data_slot = VCHIQ_SLOT_ZERO_SLOTS;

   /* Ensure there is enough memory to run an absolutely minimum system */
   num_slots -= first_data_slot;

   if (num_slots < 4)
   {
      vcos_log_error("vchiq_init_slots - insufficient memory %x bytes", mem_size);
      return NULL;
   }

   memset(slot_zero, 0, sizeof(VCHIQ_SLOT_ZERO_T));

   slot_zero->magic = VCHIQ_MAGIC;
   slot_zero->version = VCHIQ_VERSION;
   slot_zero->version_min = VCHIQ_VERSION_MIN;
   slot_zero->slot_zero_size = sizeof(VCHIQ_SLOT_ZERO_T);
   slot_zero->slot_size = VCHIQ_SLOT_SIZE;
   slot_zero->max_slots = VCHIQ_MAX_SLOTS;
   slot_zero->max_slots_per_side = VCHIQ_MAX_SLOTS_PER_SIDE;

   slot_zero->master.slot_first = first_data_slot;
   slot_zero->slave.slot_first = first_data_slot + (num_slots/2);
   slot_zero->master.slot_last = slot_zero->slave.slot_first - 1;
   slot_zero->slave.slot_last = first_data_slot + num_slots - 1;

   return slot_zero;
}

VCHIQ_STATUS_T
vchiq_init_state(VCHIQ_STATE_T *state, VCHIQ_SLOT_ZERO_T *slot_zero, int is_master)
{
   VCHIQ_SHARED_STATE_T *local;
   VCHIQ_SHARED_STATE_T *remote;
   VCOS_THREAD_ATTR_T attrs;
   VCHIQ_STATUS_T status;
   char threadname[10];
   static int id = 0;
   int i;

   vcos_log_set_level(&vchiq_core_log_category, vchiq_default_core_log_level);
   vcos_log_set_level(&vchiq_core_msg_log_category, vchiq_default_core_msg_log_level);
   vcos_log_register("vchiq_core", &vchiq_core_log_category);
   vcos_log_register("vchiq_core_msg", &vchiq_core_msg_log_category);

   vcos_log_warn( "%s: slot_zero = 0x%08lx, is_master = %d", __func__, (unsigned long)slot_zero, is_master );

   /* Check the input configuration */

   if (slot_zero->magic != VCHIQ_MAGIC)
   {
      vcos_log_error("slot_zero=%x: magic=%x (expected %x)",
         (unsigned int)slot_zero, slot_zero->magic, VCHIQ_MAGIC);
      return VCHIQ_ERROR;
   }

   if (slot_zero->version < VCHIQ_VERSION_MIN)
   {
      vcos_log_error("slot_zero=%x: peer_version=%x (minimum %x)",
         (unsigned int)slot_zero, slot_zero->version, VCHIQ_VERSION_MIN);
      return VCHIQ_ERROR;
   }

   if (VCHIQ_VERSION < slot_zero->version_min)
   {
      vcos_log_error("slot_zero=%x: version=%x (peer minimum %x)",
         (unsigned int)slot_zero, VCHIQ_VERSION, slot_zero->version_min);
      return VCHIQ_ERROR;
   }

   if (slot_zero->slot_zero_size != sizeof(VCHIQ_SLOT_ZERO_T))
   {
      vcos_log_error("slot_zero=%x: slot_zero_size=%x (expected %x)",
         (unsigned int)slot_zero, slot_zero->slot_zero_size, sizeof(VCHIQ_SLOT_ZERO_T));
      return VCHIQ_ERROR;
   }

   if (slot_zero->slot_size != VCHIQ_SLOT_SIZE)
   {
      vcos_log_error("slot_zero=%x: slot_size=%d (expected %d",
         (unsigned int)slot_zero, slot_zero->slot_size, VCHIQ_SLOT_SIZE);
      return VCHIQ_ERROR;
   }

   if (slot_zero->max_slots != VCHIQ_MAX_SLOTS)
   {
      vcos_log_error("slot_zero=%x: max_slots=%d (expected %d)",
         (unsigned int)slot_zero, slot_zero->max_slots, VCHIQ_MAX_SLOTS);
      return VCHIQ_ERROR;
   }

   if (slot_zero->max_slots_per_side != VCHIQ_MAX_SLOTS_PER_SIDE)
   {
      vcos_log_error("slot_zero=%x: max_slots_per_side=%d (expected %d)",
         (unsigned int)slot_zero, slot_zero->max_slots_per_side,
         VCHIQ_MAX_SLOTS_PER_SIDE);
      return VCHIQ_ERROR;
   }

   if (is_master)
   {
      local = &slot_zero->master;
      remote = &slot_zero->slave;
   }
   else
   {
      local = &slot_zero->slave;
      remote = &slot_zero->master;
   }

   if (local->initialised)
   {
      if (remote->initialised)
         vcos_log_error("vchiq: FATAL: local state has already been initialised");
      else
         vcos_log_error("vchiq: FATAL: master/slave mismatch - two %ss", is_master ? "master" : "slave");
      return VCHIQ_ERROR;
   }

   memset(state, 0, sizeof(VCHIQ_STATE_T));
   vcos_log_warn( "%s: called", __func__);
   state->id = id++;
   state->is_master = is_master;

   /*
      initialize shared state pointers
    */

   state->local = local;
   state->remote = remote;
   state->slot_data = (VCHIQ_SLOT_T *)slot_zero;

   /*
      initialize events and mutexes
    */

   vcos_event_create(&state->connect, "v.connect");
   vcos_mutex_create(&state->mutex, "v.mutex");
   vcos_event_create(&state->trigger_event, "v.trigger_event");
   vcos_event_create(&state->recycle_event, "v.recycle_event");

   vcos_mutex_create(&state->slot_mutex, "v.slot_mutex");
   vcos_mutex_create(&state->recycle_mutex, "v.recycle_mutex");

   vcos_event_create(&state->slot_available_event, "v.slot_available_event");
   vcos_event_create(&state->slot_remove_event, "v.slot_remove_event");

   state->slot_queue_available = 0;

   for (i = 0; i < VCHIQ_MAX_SERVICES; i++)
   {
      VCHIQ_SERVICE_QUOTA_T *service_quota = &state->service_quotas[i];
      vcos_event_create(&service_quota->quota_event, "v.quota_event");
   }

   for (i = local->slot_first; i <= local->slot_last; i++)
   {
      local->slot_queue[state->slot_queue_available++] = i;
   }

   state->default_slot_quota = state->slot_queue_available/2;
   state->default_message_quota = vcos_min(state->default_slot_quota * 256, (unsigned short)~0);

   local->trigger.event = &state->trigger_event;
   remote_event_create(&local->trigger);
   local->tx_pos = 0;

   local->recycle.event = &state->recycle_event;
   remote_event_create(&local->recycle);
   local->slot_queue_recycle = state->slot_queue_available;

   local->debug[DEBUG_ENTRIES] = DEBUG_MAX;

   /*
      bring up slot handler thread
    */

   vcos_thread_attr_init(&attrs);
   vcos_thread_attr_setstacksize(&attrs, VCHIQ_SLOT_HANDLER_STACK);
   vcos_thread_attr_setpriority(&attrs, VCOS_THREAD_PRI_REALTIME);
   vcos_snprintf(threadname, sizeof(threadname), "VCHIQ-%d", state->id);
   if (vcos_thread_create(&state->slot_handler_thread, threadname,
            &attrs, slot_handler_func, state) != VCOS_SUCCESS)
   {
      vcos_log_error("vchiq: FATAL: couldn't create thread %s", threadname);
      return VCHIQ_ERROR;
   }

   vcos_thread_attr_init(&attrs);
   vcos_thread_attr_setstacksize(&attrs, VCHIQ_SLOT_HANDLER_STACK);
   vcos_thread_attr_setpriority(&attrs, VCOS_THREAD_PRI_REALTIME);
   vcos_snprintf(threadname, sizeof(threadname), "VCHIQr-%d", state->id);
   if (vcos_thread_create(&state->recycle_thread, threadname,
            &attrs, recycle_func, state) != VCOS_SUCCESS)
   {
      vcos_log_error("vchiq: FATAL: couldn't create thread %s", threadname);
      return VCHIQ_ERROR;
   }

   status = vchiq_platform_init_state(state);

   /* Indicate readiness to the other side */
   local->initialised = 1;

   return status;
}

/* Called from application thread when a client or server service is created. */
VCHIQ_SERVICE_T *
vchiq_add_service_internal(VCHIQ_STATE_T *state,
   const VCHIQ_SERVICE_PARAMS_T *params, int srvstate,
   VCHIQ_INSTANCE_T instance)
{
   VCHIQ_SERVICE_T **pservice = NULL;
   VCHIQ_SERVICE_T *service = NULL;
   int i;

   /* Prepare to use a previously unused service */
   if (state->unused_service < VCHIQ_MAX_SERVICES)
   {
      pservice = &state->services[state->unused_service];
   }

   if (srvstate == VCHIQ_SRVSTATE_OPENING) {
      for (i = 0; i < state->unused_service; i++) {
         VCHIQ_SERVICE_T *srv = state->services[i];
         if (!srv)
         {
            pservice = &state->services[i];
            break;
         }
         if (srv->srvstate == VCHIQ_SRVSTATE_FREE) {
            service = srv;
            break;
         }
      }
   } else {
      for (i = (state->unused_service - 1); i >= 0; i--) {
         VCHIQ_SERVICE_T *srv = state->services[i];
         if (!srv)
            pservice = &state->services[i];
         else if (srv->srvstate == VCHIQ_SRVSTATE_FREE) {
            service = srv;
         } else if ((srv->public_fourcc == params->fourcc) &&
            ((srv->instance != instance)
            || (srv->base.callback != params->callback))) {
            /* There is another server using this fourcc which doesn't match */
            pservice = NULL;
            service = NULL;
         }
      }
   }

   if (pservice && !service)
   {
      service = vcos_malloc(sizeof(VCHIQ_SERVICE_T), "VCHIQ service");
      if (service)
      {
         service->srvstate = VCHIQ_SRVSTATE_FREE;
         service->localport = (pservice - state->services);
         vcos_event_create(&service->remove_event, "v.remove_event");
         vcos_event_create(&service->bulk_remove_event, "v.bulk_remove_event");
         vcos_mutex_create(&service->bulk_mutex, "v.bulk_mutex");
         *pservice = service;
      }
      else
      {
         vcos_log_error("vchiq: Out of memory");
      }
   }

   if (service) {
      VCHIQ_SERVICE_QUOTA_T *service_quota =
         &state->service_quotas[service->localport];
      if (vcos_is_log_enabled( &vchiq_core_msg_log_category, VCOS_LOG_INFO)) {
         vcos_log_impl( &vchiq_core_msg_log_category,
                     VCOS_LOG_INFO,
                     "%s Service %c%c%c%c SrcPort:%d",
                     ( srvstate == VCHIQ_SRVSTATE_OPENING )
                     ? "Open" : "Add",
                     VCHIQ_FOURCC_AS_4CHARS(params->fourcc),
                     service->localport );
      }
      service->state = state;
      service->base.fourcc   = params->fourcc;
      service->base.callback = params->callback;
      service->base.userdata = params->userdata;
      service->version       = params->version;
      service->version_min   = params->version_min;
      vchiq_set_service_state(service, srvstate);
      service->public_fourcc =
         (srvstate ==
         VCHIQ_SRVSTATE_OPENING) ? VCHIQ_FOURCC_INVALID : params->fourcc;
      service->instance = instance;
      service->remoteport = VCHIQ_PORT_FREE;
      service->client_id = 0;
      service->auto_close = 1;
      service->service_use_count = 0;
      init_bulk_queue(&service->bulk_tx);
      init_bulk_queue(&service->bulk_rx);
      service_quota->slot_quota = state->default_slot_quota;
      service_quota->message_quota = state->default_message_quota;
      if (service_quota->slot_use_count == 0)
         service_quota->previous_tx_index =
            SLOT_QUEUE_INDEX_FROM_POS(state->local_tx_pos) - 1;
      memset(&service->stats, 0, sizeof(service->stats));
      vcos_atomic_flags_create(&service->poll_flags);

      /* Ensure the events are unsignalled */
      while (vcos_event_try(&service->remove_event) == VCOS_SUCCESS)
         continue;
      while (vcos_event_try(&service_quota->quota_event) == VCOS_SUCCESS)
         continue;

      if (pservice == &state->services[state->unused_service])
         state->unused_service++;
   }

   return service;
}

VCHIQ_STATUS_T
vchiq_open_service_internal(VCHIQ_SERVICE_T *service, int client_id)
{
   VCHIQ_OPEN_PAYLOAD_T payload = {
      service->base.fourcc,
      client_id,
      service->version,
      service->version_min
   };
   VCHIQ_ELEMENT_T body = { &payload, sizeof(payload) };
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;

   service->client_id = client_id;
   vchiq_use_service(&service->base);
   status = queue_message(service->state, NULL,
                     VCHIQ_MAKE_MSG(VCHIQ_MSG_OPEN, service->localport, 0),
                     &body, 1, sizeof(payload), 1);
   if (status == VCHIQ_SUCCESS) {
      if (vcos_event_wait(&service->remove_event) != VCOS_SUCCESS) {
         status = VCHIQ_RETRY;
         vchiq_release_service(&service->base);
      } else if (service->srvstate != VCHIQ_SRVSTATE_OPEN) {
         vcos_log_info("%d: osi - srvstate = %d", service->state->id, service->srvstate);
         vcos_assert(service->srvstate == VCHIQ_SRVSTATE_CLOSEWAIT);
         status = VCHIQ_ERROR;
         VCHIQ_SERVICE_STATS_INC(service, error_count);
         vchiq_release_service(&service->base);
      }
   }
   return status;
}

/* Called by the slot handler */
VCHIQ_STATUS_T
vchiq_close_service_internal(VCHIQ_SERVICE_T *service, int close_recvd)
{
   VCHIQ_STATE_T *state = service->state;
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;

   vcos_log_trace("%d: csi:%d (%s)",
      service->state->id, service->localport,
      srvstate_names[service->srvstate]);

   switch (service->srvstate)
   {
   case VCHIQ_SRVSTATE_OPENING:
      if (close_recvd)
      {
         /* The open was rejected - tell the user */
         vchiq_set_service_state(service, VCHIQ_SRVSTATE_CLOSEWAIT);
         vcos_event_signal(&service->remove_event);
      }
      else
      {
         /* Shutdown mid-open - let the other side know */
         status = queue_message(state, NULL,
            VCHIQ_MAKE_MSG
            (VCHIQ_MSG_CLOSE,
            service->localport,
            VCHIQ_MSG_DSTPORT(service->remoteport)),
            NULL, 0, 0, 0);

         if (status == VCHIQ_SUCCESS)
            vchiq_set_service_state(service, VCHIQ_SRVSTATE_CLOSESENT);
      }
      break;

   case VCHIQ_SRVSTATE_OPEN:
      if (state->is_master)
      {
         /* Abort any outstanding bulk transfers */
         vcos_mutex_lock(&service->bulk_mutex);
         abort_outstanding_bulks(service, &service->bulk_tx);
         abort_outstanding_bulks(service, &service->bulk_rx);
         status = notify_bulks(service, &service->bulk_tx);
         if (status == VCHIQ_SUCCESS)
            status = notify_bulks(service, &service->bulk_rx);
         vcos_mutex_unlock(&service->bulk_mutex);
      }

      if (status == VCHIQ_SUCCESS)
         status = queue_message(state, NULL,
            VCHIQ_MAKE_MSG
            (VCHIQ_MSG_CLOSE,
            service->localport,
            VCHIQ_MSG_DSTPORT(service->remoteport)),
            NULL, 0, 0, 0);

      if (status == VCHIQ_SUCCESS)
      {
         if (close_recvd)
            vchiq_set_service_state(service, VCHIQ_SRVSTATE_CLOSING);
         else
            vchiq_set_service_state(service, VCHIQ_SRVSTATE_CLOSESENT);
      }
      break;

   case VCHIQ_SRVSTATE_CLOSESENT:
      vcos_assert(close_recvd);

      if (!state->is_master)
      {
         /* Abort any outstanding bulk transfers */
         vcos_mutex_lock(&service->bulk_mutex);
         abort_outstanding_bulks(service, &service->bulk_tx);
         abort_outstanding_bulks(service, &service->bulk_rx);
         status = notify_bulks(service, &service->bulk_tx);
         if (status == VCHIQ_SUCCESS)
            status = notify_bulks(service, &service->bulk_rx);
         vcos_mutex_unlock(&service->bulk_mutex);
      }

      if (status == VCHIQ_SUCCESS)
         vchiq_set_service_state(service, VCHIQ_SRVSTATE_CLOSING);
      break;

   case VCHIQ_SRVSTATE_CLOSING:
      /* We may come here after a retry */
      vcos_assert(!close_recvd);
      break;

   default:
      vcos_log_error("vchiq_close_service_internal(%d) called in state %s",
         close_recvd, srvstate_names[service->srvstate]);
      vcos_assert(0);
      break;
   }

   if (service->srvstate == VCHIQ_SRVSTATE_CLOSING)
   {
      int i;
      int uc = service->service_use_count;
      /* Complete the close process */
      for( i=0; i<uc; i++)
      { /* cater for cases where close is forced and the client may not close all it's handles */
         vchiq_release_service_internal(service);
      }
      service->client_id = 0;

      /* Now tell the client that the services is closed */
      if (service->instance)
      {
         int oldstate = service->srvstate;

         /* Change the service state now for the benefit of the callback */
         vchiq_set_service_state(service,
            ((service->public_fourcc == VCHIQ_FOURCC_INVALID) ||
            !service->auto_close) ?
            VCHIQ_SRVSTATE_CLOSEWAIT :
            VCHIQ_SRVSTATE_LISTENING);

         status = make_service_callback(service, VCHIQ_SERVICE_CLOSED, NULL, NULL);

         if (status == VCHIQ_RETRY)
         {
            /* Restore the old state, to be retried later */
            vchiq_set_service_state(service, oldstate);
         }
         else
         {
            if (status == VCHIQ_ERROR) {
               /* Signal an error (fatal, since the other end will probably have closed) */
               vchiq_set_service_state(service, VCHIQ_SRVSTATE_OPEN);
            }
         }
      }

      if (status != VCHIQ_RETRY)
      {
         if (service->srvstate == VCHIQ_SRVSTATE_CLOSING)
            vchiq_set_service_state(service, VCHIQ_SRVSTATE_CLOSEWAIT);
         vcos_event_signal(&service->remove_event);
      }
   }

   return status;
}

/* Called from the application process upon process death */
void
vchiq_terminate_service_internal(VCHIQ_SERVICE_T *service)
{
   VCHIQ_STATE_T *state = service->state;

   vcos_log_info("%d: tsi - (%d<->%d)", state->id, service->localport, service->remoteport);

   /* Disconnect from the instance, to prevent any callbacks */
   service->instance = NULL;

   /* Mark the service for termination by the slot handler */
   request_poll(state, service, VCHIQ_POLL_TERMINATE);
}

/* Called from the application process upon process death, and from
   vchiq_remove_service */
void
vchiq_free_service_internal(VCHIQ_SERVICE_T *service)
{
   VCHIQ_STATE_T *state = service->state;
   int slot_last = state->remote->slot_last;
   int i;

   vcos_log_info("%d: fsi - (%d)", state->id, service->localport);

   vcos_mutex_lock(&state->mutex);

   /* Release any claimed messages */
   for (i = state->remote->slot_first; i <= slot_last; i++)
   {
      VCHIQ_SLOT_INFO_T *slot_info = SLOT_INFO_FROM_INDEX(state, i);
      if (slot_info->release_count != slot_info->use_count)
      {
         char *data = (char *)SLOT_DATA_FROM_INDEX(state, i);
         unsigned int pos, end;

         end = VCHIQ_SLOT_SIZE;
         if (data == state->rx_data)
         {
            /* This buffer is still being read from - stop at the current read position */
            end = state->rx_pos & VCHIQ_SLOT_MASK;
         }

         pos = 0;

         while (pos < end)
         {
            VCHIQ_HEADER_T *header = (VCHIQ_HEADER_T *)(data + pos);
            int msgid = header->msgid;
            int port = VCHIQ_MSG_DSTPORT(msgid);
            if (port == service->localport)
            {
               if (msgid & VCHIQ_MSGID_CLAIMED)
               {
                  header->msgid = msgid & ~VCHIQ_MSGID_CLAIMED;
                  vcos_log_info("  fsi - hdr %x", (unsigned int)header);
                  release_slot(state, slot_info);
               }
            }
            pos += calc_stride(header->size);
            if (pos > VCHIQ_SLOT_SIZE)
            {
               vcos_log_error("fsi - pos %x: header %x, msgid %x, header->msgid %x, header->size %x",
                  pos, (unsigned int)header, msgid, header->msgid, header->size);
               vcos_assert(0);
            }
         }
      }
   }

   vcos_assert(state->services[service->localport] == service);
   vchiq_set_service_state(service, VCHIQ_SRVSTATE_FREE);
   state->services[service->localport] = NULL;
   vcos_free(service);
   vcos_mutex_unlock(&state->mutex);
}

VCHIQ_STATUS_T
vchiq_connect_internal(VCHIQ_STATE_T *state, VCHIQ_INSTANCE_T instance)
{
   int i;

   /* Find all services registered to this client and enable them. */
   for (i = 0; i < state->unused_service; i++)
   {
      VCHIQ_SERVICE_T *service = state->services[i];
      if (service && (service->instance == instance)) {
         if (service->srvstate == VCHIQ_SRVSTATE_HIDDEN)
            vchiq_set_service_state(service,
               VCHIQ_SRVSTATE_LISTENING);
      }
   }

   if (state->conn_state == VCHIQ_CONNSTATE_DISCONNECTED) {
      if (queue_message(state, NULL,
         VCHIQ_MAKE_MSG(VCHIQ_MSG_CONNECT, 0, 0), NULL, 0,
         0, 1) == VCHIQ_RETRY)
         return VCHIQ_RETRY;
      vcos_event_wait(&state->connect);

      vchiq_set_conn_state(state, VCHIQ_CONNSTATE_CONNECTED);
   }

   return VCHIQ_SUCCESS;
}

VCHIQ_STATUS_T
vchiq_shutdown_internal(VCHIQ_STATE_T *state, VCHIQ_INSTANCE_T instance)
{
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;
   int i;

   /* Find all services registered to this client and close them. */
   for (i = 0; i < state->unused_service; i++)
   {
      VCHIQ_SERVICE_T *service = state->services[i];
      if (service && (service->instance == instance) &&
         ((service->srvstate == VCHIQ_SRVSTATE_OPEN) ||
         (service->srvstate == VCHIQ_SRVSTATE_LISTENING)))
      {
         status = vchiq_remove_service(&service->base);
         if (status != VCHIQ_SUCCESS)
            break;
      }
   }

   return status;
}

VCHIQ_STATUS_T
vchiq_pause_internal(VCHIQ_STATE_T *state)
{
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;

   switch (state->conn_state)
   {
   case VCHIQ_CONNSTATE_CONNECTED:
      /* Request a pause */
      vchiq_set_conn_state(state, VCHIQ_CONNSTATE_PAUSING);
      request_poll(state, NULL, 0);
      break;
   case VCHIQ_CONNSTATE_PAUSED:
      break;
   default:
      status = VCHIQ_ERROR;
      VCHIQ_STATS_INC(state, error_count);
      break;
   }

   return status;
}

VCHIQ_STATUS_T
vchiq_resume_internal(VCHIQ_STATE_T *state)
{
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;

   if (state->conn_state == VCHIQ_CONNSTATE_PAUSED)
   {
      vchiq_set_conn_state(state, VCHIQ_CONNSTATE_RESUMING);
      request_poll(state, NULL, 0);
   }
   else
   {
      status = VCHIQ_ERROR;
      VCHIQ_STATS_INC(state, error_count);
   }

   return status;
}

VCHIQ_STATUS_T
vchiq_close_service(VCHIQ_SERVICE_HANDLE_T handle)
{
   /* Unregister the service */
   VCHIQ_SERVICE_T *service = (VCHIQ_SERVICE_T *) handle;
   VCHIQ_STATUS_T status = VCHIQ_ERROR;

   if (!is_valid_service(service))
      return VCHIQ_ERROR;

   vcos_log_info("%d: close_service:%d", service->state->id, service->localport);

   if (service->public_fourcc != VCHIQ_FOURCC_INVALID)
   {
      if (service->srvstate == VCHIQ_SRVSTATE_CLOSEWAIT)
      {
         /* This is a non-auto-close server */
         vchiq_set_service_state(service, VCHIQ_SRVSTATE_LISTENING);
         status = VCHIQ_SUCCESS;
      }
   }
   else
   {
      /* For clients, make it an alias of vchiq_remove_service */
      status = vchiq_remove_service(handle);
   }

   return status;
}

VCHIQ_STATUS_T
vchiq_remove_service(VCHIQ_SERVICE_HANDLE_T handle)
{
   /* Unregister the service */
   VCHIQ_SERVICE_T *service = (VCHIQ_SERVICE_T *) handle;
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;

   if (!is_valid_service(service))
      return VCHIQ_ERROR;

   vcos_log_info("%d: remove_service:%d", service->state->id, service->localport);

   switch (service->srvstate)
   {
   case VCHIQ_SRVSTATE_OPENING:
   case VCHIQ_SRVSTATE_OPEN:
      /* Mark the service for termination by the slot handler */
      request_poll(service->state, service, VCHIQ_POLL_TERMINATE);

      /* Drop through... */
   case VCHIQ_SRVSTATE_CLOSESENT:
   case VCHIQ_SRVSTATE_CLOSING:
      while ((service->srvstate != VCHIQ_SRVSTATE_CLOSEWAIT) &&
         (service->srvstate != VCHIQ_SRVSTATE_LISTENING))
      {
         if (vcos_event_wait(&service->remove_event) != VCOS_SUCCESS) {
            status = VCHIQ_RETRY;
            break;
         }
      }
      break;

   default:
      break;
   }

   if (status == VCHIQ_SUCCESS) {
      if (service->srvstate == VCHIQ_SRVSTATE_OPEN)
         status = VCHIQ_ERROR;
      else
      {
         service->instance = NULL;
         vchiq_free_service_internal(service);
      }
   }

   return status;
}


VCHIQ_STATUS_T
vchiq_bulk_transfer(VCHIQ_SERVICE_T *service,
   VCHI_MEM_HANDLE_T memhandle, void *offset, int size, void *userdata,
   VCHIQ_BULK_MODE_T mode, VCHIQ_BULK_DIR_T dir)
{
   VCHIQ_BULK_QUEUE_T *queue = (dir == VCHIQ_BULK_TRANSMIT) ?
      &service->bulk_tx : &service->bulk_rx;
   VCHIQ_BULK_T *bulk;
   VCHIQ_STATE_T *state;
   BULK_WAITER_T bulk_waiter;
   const char dir_char = (dir == VCHIQ_BULK_TRANSMIT) ? 't' : 'r';
   const int dir_msgtype = (dir == VCHIQ_BULK_TRANSMIT) ? VCHIQ_MSG_BULK_TX : VCHIQ_MSG_BULK_RX;
   VCHIQ_STATUS_T status = VCHIQ_ERROR;

   if (!is_valid_service(service) ||
       (service->srvstate != VCHIQ_SRVSTATE_OPEN) ||
       ((memhandle == VCHI_MEM_HANDLE_INVALID) && (offset == NULL)) ||
       (vchiq_check_service(service) != VCHIQ_SUCCESS))
      return VCHIQ_ERROR;

   state = service->state;

   if (vcos_mutex_lock(&service->bulk_mutex) != VCOS_SUCCESS)
      return VCHIQ_RETRY;

   if (queue->local_insert == queue->remove + VCHIQ_NUM_SERVICE_BULKS)
   {
      VCHIQ_SERVICE_STATS_INC(service, bulk_stalls);
      do {
         vcos_mutex_unlock(&service->bulk_mutex);
         if (vcos_event_wait(&service->bulk_remove_event) != VCOS_SUCCESS)
            return VCHIQ_RETRY;
         if (vcos_mutex_lock(&service->bulk_mutex) != VCOS_SUCCESS)
            return VCHIQ_RETRY;
      } while (queue->local_insert == queue->remove + VCHIQ_NUM_SERVICE_BULKS);
   }

   bulk = &queue->bulks[BULK_INDEX(queue->local_insert)];

   if (mode == VCHIQ_BULK_MODE_BLOCKING)
   {
      vcos_event_create(&bulk_waiter.event, "bulk_waiter");
      bulk_waiter.actual = 0;
      userdata = &bulk_waiter;
   }

   bulk->mode = mode;
   bulk->dir = dir;
   bulk->userdata = userdata;
   bulk->size = size;
   bulk->actual = VCHIQ_BULK_ACTUAL_ABORTED;

   if (vchiq_prepare_bulk_data(bulk, memhandle, offset, size, dir) != VCHIQ_SUCCESS)
   {
      goto error_exit;
   }

   vcos_log_info("%d: bt (%d->%d) %cx %x@%x %x", state->id,
      service->localport, service->remoteport, dir_char,
      size, (unsigned int)bulk->data, (unsigned int)userdata);

   if (state->is_master)
   {
      queue->local_insert++;
      if (resolve_bulks(service, queue))
         request_poll(state, service, (dir == VCHIQ_BULK_TRANSMIT) ?
            VCHIQ_POLL_TXNOTIFY : VCHIQ_POLL_RXNOTIFY);
   }
   else
   {
      int payload[2] = { (int)bulk->data, bulk->size };
      VCHIQ_ELEMENT_T element = { payload, sizeof(payload) };

      if (queue_message(state, NULL,
         VCHIQ_MAKE_MSG(dir_msgtype,
            service->localport, service->remoteport),
         &element, 1, sizeof(payload), 1) != VCHIQ_SUCCESS)
      {
         vchiq_complete_bulk(bulk);
         goto error_exit;
      }
      queue->local_insert++;
      queue->remote_insert++;
   }

   vcos_mutex_unlock(&service->bulk_mutex);
 
   vcos_log_trace("%d: bt:%d %cx li=%x ri=%x p=%x", state->id,
      service->localport, dir_char,
      queue->local_insert, queue->remote_insert, queue->process);

   status = VCHIQ_SUCCESS;

   if (mode == VCHIQ_BULK_MODE_BLOCKING)
   {
      if (vcos_event_wait(&bulk_waiter.event) != VCOS_SUCCESS)
      {
         vcos_log_info("bulk wait interrupted");
         /* Stop notify_bulks signalling a non-existent waiter */
         bulk->userdata = NULL;
         status = VCHIQ_ERROR;
      }
      else if (bulk_waiter.actual == VCHIQ_BULK_ACTUAL_ABORTED)
         status = VCHIQ_ERROR;

      vcos_event_delete(&bulk_waiter.event);
   }

   return status;

error_exit:
   if (mode == VCHIQ_BULK_MODE_BLOCKING)
      vcos_event_delete(&bulk_waiter.event);
   vcos_mutex_unlock(&service->bulk_mutex);

   return status;
}

VCHIQ_STATUS_T
vchiq_queue_bulk_transmit(VCHIQ_SERVICE_HANDLE_T handle,
   const void *data, int size, void *userdata)
{
   return vchiq_bulk_transfer((VCHIQ_SERVICE_T *)handle,
      VCHI_MEM_HANDLE_INVALID, (void *)data, size, userdata,
      VCHIQ_BULK_MODE_CALLBACK, VCHIQ_BULK_TRANSMIT);
}

VCHIQ_STATUS_T
vchiq_queue_bulk_receive(VCHIQ_SERVICE_HANDLE_T handle, void *data, int size,
   void *userdata)
{
   return vchiq_bulk_transfer((VCHIQ_SERVICE_T *)handle,
      VCHI_MEM_HANDLE_INVALID, data, size, userdata,
      VCHIQ_BULK_MODE_CALLBACK, VCHIQ_BULK_RECEIVE);
}

VCHIQ_STATUS_T
vchiq_queue_bulk_transmit_handle(VCHIQ_SERVICE_HANDLE_T handle,
   VCHI_MEM_HANDLE_T memhandle, const void *offset, int size, void *userdata)
{
   return vchiq_bulk_transfer((VCHIQ_SERVICE_T *)handle,
      memhandle, (void *)offset, size, userdata,
      VCHIQ_BULK_MODE_CALLBACK, VCHIQ_BULK_TRANSMIT);
}

VCHIQ_STATUS_T
vchiq_queue_bulk_receive_handle(VCHIQ_SERVICE_HANDLE_T handle,
   VCHI_MEM_HANDLE_T memhandle, void *offset, int size, void *userdata)
{
   return vchiq_bulk_transfer((VCHIQ_SERVICE_T *)handle,
      memhandle, offset, size, userdata,
      VCHIQ_BULK_MODE_CALLBACK, VCHIQ_BULK_RECEIVE);
}

VCHIQ_STATUS_T
vchiq_bulk_transmit(VCHIQ_SERVICE_HANDLE_T handle, const void *data, int size,
   void *userdata, VCHIQ_BULK_MODE_T mode)
{
   return vchiq_bulk_transfer((VCHIQ_SERVICE_T *)handle,
      VCHI_MEM_HANDLE_INVALID, (void *)data, size, userdata,
      mode, VCHIQ_BULK_TRANSMIT);
}

VCHIQ_STATUS_T
vchiq_bulk_receive(VCHIQ_SERVICE_HANDLE_T handle, void *data, int size,
   void *userdata, VCHIQ_BULK_MODE_T mode)
{
   return vchiq_bulk_transfer((VCHIQ_SERVICE_T *)handle,
      VCHI_MEM_HANDLE_INVALID, data, size, userdata,
      mode, VCHIQ_BULK_RECEIVE);
}

VCHIQ_STATUS_T
vchiq_bulk_transmit_handle(VCHIQ_SERVICE_HANDLE_T handle,
   VCHI_MEM_HANDLE_T memhandle, const void *offset, int size, void *userdata,
   VCHIQ_BULK_MODE_T mode)
{
   return vchiq_bulk_transfer((VCHIQ_SERVICE_T *)handle,
      memhandle, (void *)offset, size, userdata,
      mode, VCHIQ_BULK_TRANSMIT);
}

VCHIQ_STATUS_T
vchiq_bulk_receive_handle(VCHIQ_SERVICE_HANDLE_T handle,
   VCHI_MEM_HANDLE_T memhandle, void *offset, int size, void *userdata,
   VCHIQ_BULK_MODE_T mode)
{
   return vchiq_bulk_transfer((VCHIQ_SERVICE_T *)handle,
      memhandle, offset, size, userdata,
      mode, VCHIQ_BULK_RECEIVE);
}

VCHIQ_STATUS_T
vchiq_queue_message(VCHIQ_SERVICE_HANDLE_T handle,
   const VCHIQ_ELEMENT_T *elements, int count)
{
   VCHIQ_SERVICE_T *service = (VCHIQ_SERVICE_T *) handle;

   unsigned int size = 0;
   unsigned int i;

   if (!is_valid_service(service) ||
      (service->srvstate != VCHIQ_SRVSTATE_OPEN) ||
      (vchiq_check_service(service) != VCHIQ_SUCCESS))
      return VCHIQ_ERROR;

   for (i = 0; i < (unsigned int)count; i++)
   {
      if (elements[i].size)
      {
         if (elements[i].data == NULL)
         {
            VCHIQ_SERVICE_STATS_INC(service, error_count);
            return VCHIQ_ERROR;
         }
         size += elements[i].size;
      }
   }

   if (size > VCHIQ_MAX_MSG_SIZE)
   {
      VCHIQ_SERVICE_STATS_INC(service, error_count);
      return VCHIQ_ERROR;
   }

   return queue_message(service->state, service,
            VCHIQ_MAKE_MSG(VCHIQ_MSG_DATA, service->localport,
               service->remoteport), elements, count, size, 1);
}

void
vchiq_release_message(VCHIQ_SERVICE_HANDLE_T handle, VCHIQ_HEADER_T *header)
{
   VCHIQ_SERVICE_T *service = (VCHIQ_SERVICE_T *)handle;
   VCHIQ_STATE_T *state;
   int slot_index;
   int msgid;

   if (!is_valid_service(service))
      return;

   state = service->state;

   slot_index = SLOT_INDEX_FROM_DATA(state, (void *)header);

   if ((slot_index >= state->remote->slot_first) &&
      (slot_index <= state->remote->slot_last) &&
      ((msgid = header->msgid) & VCHIQ_MSGID_CLAIMED))
   {
      VCHIQ_SLOT_INFO_T *slot_info = SLOT_INFO_FROM_INDEX(state, slot_index);

      /* Rewrite the message header to prevent a double release */
      header->msgid = msgid & ~VCHIQ_MSGID_CLAIMED;

      release_slot(state, slot_info);
   }
}

int
vchiq_get_client_id(VCHIQ_SERVICE_HANDLE_T handle)
{
   VCHIQ_SERVICE_T *service = (VCHIQ_SERVICE_T *)handle;
   return service ? service->client_id : 0;
}

VCHIQ_STATUS_T
vchiq_get_config(VCHIQ_INSTANCE_T instance,
   int config_size, VCHIQ_CONFIG_T *pconfig)
{
   VCHIQ_CONFIG_T config;

   vcos_unused(instance);

   config.max_msg_size           = VCHIQ_MAX_MSG_SIZE;
   config.bulk_threshold         = VCHIQ_MAX_MSG_SIZE;
   config.max_outstanding_bulks  = VCHIQ_NUM_SERVICE_BULKS;
   config.max_services           = VCHIQ_MAX_SERVICES;
   config.version                = VCHIQ_VERSION;
   config.version_min            = VCHIQ_VERSION_MIN;

   if (config_size > sizeof(VCHIQ_CONFIG_T))
      return VCHIQ_ERROR;

   memcpy(pconfig, &config, vcos_min(config_size, sizeof(VCHIQ_CONFIG_T)));

   return VCHIQ_SUCCESS;
}

VCHIQ_STATUS_T
vchiq_set_service_option(VCHIQ_SERVICE_HANDLE_T handle,
   VCHIQ_SERVICE_OPTION_T option, int value)
{
   VCHIQ_SERVICE_T *service = (VCHIQ_SERVICE_T *)handle;
   VCHIQ_STATUS_T status = VCHIQ_ERROR;

   if (is_valid_service(service))
   {
      switch (option)
      {
      case VCHIQ_SERVICE_OPTION_AUTOCLOSE:
         service->auto_close = value;
         status = VCHIQ_SUCCESS;
         break;

      case VCHIQ_SERVICE_OPTION_SLOT_QUOTA:
         {
            VCHIQ_SERVICE_QUOTA_T *service_quota =
               &service->state->service_quotas[service->localport];
            if (value == 0)
               value = service->state->default_slot_quota;
            if ((value >= service_quota->slot_use_count) &&
                (value < (unsigned short)~0))
            {
               service_quota->slot_quota = value;
               if ((value >= service_quota->slot_use_count) &&
                   (service_quota->message_quota >= service_quota->message_use_count))
               {
                  /* Signal the service that it may have dropped below its quota */
                  vcos_event_signal(&service_quota->quota_event);
               }
               status = VCHIQ_SUCCESS;
            }
         }
         break;

      case VCHIQ_SERVICE_OPTION_MESSAGE_QUOTA:
         {
            VCHIQ_SERVICE_QUOTA_T *service_quota =
               &service->state->service_quotas[service->localport];
            if (value == 0)
               value = service->state->default_message_quota;
            if ((value >= service_quota->message_use_count) &&
                (value < (unsigned short)~0))
            {
               service_quota->message_quota = value;
               if ((value >= service_quota->message_use_count) &&
                   (service_quota->slot_quota >= service_quota->slot_use_count))
               {
                  /* Signal the service that it may have dropped below its quota */
                  vcos_event_signal(&service_quota->quota_event);
               }
               status = VCHIQ_SUCCESS;
            }
         }
         break;

      default:
         break;
      }
   }

   return status;
}

void
vchiq_dump_shared_state(void *dump_context, VCHIQ_STATE_T *state,
   VCHIQ_SHARED_STATE_T *shared, const char *label)
{
   static const char *const debug_names[] =
   {
      "<entries>",
      "SLOT_HANDLER_COUNT",
      "SLOT_HANDLER_LINE",
      "PARSE_LINE",
      "PARSE_HEADER",
      "PARSE_MSGID",
      "AWAIT_COMPLETION_LINE",
      "DEQUEUE_MESSAGE_LINE",
      "SERVICE_CALLBACK_LINE",
      "MSG_QUEUE_FULL_COUNT",
      "COMPLETION_QUEUE_FULL_COUNT"
   };
   int i;

   char buf[80];
   int len;
   len = vcos_snprintf(buf, sizeof(buf),
      "  %s: slots %d-%d tx_pos=%x recycle=%x",
      label, shared->slot_first, shared->slot_last,
      shared->tx_pos, shared->slot_queue_recycle);
   vchiq_dump(dump_context, buf, len + 1);

   len = vcos_snprintf(buf, sizeof(buf),
      "    Slots claimed:"); 
   vchiq_dump(dump_context, buf, len + 1);

   for (i = shared->slot_first; i <= shared->slot_last; i++)
   {
      VCHIQ_SLOT_INFO_T slot_info = *SLOT_INFO_FROM_INDEX(state, i);
      if (slot_info.use_count != slot_info.release_count)
      {
         len = vcos_snprintf(buf, sizeof(buf),
            "      %d: %d/%d", i, slot_info.use_count, slot_info.release_count);
         vchiq_dump(dump_context, buf, len + 1);
      }
   }

   for (i = 1; i < shared->debug[DEBUG_ENTRIES]; i++)
   {
      len = vcos_snprintf(buf, sizeof(buf), "    DEBUG: %s = %d(%x)",
         debug_names[i], shared->debug[i], shared->debug[i]);
      vchiq_dump(dump_context, buf, len + 1);
   }
}

void
vchiq_dump_state(void *dump_context, VCHIQ_STATE_T *state)
{
   char buf[80];
   int len;
   int i;

   len = vcos_snprintf(buf, sizeof(buf), "State %d: %s", state->id,
      conn_state_names[state->conn_state]);
   vchiq_dump(dump_context, buf, len + 1);

   len = vcos_snprintf(buf, sizeof(buf),
      "  tx_pos=%x(@%x), rx_pos=%x(@%x)",
      state->id, state->local->tx_pos,
      (uint32_t)state->tx_data + (state->local_tx_pos & VCHIQ_SLOT_MASK),
      state->rx_pos,
      (uint32_t)state->rx_data + (state->rx_pos & VCHIQ_SLOT_MASK));
   vchiq_dump(dump_context, buf, len + 1);

   len = vcos_snprintf(buf, sizeof(buf),
      "  Version: %d (min %d)",
      VCHIQ_VERSION, VCHIQ_VERSION_MIN);
   vchiq_dump(dump_context, buf, len + 1);

   if (VCHIQ_ENABLE_STATS)
   {
      len = vcos_snprintf(buf, sizeof(buf),
         "  Stats: ctrl_tx_count=%d, ctrl_rx_count=%d, error_count=%d",
         state->stats.ctrl_tx_count, state->stats.ctrl_rx_count,
         state->stats.slot_stalls);
      vchiq_dump(dump_context, buf, len + 1);
   }

   len = vcos_snprintf(buf, sizeof(buf),
      "  Slots: %d available, %d recyclable, %d stalls",
      state->slot_queue_available - SLOT_QUEUE_INDEX_FROM_POS(state->local_tx_pos),
      state->local->slot_queue_recycle - state->slot_queue_available,
      state->stats.slot_stalls);
   vchiq_dump(dump_context, buf, len + 1);

   vchiq_dump_platform_state(dump_context);

   vchiq_dump_shared_state(dump_context, state, state->local, "Local");
   vchiq_dump_shared_state(dump_context, state, state->remote, "Remote");

   vchiq_dump_platform_instances(dump_context);

   for (i = 0; i < state->unused_service; i++) {
      VCHIQ_SERVICE_T *service = state->services[i];

      if (service && (service->srvstate != VCHIQ_SRVSTATE_FREE))
         vchiq_dump_service_state(dump_context, service);
   }
}

void
vchiq_dump_service_state(void *dump_context, VCHIQ_SERVICE_T *service)
{
   char buf[80];
   int len;

   len = vcos_snprintf(buf, sizeof(buf), "Service %d: %s",
      service->localport, srvstate_names[service->srvstate]);

   if (service->srvstate != VCHIQ_SRVSTATE_FREE)
   {
      char remoteport[30];
      VCHIQ_SERVICE_QUOTA_T *service_quota =
         &service->state->service_quotas[service->localport];
      int fourcc = service->base.fourcc;
      if (service->remoteport != VCHIQ_PORT_FREE)
      {
         int len2 = vcos_snprintf(remoteport, sizeof(remoteport), "%d",
            service->remoteport);
         if (service->public_fourcc != VCHIQ_FOURCC_INVALID)
            vcos_snprintf(remoteport + len2, sizeof(remoteport) - len2,
               " (client %x)", service->client_id);
      }
      else
         vcos_strcpy(remoteport, "n/a");

      len += vcos_snprintf(buf + len, sizeof(buf) - len,
         " '%c%c%c%c' remote %s (msg use %d/%d, slot use %d/%d)",
         VCHIQ_FOURCC_AS_4CHARS(fourcc),
         remoteport,
         service_quota->message_use_count,
         service_quota->message_quota,
         service_quota->slot_use_count,
         service_quota->slot_quota);

      if (VCHIQ_ENABLE_STATS)
      {
         vchiq_dump(dump_context, buf, len + 1);

         len = vcos_snprintf(buf, sizeof(buf),
            "  Ctrl: tx_count=%d, tx_bytes=%" PRIu64 ", rx_count=%d, rx_bytes=%" PRIu64,
            service->stats.ctrl_tx_count, service->stats.ctrl_tx_bytes,
            service->stats.ctrl_rx_count, service->stats.ctrl_rx_bytes);
         vchiq_dump(dump_context, buf, len + 1);

         len = vcos_snprintf(buf, sizeof(buf),
            "  Bulk: tx_count=%d, tx_bytes=%" PRIu64 ", rx_count=%d, rx_bytes=%" PRIu64,
            service->stats.bulk_tx_count, service->stats.bulk_tx_bytes,
            service->stats.bulk_rx_count, service->stats.bulk_rx_bytes);
         vchiq_dump(dump_context, buf, len + 1);

         len = vcos_snprintf(buf, sizeof(buf),
            "  %d quota stalls, %d slot stalls, %d bulk stalls, %d aborted, %d errors",
            service->stats.quota_stalls, service->stats.slot_stalls,
            service->stats.bulk_stalls, service->stats.bulk_aborted_count,
            service->stats.error_count);
       }
   }

   vchiq_dump(dump_context, buf, len + 1);

   vchiq_dump_platform_service_state(dump_context, service);
}


VCHIQ_STATUS_T vchiq_send_remote_use(VCHIQ_STATE_T * state)
{
   return queue_message(state, NULL, VCHIQ_MAKE_MSG(VCHIQ_MSG_REMOTE_USE, 0, 0), NULL, 0, 0, 0);
}

VCHIQ_STATUS_T vchiq_send_remote_release(VCHIQ_STATE_T * state)
{
   return queue_message(state, NULL, VCHIQ_MAKE_MSG(VCHIQ_MSG_REMOTE_RELEASE, 0, 0), NULL, 0, 0, 0);
}

VCHIQ_STATUS_T vchiq_send_remote_use_active(VCHIQ_STATE_T * state)
{
   return queue_message(state, NULL, VCHIQ_MAKE_MSG(VCHIQ_MSG_REMOTE_USE_ACTIVE, 0, 0), NULL, 0, 0, 0);
}
