// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/

#include "integrated_nnc_utils.h"
#include "utils/logs.h"

#include <linux/uaccess.h>
#include <asm/io.h>
#include <linux/of_address.h>
#include <linux/cdev.h>

int hailo_ioremap_resource(struct platform_device *pdev, struct hailo_resource *resource,
    const char *name)
{
    void __iomem *address;
    struct resource *platform_resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
    if (NULL == platform_resource) {
        return -ENOENT;
    }

    address = devm_ioremap_resource(&pdev->dev, platform_resource);
    if (IS_ERR(address)) {
        return PTR_ERR(address);
    }

    resource->address = (uintptr_t)address;
    resource->size = resource_size(platform_resource);

    hailo_dev_dbg(&pdev->dev, "resource[%s]: remap %pr of %zx bytes to virtual start address %lx\n",
        platform_resource->name, platform_resource, resource->size, (uintptr_t)address);

    return 0;
}

// TODO: HRT-8475 - change to name instead of index
int hailo_ioremap_shmem(struct platform_device *pdev, int index, struct hailo_resource *resource)
{
    int ret;
    struct resource res;
    struct device_node *shmem;
    void __iomem * remap_ptr;

    shmem = of_parse_phandle(pdev->dev.of_node, "shmem", index);
    if (!shmem) {
        hailo_dev_err(&pdev->dev, "Failed to find shmem node index: %d in device tree\n", index);
        return -ENODEV;
    }

    ret = of_address_to_resource(shmem, 0, &res);
    if (ret) {
        hailo_dev_err(&pdev->dev, "hailo_ioremap_shmem, failed to get memory (index: %d)\n", index);
        of_node_put(shmem);
        return ret;
    }

    // Decrement the refcount of the node
    of_node_put(shmem);

    remap_ptr = devm_ioremap(&pdev->dev, res.start, resource_size(&res));
    if (!remap_ptr) {
        hailo_dev_err(&pdev->dev, "hailo_ioremap_shmem, failed to ioremap shmem (index: %d)\n", index);
        return -EADDRNOTAVAIL;
    }

    resource->address = (uintptr_t)remap_ptr;
    resource->size = resource_size(&res);

    return 0;
}

int direct_memory_transfer(struct platform_device *pdev, struct hailo_memory_transfer_params *params)
{
    int err = -EINVAL;
    void __iomem *mem = ioremap(params->address, params->count);
    if (NULL == mem) {
        hailo_dev_err(&pdev->dev, "Failed ioremap %llu %zu\n", params->address, params->count);
        return -ENOMEM;
    }

    switch (params->transfer_direction) {
    case TRANSFER_READ:
        memcpy_fromio(params->buffer, mem, params->count);
        err = 0;
        break;
    case TRANSFER_WRITE:
        memcpy_toio(mem, params->buffer, params->count);
        err = 0;
        break;
    default:
        hailo_dev_err(&pdev->dev, "Invalid transfer direction %d\n", (int)params->transfer_direction);
        err = -EINVAL;
    }

    iounmap(mem);
    return err;
}

int hailo_get_resource_physical_addr(struct platform_device *pdev, const char *name, u64 *address)
{
    struct resource *platform_resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
    if (NULL == platform_resource) {
        return -ENOENT;
    }

    *address = (u64)(platform_resource->start);
    return 0;
}