// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
 **/

#include "ioctl.h"
#include "memory.h"
#include "utils/logs.h"
#include "utils.h"

#include <linux/slab.h>
#include <linux/uaccess.h>


long hailo_vdma_enable_channels_ioctl(struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_vdma_enable_channels_params input;
    struct hailo_vdma_engine *engine = NULL;
    u8 engine_index = 0;
    u32 channels_bitmap = 0;

    if (copy_from_user(&input, (void *)arg, sizeof(input))) {
        hailo_dev_err(controller->dev, "copy_from_user fail\n");
        return -ENOMEM;
    }

    // Validate params (ignoring engine_index >= controller->vdma_engines_count).
    for_each_vdma_engine(controller, engine, engine_index) {
        channels_bitmap = input.channels_bitmap_per_engine[engine_index];
        if (0 != (channels_bitmap & engine->enabled_channels)) {
            hailo_dev_err(controller->dev, "Trying to enable channels that are already enabled\n");
            return -EINVAL;
        }
    }

    for_each_vdma_engine(controller, engine, engine_index) {
        channels_bitmap = input.channels_bitmap_per_engine[engine_index];
        hailo_vdma_engine_enable_channels(engine, channels_bitmap,
            input.enable_timestamps_measure);
        hailo_vdma_update_interrupts_mask(controller, engine_index);
        hailo_dev_info(controller->dev, "Enabled interrupts for engine %u, channels bitmap 0x%x\n",
            engine_index, channels_bitmap);
    }

    return 0;
}

long hailo_vdma_disable_channels_ioctl(struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_vdma_disable_channels_params input;
    struct hailo_vdma_engine *engine = NULL;
    u8 engine_index = 0;
    u32 channels_bitmap = 0;
    unsigned long irq_saved_flags = 0;

    if (copy_from_user(&input, (void*)arg, sizeof(input))) {
        hailo_dev_err(controller->dev, "copy_from_user fail\n");
        return -ENOMEM;
    }

    // Validate params (ignoring engine_index >= controller->vdma_engines_count).
    for_each_vdma_engine(controller, engine, engine_index) {
        channels_bitmap = input.channels_bitmap_per_engine[engine_index];
        if (channels_bitmap != (channels_bitmap & engine->enabled_channels)) {
            hailo_dev_warn(controller->dev, "Trying to disable channels that were not enabled\n");
        }
    }

    for_each_vdma_engine(controller, engine, engine_index) {
        channels_bitmap = input.channels_bitmap_per_engine[engine_index];
        hailo_vdma_engine_disable_channels(engine, channels_bitmap);
        hailo_vdma_update_interrupts_mask(controller, engine_index);

        spin_lock_irqsave(&controller->interrupts_lock, irq_saved_flags);
        hailo_vdma_engine_clear_channel_interrupts(engine, channels_bitmap);
        spin_unlock_irqrestore(&controller->interrupts_lock, irq_saved_flags);

        hailo_dev_info(controller->dev, "Disabled channels for engine %u, bitmap 0x%x\n",
            engine_index, channels_bitmap);
    }

    // Wake up threads waiting
    wake_up_interruptible_all(&controller->interrupts_wq);

    return 0;
}

static bool got_interrupt(struct hailo_vdma_controller *controller,
    u32 channels_bitmap_per_engine[MAX_VDMA_ENGINES])
{
    struct hailo_vdma_engine *engine = NULL;
    u8 engine_index = 0;
    for_each_vdma_engine(controller, engine, engine_index) {
        if (hailo_vdma_engine_got_interrupt(engine,
                channels_bitmap_per_engine[engine_index])) {
            return true;
        }
    }
    return false;
}

static void transfer_done(struct hailo_ongoing_transfer *transfer, void *opaque)
{
    u8 i = 0;
    struct hailo_vdma_controller *controller = (struct hailo_vdma_controller *)opaque;
    for (i = 0; i < transfer->buffers_count; i++) {
        struct hailo_vdma_buffer *mapped_buffer = (struct hailo_vdma_buffer *)transfer->buffers[i].opaque;
        hailo_vdma_buffer_sync_cyclic(controller, mapped_buffer, HAILO_SYNC_FOR_CPU,
            transfer->buffers[i].offset, transfer->buffers[i].size);
    }
}

