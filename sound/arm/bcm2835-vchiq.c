/*****************************************************************************
* Copyright 2011 Broadcom Corporation.  All rights reserved.
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

#include <linux/device.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/syscalls.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/atomic.h>

#include "bcm2835.h"


// ---- Include Files --------------------------------------------------------

#include "interface/vchi/vchi.h"
#include "interface/vcos/vcos.h"
#include "interface/vcos/vcos_logging.h"
#include "vc_vchi_audioserv_defs.h"

// ---- Private Constants and Types ------------------------------------------

// VCOS logging category for this service
#define VCOS_LOG_CATEGORY (&audio_log_category)

// Default VCOS logging level
#define LOG_LEVEL  VCOS_LOG_TRACE

// Logging macros (for remapping to other logging mechanisms, i.e., printf)
#define LOG_ERR( fmt, arg... )   pr_err(fmt, ##arg) //vcos_log_error( fmt, ##arg )
#define LOG_WARN( fmt, arg... )  pr_err(fmt, ##arg) //vcos_log_warn( fmt, ##arg )
#define LOG_INFO( fmt, arg... )  pr_err(fmt, ##arg) //vcos_log_info( fmt, ##arg )
#define LOG_DBG( fmt, arg... )   pr_err( fmt, ##arg )

typedef struct opaque_AUDIO_INSTANCE_T {
	uint32_t num_connections;
	VCHI_SERVICE_HANDLE_T vchi_handle[VCHI_MAX_NUM_CONNECTIONS];
	VCOS_EVENT_T msg_avail_event;
	VCOS_MUTEX_T vchi_mutex;
	bcm2835_alsa_stream_t *alsa_stream;
	int32_t result, got_result;
	atomic_t callbacks_expected, callbacks_received;
} AUDIO_INSTANCE_T;

// ---- Private Variables ----------------------------------------------------

// VCOS logging category for this service
static VCOS_LOG_CAT_T audio_log_category;

// ---- Private Function Prototypes ------------------------------------------

// ---- Private Functions ----------------------------------------------------

static int bcm2835_audio_stop_worker(bcm2835_alsa_stream_t * alsa_stream);
static int bcm2835_audio_start_worker(bcm2835_alsa_stream_t * alsa_stream);

#if 1
typedef struct {
  struct work_struct my_work;
  bcm2835_alsa_stream_t *alsa_stream;
  int    x;
} my_work_t;

static void my_wq_function( struct work_struct *work)
{
  my_work_t *w = (my_work_t *)work;
  int ret=-9;
  audio_debug(" .. IN %p:%d\n", w->alsa_stream, w->x);  
  switch (w->x) {
    case 1: ret=bcm2835_audio_start_worker(w->alsa_stream); break;
    case 2: ret=bcm2835_audio_stop_worker(w->alsa_stream); break;
    default:  audio_error(" Unexpected work: %p:%d\n", w->alsa_stream, w->x); break;
  }
  kfree( (void *)work );
  audio_debug(" .. OUT %d\n", ret);  
}

int bcm2835_audio_start(bcm2835_alsa_stream_t *alsa_stream)
{
  int ret = -1;
  audio_debug(" .. IN\n");
  if (alsa_stream->my_wq) {
    my_work_t *work = kmalloc(sizeof(my_work_t), GFP_KERNEL);
    /* Queue some work (item 1) */
    if (work) {
      INIT_WORK( (struct work_struct *)work, my_wq_function );
      work->alsa_stream = alsa_stream;
      work->x = 1;
      if (queue_work( alsa_stream->my_wq, (struct work_struct *)work ))
        ret = 0;
    } else
      audio_error(" .. Error: NULL work kmalloc\n"); 
  }
  audio_debug(" .. OUT %d\n", ret);  
  return ret;
}

