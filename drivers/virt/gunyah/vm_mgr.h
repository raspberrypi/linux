/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _GUNYAH_VM_MGR_PRIV_H
#define _GUNYAH_VM_MGR_PRIV_H

#include <linux/device.h>
#include <linux/rwsem.h>
#include <linux/wait.h>

#include <uapi/linux/gunyah.h>

#include "rsc_mgr.h"

long gunyah_dev_vm_mgr_ioctl(struct gunyah_rm *rm, unsigned int cmd,
			     unsigned long arg);

/**
 * struct gunyah_vm - Main representation of a Gunyah Virtual machine
 * @vmid: Gunyah's VMID for this virtual machine
 * @rm: Pointer to the resource manager struct to make RM calls
 * @parent: For logging
 * @nb: Notifier block for RM notifications
 * @vm_status: Current state of the VM, as last reported by RM
 * @vm_status_wait: Wait queue for status @vm_status changes
 * @status_lock: Serializing state transitions
 * @auth: Authentication mechanism to be used by resource manager when
 *        launching the VM
 *
 * Members are grouped by hot path.
 */
struct gunyah_vm {
	u16 vmid;
	struct gunyah_rm *rm;

	struct notifier_block nb;
	enum gunyah_rm_vm_status vm_status;
	wait_queue_head_t vm_status_wait;
	struct rw_semaphore status_lock;

	struct device *parent;
	enum gunyah_rm_vm_auth_mechanism auth;
};

#endif