long hailo_vdma_interrupts_wait_ioctl(struct hailo_vdma_controller *controller, unsigned long arg,
    struct semaphore *mutex, bool *should_up_board_mutex)
{
    long err = 0;
    struct hailo_vdma_interrupts_wait_params params = {0};
    struct hailo_vdma_engine *engine = NULL;
    bool bitmap_not_empty = false;
    u8 engine_index = 0;
    u32 irq_bitmap = 0;
    unsigned long irq_saved_flags = 0;

    if (copy_from_user(&params, (void*)arg, sizeof(params))) {
        hailo_dev_err(controller->dev, "HAILO_VDMA_INTERRUPTS_WAIT, copy_from_user fail\n");
        return -ENOMEM;
    }

    // We don't need to validate that channels_bitmap_per_engine are enabled -
    // If the channel is not enabled we just return an empty interrupts list.

    // Validate params (ignoring engine_index >= controller->vdma_engines_count).
    // It us ok to wait on a disabled channel - the wait will just exit.
    for_each_vdma_engine(controller, engine, engine_index) {
        if (0 != params.channels_bitmap_per_engine[engine_index]) {
            bitmap_not_empty = true;
        }
    }
    if (!bitmap_not_empty) {
        hailo_dev_err(controller->dev, "Got an empty bitmap for wait interrupts\n");
        return -EINVAL;
    }

    up(mutex);
    err = wait_event_interruptible(controller->interrupts_wq,
        got_interrupt(controller, params.channels_bitmap_per_engine));
    if (err < 0) {
        hailo_dev_info(controller->dev,
            "wait channel interrupts failed with err=%ld (process was interrupted or killed)\n", err);
        *should_up_board_mutex = false;
        return err;
    }

    if (down_interruptible(mutex)) {
        hailo_dev_info(controller->dev, "down_interruptible error (process was interrupted or killed)\n");
        *should_up_board_mutex = false;
        return -ERESTARTSYS;
    }

    params.channels_count = 0;
    for_each_vdma_engine(controller, engine, engine_index) {

        spin_lock_irqsave(&controller->interrupts_lock, irq_saved_flags);
        irq_bitmap = hailo_vdma_engine_read_interrupts(engine,
            params.channels_bitmap_per_engine[engine->index]);
        spin_unlock_irqrestore(&controller->interrupts_lock, irq_saved_flags);

        err = hailo_vdma_engine_fill_irq_data(&params, engine, irq_bitmap,
            transfer_done, controller);
        if (err < 0) {
            hailo_dev_err(controller->dev, "Failed fill irq data %ld", err);
            return err;
        }
    }

    if (copy_to_user((void __user*)arg, &params, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        return -ENOMEM;
    }

    return 0;
}

static uintptr_t hailo_get_next_vdma_handle(struct hailo_vdma_file_context *context)
{
    // Note: The kernel code left-shifts the 'offset' param from the user-space call to mmap by PAGE_SHIFT bits and
    // stores the result in 'vm_area_struct.vm_pgoff'. We pass the desc_handle to mmap in the offset param. To
    // counter this, we right-shift the desc_handle. See also 'mmap function'.
    uintptr_t next_handle = 0;
    next_handle = atomic_inc_return(&context->last_vdma_handle);
    return (next_handle << PAGE_SHIFT);
}

long hailo_vdma_buffer_map_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_vdma_buffer_map_params buf_info;
    struct hailo_vdma_buffer *mapped_buffer = NULL;
    enum dma_data_direction direction = DMA_NONE;
    struct hailo_vdma_low_memory_buffer *low_memory_buffer = NULL;

