/*
 ****************************************************************************
 * Copyright 2011-2012 Broadcom Corporation.  All rights reserved.
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
 ****************************************************************************
 */

/* ---- Include Files ----------------------------------------------------- */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/kthread.h>

#include "vc_vchi_sm.h"

#define VC_SM_VER  1
#define VC_SM_MIN_VER 0

/* ---- Private Constants and Types -------------------------------------- */

/* Command blocks come from a pool */
#define SM_MAX_NUM_CMD_RSP_BLKS 32

struct sm_cmd_rsp_blk {
	struct list_head head;	/* To create lists */
	struct semaphore sema;	/* To be signaled when the response is there */

	uint16_t id;
	uint16_t length;

	uint8_t msg[VC_SM_MAX_MSG_LEN];

	uint32_t wait:1;
	uint32_t sent:1;
	uint32_t alloc:1;

};

struct sm_instance {
	uint32_t num_connections;
	VCHI_SERVICE_HANDLE_T vchi_handle[VCHI_MAX_NUM_CONNECTIONS];
	struct task_struct *io_thread;
	struct semaphore io_sema;

	uint32_t trans_id;

	struct mutex lock;
	struct list_head cmd_list;
	struct list_head rsp_list;
	struct list_head dead_list;

	struct sm_cmd_rsp_blk free_blk[SM_MAX_NUM_CMD_RSP_BLKS];
	struct list_head free_list;
	struct mutex free_lock;
	struct semaphore free_sema;

};

/* ---- Private Variables ------------------------------------------------ */

/* ---- Private Function Prototypes -------------------------------------- */

/* ---- Private Functions ------------------------------------------------ */
static int
bcm2835_vchi_msg_queue(VCHI_SERVICE_HANDLE_T handle,
		       void *data,
		       unsigned int size)
{
	return vchi_queue_kernel_message(handle,
					 data,
					 size);
}

static struct
sm_cmd_rsp_blk *vc_vchi_cmd_create(struct sm_instance *instance,
		enum vc_sm_msg_type id, void *msg,
		uint32_t size, int wait)
{
	struct sm_cmd_rsp_blk *blk;
	struct vc_sm_msg_hdr_t *hdr;

	if (down_interruptible(&instance->free_sema)) {
		blk = kmalloc(sizeof(*blk), GFP_KERNEL);
		if (!blk)
			return NULL;

		blk->alloc = 1;
		sema_init(&blk->sema, 0);
	} else {
		mutex_lock(&instance->free_lock);
		blk =
		    list_first_entry(&instance->free_list,
				    struct sm_cmd_rsp_blk, head);
		list_del(&blk->head);
		mutex_unlock(&instance->free_lock);
	}

	blk->sent = 0;
	blk->wait = wait;
	blk->length = sizeof(*hdr) + size;

	hdr = (struct vc_sm_msg_hdr_t *) blk->msg;
	hdr->type = id;
	mutex_lock(&instance->lock);
	hdr->trans_id = blk->id = ++instance->trans_id;
	mutex_unlock(&instance->lock);

	if (size)
		memcpy(hdr->body, msg, size);

	return blk;
}

static void
vc_vchi_cmd_delete(struct sm_instance *instance, struct sm_cmd_rsp_blk *blk)
{
	if (blk->alloc) {
		kfree(blk);
		return;
	}

	mutex_lock(&instance->free_lock);
	list_add(&blk->head, &instance->free_list);
	mutex_unlock(&instance->free_lock);
	up(&instance->free_sema);
}

