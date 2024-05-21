// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
 **/

#include "hailo_resource.h"

#include <linux/io.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/kernel.h>


u8 hailo_resource_read8(struct hailo_resource *resource, size_t offset)
{
    return ioread8((u8*)resource->address + offset);
}

u16 hailo_resource_read16(struct hailo_resource *resource, size_t offset)
{
    return ioread16((u8*)resource->address + offset);
}

u32 hailo_resource_read32(struct hailo_resource *resource, size_t offset)
{
    return ioread32((u8*)resource->address + offset);
}

void hailo_resource_write8(struct hailo_resource *resource, size_t offset, u8 value)
{
    iowrite8(value, (u8*)resource->address + offset);
}

void hailo_resource_write16(struct hailo_resource *resource, size_t offset, u16 value)
{
    iowrite16(value, (u8*)resource->address + offset);
}

void hailo_resource_write32(struct hailo_resource *resource, size_t offset, u32 value)
{
    iowrite32(value, (u8*)resource->address + offset);
}

void hailo_resource_read_buffer(struct hailo_resource *resource, size_t offset, size_t count, void *to)
{
    // Copied and modified from linux aarch64 (using ioread32 instead of readq that does not work all the time)
    uintptr_t to_ptr = (uintptr_t)to;
    while ((count > 0) && (!IS_ALIGNED(to_ptr, 4) || !IS_ALIGNED((uintptr_t)resource->address + offset, 4))) {
        *(u8*)to_ptr = hailo_resource_read8(resource, offset);
        to_ptr++;
        offset++;
        count--;
    }

    while (count >= 4) {
        *(u32*)to_ptr = hailo_resource_read32(resource, offset);
        to_ptr += 4;
        offset += 4;
        count -= 4;
    }

    while (count > 0) {
        *(u8*)to_ptr = hailo_resource_read8(resource, offset);
        to_ptr++;
        offset++;
        count--;
    }
}

int hailo_resource_write_buffer(struct hailo_resource *resource, size_t offset, size_t count, const void *from)
{
    // read the bytes after writing them for flushing the data. This function also checks if the pcie link
    // is broken.
    uintptr_t from_ptr = (uintptr_t)from;
    while (count && (!IS_ALIGNED(resource->address + offset, 4) || !IS_ALIGNED(from_ptr, 4))) {
        hailo_resource_write8(resource, offset, *(u8*)from_ptr);
        if (hailo_resource_read8(resource, offset) != *(u8*)from_ptr) {
            return -EIO;
        }
        from_ptr++;
        offset++;
        count--;
    }

    while (count >= 4) {
        hailo_resource_write32(resource, offset, *(u32*)from_ptr);
        if (hailo_resource_read32(resource, offset) != *(u32*)from_ptr) {
            return -EIO;
        }
        from_ptr += 4;
        offset += 4;
        count -= 4;
    }

    while (count) {
        hailo_resource_write8(resource, offset, *(u8*)from_ptr);
         if (hailo_resource_read8(resource, offset) != *(u8*)from_ptr) {
            return -EIO;
        }
        from_ptr++;
        offset++;
        count--;
    }

    return 0;
}

int hailo_resource_transfer(struct hailo_resource *resource, struct hailo_memory_transfer_params *transfer)
{
    // Check for transfer size (address is in resources address-space)
    if ((transfer->address + transfer->count) > (u64)resource->size) {
        return -EINVAL;
    }

    if (transfer->count > ARRAY_SIZE(transfer->buffer)) {
        return -EINVAL;
    }

    switch (transfer->transfer_direction) {
    case TRANSFER_READ:
        hailo_resource_read_buffer(resource, (u32)transfer->address, transfer->count, transfer->buffer);
        return 0;
    case TRANSFER_WRITE:
        return hailo_resource_write_buffer(resource, (u32)transfer->address, transfer->count, transfer->buffer);
    default:
        return -EINVAL;
    }
}