int bcm2835_audio_stop(bcm2835_alsa_stream_t * alsa_stream)
{
  int ret = -1;
  audio_debug(" .. IN\n");
  if (alsa_stream->my_wq) {
    my_work_t *work = kmalloc(sizeof(my_work_t), GFP_KERNEL);
    /* Queue some work (item 1) */
    if (work) {
      INIT_WORK( (struct work_struct *)work, my_wq_function );
      work->alsa_stream = alsa_stream;
      work->x = 2;
      if (queue_work( alsa_stream->my_wq, (struct work_struct *)work ))
        ret = 0;
    } else
      audio_error(" .. Error: NULL work kmalloc\n"); 
  }
  audio_debug(" .. OUT %d\n", ret);  
  return ret;
}

void my_workqueue_init(bcm2835_alsa_stream_t * alsa_stream)
{
  alsa_stream->my_wq = create_workqueue("my_queue");
}

void my_workqueue_quit(bcm2835_alsa_stream_t * alsa_stream)
{
  if (alsa_stream->my_wq) {
    flush_workqueue( alsa_stream->my_wq );
    destroy_workqueue( alsa_stream->my_wq );
    alsa_stream->my_wq = NULL;
  }
}

#else
static void *my_tasklet_data;

/* Bottom Half Function */
void my_tasklet_function( unsigned long data )
{
	int err = 0;
	bcm2835_alsa_stream_t *alsa_stream = (bcm2835_alsa_stream_t *)my_tasklet_data;
	audio_info("IN ..(%d)\n", (int)data);
	if (data)
		err = bcm2835_audio_stop_worker(alsa_stream);
	else
		err = bcm2835_audio_start_worker(alsa_stream);
	if (err != 0)
		audio_error(" Failed to START/STOP alsa device\n");
	audio_info("OUT ..\n");
}

DECLARE_TASKLET( my_tasklet_start, my_tasklet_function, 0);
DECLARE_TASKLET( my_tasklet_stop, my_tasklet_function, 1);


int bcm2835_audio_start(bcm2835_alsa_stream_t * alsa_stream)
{
				my_tasklet_data = alsa_stream;
				  tasklet_schedule( &my_tasklet_stop );

}
int bcm2835_audio_stop(bcm2835_alsa_stream_t * alsa_stream)
{
				my_tasklet_data = alsa_stream;
				  tasklet_schedule( &my_tasklet_start );
}
void my_workqueue_init(bcm2835_alsa_stream_t * alsa_stream){}
void my_workqueue_quit(bcm2835_alsa_stream_t * alsa_stream){}

#endif
static void audio_vchi_callback(void *param,
				const VCHI_CALLBACK_REASON_T reason,
				void *msg_handle)
{
	AUDIO_INSTANCE_T *instance = (AUDIO_INSTANCE_T *)param;
	int32_t status;
	int32_t msg_len;
	VC_AUDIO_MSG_T m;
	bcm2835_alsa_stream_t *alsa_stream = 0;
	audio_debug(" .. IN instance=%p, param=%p, reason=%d, handle=%p outstanding_completes=%d\n", instance, param, reason, msg_handle, atomic_read(&instance->callbacks_expected)-atomic_read(&instance->callbacks_received));

	if (!instance || reason != VCHI_CALLBACK_MSG_AVAILABLE) {
		return;
	}
	alsa_stream = instance->alsa_stream;
	status = vchi_msg_dequeue(instance->vchi_handle[0],
				   &m, sizeof m, &msg_len, VCHI_FLAGS_NONE);
	if (m.type == VC_AUDIO_MSG_TYPE_RESULT) {
		audio_debug(" .. instance=%p, m.type=VC_AUDIO_MSG_TYPE_RESULT, success=%d\n", instance, m.u.result.success);
		BUG_ON(instance->got_result);
		instance->result = m.u.result.success;
		instance->got_result = 1;
		vcos_event_signal(&instance->msg_avail_event);
	} else if (m.type == VC_AUDIO_MSG_TYPE_COMPLETE) {
		irq_handler_t callback = (irq_handler_t)m.u.complete.callback;
		audio_debug(" .. instance=%p, m.type=VC_AUDIO_MSG_TYPE_COMPLETE, complete=%d\n", instance, m.u.complete.count);
		if (alsa_stream && callback) {
			atomic_add(m.u.complete.count, &alsa_stream->retrieved);
			callback(0, alsa_stream);
		} else {
			audio_debug(" .. unexpected alsa_stream=%p, callback=%p\n", alsa_stream, callback);
		}
		atomic_inc(&instance->callbacks_received);
		//BUG_ON(atomic_read(&instance->callbacks_expected)-atomic_read(&instance->callbacks_received) < 0);
		vcos_event_signal(&instance->msg_avail_event);
	} else {
		audio_debug(" .. unexpected m.type=%d\n", m.type);
	}
}

