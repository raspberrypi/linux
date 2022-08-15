// SPDX-License-Identifier: GPL-2.0
/*
 * VideoCore Shared Memory CMA allocator
 *
 * Copyright: 2018, Raspberry Pi (Trading) Ltd
 * Copyright 2011-2012 Broadcom Corporation.  All rights reserved.
 *
 * Based on vmcs_sm driver from Broadcom Corporation.
 *
 */

/* ---- Include Files ----------------------------------------------------- */
#include <linux/completion.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "vc_sm_cma_vchi.h"

#define VC_SM_VER  1
#define VC_SM_MIN_VER 0

/* ---- Private Constants and Types -------------------------------------- */

/* Command blocks come from a pool */
#define SM_MAX_NUM_CMD_RSP_BLKS 32

/* The number of supported connections */
#define SM_MAX_NUM_CONNECTIONS 3

struct sm_cmd_rsp_blk {
	struct list_head head;	/* To create lists */
	/* To be signaled when the response is there */
	struct completion cmplt;

	u32 id;
	u16 length;

	u8 msg[VC_SM_MAX_MSG_LEN];

	uint32_t wait:1;
	uint32_t sent:1;
	uint32_t alloc:1;

};

struct sm_instance {
	u32 num_connections;
	unsigned int service_handle[SM_MAX_NUM_CONNECTIONS];
	struct task_struct *io_thread;
	struct completion io_cmplt;

	vpu_event_cb vpu_event;

	/* Mutex over the following lists */
	struct mutex lock;
	u32 trans_id;
	struct list_head cmd_list;
	struct list_head rsp_list;
	struct list_head dead_list;

	struct sm_cmd_rsp_blk free_blk[SM_MAX_NUM_CMD_RSP_BLKS];

	/* Mutex over the free_list */
	struct mutex free_lock;
	struct list_head free_list;

	struct semaphore free_sema;
	struct vchiq_instance *vchiq_instance;
};

/* ---- Private Variables ------------------------------------------------ */

/* ---- Private Function Prototypes -------------------------------------- */

/* ---- Private Functions ------------------------------------------------ */
static int
bcm2835_vchi_msg_queue(struct vchiq_instance *vchiq_instance, unsigned int handle,
		       void *data,
		       unsigned int size)
{
	return vchiq_queue_kernel_message(vchiq_instance, handle, data, size);
}

