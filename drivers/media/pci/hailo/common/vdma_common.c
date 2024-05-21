// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
 **/

#include "vdma_common.h"

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/bug.h>
#include <linux/circ_buf.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/kernel.h>
#include <linux/kconfig.h>
#include <linux/printk.h>


#define CHANNEL_BASE_OFFSET(channel_index) ((channel_index) << 5)
#define CHANNEL_HOST_OFFSET(channel_index) CHANNEL_BASE_OFFSET(channel_index) + \
    (channel_index < VDMA_DEST_CHANNELS_START ? 0 : 0x10)
#define CHANNEL_DEVICE_OFFSET(channel_index) CHANNEL_BASE_OFFSET(channel_index) + \
    (channel_index < VDMA_DEST_CHANNELS_START ? 0x10 : 0)

#define CHANNEL_CONTROL_OFFSET      (0x0)
#define CHANNEL_NUM_AVAIL_OFFSET    (0x2)
#define CHANNEL_NUM_PROC_OFFSET     (0x4)
#define CHANNEL_ERROR_OFFSET        (0x8)

#define VDMA_CHANNEL_CONTROL_START (0x1)
#define VDMA_CHANNEL_CONTROL_ABORT (0b00)
#define VDMA_CHANNEL_CONTROL_ABORT_PAUSE (0b10)
#define VDMA_CHANNEL_CONTROL_START_ABORT_PAUSE_RESUME_BITMASK (0x3)
#define VDMA_CHANNEL_CONTROL_START_ABORT_BITMASK (0x1)

#define DESCRIPTOR_PAGE_SIZE_SHIFT (8)
#define DESCRIPTOR_DESC_CONTROL (0x2)
#define DESCRIPTOR_ADDR_L_MASK (0xFFFFFFC0)

#define DESCRIPTOR_DESC_STATUS_DONE_BIT  (0x0)
#define DESCRIPTOR_DESC_STATUS_ERROR_BIT (0x1)
#define DESCRIPTOR_DESC_STATUS_MASK (0xFF)

#define DESC_STATUS_REQ                       (1 << 0)
#define DESC_STATUS_REQ_ERR                   (1 << 1)
#define DESC_REQUEST_IRQ_PROCESSED            (1 << 2)
#define DESC_REQUEST_IRQ_ERR                  (1 << 3)


#define DWORD_SIZE                  (4)
#define WORD_SIZE                   (2)
#define BYTE_SIZE                   (1)

#define TIMESTAMPS_CIRC_SPACE(timestamp_list) \
    CIRC_SPACE((timestamp_list).head, (timestamp_list).tail, CHANNEL_IRQ_TIMESTAMPS_SIZE)
#define TIMESTAMPS_CIRC_CNT(timestamp_list) \
    CIRC_CNT((timestamp_list).head, (timestamp_list).tail, CHANNEL_IRQ_TIMESTAMPS_SIZE)

#define ONGOING_TRANSFERS_CIRC_SPACE(transfers_list) \
    CIRC_SPACE((transfers_list).head, (transfers_list).tail, HAILO_VDMA_MAX_ONGOING_TRANSFERS)
#define ONGOING_TRANSFERS_CIRC_CNT(transfers_list) \
    CIRC_CNT((transfers_list).head, (transfers_list).tail, HAILO_VDMA_MAX_ONGOING_TRANSFERS)

#ifndef for_each_sgtable_dma_sg
#define for_each_sgtable_dma_sg(sgt, sg, i)	\
    for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)
#endif /* for_each_sgtable_dma_sg */


static int ongoing_transfer_push(struct hailo_vdma_channel *channel,
    struct hailo_ongoing_transfer *ongoing_transfer)
{
    struct hailo_ongoing_transfers_list *transfers = &channel->ongoing_transfers;
    if (!ONGOING_TRANSFERS_CIRC_SPACE(*transfers)) {
        return -EFAULT;
    }

    if (ongoing_transfer->dirty_descs_count > ARRAY_SIZE(ongoing_transfer->dirty_descs)) {
        return -EFAULT;
    }