static AUDIO_INSTANCE_T *vc_vchi_audio_init(VCHI_INSTANCE_T vchi_instance,
					  VCHI_CONNECTION_T ** vchi_connections,
					  uint32_t num_connections)
{
	uint32_t i;
	AUDIO_INSTANCE_T *instance;
	VCOS_STATUS_T status;

	LOG_DBG("%s: start", __func__);

	if (num_connections > VCHI_MAX_NUM_CONNECTIONS) {
		LOG_ERR("%s: unsupported number of connections %u (max=%u)",
			__func__, num_connections, VCHI_MAX_NUM_CONNECTIONS);

		return NULL;
	}
	// Allocate memory for this instance
	instance = vcos_malloc(sizeof(*instance), "audio_instance");
	memset(instance, 0, sizeof(*instance));

	instance->num_connections = num_connections;
	// Create the message available event
	status =
	    vcos_event_create(&instance->msg_avail_event, "audio_msg_avail");
	if (status != VCOS_SUCCESS) {
		LOG_ERR("%s: failed to create event (status=%d)", __func__,
			status);

		goto err_free_mem;
	}
	// Create a lock for exclusive, serialized VCHI connection access
	status = vcos_mutex_create(&instance->vchi_mutex, "audio_vchi_mutex");
	if (status != VCOS_SUCCESS) {
		LOG_ERR("%s: failed to create event (status=%d)", __func__,
			status);

		goto err_delete_event;
	}
	// Open the VCHI service connections
	for (i = 0; i < num_connections; i++) {
		SERVICE_CREATION_T params = {
			VC_AUDIO_SERVER_NAME,	// 4cc service code
			vchi_connections[i],	// passed in fn pointers
			0,	// rx fifo size (unused)
			0,	// tx fifo size (unused)
			audio_vchi_callback,	// service callback
			instance,	// service callback parameter
			VCOS_TRUE, //TODO: remove VCOS_FALSE,	// unaligned bulk recieves
			VCOS_TRUE, //TODO: remove VCOS_FALSE,	// unaligned bulk transmits
			VCOS_FALSE	// want crc check on bulk transfers
		};

		status = vchi_service_open(vchi_instance, &params,
					   &instance->vchi_handle[i]);
		if (status != VCOS_SUCCESS) {
			LOG_ERR
			    ("%s: failed to open VCHI service connection (status=%d)",
			     __func__, status);

			goto err_close_services;
		}
		// Finished with the service for now
		vchi_service_release(instance->vchi_handle[i]);
	}

	return instance;

err_close_services:
	for (i = 0; i < instance->num_connections; i++) {
		vchi_service_close(instance->vchi_handle[i]);
	}

	vcos_mutex_delete(&instance->vchi_mutex);

err_delete_event:
	vcos_event_delete(&instance->msg_avail_event);

err_free_mem:
	vcos_free(instance);

	return NULL;
}

static int32_t vc_vchi_audio_deinit(AUDIO_INSTANCE_T *instance)
{
	uint32_t i;

	audio_debug(" .. IN\n");

	if (instance == NULL) {
		LOG_ERR("%s: invalid handle %p", __func__, instance);

		return -1;
	}

	audio_debug(" .. about to lock (%d)\n", instance->num_connections);
	vcos_mutex_lock(&instance->vchi_mutex);

	// Close all VCHI service connections
	for (i = 0; i < instance->num_connections; i++) {
		int32_t success;
		audio_debug(" .. %i:closing %p\n", i, instance->vchi_handle[i]);
		vchi_service_use(instance->vchi_handle[i]);

		success = vchi_service_close(instance->vchi_handle[i]);
		if (success != 0) {
			LOG_ERR
			    ("%s: failed to close VCHI service connection (status=%d)",
			     __func__, success);
		}
	}

	vcos_mutex_unlock(&instance->vchi_mutex);

	vcos_mutex_delete(&instance->vchi_mutex);

	vcos_event_delete(&instance->msg_avail_event);

	vcos_free(instance);

	// Unregister the log category so we can add it back next time
	vcos_log_unregister(&audio_log_category);

	audio_debug(" .. OUT\n");

	return 0;
}

