// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
 **/

#include <linux/version.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "pcie.h"
#include "utils.h"
#include "utils/logs.h"


void hailo_pcie_clear_notification_wait_list(struct hailo_pcie_board *pBoard, struct file *filp)
{
    struct hailo_notification_wait *cur = NULL, *next = NULL;
    list_for_each_entry_safe(cur, next, &pBoard->notification_wait_list, notification_wait_list) {
        if (cur->filp == filp) {
            list_del_rcu(&cur->notification_wait_list);
            synchronize_rcu();
            kfree(cur);
        }
    }
}