    transfers->transfers[transfers->head] = *ongoing_transfer;
    transfers->head = (transfers->head + 1) & HAILO_VDMA_MAX_ONGOING_TRANSFERS_MASK;
    return 0;
}

static int ongoing_transfer_pop(struct hailo_vdma_channel *channel,
    struct hailo_ongoing_transfer *ongoing_transfer)
{
    struct hailo_ongoing_transfers_list *transfers = &channel->ongoing_transfers;
    if (!ONGOING_TRANSFERS_CIRC_CNT(*transfers)) {
        return -EFAULT;
    }

    if (ongoing_transfer) {
        *ongoing_transfer = transfers->transfers[transfers->tail];
    }
    transfers->tail = (transfers->tail + 1) & HAILO_VDMA_MAX_ONGOING_TRANSFERS_MASK;
    return 0;
}

static void clear_dirty_desc(struct hailo_vdma_descriptors_list *desc_list, u16 desc)
{
    desc_list->desc_list[desc].PageSize_DescControl =
        (u32)((desc_list->desc_page_size << DESCRIPTOR_PAGE_SIZE_SHIFT) + DESCRIPTOR_DESC_CONTROL);
}

static void clear_dirty_descs(struct hailo_vdma_channel *channel,
    struct hailo_ongoing_transfer *ongoing_transfer)
{
    u8 i = 0;
    struct hailo_vdma_descriptors_list *desc_list = channel->last_desc_list;
    BUG_ON(ongoing_transfer->dirty_descs_count > ARRAY_SIZE(ongoing_transfer->dirty_descs));
    for (i = 0; i < ongoing_transfer->dirty_descs_count; i++) {
        clear_dirty_desc(desc_list, ongoing_transfer->dirty_descs[i]);
    }
}

static bool validate_last_desc_status(struct hailo_vdma_channel *channel,
    struct hailo_ongoing_transfer *ongoing_transfer)
{
    u16 last_desc = ongoing_transfer->last_desc;
    u32 last_desc_control = channel->last_desc_list->desc_list[last_desc].RemainingPageSize_Status &
        DESCRIPTOR_DESC_STATUS_MASK;
    if (!hailo_test_bit(DESCRIPTOR_DESC_STATUS_DONE_BIT, &last_desc_control)) {
        pr_err("Expecting desc %d to be done\n", last_desc);
        return false;
    }
    if (hailo_test_bit(DESCRIPTOR_DESC_STATUS_ERROR_BIT, &last_desc_control)) {
        pr_err("Got unexpected error on desc %d\n", last_desc);
        return false;
    }

    return true;
}

void hailo_vdma_program_descriptor(struct hailo_vdma_descriptor *descriptor, u64 dma_address, size_t page_size,
    u8 data_id)
{
    descriptor->PageSize_DescControl = (u32)((page_size << DESCRIPTOR_PAGE_SIZE_SHIFT) +
        DESCRIPTOR_DESC_CONTROL);
    descriptor->AddrL_rsvd_DataID = (u32)(((dma_address & DESCRIPTOR_ADDR_L_MASK)) | data_id);
    descriptor->AddrH = (u32)(dma_address >> 32);
    descriptor->RemainingPageSize_Status = 0 ;
}

static u8 get_channel_id(u8 channel_index)
{
    if (channel_index < VDMA_DEST_CHANNELS_START) {
        // H2D channel
        return channel_index;
    }
    else if ((channel_index >= VDMA_DEST_CHANNELS_START) &&
             (channel_index < MAX_VDMA_CHANNELS_PER_ENGINE)) {
        // D2H channel
        return channel_index - VDMA_DEST_CHANNELS_START;
    }
    else {
        return INVALID_VDMA_CHANNEL;
    }
}