static int bcm2835_audio_open_connection(bcm2835_alsa_stream_t * alsa_stream)
{
	int ret = 0, err;
	static VCHI_INSTANCE_T vchi_instance;
	static VCHI_CONNECTION_T *vchi_connection;
	AUDIO_INSTANCE_T *instance = alsa_stream->instance;
	audio_debug(" .. IN\n");

	LOG_INFO("%s: start", __func__);
	//BUG_ON(instance);
	if (instance) {
		LOG_ERR("%s: VCHI instance already open (%p)",
			__func__, instance);
		//BUG_ON(atomic_read(&instance->callbacks_expected)-atomic_read(&instance->callbacks_received) < 0);
		instance->alsa_stream = alsa_stream;
		alsa_stream->instance = instance;
		ret = 0; // xxx todo -1;
		goto err_free_mem;
	}

	// Initialize and create a VCHI connection
	ret = vchi_initialise(&vchi_instance);
	if (ret != 0) {
		LOG_ERR("%s: failed to initialise VCHI instance (ret=%d)",
			__func__, ret);

		ret = -EIO;
		goto err_free_mem;
	}
	ret = vchi_connect(NULL, 0, vchi_instance);
	if (ret != 0) {
		LOG_ERR("%s: failed to connect VCHI instance (ret=%d)",
			__func__, ret);

		ret = -EIO;
		goto err_free_mem;
	}

	// Set up the VCOS logging
	vcos_log_set_level(VCOS_LOG_CATEGORY, LOG_LEVEL);
	vcos_log_register("audio", VCOS_LOG_CATEGORY);


	// Initialize an instance of the audio service
	instance = vc_vchi_audio_init(vchi_instance, &vchi_connection, 1);

	if (instance == NULL /*|| audio_handle != instance*/) {
		LOG_ERR("%s: failed to initialize audio service",
			__func__);

		ret = -EPERM;
		goto err_free_mem;
	}

	instance->alsa_stream = alsa_stream;
	alsa_stream->instance = instance;


	audio_debug(" success !\n");
err_free_mem:
	audio_debug(" .. OUT\n");

	return ret;
}

int bcm2835_audio_open(bcm2835_alsa_stream_t * alsa_stream)
{
	AUDIO_INSTANCE_T *instance;
	VC_AUDIO_MSG_T m;
	int32_t success;
	int ret;
	audio_debug(" .. IN\n");

	my_workqueue_init(alsa_stream);

        ret = bcm2835_audio_open_connection(alsa_stream);
	if (ret != 0) {
		ret = -1;
		goto exit;
	}
	instance = alsa_stream->instance;

	vcos_mutex_lock(&instance->vchi_mutex);
	vchi_service_use(instance->vchi_handle[0]);

	m.type = VC_AUDIO_MSG_TYPE_OPEN;

	// Send the message to the videocore
	success = vchi_msg_queue(instance->vchi_handle[0],
				 &m, sizeof m,
				 VCHI_FLAGS_BLOCK_UNTIL_QUEUED, NULL);

	if (success != 0) {
		LOG_ERR("%s: failed on vchi_msg_queue (status=%d)",
			__func__, success);

		ret = -1;
		goto unlock;
	}

	ret = 0;

unlock:
	vchi_service_release(instance->vchi_handle[0]);
	vcos_mutex_unlock(&instance->vchi_mutex);
exit:
	audio_debug(" .. OUT\n");
	return ret;
}


