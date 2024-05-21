// SPDX-License-Identifier: MIT
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/

#include "fw_operation.h"

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/bug.h>

typedef struct {
    u32 host_offset;
    u32 chip_offset;
} FW_DEBUG_BUFFER_HEADER_t;

#define DEBUG_BUFFER_DATA_SIZE              (DEBUG_BUFFER_TOTAL_SIZE - sizeof(FW_DEBUG_BUFFER_HEADER_t))
#define PCIE_D2H_NOTIFICATION_SRAM_OFFSET   (0x640 + 0x640)
#define PCIE_APP_CPU_DEBUG_OFFSET           (8*1024)
#define PCIE_CORE_CPU_DEBUG_OFFSET          (PCIE_APP_CPU_DEBUG_OFFSET + DEBUG_BUFFER_TOTAL_SIZE)

int hailo_read_firmware_notification(struct hailo_resource *resource, struct hailo_d2h_notification *notification)
{
    hailo_d2h_buffer_details_t d2h_buffer_details = {0, 0};
    hailo_resource_read_buffer(resource, 0, sizeof(d2h_buffer_details),
        &d2h_buffer_details);

    if ((sizeof(notification->buffer) < d2h_buffer_details.buffer_len) || (0 == d2h_buffer_details.is_buffer_in_use)) {
        return -EINVAL;
    }

    notification->buffer_len = d2h_buffer_details.buffer_len;
    hailo_resource_read_buffer(resource, sizeof(d2h_buffer_details), notification->buffer_len, notification->buffer);

    // Write is_buffer_in_use = false
    hailo_resource_write16(resource, 0, 0);
    return 0;
}

int hailo_pcie_read_firmware_notification(struct hailo_resource *resource,
    struct hailo_d2h_notification *notification)
{
    struct hailo_resource notification_resource;

    if (PCIE_D2H_NOTIFICATION_SRAM_OFFSET > resource->size) {
        return -EINVAL;
    }

    notification_resource.address = resource->address + PCIE_D2H_NOTIFICATION_SRAM_OFFSET,
    notification_resource.size = sizeof(struct hailo_d2h_notification);

    return hailo_read_firmware_notification(&notification_resource, notification);
}

static inline size_t calculate_log_ready_to_read(FW_DEBUG_BUFFER_HEADER_t *header)
{
    size_t ready_to_read = 0;
    size_t host_offset = header->host_offset;
    size_t chip_offset = header->chip_offset;

    if (chip_offset >= host_offset) {
        ready_to_read = chip_offset - host_offset;
    } else {
        ready_to_read = DEBUG_BUFFER_DATA_SIZE - (host_offset - chip_offset);
    }

    return ready_to_read;
}

long hailo_read_firmware_log(struct hailo_resource *fw_logger_resource, struct hailo_read_log_params *params)
{
    FW_DEBUG_BUFFER_HEADER_t debug_buffer_header = {0};
    size_t read_offset = 0;
    size_t ready_to_read = 0;
    size_t size_to_read = 0;
    uintptr_t user_buffer = (uintptr_t)params->buffer;

    if (params->buffer_size > ARRAY_SIZE(params->buffer)) {
        return -EINVAL;
    }

    hailo_resource_read_buffer(fw_logger_resource, 0, sizeof(debug_buffer_header),
        &debug_buffer_header);

    /* Point to the start of the data buffer. */
    ready_to_read = calculate_log_ready_to_read(&debug_buffer_header);
    if (0 == ready_to_read) {
        params->read_bytes = 0;
        return 0;
    }
    /* If ready to read is bigger than the buffer size, read only buffer size bytes. */
    ready_to_read = min(ready_to_read, params->buffer_size);
    
    /* Point to the data that is read to be read by the host. */
    read_offset = sizeof(debug_buffer_header) + debug_buffer_header.host_offset;
    /* Check if the offset should cycle back to beginning. */
    if (DEBUG_BUFFER_DATA_SIZE <= debug_buffer_header.host_offset + ready_to_read) {
        size_to_read = DEBUG_BUFFER_DATA_SIZE - debug_buffer_header.host_offset;
        hailo_resource_read_buffer(fw_logger_resource, read_offset, size_to_read, (void*)user_buffer);

        user_buffer += size_to_read;
        size_to_read = ready_to_read - size_to_read;
        /* Point back to the beginning of the data buffer. */
        read_offset -= debug_buffer_header.host_offset;
    }
    else {
        size_to_read = ready_to_read;
    }

    /* size_to_read may become 0 if the read reached DEBUG_BUFFER_DATA_SIZE exactly */
    hailo_resource_read_buffer(fw_logger_resource, read_offset, size_to_read, (void*)user_buffer);

    /* Change current_offset to represent the new host offset. */
    read_offset += size_to_read;
    hailo_resource_write32(fw_logger_resource, offsetof(FW_DEBUG_BUFFER_HEADER_t, host_offset),
        (u32)(read_offset - sizeof(debug_buffer_header)));
    
    params->read_bytes = ready_to_read;
    return 0;
}

long hailo_pcie_read_firmware_log(struct hailo_resource *resource, struct hailo_read_log_params *params)
{
    long err = 0;
    struct hailo_resource log_resource = {resource->address, DEBUG_BUFFER_TOTAL_SIZE};

    if (HAILO_CPU_ID_CPU0 == params->cpu_id) {
        log_resource.address += PCIE_APP_CPU_DEBUG_OFFSET;
    } else if (HAILO_CPU_ID_CPU1 == params->cpu_id) {
        log_resource.address += PCIE_CORE_CPU_DEBUG_OFFSET;
    } else {
        return -EINVAL;
    }

    if (0 == params->buffer_size) {
        params->read_bytes = 0;
        return 0;
    }

    err = hailo_read_firmware_log(&log_resource, params);
    if (0 != err) {
        return err;
    }

    return 0;
}