static int program_descriptors_in_chunk(
    struct hailo_vdma_hw *vdma_hw,
    dma_addr_t chunk_addr,
    unsigned int chunk_size,
    struct hailo_vdma_descriptors_list *desc_list,
    u32 desc_index,
    u32 max_desc_index,
    u8 channel_id)
{
    const u32 desc_per_chunk = DIV_ROUND_UP(chunk_size, desc_list->desc_page_size);
    struct hailo_vdma_descriptor *dma_desc = NULL;
    u16 size_to_program = 0;
    u32 index = 0;
    u64 encoded_addr = 0;

    for (index = 0; index < desc_per_chunk; index++) {
        if (desc_index > max_desc_index) {
            return -ERANGE;
        }

        encoded_addr = vdma_hw->hw_ops.encode_desc_dma_address(chunk_addr, channel_id);
        if (INVALID_VDMA_ADDRESS == encoded_addr) {
            return -EFAULT;
        }

        dma_desc = &desc_list->desc_list[desc_index % desc_list->desc_count];
        size_to_program = chunk_size > desc_list->desc_page_size ?
            desc_list->desc_page_size : (u16)chunk_size;
        hailo_vdma_program_descriptor(dma_desc, encoded_addr, size_to_program, vdma_hw->ddr_data_id);

        chunk_addr += size_to_program;
        chunk_size -= size_to_program;
        desc_index++;
    }

    return (int)desc_per_chunk;
}

int hailo_vdma_program_descriptors_list(
    struct hailo_vdma_hw *vdma_hw,
    struct hailo_vdma_descriptors_list *desc_list,
    u32 starting_desc,
    struct hailo_vdma_mapped_transfer_buffer *buffer,
    u8 channel_index)
{
    const u8 channel_id = get_channel_id(channel_index);
    int desc_programmed = 0;
    u32 max_desc_index = 0;
    u32 chunk_size = 0;
    struct scatterlist *sg_entry = NULL;
    unsigned int i = 0;
    int ret = 0;
    size_t buffer_current_offset = 0;
    dma_addr_t chunk_start_addr = 0;
    u32 program_size = buffer->size;

    if (starting_desc >= desc_list->desc_count) {
        return -EFAULT;
    }

    if (buffer->offset % desc_list->desc_page_size != 0) {
        return -EFAULT;
    }

    // On circular buffer, allow programming  desc_count descriptors (starting
    // from starting_desc). On non circular, don't allow is to pass desc_count
    max_desc_index = desc_list->is_circular ?
        starting_desc + desc_list->desc_count - 1 :
        desc_list->desc_count - 1;
    for_each_sgtable_dma_sg(buffer->sg_table, sg_entry, i) {
        // Skip sg entries until we reach the right buffer offset. offset can be in the middle of an sg entry.
        if (buffer_current_offset + sg_dma_len(sg_entry) < buffer->offset) {
            buffer_current_offset += sg_dma_len(sg_entry);
            continue;
        }
        chunk_start_addr = (buffer_current_offset < buffer->offset) ?
            sg_dma_address(sg_entry) + (buffer->offset - buffer_current_offset) :
            sg_dma_address(sg_entry);
        chunk_size = (buffer_current_offset < buffer->offset) ?
            (u32)(sg_dma_len(sg_entry) - (buffer->offset - buffer_current_offset)) :
            (u32)(sg_dma_len(sg_entry));
        chunk_size = min((u32)program_size, chunk_size);

        ret = program_descriptors_in_chunk(vdma_hw, chunk_start_addr, chunk_size, desc_list,
            starting_desc, max_desc_index, channel_id);
        if (ret < 0) {
            return ret;
        }

        desc_programmed += ret;
        starting_desc = starting_desc + ret;
        program_size -= chunk_size;
        buffer_current_offset += sg_dma_len(sg_entry);
    }

    if (program_size != 0) {
        // We didn't program all the buffer.
        return -EFAULT;
    }

    return desc_programmed;
}

static bool channel_control_reg_is_active(u8 control)
{
    return (control & VDMA_CHANNEL_CONTROL_START_ABORT_BITMASK) == VDMA_CHANNEL_CONTROL_START;
}