int bcm2835_audio_set_params(bcm2835_alsa_stream_t * alsa_stream,
			     uint32_t channels, uint32_t samplerate,
			     uint32_t bps)
{
	VC_AUDIO_MSG_T m;
	AUDIO_INSTANCE_T *instance = alsa_stream->instance;
	int32_t success;
	uint32_t msg_len;
	int ret;
	audio_debug(" .. IN\n");

	if (channels < 1 || channels > 2) {
		audio_error(" channels (%d) not supported\n", channels);
		return -EINVAL;
	}

	if (samplerate < 8000  || samplerate > 48000) {
		audio_error(" samplerate (%d) not supported\n", samplerate);
		return -EINVAL;
	}

	if (bps != 8 && bps != 16) {
		audio_error(" Bits per sample (%d) not supported\n", bps);
		return -EINVAL;
	}

	audio_info
	    (" Setting ALSA channels(%d), samplerate(%d), bits-per-sample(%d)\n",
	     channels, samplerate, bps);

	vcos_mutex_lock(&instance->vchi_mutex);
	vchi_service_use(instance->vchi_handle[0]);

	instance->got_result = 0;
	instance->result = -1;

	m.type = VC_AUDIO_MSG_TYPE_CONFIG;
	m.u.config.channels = channels;
	m.u.config.samplerate = samplerate;
	m.u.config.bps = bps;

	// Send the message to the videocore
	success = vchi_msg_queue(instance->vchi_handle[0],
				 &m, sizeof m,
				 VCHI_FLAGS_BLOCK_UNTIL_QUEUED, NULL);

	if (success != 0) {
		LOG_ERR("%s: failed on vchi_msg_queue (status=%d)",
			__func__, success);

		ret = -1;
		goto unlock;
	}

	// We are expecting a reply from the videocore
	while (!instance->got_result) {
		success = vcos_event_wait(&instance->msg_avail_event);
		if (success != VCOS_SUCCESS) {
			LOG_ERR("%s: failed on waiting for event (status=%d)",
				__func__, success);

			ret = -1;
			goto unlock;
		}
	}

	if (instance->result != 0) {
		LOG_ERR("%s: result=%d",
			__func__, instance->result);

		ret = -1;
		goto unlock;
	}

	ret = 0;

unlock:
	vchi_service_release(instance->vchi_handle[0]);
	vcos_mutex_unlock(&instance->vchi_mutex);

	audio_debug(" .. OUT\n");
	return ret;
}

int bcm2835_audio_setup(bcm2835_alsa_stream_t * alsa_stream)
{
	audio_debug(" .. IN\n");

	audio_debug(" .. OUT\n");

	return 0;
}


static int bcm2835_audio_start_worker(bcm2835_alsa_stream_t * alsa_stream)
{
	VC_AUDIO_MSG_T m;
	AUDIO_INSTANCE_T *instance = alsa_stream->instance;
	int32_t success;
	int ret;
	audio_debug(" .. IN\n");

	vcos_mutex_lock(&instance->vchi_mutex);
	vchi_service_use(instance->vchi_handle[0]);

	m.type = VC_AUDIO_MSG_TYPE_START;

	// Send the message to the videocore
	success = vchi_msg_queue(instance->vchi_handle[0],
				 &m, sizeof m,
				 VCHI_FLAGS_BLOCK_UNTIL_QUEUED, NULL);

	if (success != 0) {
		LOG_ERR("%s: failed on vchi_msg_queue (status=%d)",
			__func__, success);

		ret = -1;
		goto unlock;
	}

	ret = 0;

unlock:
	vchi_service_release(instance->vchi_handle[0]);
	vcos_mutex_unlock(&instance->vchi_mutex);
	audio_debug(" .. OUT\n");
	return ret;
}