static struct
sm_cmd_rsp_blk *vc_vchi_cmd_create(struct sm_instance *instance,
				   enum vc_sm_msg_type id, void *msg,
				   u32 size, int wait)
{
	struct sm_cmd_rsp_blk *blk;
	struct vc_sm_msg_hdr_t *hdr;

	if (down_interruptible(&instance->free_sema)) {
		blk = kmalloc(sizeof(*blk), GFP_KERNEL);
		if (!blk)
			return NULL;

		blk->alloc = 1;
		init_completion(&blk->cmplt);
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

	hdr = (struct vc_sm_msg_hdr_t *)blk->msg;
	hdr->type = id;
	mutex_lock(&instance->lock);
	instance->trans_id++;
	/*
	 * Retain the top bit for identifying asynchronous events, or VPU cmds.
	 */
	instance->trans_id &= ~0x80000000;
	hdr->trans_id = instance->trans_id;
	blk->id = instance->trans_id;
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

static void vc_sm_cma_vchi_rx_ack(struct sm_instance *instance,
				  struct sm_cmd_rsp_blk *cmd,
				  struct vc_sm_result_t *reply,
				  u32 reply_len)
{
	mutex_lock(&instance->lock);
	list_for_each_entry(cmd,
			    &instance->rsp_list,
			    head) {
		if (cmd->id == reply->trans_id)
			break;
	}
	mutex_unlock(&instance->lock);

	if (&cmd->head == &instance->rsp_list) {
		//pr_debug("%s: received response %u, throw away...",
		pr_err("%s: received response %u, throw away...",
		       __func__,
		       reply->trans_id);
	} else if (reply_len > sizeof(cmd->msg)) {
		pr_err("%s: reply too big (%u) %u, throw away...",
		       __func__, reply_len,
		     reply->trans_id);
	} else {
		memcpy(cmd->msg, reply,
		       reply_len);
		complete(&cmd->cmplt);
	}
}

static int vc_sm_cma_vchi_videocore_io(void *arg)
{
	struct sm_instance *instance = arg;
	struct sm_cmd_rsp_blk *cmd = NULL, *cmd_tmp;
	struct vc_sm_result_t *reply;
	struct vchiq_header *header;
	s32 status;
	int svc_use = 1;

	while (1) {
		if (svc_use)
			vchiq_release_service(instance->vchiq_instance, instance->service_handle[0]);
		svc_use = 0;

		if (wait_for_completion_interruptible(&instance->io_cmplt))
			continue;
		vchiq_use_service(instance->vchiq_instance, instance->service_handle[0]);
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
			cmd = list_first_entry(&instance->cmd_list,
					       struct sm_cmd_rsp_blk, head);
			list_move(&cmd->head, &instance->rsp_list);
			cmd->sent = 1;
			mutex_unlock(&instance->lock);
			/* Send the command */
			status =
				bcm2835_vchi_msg_queue(instance->vchiq_instance,
						       instance->service_handle[0],
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
				complete(&cmd->cmplt);
				continue;
			}

		} while (1);

		while ((header = vchiq_msg_hold(instance->vchiq_instance,
						instance->service_handle[0]))) {
			reply = (struct vc_sm_result_t *)header->data;
			if (reply->trans_id & 0x80000000) {
				/* Async event or cmd from the VPU */
				if (instance->vpu_event)
					instance->vpu_event(instance, reply,
							    header->size);
			} else {
				vc_sm_cma_vchi_rx_ack(instance, cmd, reply,
						      header->size);
			}

			vchiq_release_message(instance->vchiq_instance,
					      instance->service_handle[0],
					      header);
		}

		/* Go through the dead list and free them */
		mutex_lock(&instance->lock);
		list_for_each_entry_safe(cmd, cmd_tmp, &instance->dead_list,
					 head) {
			list_del(&cmd->head);
			vc_vchi_cmd_delete(instance, cmd);
		}
		mutex_unlock(&instance->lock);
	}

	return 0;
}

static enum vchiq_status vc_sm_cma_vchi_callback(struct vchiq_instance *vchiq_instance,
						 enum vchiq_reason reason,
						 struct vchiq_header *header,
						 unsigned int handle, void *userdata)
{
	struct sm_instance *instance = vchiq_get_service_userdata(vchiq_instance, handle);

	switch (reason) {
	case VCHIQ_MESSAGE_AVAILABLE:
		vchiq_msg_queue_push(vchiq_instance, handle, header);
		complete(&instance->io_cmplt);
		break;

	case VCHIQ_SERVICE_CLOSED:
		pr_info("%s: service CLOSED!!", __func__);
		break;

	default:
		break;
	}

	return VCHIQ_SUCCESS;
}

struct sm_instance *vc_sm_cma_vchi_init(struct vchiq_instance *vchiq_instance,
					unsigned int num_connections,
					vpu_event_cb vpu_event)
{
	u32 i;
	struct sm_instance *instance;
	int status;

	pr_debug("%s: start", __func__);

	if (num_connections > SM_MAX_NUM_CONNECTIONS) {
		pr_err("%s: unsupported number of connections %u (max=%u)",
		       __func__, num_connections, SM_MAX_NUM_CONNECTIONS);

		goto err_null;
	}
	/* Allocate memory for this instance */
	instance = kzalloc(sizeof(*instance), GFP_KERNEL);

	/* Misc initialisations */
	mutex_init(&instance->lock);
	init_completion(&instance->io_cmplt);
	INIT_LIST_HEAD(&instance->cmd_list);
	INIT_LIST_HEAD(&instance->rsp_list);
	INIT_LIST_HEAD(&instance->dead_list);
	INIT_LIST_HEAD(&instance->free_list);
	sema_init(&instance->free_sema, SM_MAX_NUM_CMD_RSP_BLKS);
	mutex_init(&instance->free_lock);
	for (i = 0; i < SM_MAX_NUM_CMD_RSP_BLKS; i++) {
		init_completion(&instance->free_blk[i].cmplt);
		list_add(&instance->free_blk[i].head, &instance->free_list);
	}

	instance->vchiq_instance = vchiq_instance;

	/* Open the VCHI service connections */
	instance->num_connections = num_connections;
	for (i = 0; i < num_connections; i++) {
		struct vchiq_service_params_kernel params = {
			.version = VC_SM_VER,
			.version_min = VC_SM_MIN_VER,
			.fourcc = VCHIQ_MAKE_FOURCC('S', 'M', 'E', 'M'),
			.callback = vc_sm_cma_vchi_callback,
			.userdata = instance,
		};

		status = vchiq_open_service(vchiq_instance, &params,
					    &instance->service_handle[i]);
		if (status) {
			pr_err("%s: failed to open VCHI service (%d)",
			       __func__, status);

			goto err_close_services;
		}
	}
	/* Create the thread which takes care of all io to/from videoocore. */
	instance->io_thread = kthread_create(&vc_sm_cma_vchi_videocore_io,
					     (void *)instance, "SMIO");
	if (!instance->io_thread) {
		pr_err("%s: failed to create SMIO thread", __func__);

		goto err_close_services;
	}
	instance->vpu_event = vpu_event;
	set_user_nice(instance->io_thread, -10);
	wake_up_process(instance->io_thread);

	pr_debug("%s: success - instance %p", __func__, instance);
	return instance;

err_close_services:
	for (i = 0; i < instance->num_connections; i++) {
		if (instance->service_handle[i])
			vchiq_close_service(vchiq_instance, instance->service_handle[i]);
	}
	kfree(instance);
err_null:
	pr_debug("%s: FAILED", __func__);
	return NULL;
}

int vc_sm_cma_vchi_stop(struct vchiq_instance *vchiq_instance, struct sm_instance **handle)
{
	struct sm_instance *instance;
	u32 i;

	if (!handle) {
		pr_err("%s: invalid pointer to handle %p", __func__, handle);
		goto lock;
	}

	if (!*handle) {
		pr_err("%s: invalid handle %p", __func__, *handle);
		goto lock;
	}

	instance = *handle;

	/* Close all VCHI service connections */
	for (i = 0; i < instance->num_connections; i++) {
		vchiq_use_service(vchiq_instance, instance->service_handle[i]);
		vchiq_close_service(vchiq_instance, instance->service_handle[i]);
	}

	kfree(instance);

	*handle = NULL;
	return 0;

lock:
	return -EINVAL;
}

static int vc_sm_cma_vchi_send_msg(struct sm_instance *handle,
				   enum vc_sm_msg_type msg_id, void *msg,
				   u32 msg_size, void *result, u32 result_size,
				   u32 *cur_trans_id, u8 wait_reply)
{
	int status = 0;
	struct sm_instance *instance = handle;
	struct sm_cmd_rsp_blk *cmd_blk;

	if (!handle) {
		pr_err("%s: invalid handle", __func__);
		return -EINVAL;
	}
	if (!msg) {
		pr_err("%s: invalid msg pointer", __func__);
		return -EINVAL;
	}

	cmd_blk =
	    vc_vchi_cmd_create(instance, msg_id, msg, msg_size, wait_reply);
	if (!cmd_blk) {
		pr_err("[%s]: failed to allocate global tracking resource",
		       __func__);
		return -ENOMEM;
	}

	if (cur_trans_id)
		*cur_trans_id = cmd_blk->id;

	mutex_lock(&instance->lock);
	list_add_tail(&cmd_blk->head, &instance->cmd_list);
	mutex_unlock(&instance->lock);
	complete(&instance->io_cmplt);

	if (!wait_reply)
		/* We're done */
		return 0;

	/* Wait for the response */
	if (wait_for_completion_interruptible(&cmd_blk->cmplt)) {
		mutex_lock(&instance->lock);
		if (!cmd_blk->sent) {
			list_del(&cmd_blk->head);
			mutex_unlock(&instance->lock);
			vc_vchi_cmd_delete(instance, cmd_blk);
			return -ENXIO;
		}

		list_move(&cmd_blk->head, &instance->dead_list);
		mutex_unlock(&instance->lock);
		complete(&instance->io_cmplt);
		return -EINTR;	/* We're done */
	}

	if (result && result_size) {
		memcpy(result, cmd_blk->msg, result_size);
	} else {
		struct vc_sm_result_t *res =
			(struct vc_sm_result_t *)cmd_blk->msg;
		status = (res->success == 0) ? 0 : -ENXIO;
	}

	mutex_lock(&instance->lock);
	list_del(&cmd_blk->head);
	mutex_unlock(&instance->lock);
	vc_vchi_cmd_delete(instance, cmd_blk);
	return status;
}

int vc_sm_cma_vchi_free(struct sm_instance *handle, struct vc_sm_free_t *msg,
			u32 *cur_trans_id)
{
	return vc_sm_cma_vchi_send_msg(handle, VC_SM_MSG_TYPE_FREE,
				   msg, sizeof(*msg), 0, 0, cur_trans_id, 0);
}

int vc_sm_cma_vchi_import(struct sm_instance *handle, struct vc_sm_import *msg,
			  struct vc_sm_import_result *result, u32 *cur_trans_id)
{
	return vc_sm_cma_vchi_send_msg(handle, VC_SM_MSG_TYPE_IMPORT,
				   msg, sizeof(*msg), result, sizeof(*result),
				   cur_trans_id, 1);
}

int vc_sm_cma_vchi_client_version(struct sm_instance *handle,
				  struct vc_sm_version *msg,
				  struct vc_sm_result_t *result,
				  u32 *cur_trans_id)
{
	return vc_sm_cma_vchi_send_msg(handle, VC_SM_MSG_TYPE_CLIENT_VERSION,
				   //msg, sizeof(*msg), result, sizeof(*result),
				   //cur_trans_id, 1);
				   msg, sizeof(*msg), NULL, 0,
				   cur_trans_id, 0);
}

int vc_sm_vchi_client_vc_mem_req_reply(struct sm_instance *handle,
				       struct vc_sm_vc_mem_request_result *msg,
				       uint32_t *cur_trans_id)
{
	return vc_sm_cma_vchi_send_msg(handle,
				       VC_SM_MSG_TYPE_VC_MEM_REQUEST_REPLY,
				       msg, sizeof(*msg), 0, 0, cur_trans_id,
				       0);
}