static int vc_vchi_sm_videocore_io(void *arg)
{
	struct sm_instance *instance = arg;
	struct sm_cmd_rsp_blk *cmd = NULL, *cmd_tmp;
	struct vc_sm_result_t *reply;
	uint32_t reply_len;
	int32_t status;
	int svc_use = 1;

	while (1) {
		if (svc_use)
			vchi_service_release(instance->vchi_handle[0]);
		svc_use = 0;
		if (!down_interruptible(&instance->io_sema)) {
			vchi_service_use(instance->vchi_handle[0]);
			svc_use = 1;

			do {
				/*
				 * Get new command and move it to response list
				 */
				mutex_lock(&instance->lock);
				if (list_empty(&instance->cmd_list)) {
					/* no more commands to process */
					mutex_unlock(&instance->lock);
					break;
				}
				cmd =
				    list_first_entry(&instance->cmd_list,
						     struct sm_cmd_rsp_blk,
						     head);
				list_move(&cmd->head, &instance->rsp_list);
				cmd->sent = 1;
				mutex_unlock(&instance->lock);

				/* Send the command */
				status = bcm2835_vchi_msg_queue(
						instance->vchi_handle[0],
						cmd->msg, cmd->length);
				if (status) {
					pr_err("%s: failed to queue message (%d)",
					     __func__, status);
				}

				/* If no reply is needed then we're done */
				if (!cmd->wait) {
					mutex_lock(&instance->lock);
					list_del(&cmd->head);
					mutex_unlock(&instance->lock);
					vc_vchi_cmd_delete(instance, cmd);
					continue;
				}

				if (status) {
					up(&cmd->sema);
					continue;
				}

			} while (1);

			while (!vchi_msg_peek
			       (instance->vchi_handle[0], (void **)&reply,
				&reply_len, VCHI_FLAGS_NONE)) {
				mutex_lock(&instance->lock);
				list_for_each_entry(cmd, &instance->rsp_list,
						    head) {
					if (cmd->id == reply->trans_id)
						break;
				}
				mutex_unlock(&instance->lock);

				if (&cmd->head == &instance->rsp_list) {
					pr_debug("%s: received response %u, throw away...",
					     __func__, reply->trans_id);
				} else if (reply_len > sizeof(cmd->msg)) {
					pr_err("%s: reply too big (%u) %u, throw away...",
					     __func__, reply_len,
					     reply->trans_id);
				} else {
					memcpy(cmd->msg, reply, reply_len);
					up(&cmd->sema);
				}

				vchi_msg_remove(instance->vchi_handle[0]);
			}

			/* Go through the dead list and free them */
			mutex_lock(&instance->lock);
			list_for_each_entry_safe(cmd, cmd_tmp,
						 &instance->dead_list, head) {
				list_del(&cmd->head);
				vc_vchi_cmd_delete(instance, cmd);
			}
			mutex_unlock(&instance->lock);
		}
	}

	return 0;
}

static void vc_sm_vchi_callback(void *param,
				const VCHI_CALLBACK_REASON_T reason,
				void *msg_handle)
{
	struct sm_instance *instance = param;

	(void)msg_handle;

	switch (reason) {
	case VCHI_CALLBACK_MSG_AVAILABLE:
		up(&instance->io_sema);
		break;

	case VCHI_CALLBACK_SERVICE_CLOSED:
		pr_info("%s: service CLOSED!!", __func__);
	default:
		break;
	}
}