static int bcm2835_audio_stop_worker(bcm2835_alsa_stream_t * alsa_stream)
{
	VC_AUDIO_MSG_T m;
	AUDIO_INSTANCE_T *instance = alsa_stream->instance;
	int32_t success;
	int ret;
	audio_debug(" .. IN\n");

	vcos_mutex_lock(&instance->vchi_mutex);
	vchi_service_use(instance->vchi_handle[0]);

	m.type = VC_AUDIO_MSG_TYPE_STOP;
	m.u.stop.draining = alsa_stream->draining;

	// Send the message to the videocore
	success = vchi_msg_queue(instance->vchi_handle[0],
				 &m, sizeof m,
				 VCHI_FLAGS_BLOCK_UNTIL_QUEUED, NULL);

	if (success != 0) {
		LOG_ERR("%s: failed on vchi_msg_queue (status=%d)",
			__func__, success);

		ret = -1;
		goto unlock;
	}

	ret = 0;

unlock:
	vchi_service_release(instance->vchi_handle[0]);
	vcos_mutex_unlock(&instance->vchi_mutex);
	audio_debug(" .. OUT\n");
	return ret;
}

int bcm2835_audio_close(bcm2835_alsa_stream_t * alsa_stream)
{
	VC_AUDIO_MSG_T m;
	AUDIO_INSTANCE_T *instance = alsa_stream->instance;
	int32_t success;
	int ret;
	audio_debug(" .. IN outstanding_completes=%d\n", atomic_read(&instance->callbacks_expected)-atomic_read(&instance->callbacks_received));

	my_workqueue_quit(alsa_stream);

	vcos_mutex_lock(&instance->vchi_mutex);
	vchi_service_use(instance->vchi_handle[0]);

	m.type = VC_AUDIO_MSG_TYPE_CLOSE;
	instance->got_result = 0;
	// Send the message to the videocore
	success = vchi_msg_queue(instance->vchi_handle[0],
				 &m, sizeof m,
				 VCHI_FLAGS_BLOCK_UNTIL_QUEUED, NULL);

	if (success != 0) {
		LOG_ERR("%s: failed on vchi_msg_queue (status=%d)",
			__func__, success);

		ret = -1;
		goto unlock;
	}
	while (!instance->got_result /*|| atomic_read(&instance->callbacks_expected)-atomic_read(&instance->callbacks_received) < 0*/ ) {
		success = vcos_event_wait(&instance->msg_avail_event);
		if (success != VCOS_SUCCESS) {
			LOG_ERR("%s: failed on waiting for event (status=%d)",
				__func__, success);

			ret = -1;
			goto unlock;
		}
	}
	if (instance->result != 0) {
		LOG_ERR("%s: failed result (status=%d)",
			__func__, instance->result);

		ret = -1;
		goto unlock;
	}

	ret = 0;

unlock:
	vchi_service_release(instance->vchi_handle[0]);
	vcos_mutex_unlock(&instance->vchi_mutex);

	// Stop the audio service
	if (instance) {
		vc_vchi_audio_deinit(instance);
		alsa_stream->instance = NULL;
	}
	audio_debug(" .. OUT\n");
	return ret;
}

static int bcm2835_audio_set_ctls_chan(bcm2835_alsa_stream_t *alsa_stream, bcm2835_chip_t *chip)
{
	VC_AUDIO_MSG_T m;
	AUDIO_INSTANCE_T *instance = alsa_stream->instance;
	int32_t success;
	uint32_t msg_len;
	int ret;
	audio_debug(" .. IN\n");

	audio_info
	    (" Setting ALSA dest(%d), volume(%d)\n", chip->dest, chip->volume);

	vcos_mutex_lock(&instance->vchi_mutex);
	vchi_service_use(instance->vchi_handle[0]);

	instance->got_result = 0;
	instance->result = -1;

	m.type = VC_AUDIO_MSG_TYPE_CONTROL;
	m.u.control.dest = chip->dest;
	m.u.control.volume = chip->volume;

	// Send the message to the videocore
	success = vchi_msg_queue(instance->vchi_handle[0],
				 &m, sizeof m,
				 VCHI_FLAGS_BLOCK_UNTIL_QUEUED, NULL);

	if (success != 0) {
		LOG_ERR("%s: failed on vchi_msg_queue (status=%d)",
			__func__, success);

		ret = -1;
		goto unlock;
	}

	// We are expecting a reply from the videocore
	while (!instance->got_result) {
		success = vcos_event_wait(&instance->msg_avail_event);
		if (success != VCOS_SUCCESS) {
			LOG_ERR("%s: failed on waiting for event (status=%d)",
				__func__, success);

			ret = -1;
			goto unlock;
		}
	}

	if (instance->result != 0) {
		LOG_ERR("%s: result=%d",
			__func__, instance->result);

		ret = -1;
		goto unlock;
	}

	ret = 0;

unlock:
	vchi_service_release(instance->vchi_handle[0]);
	vcos_mutex_unlock(&instance->vchi_mutex);

	audio_debug(" .. OUT\n");
	return ret;
}


