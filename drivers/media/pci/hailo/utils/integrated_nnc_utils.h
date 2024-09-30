// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef _INTEGRATED_NNC_UTILS_H_
#define _INTEGRATED_NNC_UTILS_H_

#include <linux/platform_device.h>
#include "hailo_resource.h"

#define HAILO15_CORE_CONTROL_MAILBOX_INDEX (0)
#define HAILO15_CORE_NOTIFICATION_MAILBOX_INDEX (1)
#define HAILO15_CORE_DRIVER_DOWN_MAILBOX_INDEX (2)

#define HAILO15_CORE_CONTROL_MAILBOX_TX_SHMEM_INDEX (0)
#define HAILO15_CORE_CONTROL_MAILBOX_RX_SHMEM_INDEX (1)
#define HAILO15_CORE_NOTIFICATION_MAILBOX_RX_SHMEM_INDEX (2)

int hailo_ioremap_resource(struct platform_device *pdev, struct hailo_resource *resource,
    const char *name);

// TODO: HRT-8475 - change to name instead of index
int hailo_ioremap_shmem(struct platform_device *pdev, int index, struct hailo_resource *resource);

int direct_memory_transfer(struct platform_device *pDev, struct hailo_memory_transfer_params *params);

int hailo_get_resource_physical_addr(struct platform_device *pdev, const char *name, u64 *address);

#endif /* _INTEGRATED_NNC_UTILS_H_ */
