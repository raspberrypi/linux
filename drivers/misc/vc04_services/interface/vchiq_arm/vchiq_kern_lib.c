/*****************************************************************************
* Copyright 2001 - 2011 Broadcom Corporation.  All rights reserved.
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

/* ---- Include Files ---------------------------------------------------- */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include "vchiq_core.h"
#include "vchiq_arm.h"

/* ---- Public Variables ------------------------------------------------- */

/* ---- Private Constants and Types -------------------------------------- */

struct vchiq_instance_struct {
	VCHIQ_STATE_T *state;

	int connected;
};

/****************************************************************************
*
*   vchiq_initialise
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_initialise(VCHIQ_INSTANCE_T *instanceOut)
{
	VCHIQ_STATUS_T status = VCHIQ_ERROR;
	VCHIQ_STATE_T *state;
	VCHIQ_INSTANCE_T instance = NULL;

	vchiq_log_trace(vchiq_core_log_level, "%s called", __func__);

	state = vchiq_get_state();
	if (!state) {
		vchiq_log_error(vchiq_core_log_level,
			"%s: videocore not initialized\n", __func__);
		goto failed;
	}

	instance = kzalloc(sizeof(*instance), GFP_KERNEL);
	if (!instance) {
		vchiq_log_error(vchiq_core_log_level,
			"%s: error allocating vchiq instance\n", __func__);
		goto failed;
	}

	instance->connected = 0;
	instance->state = state;

	*instanceOut = instance;

	status = VCHIQ_SUCCESS;

failed:
	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p): returning %d", __func__, instance, status);

	return status;
}
EXPORT_SYMBOL(vchiq_initialise);

/****************************************************************************
*
*   vchiq_shutdown
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_shutdown(VCHIQ_INSTANCE_T instance)
{
	VCHIQ_STATUS_T status;
	VCHIQ_STATE_T *state = instance->state;

	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p) called", __func__, instance);

	if (mutex_lock_interruptible(&state->mutex) != 0)
		return VCHIQ_RETRY;

	/* Remove all services */
	status = vchiq_shutdown_internal(state, instance);

	mutex_unlock(&state->mutex);

	if (status == VCHIQ_SUCCESS)
		kfree(instance);

	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p): returning %d", __func__, instance, status);

	return status;
}
EXPORT_SYMBOL(vchiq_shutdown);

/****************************************************************************
*
*   vchiq_is_connected
*
***************************************************************************/

int vchiq_is_connected(VCHIQ_INSTANCE_T instance)
{
	return instance->connected;
}

/****************************************************************************
*
*   vchiq_connect
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_connect(VCHIQ_INSTANCE_T instance)
{
	VCHIQ_STATUS_T status;
	VCHIQ_STATE_T *state = instance->state;

	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p) called", __func__, instance);

	if (mutex_lock_interruptible(&state->mutex) != 0) {
		vchiq_log_trace(vchiq_core_log_level,
			"%s: call to mutex_lock failed", __func__);
		status = VCHIQ_RETRY;
		goto failed;
	}
	status = vchiq_connect_internal(state, instance);

	if (status == VCHIQ_SUCCESS)
		instance->connected = 1;

	mutex_unlock(&state->mutex);

failed:
	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p): returning %d", __func__, instance, status);

	return status;
}
EXPORT_SYMBOL(vchiq_connect);

/****************************************************************************
*
*   vchiq_add_service
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_add_service(
	VCHIQ_INSTANCE_T              instance,
	const VCHIQ_SERVICE_PARAMS_T *params,
	VCHIQ_SERVICE_HANDLE_T       *phandle)
{
	VCHIQ_STATUS_T status;
	VCHIQ_STATE_T *state = instance->state;
	VCHIQ_SERVICE_T *service = NULL;
	int srvstate;

	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p) called", __func__, instance);

	*phandle = VCHIQ_SERVICE_HANDLE_INVALID;

	srvstate = vchiq_is_connected(instance)
		? VCHIQ_SRVSTATE_LISTENING
		: VCHIQ_SRVSTATE_HIDDEN;

	service = vchiq_add_service_internal(
		state,
		params,
		srvstate,
		instance);

	if (service) {
		*phandle = service->handle;
		status = VCHIQ_SUCCESS;
	} else
		status = VCHIQ_ERROR;

	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p): returning %d", __func__, instance, status);

	return status;
}
EXPORT_SYMBOL(vchiq_add_service);

/****************************************************************************
*
*   vchiq_open_service
*
***************************************************************************/

VCHIQ_STATUS_T vchiq_open_service(
	VCHIQ_INSTANCE_T              instance,
	const VCHIQ_SERVICE_PARAMS_T *params,
	VCHIQ_SERVICE_HANDLE_T       *phandle)
{
	VCHIQ_STATUS_T   status = VCHIQ_ERROR;
	VCHIQ_STATE_T   *state = instance->state;
	VCHIQ_SERVICE_T *service = NULL;

	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p) called", __func__, instance);

	*phandle = VCHIQ_SERVICE_HANDLE_INVALID;

	if (!vchiq_is_connected(instance))
		goto failed;

	service = vchiq_add_service_internal(state,
		params,
		VCHIQ_SRVSTATE_OPENING,
		instance);

	if (service) {
		status = vchiq_open_service_internal(service, current->pid);
		if (status == VCHIQ_SUCCESS)
			*phandle = service->handle;
		else
			vchiq_remove_service(service->handle);
	}

failed:
	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p): returning %d", __func__, instance, status);

	return status;
}
EXPORT_SYMBOL(vchiq_open_service);