struct sm_instance *vc_vchi_sm_init(VCHI_INSTANCE_T vchi_instance,
				    VCHI_CONNECTION_T **vchi_connections,
				    uint32_t num_connections)
{
	uint32_t i;
	struct sm_instance *instance;
	int status;

	pr_debug("%s: start", __func__);

	if (num_connections > VCHI_MAX_NUM_CONNECTIONS) {
		pr_err("%s: unsupported number of connections %u (max=%u)",
			__func__, num_connections, VCHI_MAX_NUM_CONNECTIONS);

		goto err_null;
	}
	/* Allocate memory for this instance */
	instance = kzalloc(sizeof(*instance), GFP_KERNEL);

	/* Misc initialisations */
	mutex_init(&instance->lock);
	sema_init(&instance->io_sema, 0);
	INIT_LIST_HEAD(&instance->cmd_list);
	INIT_LIST_HEAD(&instance->rsp_list);
	INIT_LIST_HEAD(&instance->dead_list);
	INIT_LIST_HEAD(&instance->free_list);
	sema_init(&instance->free_sema, SM_MAX_NUM_CMD_RSP_BLKS);
	mutex_init(&instance->free_lock);
	for (i = 0; i < SM_MAX_NUM_CMD_RSP_BLKS; i++) {
		sema_init(&instance->free_blk[i].sema, 0);
		list_add(&instance->free_blk[i].head, &instance->free_list);
	}

	/* Open the VCHI service connections */
	instance->num_connections = num_connections;
	for (i = 0; i < num_connections; i++) {
		SERVICE_CREATION_T params = {
			VCHI_VERSION_EX(VC_SM_VER, VC_SM_MIN_VER),
			VC_SM_SERVER_NAME,
			vchi_connections[i],
			0,
			0,
			vc_sm_vchi_callback,
			instance,
			0,
			0,
			0,
		};

		status = vchi_service_open(vchi_instance,
					   &params, &instance->vchi_handle[i]);
		if (status) {
			pr_err("%s: failed to open VCHI service (%d)",
					__func__, status);

			goto err_close_services;
		}
	}

	/* Create the thread which takes care of all io to/from videoocore. */
	instance->io_thread = kthread_create(&vc_vchi_sm_videocore_io,
					     (void *)instance, "SMIO");
	if (instance->io_thread == NULL) {
		pr_err("%s: failed to create SMIO thread", __func__);

		goto err_close_services;
	}
	set_user_nice(instance->io_thread, -10);
	wake_up_process(instance->io_thread);

	pr_debug("%s: success - instance 0x%x", __func__,
		 (unsigned int)instance);
	return instance;

err_close_services:
	for (i = 0; i < instance->num_connections; i++) {
		if (instance->vchi_handle[i] != NULL)
			vchi_service_close(instance->vchi_handle[i]);
	}
	kfree(instance);
err_null:
	pr_debug("%s: FAILED", __func__);
	return NULL;
}

int vc_vchi_sm_stop(struct sm_instance **handle)
{
	struct sm_instance *instance;
	uint32_t i;

	if (handle == NULL) {
		pr_err("%s: invalid pointer to handle %p", __func__, handle);
		goto lock;
	}

	if (*handle == NULL) {
		pr_err("%s: invalid handle %p", __func__, *handle);
		goto lock;
	}

	instance = *handle;

	/* Close all VCHI service connections */
	for (i = 0; i < instance->num_connections; i++) {
		vchi_service_use(instance->vchi_handle[i]);

		vchi_service_close(instance->vchi_handle[i]);
	}

	kfree(instance);

	*handle = NULL;
	return 0;

lock:
	return -EINVAL;
}

static int vc_vchi_sm_send_msg(struct sm_instance *handle,
			enum vc_sm_msg_type msg_id,
			void *msg, uint32_t msg_size,
			void *result, uint32_t result_size,
			uint32_t *cur_trans_id, uint8_t wait_reply)
{
	int status = 0;
	struct sm_instance *instance = handle;
	struct sm_cmd_rsp_blk *cmd_blk;

	if (handle == NULL) {
		pr_err("%s: invalid handle", __func__);
		return -EINVAL;
	}
	if (msg == NULL) {
		pr_err("%s: invalid msg pointer", __func__);
		return -EINVAL;
	}

	cmd_blk =
	    vc_vchi_cmd_create(instance, msg_id, msg, msg_size, wait_reply);
	if (cmd_blk == NULL) {
		pr_err("[%s]: failed to allocate global tracking resource",
			__func__);
		return -ENOMEM;
	}

	if (cur_trans_id != NULL)
		*cur_trans_id = cmd_blk->id;

	mutex_lock(&instance->lock);
	list_add_tail(&cmd_blk->head, &instance->cmd_list);
	mutex_unlock(&instance->lock);
	up(&instance->io_sema);

	if (!wait_reply)
		/* We're done */
		return 0;

	/* Wait for the response */
	if (down_interruptible(&cmd_blk->sema)) {
		mutex_lock(&instance->lock);
		if (!cmd_blk->sent) {
			list_del(&cmd_blk->head);
			mutex_unlock(&instance->lock);
			vc_vchi_cmd_delete(instance, cmd_blk);
			return -ENXIO;
		}
		mutex_unlock(&instance->lock);

		mutex_lock(&instance->lock);
		list_move(&cmd_blk->head, &instance->dead_list);
		mutex_unlock(&instance->lock);
		up(&instance->io_sema);
		return -EINTR;	/* We're done */
	}

	if (result && result_size) {
		memcpy(result, cmd_blk->msg, result_size);
	} else {
		struct vc_sm_result_t *res =
			(struct vc_sm_result_t *) cmd_blk->msg;
		status = (res->success == 0) ? 0 : -ENXIO;
	}

	mutex_lock(&instance->lock);
	list_del(&cmd_blk->head);
	mutex_unlock(&instance->lock);
	vc_vchi_cmd_delete(instance, cmd_blk);
	return status;
}

