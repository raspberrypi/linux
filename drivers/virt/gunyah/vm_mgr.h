/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _GUNYAH_VM_MGR_PRIV_H
#define _GUNYAH_VM_MGR_PRIV_H

#include <linux/device.h>

#include <uapi/linux/gunyah.h>

#include "rsc_mgr.h"

long gunyah_dev_vm_mgr_ioctl(struct gunyah_rm *rm, unsigned int cmd,
			     unsigned long arg);

/**
 * struct gunyah_vm - Main representation of a Gunyah Virtual machine
 * @rm: Pointer to the resource manager struct to make RM calls
 * @parent: For logging
 */
struct gunyah_vm {
	struct gunyah_rm *rm;
	struct device *parent;
};

#endif
