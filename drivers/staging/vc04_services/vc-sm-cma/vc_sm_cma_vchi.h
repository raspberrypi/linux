/* SPDX-License-Identifier: GPL-2.0 */

/*
 * VideoCore Shared Memory CMA allocator
 *
 * Copyright: 2018, Raspberry Pi (Trading) Ltd
 * Copyright 2011-2012 Broadcom Corporation.  All rights reserved.
 *
 * Based on vmcs_sm driver from Broadcom Corporation.
 *
 */

#ifndef __VC_SM_CMA_VCHI_H__INCLUDED__
#define __VC_SM_CMA_VCHI_H__INCLUDED__

#include "interface/vchi/vchi.h"

#include "vc_sm_defs.h"

/*
 * Forward declare.
 */
struct sm_instance;

typedef void (*vpu_event_cb)(struct sm_instance *instance,
			     struct vc_sm_result_t *reply, int reply_len);

/*
 * Initialize the shared memory service, opens up vchi connection to talk to it.
 */
struct sm_instance *vc_sm_cma_vchi_init(VCHI_INSTANCE_T vchi_instance,
					unsigned int num_connections,
					vpu_event_cb vpu_event);

/*
 * Terminates the shared memory service.
 */
int vc_sm_cma_vchi_stop(struct sm_instance **handle);

/*
 * Ask the shared memory service to free up some memory that was previously
 * allocated by the vc_sm_cma_vchi_alloc function call.
 */
int vc_sm_cma_vchi_free(struct sm_instance *handle, struct vc_sm_free_t *msg,
			u32 *cur_trans_id);

/*
 * Import a contiguous block of memory and wrap it in a GPU MEM_HANDLE_T.
 */
int vc_sm_cma_vchi_import(struct sm_instance *handle, struct vc_sm_import *msg,
			  struct vc_sm_import_result *result,
			  u32 *cur_trans_id);

int vc_sm_cma_vchi_client_version(struct sm_instance *handle,
				  struct vc_sm_version *msg,
				  struct vc_sm_result_t *result,
				  u32 *cur_trans_id);

#endif /* __VC_SM_CMA_VCHI_H__INCLUDED__ */