static int validate_channel_state(struct hailo_vdma_channel *channel)
{
    const u8 control = ioread8(channel->host_regs + CHANNEL_CONTROL_OFFSET);
    const u16 hw_num_avail = ioread16(channel->host_regs + CHANNEL_NUM_AVAIL_OFFSET);

    if (!channel_control_reg_is_active(control)) {
        pr_err("Channel %d is not active\n", channel->index);
        return -EBUSY;
    }

    if (hw_num_avail != channel->state.num_avail) {
        pr_err("Channel %d hw state out of sync. num available is %d, expected %d\n",
            channel->index, hw_num_avail, channel->state.num_avail);
        return -EFAULT;
    }

    return 0;
}

static unsigned long get_interrupts_bitmask(struct hailo_vdma_hw *vdma_hw,
    enum hailo_vdma_interrupts_domain interrupts_domain, bool is_debug)
{
    unsigned long bitmask = 0;

    if (0 != (HAILO_VDMA_INTERRUPTS_DOMAIN_DEVICE & interrupts_domain)) {
        bitmask |= vdma_hw->device_interrupts_bitmask;
    }
    if (0 != (HAILO_VDMA_INTERRUPTS_DOMAIN_HOST & interrupts_domain)) {
        bitmask |= vdma_hw->host_interrupts_bitmask;
    }

    if (bitmask != 0) {
        bitmask |= DESC_REQUEST_IRQ_PROCESSED | DESC_REQUEST_IRQ_ERR;
        if (is_debug) {
            bitmask |= DESC_STATUS_REQ | DESC_STATUS_REQ_ERR;
        }
    }

    return bitmask;
}

static void set_num_avail(u8 __iomem *host_regs, u16 num_avail)
{
    iowrite16(num_avail, host_regs + CHANNEL_NUM_AVAIL_OFFSET);
}

static u16 get_num_proc(u8 __iomem *host_regs)
{
    return ioread16(host_regs + CHANNEL_NUM_PROC_OFFSET);
}

static int program_last_desc(
    struct hailo_vdma_descriptors_list *desc_list,
    u32 starting_desc,
    struct hailo_vdma_mapped_transfer_buffer *transfer_buffer)
{
    u32 total_descs = DIV_ROUND_UP(transfer_buffer->size, desc_list->desc_page_size);
    u32 last_desc = (starting_desc + total_descs - 1) % desc_list->desc_count;
    u32 last_desc_size = transfer_buffer->size - (total_descs - 1) * desc_list->desc_page_size;

    // Configure only last descriptor with residue size
    desc_list->desc_list[last_desc].PageSize_DescControl = (u32)
        ((last_desc_size << DESCRIPTOR_PAGE_SIZE_SHIFT) + DESCRIPTOR_DESC_CONTROL);
    return (int)total_descs;
}