int vc_vchi_sm_alloc(struct sm_instance *handle, struct vc_sm_alloc_t *msg,
		     struct vc_sm_alloc_result_t *result,
		     uint32_t *cur_trans_id)
{
	return vc_vchi_sm_send_msg(handle, VC_SM_MSG_TYPE_ALLOC,
				   msg, sizeof(*msg), result, sizeof(*result),
				   cur_trans_id, 1);
}

int vc_vchi_sm_free(struct sm_instance *handle,
		    struct vc_sm_free_t *msg, uint32_t *cur_trans_id)
{
	return vc_vchi_sm_send_msg(handle, VC_SM_MSG_TYPE_FREE,
				   msg, sizeof(*msg), 0, 0, cur_trans_id, 0);
}

int vc_vchi_sm_lock(struct sm_instance *handle,
		    struct vc_sm_lock_unlock_t *msg,
		    struct vc_sm_lock_result_t *result,
		    uint32_t *cur_trans_id)
{
	return vc_vchi_sm_send_msg(handle, VC_SM_MSG_TYPE_LOCK,
				   msg, sizeof(*msg), result, sizeof(*result),
				   cur_trans_id, 1);
}

int vc_vchi_sm_unlock(struct sm_instance *handle,
		      struct vc_sm_lock_unlock_t *msg,
		      uint32_t *cur_trans_id, uint8_t wait_reply)
{
	return vc_vchi_sm_send_msg(handle, wait_reply ?
				   VC_SM_MSG_TYPE_UNLOCK :
				   VC_SM_MSG_TYPE_UNLOCK_NOANS, msg,
				   sizeof(*msg), 0, 0, cur_trans_id,
				   wait_reply);
}

int vc_vchi_sm_resize(struct sm_instance *handle, struct vc_sm_resize_t *msg,
		      uint32_t *cur_trans_id)
{
	return vc_vchi_sm_send_msg(handle, VC_SM_MSG_TYPE_RESIZE,
				   msg, sizeof(*msg), 0, 0, cur_trans_id, 1);
}

int vc_vchi_sm_walk_alloc(struct sm_instance *handle)
{
	return vc_vchi_sm_send_msg(handle, VC_SM_MSG_TYPE_WALK_ALLOC,
				   0, 0, 0, 0, 0, 0);
}

int vc_vchi_sm_clean_up(struct sm_instance *handle,
			struct vc_sm_action_clean_t *msg)
{
	return vc_vchi_sm_send_msg(handle, VC_SM_MSG_TYPE_ACTION_CLEAN,
				   msg, sizeof(*msg), 0, 0, 0, 0);
}

int vc_vchi_sm_import(struct sm_instance *handle, struct vc_sm_import *msg,
		      struct vc_sm_import_result *result,
		      uint32_t *cur_trans_id)
{
	return vc_vchi_sm_send_msg(handle, VC_SM_MSG_TYPE_IMPORT,
				   msg, sizeof(*msg), result, sizeof(*result),
				   cur_trans_id, 1);
}