    if (copy_from_user(&buf_info, (void __user*)arg, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    hailo_dev_info(controller->dev, "address %lx tgid %d size: %zu\n",
        buf_info.user_address, current->tgid, buf_info.size);

    direction = get_dma_direction(buf_info.data_direction);
    if (DMA_NONE == direction) {
        hailo_dev_err(controller->dev, "invalid data direction %d\n", buf_info.data_direction);
        return -EINVAL;
    }

    low_memory_buffer = hailo_vdma_find_low_memory_buffer(context, buf_info.allocated_buffer_handle);

    mapped_buffer = hailo_vdma_buffer_map(controller->dev,
        buf_info.user_address, buf_info.size, direction, buf_info.buffer_type, low_memory_buffer);
    if (IS_ERR(mapped_buffer)) {
        hailo_dev_err(controller->dev, "failed map buffer %lx\n", buf_info.user_address);
        return PTR_ERR(mapped_buffer);
    }

    mapped_buffer->handle = atomic_inc_return(&context->last_vdma_user_buffer_handle);
    buf_info.mapped_handle = mapped_buffer->handle;
    if (copy_to_user((void __user*)arg, &buf_info, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        hailo_vdma_buffer_put(mapped_buffer);
        return -EFAULT;
    }

    list_add(&mapped_buffer->mapped_user_buffer_list, &context->mapped_user_buffer_list);
    hailo_dev_info(controller->dev, "buffer %lx (handle %zu) is mapped\n",
        buf_info.user_address, buf_info.mapped_handle);
    return 0;
}

long hailo_vdma_buffer_unmap_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_vdma_buffer *mapped_buffer = NULL;
    struct hailo_vdma_buffer_unmap_params buffer_unmap_params;

    if (copy_from_user(&buffer_unmap_params, (void __user*)arg, sizeof(buffer_unmap_params))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    hailo_dev_info(controller->dev, "unmap user buffer handle %zu\n", buffer_unmap_params.mapped_handle);

    mapped_buffer = hailo_vdma_find_mapped_user_buffer(context, buffer_unmap_params.mapped_handle);
    if (mapped_buffer == NULL) {
        hailo_dev_warn(controller->dev, "buffer handle %zu not found\n", buffer_unmap_params.mapped_handle);
        return -EINVAL;
    }

    list_del(&mapped_buffer->mapped_user_buffer_list);
    hailo_vdma_buffer_put(mapped_buffer);
    return 0;
}

long hailo_vdma_buffer_sync_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_vdma_buffer_sync_params sync_info = {};
    struct hailo_vdma_buffer *mapped_buffer = NULL;

    if (copy_from_user(&sync_info, (void __user*)arg, sizeof(sync_info))) {
        hailo_dev_err(controller->dev, "copy_from_user fail\n");
        return -EFAULT;
    }

    if (!(mapped_buffer = hailo_vdma_find_mapped_user_buffer(context, sync_info.handle))) {
        hailo_dev_err(controller->dev, "buffer handle %zu doesn't exist\n", sync_info.handle);
        return -EINVAL;
    }

    if ((sync_info.sync_type != HAILO_SYNC_FOR_CPU) && (sync_info.sync_type != HAILO_SYNC_FOR_DEVICE)) {
        hailo_dev_err(controller->dev, "Invalid sync_type given for vdma buffer sync.\n");
        return -EINVAL;
    }

    if (sync_info.offset + sync_info.count > mapped_buffer->size) {
        hailo_dev_err(controller->dev, "Invalid offset/count given for vdma buffer sync. offset %zu count %zu buffer size %u\n",
            sync_info.offset, sync_info.count, mapped_buffer->size);
        return -EINVAL;
    }

    hailo_vdma_buffer_sync(controller, mapped_buffer, sync_info.sync_type,
        sync_info.offset, sync_info.count);
    return 0;
}

long hailo_desc_list_create_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_desc_list_create_params params;
    struct hailo_descriptors_list_buffer *descriptors_buffer = NULL;
    uintptr_t next_handle = 0;
    long err = -EINVAL;

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy_from_user fail\n");
        return -EFAULT;
    }

    if (params.is_circular && !is_powerof2(params.desc_count)) {
        hailo_dev_err(controller->dev, "Invalid desc count given : %zu , circular descriptors count must be power of 2\n",
            params.desc_count);
        return -EINVAL;
    }

    if (!is_powerof2(params.desc_page_size)) {
        hailo_dev_err(controller->dev, "Invalid desc page size given : %u\n",
            params.desc_page_size);
        return -EINVAL;
    }

    hailo_dev_info(controller->dev,
        "Create desc list desc_count: %zu desc_page_size: %u\n",
        params.desc_count, params.desc_page_size);

    descriptors_buffer = kzalloc(sizeof(*descriptors_buffer), GFP_KERNEL);
    if (NULL == descriptors_buffer) {
        hailo_dev_err(controller->dev, "Failed to allocate buffer for descriptors list struct\n");
        return -ENOMEM;
    }

    next_handle = hailo_get_next_vdma_handle(context);

    err = hailo_desc_list_create(controller->dev, params.desc_count,
        params.desc_page_size, next_handle, params.is_circular,
        descriptors_buffer);
    if (err < 0) {
        hailo_dev_err(controller->dev, "failed to allocate descriptors buffer\n");
        kfree(descriptors_buffer);
        return err;
    }

    list_add(&descriptors_buffer->descriptors_buffer_list, &context->descriptors_buffer_list);