int hailo_vdma_launch_transfer(
    struct hailo_vdma_hw *vdma_hw,
    struct hailo_vdma_channel *channel,
    struct hailo_vdma_descriptors_list *desc_list,
    u32 starting_desc,
    u8 buffers_count,
    struct hailo_vdma_mapped_transfer_buffer *buffers,
    bool should_bind,
    enum hailo_vdma_interrupts_domain first_interrupts_domain,
    enum hailo_vdma_interrupts_domain last_desc_interrupts,
    bool is_debug)
{
    int ret = -EFAULT;
    u32 total_descs = 0;
    u32 first_desc = starting_desc;
    u32 last_desc = U32_MAX;
    u16 new_num_avail = 0;
    struct hailo_ongoing_transfer ongoing_transfer = {0};
    u8 i = 0;

    channel->state.desc_count_mask = (desc_list->desc_count - 1);

    if (NULL == channel->last_desc_list) {
        // First transfer on this active channel, store desc list.
        channel->last_desc_list = desc_list;
    } else if (desc_list != channel->last_desc_list) {
        // Shouldn't happen, desc list may change only after channel deactivation.
        pr_err("Inconsistent desc list given to channel %d\n", channel->index);
        return -EINVAL;
    }

    if (channel->state.num_avail != (u16)starting_desc) {
        pr_err("Channel %d state out of sync. num available is %d, expected %d\n",
            channel->index, channel->state.num_avail, (u16)starting_desc);
        return -EFAULT;
    }

    if (buffers_count > HAILO_MAX_BUFFERS_PER_SINGLE_TRANSFER) {
        pr_err("Too many buffers %u for single transfer\n", buffers_count);
        return -EINVAL;
    }

    if (is_debug) {
        ret = validate_channel_state(channel);
        if (ret < 0) {
            return ret;
        }
    }

    BUILD_BUG_ON_MSG((HAILO_MAX_BUFFERS_PER_SINGLE_TRANSFER + 1) != ARRAY_SIZE(ongoing_transfer.dirty_descs),
        "Unexpected amount of dirty descriptors");
    ongoing_transfer.dirty_descs_count = buffers_count + 1;
    ongoing_transfer.dirty_descs[0] = (u16)starting_desc;

    for (i = 0; i < buffers_count; i++) {
        ret = should_bind ?
            hailo_vdma_program_descriptors_list(vdma_hw, desc_list, starting_desc, &buffers[i], channel->index) :
            program_last_desc(desc_list, starting_desc, &buffers[i]);
        if (ret < 0) {
            return ret;
        }
        total_descs += ret;
        last_desc = (starting_desc + ret - 1) % desc_list->desc_count;
        starting_desc = (starting_desc + ret) % desc_list->desc_count;

        ongoing_transfer.dirty_descs[i+1] = (u16)last_desc;
        ongoing_transfer.buffers[i] = buffers[i];
    }
    ongoing_transfer.buffers_count = buffers_count;

    desc_list->desc_list[first_desc].PageSize_DescControl |=
        get_interrupts_bitmask(vdma_hw, first_interrupts_domain, is_debug);
    desc_list->desc_list[last_desc].PageSize_DescControl |=
        get_interrupts_bitmask(vdma_hw, last_desc_interrupts, is_debug);

    ongoing_transfer.last_desc = (u16)last_desc;
    ongoing_transfer.is_debug = is_debug;
    ret = ongoing_transfer_push(channel, &ongoing_transfer);
    if (ret < 0) {
        pr_err("Failed push ongoing transfer to channel %d\n", channel->index);
        return ret;
    }

    new_num_avail = (u16)((last_desc + 1) % desc_list->desc_count);
    channel->state.num_avail = new_num_avail;
    set_num_avail(channel->host_regs, new_num_avail);

    return (int)total_descs;
}

static void hailo_vdma_push_timestamp(struct hailo_vdma_channel *channel)
{
    struct hailo_channel_interrupt_timestamp_list *timestamp_list = &channel->timestamp_list;
    const u16 num_proc = get_num_proc(channel->host_regs);
    if (TIMESTAMPS_CIRC_SPACE(*timestamp_list) != 0) {
        timestamp_list->timestamps[timestamp_list->head].timestamp_ns = ktime_get_ns();
        timestamp_list->timestamps[timestamp_list->head].desc_num_processed = num_proc;
        timestamp_list->head = (timestamp_list->head + 1) & CHANNEL_IRQ_TIMESTAMPS_SIZE_MASK;
    }
}

// Returns false if there are no items
static bool hailo_vdma_pop_timestamp(struct hailo_channel_interrupt_timestamp_list *timestamp_list,
    struct hailo_channel_interrupt_timestamp *out_timestamp)
{
    if (0 == TIMESTAMPS_CIRC_CNT(*timestamp_list)) {
        return false;
    }

    *out_timestamp = timestamp_list->timestamps[timestamp_list->tail];
    timestamp_list->tail = (timestamp_list->tail+1) & CHANNEL_IRQ_TIMESTAMPS_SIZE_MASK;
    return true;
}

static void hailo_vdma_pop_timestamps_to_response(struct hailo_vdma_channel *channel,
    struct hailo_vdma_interrupts_read_timestamp_params *result)
{
    const u32 max_timestamps = ARRAY_SIZE(result->timestamps);
    u32 i = 0;