int bcm2835_audio_set_ctls(bcm2835_chip_t *chip)
{
	int i;
	int ret = 0;
	audio_debug(" .. IN\n");

	/* change ctls for all substreams */
	for (i = 0; i < MAX_SUBSTREAMS; i++) {
		if (chip->avail_substreams & (1 << i)) {
			if (!chip->alsa_stream[i])
				ret = -1;			
			else if (bcm2835_audio_set_ctls_chan(chip->alsa_stream[i], chip) != 0)
				ret = -1;			
		}
	}
	audio_debug(" .. OUT ret=%d\n", ret);
	return ret;
}

int bcm2835_audio_write(bcm2835_alsa_stream_t * alsa_stream, uint32_t count,
			void *src)
{
	VC_AUDIO_MSG_T m;
	AUDIO_INSTANCE_T *instance = alsa_stream->instance;
	int32_t success;
	int ret;

	audio_debug(" .. IN outstanding=%d\n", atomic_read(&instance->callbacks_expected)-atomic_read(&instance->callbacks_received));

	audio_info
	    (" Writing %d bytes from %p\n", count, src);

	vcos_mutex_lock(&instance->vchi_mutex);
	vchi_service_use(instance->vchi_handle[0]);

	m.type = VC_AUDIO_MSG_TYPE_WRITE;
	m.u.write.count = count;
	m.u.write.callback = alsa_stream->fifo_irq_handler;
	m.u.write.cookie = alsa_stream;
	m.u.write.silence = src == NULL;

	atomic_add(1, &instance->callbacks_expected);
	// Send the message to the videocore
	success = vchi_msg_queue(instance->vchi_handle[0],
				 &m, sizeof m,
				 VCHI_FLAGS_BLOCK_UNTIL_QUEUED, NULL);

	if (success != 0) {
		LOG_ERR("%s: failed on vchi_msg_queue (status=%d)",
			__func__, success);

		ret = -1;
		goto unlock;
	}
        audio_debug(" ... send header\n");

	// Send the message to the videocore
	success = vchi_bulk_queue_transmit(instance->vchi_handle[0],
				 src, count, 0*VCHI_FLAGS_BLOCK_UNTIL_QUEUED + 1*VCHI_FLAGS_BLOCK_UNTIL_DATA_READ, NULL);
	if (success != 0) {
		LOG_ERR("%s: failed on vchi_msg_queue (status=%d)",
			__func__, success);

		ret = -1;
		goto unlock;
	}
	ret = 0;

unlock:
	if (ret != 0) {
		atomic_dec(&instance->callbacks_expected);
	}
	vchi_service_release(instance->vchi_handle[0]);
	vcos_mutex_unlock(&instance->vchi_mutex);
	audio_debug(" .. OUT\n");
	return ret;
}

/**
  * Returns all buffers from arm->vc
  */
void bcm2835_audio_flush_buffers(bcm2835_alsa_stream_t * alsa_stream)
{
	audio_debug(" .. IN\n");
	audio_debug(" .. OUT\n");
	return;
}

/**
  * Forces VC to flush(drop) its filled playback buffers and 
  * return them the us. (VC->ARM)
  */
void bcm2835_audio_flush_playback_buffers(bcm2835_alsa_stream_t * alsa_stream)
{
	audio_debug(" .. IN\n");
	audio_debug(" .. OUT\n");
}

uint32_t bcm2835_audio_retrieve_buffers(bcm2835_alsa_stream_t *alsa_stream)
{
	uint32_t count = atomic_read(&alsa_stream->retrieved);
	atomic_sub(count, &alsa_stream->retrieved);
	return count;
}

