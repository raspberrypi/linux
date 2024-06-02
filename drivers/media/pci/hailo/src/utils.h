// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef _HAILO_PCI_UTILS_H_
#define _HAILO_PCI_UTILS_H_

#include <linux/version.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/pagemap.h>

#include "pcie.h"

void hailo_pcie_clear_notification_wait_list(struct hailo_pcie_board *pBoard, struct file *filp);

#endif /* _HAILO_PCI_UTILS_H_ */