    // Note: The physical address is required for CONTEXT_SWITCH firmware controls
    BUILD_BUG_ON(sizeof(params.dma_address) < sizeof(descriptors_buffer->dma_address));
    params.dma_address = descriptors_buffer->dma_address;
    params.desc_handle = descriptors_buffer->handle;

    if(copy_to_user((void __user*)arg, &params, sizeof(params))){
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        list_del(&descriptors_buffer->descriptors_buffer_list);
        hailo_desc_list_release(controller->dev, descriptors_buffer);
        kfree(descriptors_buffer);
        return -EFAULT;
    }

    hailo_dev_info(controller->dev, "Created desc list, handle 0x%llu\n",
        (u64)params.desc_handle);
    return 0;
}

long hailo_desc_list_release_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_desc_list_release_params params;
    struct hailo_descriptors_list_buffer *descriptors_buffer = NULL;

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy_from_user fail\n");
        return -EFAULT;
    }

    descriptors_buffer = hailo_vdma_find_descriptors_buffer(context, params.desc_handle);
    if (descriptors_buffer == NULL) {
        hailo_dev_warn(controller->dev, "not found desc handle %llu\n", (unsigned long long)params.desc_handle);
        return -EINVAL;
    }

    list_del(&descriptors_buffer->descriptors_buffer_list);
    hailo_desc_list_release(controller->dev, descriptors_buffer);
    kfree(descriptors_buffer);
    return 0;
}

long hailo_desc_list_program_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_desc_list_program_params configure_info;
    struct hailo_vdma_buffer *mapped_buffer = NULL;
    struct hailo_descriptors_list_buffer *descriptors_buffer = NULL;
    struct hailo_vdma_mapped_transfer_buffer transfer_buffer = {0};

    if (copy_from_user(&configure_info, (void __user*)arg, sizeof(configure_info))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }
    hailo_dev_info(controller->dev, "config buffer_handle=%zu desc_handle=%llu starting_desc=%u\n",
        configure_info.buffer_handle, (u64)configure_info.desc_handle, configure_info.starting_desc);

    mapped_buffer = hailo_vdma_find_mapped_user_buffer(context, configure_info.buffer_handle);
    descriptors_buffer = hailo_vdma_find_descriptors_buffer(context, configure_info.desc_handle);
    if (mapped_buffer == NULL || descriptors_buffer == NULL) {
        hailo_dev_err(controller->dev, "invalid user/descriptors buffer\n");
        return -EFAULT;
    }

    if (configure_info.buffer_size > mapped_buffer->size) {
        hailo_dev_err(controller->dev, "invalid buffer size. \n");
        return -EFAULT;
    }

    transfer_buffer.sg_table = &mapped_buffer->sg_table;
    transfer_buffer.size = configure_info.buffer_size;
    transfer_buffer.offset = configure_info.buffer_offset;

    return hailo_vdma_program_descriptors_list(
        controller->hw,
        &descriptors_buffer->desc_list,
        configure_info.starting_desc,
        &transfer_buffer,
        configure_info.should_bind,
        configure_info.channel_index,
        configure_info.last_interrupts_domain,
        configure_info.is_debug
    );
}

