// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
 **/

#include "sysfs.h"
#include "pcie.h"

#include <linux/device.h>
#include <linux/sysfs.h>

static ssize_t board_location_show(struct device *dev, struct device_attribute *_attr,
    char *buf)
{
    struct hailo_pcie_board *board = (struct hailo_pcie_board *)dev_get_drvdata(dev);
    const char *dev_info = pci_name(board->pDev);
    return sprintf(buf, "%s", dev_info);
}
static DEVICE_ATTR_RO(board_location);

static ssize_t device_id_show(struct device *dev, struct device_attribute *_attr,
    char *buf)
{
    struct hailo_pcie_board *board = (struct hailo_pcie_board *)dev_get_drvdata(dev);
    return sprintf(buf, "%x:%x", board->pDev->vendor, board->pDev->device);
}
static DEVICE_ATTR_RO(device_id);

static ssize_t accelerator_type_show(struct device *dev, struct device_attribute *_attr,
    char *buf)
{
    struct hailo_pcie_board *board = (struct hailo_pcie_board *)dev_get_drvdata(dev);
    return sprintf(buf, "%d", board->pcie_resources.accelerator_type);
}
static DEVICE_ATTR_RO(accelerator_type);

static struct attribute *hailo_dev_attrs[] = {
    &dev_attr_board_location.attr,
    &dev_attr_device_id.attr,
    &dev_attr_accelerator_type.attr,
    NULL
};

ATTRIBUTE_GROUPS(hailo_dev);
const struct attribute_group **g_hailo_dev_groups = hailo_dev_groups;