    while (hailo_vdma_pop_timestamp(&channel->timestamp_list, &result->timestamps[i]) &&
        (i < max_timestamps)) {
        // Although the hw_num_processed should be a number between 0 and
        // desc_count-1, if desc_count < 0x10000 (the maximum desc size),
        // the actual hw_num_processed is a number between 1 and desc_count.
        // Therefore the value can be desc_count, in this case we change it to
        // zero.
        result->timestamps[i].desc_num_processed = result->timestamps[i].desc_num_processed &
            channel->state.desc_count_mask;
        i++;
    }

    result->timestamps_count = i;
}

static void channel_state_init(struct hailo_vdma_channel_state *state)
{
    state->num_avail = state->num_proc = 0;

    // Special value used when the channel is not activate.
    state->desc_count_mask = U32_MAX;
}

void hailo_vdma_engine_init(struct hailo_vdma_engine *engine, u8 engine_index,
    const struct hailo_resource *channel_registers)
{
    u8 channel_index = 0;
    struct hailo_vdma_channel *channel;

    engine->index = engine_index;
    engine->enabled_channels = 0x0;
    engine->interrupted_channels = 0x0;

    for_each_vdma_channel(engine, channel, channel_index) {
        u8 __iomem *regs_base = (u8 __iomem *)channel_registers->address;
        channel->host_regs = regs_base + CHANNEL_HOST_OFFSET(channel_index);
        channel->device_regs = regs_base + CHANNEL_DEVICE_OFFSET(channel_index);
        channel->index = channel_index;
        channel->timestamp_measure_enabled = false;

        channel_state_init(&channel->state);
        channel->last_desc_list = NULL;

        channel->ongoing_transfers.head = 0;
        channel->ongoing_transfers.tail = 0;
    }
}

void hailo_vdma_engine_enable_channel_interrupts(struct hailo_vdma_engine *engine, u32 bitmap,
    bool measure_timestamp)
{
    struct hailo_vdma_channel *channel = NULL;
    u8 channel_index = 0;

    for_each_vdma_channel(engine, channel, channel_index) {
        if (hailo_test_bit(channel_index, &bitmap)) {
            channel->timestamp_measure_enabled = measure_timestamp;
            channel->timestamp_list.head = channel->timestamp_list.tail = 0;
        }
    }

    engine->enabled_channels |= bitmap;
}

void hailo_vdma_engine_disable_channel_interrupts(struct hailo_vdma_engine *engine, u32 bitmap)
{
    struct hailo_vdma_channel *channel = NULL;
    u8 channel_index = 0;

    engine->enabled_channels &= ~bitmap;

    for_each_vdma_channel(engine, channel, channel_index) {
        channel_state_init(&channel->state);

        while (ONGOING_TRANSFERS_CIRC_CNT(channel->ongoing_transfers) > 0) {
            struct hailo_ongoing_transfer transfer;
            ongoing_transfer_pop(channel, &transfer);

            if (channel->last_desc_list == NULL) {
                pr_err("Channel %d has ongoing transfers but no desc list\n", channel->index);
                continue;
            }

            clear_dirty_descs(channel, &transfer);
        }

        channel->last_desc_list = NULL;
    }
}

void hailo_vdma_engine_push_timestamps(struct hailo_vdma_engine *engine, u32 bitmap)
{
    struct hailo_vdma_channel *channel = NULL;
    u8 channel_index = 0;

    for_each_vdma_channel(engine, channel, channel_index) {
        if (unlikely(hailo_test_bit(channel_index, &bitmap) &&
                channel->timestamp_measure_enabled)) {
            hailo_vdma_push_timestamp(channel);
        }
    }
}

int hailo_vdma_engine_read_timestamps(struct hailo_vdma_engine *engine,
    struct hailo_vdma_interrupts_read_timestamp_params *params)
{
    struct hailo_vdma_channel *channel = NULL;

    if (params->channel_index >= MAX_VDMA_CHANNELS_PER_ENGINE) {
        return -EINVAL;
    }

    channel = &engine->channels[params->channel_index];
    hailo_vdma_pop_timestamps_to_response(channel, params);
    return 0;
}

void hailo_vdma_engine_clear_channel_interrupts(struct hailo_vdma_engine *engine, u32 bitmap)
{
    engine->interrupted_channels &= ~bitmap;
}