long hailo_vdma_low_memory_buffer_alloc_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_allocate_low_memory_buffer_params buf_info = {0};
    struct hailo_vdma_low_memory_buffer *low_memory_buffer = NULL;
    long err = -EINVAL;

    if (copy_from_user(&buf_info, (void __user*)arg, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    low_memory_buffer = kzalloc(sizeof(*low_memory_buffer), GFP_KERNEL);
    if (NULL == low_memory_buffer) {
        hailo_dev_err(controller->dev, "memory alloc failed\n");
        return -ENOMEM;
    }

    err = hailo_vdma_low_memory_buffer_alloc(buf_info.buffer_size, low_memory_buffer);
    if (err < 0) {
        kfree(low_memory_buffer);
        hailo_dev_err(controller->dev, "failed allocating buffer from driver\n");
        return err;
    }

    // Get handle for allocated buffer
    low_memory_buffer->handle = hailo_get_next_vdma_handle(context);

    list_add(&low_memory_buffer->vdma_low_memory_buffer_list, &context->vdma_low_memory_buffer_list);

    buf_info.buffer_handle = low_memory_buffer->handle;
    if (copy_to_user((void __user*)arg, &buf_info, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        list_del(&low_memory_buffer->vdma_low_memory_buffer_list);
        hailo_vdma_low_memory_buffer_free(low_memory_buffer);
        kfree(low_memory_buffer);
        return -EFAULT;
    }

    return 0;
}

long hailo_vdma_low_memory_buffer_free_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_vdma_low_memory_buffer *low_memory_buffer = NULL;
    struct hailo_free_low_memory_buffer_params params = {0};

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    low_memory_buffer = hailo_vdma_find_low_memory_buffer(context, params.buffer_handle);
    if (NULL == low_memory_buffer) {
        hailo_dev_warn(controller->dev, "vdma buffer handle %lx not found\n", params.buffer_handle);
        return -EINVAL;
    }

    list_del(&low_memory_buffer->vdma_low_memory_buffer_list);
    hailo_vdma_low_memory_buffer_free(low_memory_buffer);
    kfree(low_memory_buffer);
    return 0;
}

long hailo_mark_as_in_use(struct hailo_vdma_controller *controller, unsigned long arg, struct file *filp)
{
    struct hailo_mark_as_in_use_params params = {0};

    // If device is used by this FD, return false to indicate its free for usage
    if (filp == controller->used_by_filp) {
        params.in_use = false;
    } else if (NULL != controller->used_by_filp) {
        params.in_use = true;
    } else {
        controller->used_by_filp = filp;
        params.in_use = false;
    }

    if (copy_to_user((void __user*)arg, &params, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        return -EFAULT;
    }

    return 0;
}

long hailo_vdma_continuous_buffer_alloc_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_allocate_continuous_buffer_params buf_info = {0};
    struct hailo_vdma_continuous_buffer *continuous_buffer = NULL;
    long err = -EINVAL;
    size_t aligned_buffer_size = 0;

    if (copy_from_user(&buf_info, (void __user*)arg, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    continuous_buffer = kzalloc(sizeof(*continuous_buffer), GFP_KERNEL);
    if (NULL == continuous_buffer) {
        hailo_dev_err(controller->dev, "memory alloc failed\n");
        return -ENOMEM;
    }

    // We use PAGE_ALIGN to support mmap
    aligned_buffer_size = PAGE_ALIGN(buf_info.buffer_size);
    err = hailo_vdma_continuous_buffer_alloc(controller->dev, aligned_buffer_size, continuous_buffer);
    if (err < 0) {
        kfree(continuous_buffer);
        return err;
    }

    continuous_buffer->handle = hailo_get_next_vdma_handle(context);
    list_add(&continuous_buffer->continuous_buffer_list, &context->continuous_buffer_list);

    buf_info.buffer_handle = continuous_buffer->handle;
    buf_info.dma_address = continuous_buffer->dma_address;
    if (copy_to_user((void __user*)arg, &buf_info, sizeof(buf_info))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        list_del(&continuous_buffer->continuous_buffer_list);
        hailo_vdma_continuous_buffer_free(controller->dev, continuous_buffer);
        kfree(continuous_buffer);
        return -EFAULT;
    }

    return 0;
}

long hailo_vdma_continuous_buffer_free_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_free_continuous_buffer_params params;
    struct hailo_vdma_continuous_buffer *continuous_buffer = NULL;

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    continuous_buffer = hailo_vdma_find_continuous_buffer(context, params.buffer_handle);
    if (NULL == continuous_buffer) {
        hailo_dev_warn(controller->dev, "vdma buffer handle %lx not found\n", params.buffer_handle);
        return -EINVAL;
    }

    list_del(&continuous_buffer->continuous_buffer_list);
    hailo_vdma_continuous_buffer_free(controller->dev, continuous_buffer);
    kfree(continuous_buffer);
    return 0;
}

long hailo_vdma_interrupts_read_timestamps_ioctl(struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_vdma_interrupts_read_timestamp_params *params = &controller->read_interrupt_timestamps_params;
    struct hailo_vdma_engine *engine = NULL;
    int err = -EINVAL;

    hailo_dev_dbg(controller->dev, "Start read interrupt timestamps ioctl\n");

    if (copy_from_user(params, (void __user*)arg, sizeof(*params))) {
        hailo_dev_err(controller->dev, "copy_from_user fail\n");
        return -ENOMEM;
    }

    if (params->engine_index >= controller->vdma_engines_count) {
        hailo_dev_err(controller->dev, "Invalid engine %u", params->engine_index);
        return -EINVAL;
    }
    engine = &controller->vdma_engines[params->engine_index];

    err = hailo_vdma_engine_read_timestamps(engine, params);
    if (err < 0) {
        hailo_dev_err(controller->dev, "Failed read engine interrupts for %u:%u",
            params->engine_index, params->channel_index);
        return err;
    }

    if (copy_to_user((void __user*)arg, params, sizeof(*params))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        return -ENOMEM;
    }

    return 0;
}

long hailo_vdma_launch_transfer_ioctl(struct hailo_vdma_file_context *context, struct hailo_vdma_controller *controller,
    unsigned long arg)
{
    struct hailo_vdma_launch_transfer_params params;
    struct hailo_vdma_engine *engine = NULL;
    struct hailo_vdma_channel *channel = NULL;
    struct hailo_descriptors_list_buffer *descriptors_buffer = NULL;
    struct hailo_vdma_mapped_transfer_buffer mapped_transfer_buffers[ARRAY_SIZE(params.buffers)] = {0};
    int ret = -EINVAL;
    u8 i = 0;

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy from user fail\n");
        return -EFAULT;
    }

    if (params.engine_index >= controller->vdma_engines_count) {
        hailo_dev_err(controller->dev, "Invalid engine %u", params.engine_index);
        return -EINVAL;
    }
    engine = &controller->vdma_engines[params.engine_index];

    if (params.channel_index >= ARRAY_SIZE(engine->channels)) {
        hailo_dev_err(controller->dev, "Invalid channel %u", params.channel_index);
        return -EINVAL;
    }
    channel = &engine->channels[params.channel_index];

    if (params.buffers_count > ARRAY_SIZE(params.buffers)) {
        hailo_dev_err(controller->dev, "too many buffers %u\n", params.buffers_count);
        return -EINVAL;
    }

    descriptors_buffer = hailo_vdma_find_descriptors_buffer(context, params.desc_handle);
    if (descriptors_buffer == NULL) {
        hailo_dev_err(controller->dev, "invalid descriptors list handle\n");
        return -EFAULT;
    }

    for (i = 0; i < params.buffers_count; i++) {
        struct hailo_vdma_buffer *mapped_buffer =
            hailo_vdma_find_mapped_user_buffer(context, params.buffers[i].mapped_buffer_handle);
        if (mapped_buffer == NULL) {
            hailo_dev_err(controller->dev, "invalid user buffer\n");
            return -EFAULT;
        }

        if (params.buffers[i].size > mapped_buffer->size) {
            hailo_dev_err(controller->dev, "Syncing size %u while buffer size is %u\n",
                params.buffers[i].size, mapped_buffer->size);
            return -EINVAL;
        }

        if (params.buffers[i].offset > mapped_buffer->size) {
            hailo_dev_err(controller->dev, "Syncing offset %u while buffer size is %u\n",
                params.buffers[i].offset, mapped_buffer->size);
            return -EINVAL;
        }

        // Syncing the buffer to device change its ownership from host to the device.
        // We sync on D2H as well if the user owns the buffer since the buffer might have been changed by
        // the host between the time it was mapped and the current async transfer.
        hailo_vdma_buffer_sync_cyclic(controller, mapped_buffer, HAILO_SYNC_FOR_DEVICE,
            params.buffers[i].offset, params.buffers[i].size);

        mapped_transfer_buffers[i].sg_table = &mapped_buffer->sg_table;
        mapped_transfer_buffers[i].size = params.buffers[i].size;
        mapped_transfer_buffers[i].offset = params.buffers[i].offset;
        mapped_transfer_buffers[i].opaque = mapped_buffer;
    }

    ret = hailo_vdma_launch_transfer(
        controller->hw,
        channel,
        &descriptors_buffer->desc_list,
        params.starting_desc,
        params.buffers_count,
        mapped_transfer_buffers,
        params.should_bind,
        params.first_interrupts_domain,
        params.last_interrupts_domain,
        params.is_debug
    );
    if (ret < 0) {
        params.launch_transfer_status = ret;
        if (-ECONNRESET != ret) {
            hailo_dev_err(controller->dev, "Failed launch transfer %d\n", ret);
        }
        // Still need to copy fail status back to userspace - success oriented
        if (copy_to_user((void __user*)arg, &params, sizeof(params))) {
            hailo_dev_err(controller->dev, "copy_to_user fail\n");
        }
        return ret;
    }

    params.descs_programed = ret;
    params.launch_transfer_status = 0;

    if (copy_to_user((void __user*)arg, &params, sizeof(params))) {
        hailo_dev_err(controller->dev, "copy_to_user fail\n");
        return -EFAULT;
    }

    return 0;
}