void hailo_vdma_engine_set_channel_interrupts(struct hailo_vdma_engine *engine, u32 bitmap)
{
    engine->interrupted_channels |= bitmap;
}

static void fill_channel_irq_data(struct hailo_vdma_interrupts_channel_data *irq_data,
    struct hailo_vdma_engine *engine, struct hailo_vdma_channel *channel, u16 num_proc,
    bool validation_success)
{
    u8 host_control = ioread8(channel->host_regs + CHANNEL_CONTROL_OFFSET);
    u8 device_control = ioread8(channel->device_regs + CHANNEL_CONTROL_OFFSET);

    irq_data->engine_index = engine->index;
    irq_data->channel_index = channel->index;

    irq_data->is_active = channel_control_reg_is_active(host_control) &&
        channel_control_reg_is_active(device_control);

    irq_data->host_num_processed = num_proc;
    irq_data->host_error = ioread8(channel->host_regs + CHANNEL_ERROR_OFFSET);
    irq_data->device_error = ioread8(channel->device_regs + CHANNEL_ERROR_OFFSET);
    irq_data->validation_success = validation_success;
}

static bool is_desc_between(u16 begin, u16 end, u16 desc)
{
    if (begin == end) {
        // There is nothing between
        return false;
    }
    if (begin < end) {
        // desc needs to be in [begin, end)
        return (begin <= desc) && (desc < end);
    }
    else {
        // desc needs to be in [0, end) or [begin, m_descs.size()-1]
        return (desc < end) || (begin <= desc);
    }
}

static bool is_transfer_complete(struct hailo_vdma_channel *channel,
    struct hailo_ongoing_transfer *transfer, u16 hw_num_proc)
{
    if (channel->state.num_avail == hw_num_proc) {
        return true;
    }

    return is_desc_between(channel->state.num_proc, hw_num_proc, transfer->last_desc);
}

int hailo_vdma_engine_fill_irq_data(struct hailo_vdma_interrupts_wait_params *irq_data,
    struct hailo_vdma_engine *engine, u32 irq_channels_bitmap,
    transfer_done_cb_t transfer_done, void *transfer_done_opaque)
{
    struct hailo_vdma_channel *channel = NULL;
    u8 channel_index = 0;
    bool validation_success = true;

    for_each_vdma_channel(engine, channel, channel_index) {
        u16 hw_num_proc = U16_MAX;
        if (!hailo_test_bit(channel->index, &irq_channels_bitmap)) {
            continue;
        }

        if (channel->last_desc_list == NULL) {
            // Channel not active or no transfer, skipping.
            continue;
        }

        if (irq_data->channels_count >= ARRAY_SIZE(irq_data->irq_data)) {
            return -EINVAL;
        }

        // Although the hw_num_processed should be a number between 0 and
        // desc_count-1, if desc_count < 0x10000 (the maximum desc size),
        // the actual hw_num_processed is a number between 1 and desc_count.
        // Therefore the value can be desc_count, in this case we change it to
        // zero.
        hw_num_proc = get_num_proc(channel->host_regs) & channel->state.desc_count_mask;

        while (ONGOING_TRANSFERS_CIRC_CNT(channel->ongoing_transfers) > 0) {
            struct hailo_ongoing_transfer *cur_transfer =
                &channel->ongoing_transfers.transfers[channel->ongoing_transfers.tail];
            if (!is_transfer_complete(channel, cur_transfer, hw_num_proc)) {
                break;
            }

            if (cur_transfer->is_debug &&
                !validate_last_desc_status(channel, cur_transfer)) {
                validation_success = false;
            }

            clear_dirty_descs(channel, cur_transfer);
            transfer_done(cur_transfer, transfer_done_opaque);
            channel->state.num_proc = (u16)((cur_transfer->last_desc + 1) & channel->state.desc_count_mask);

            ongoing_transfer_pop(channel, NULL);
        }

        fill_channel_irq_data(&irq_data->irq_data[irq_data->channels_count],
            engine, channel, hw_num_proc, validation_success);
        irq_data->channels_count++;
    }

    return 0;
}