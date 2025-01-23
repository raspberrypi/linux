// SPDX-License-Identifier: MIT
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
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

#define VDMA_CHANNEL_CONTROL_START (0x1)
#define VDMA_CHANNEL_CONTROL_ABORT (0b00)
#define VDMA_CHANNEL_CONTROL_ABORT_PAUSE (0b10)
#define VDMA_CHANNEL_CONTROL_START_ABORT_PAUSE_RESUME_BITMASK (0x3)
#define VDMA_CHANNEL_CONTROL_START_ABORT_BITMASK (0x1)
#define VDMA_CHANNEL_CONTROL_MASK (0xFC)
#define VDMA_CHANNEL_CONTROL_START_RESUME (0b01)
#define VDMA_CHANNEL_CONTROL_START_PAUSE (0b11)
#define VDMA_CHANNEL_CONTROL_ABORT (0b00)
#define VDMA_CHANNEL_CONTROL_ABORT_PAUSE (0b10)
#define VDMA_CHANNEL_CONTROL_START_ABORT_PAUSE_RESUME_BITMASK (0x3)
#define VDMA_CHANNEL_DESC_DEPTH_WIDTH (4)
#define VDMA_CHANNEL_DESC_DEPTH_SHIFT (11)
#define VDMA_CHANNEL_DATA_ID_SHIFT (8)
#define VDMA_CHANNEL__MAX_CHECKS_CHANNEL_IS_IDLE (10000)
#define VDMA_CHANNEL__ADDRESS_L_OFFSET          (0x0A)
#define VDMA_CHANNEL__ALIGNED_ADDRESS_L_OFFSET  (0x8)
#define VDMA_CHANNEL__ADDRESS_H_OFFSET          (0x0C)

#define DESCRIPTOR_PAGE_SIZE_SHIFT (8)
#define DESCRIPTOR_DESC_CONTROL (0x2)
#define DESCRIPTOR_ADDR_L_MASK (0xFFFFFFC0)
#define DESCRIPTOR_LIST_MAX_DEPTH (16)

#define DESCRIPTOR_DESC_STATUS_DONE_BIT  (0x0)
#define DESCRIPTOR_DESC_STATUS_ERROR_BIT (0x1)
#define DESCRIPTOR_DESC_STATUS_MASK (0xFF)

#define DESC_STATUS_REQ                       (1 << 0)
#define DESC_STATUS_REQ_ERR                   (1 << 1)
#define DESC_REQUEST_IRQ_PROCESSED            (1 << 2)
#define DESC_REQUEST_IRQ_ERR                  (1 << 3)

#define VDMA_CHANNEL_NUM_PROCESSED_WIDTH (16)
#define VDMA_CHANNEL_NUM_PROCESSED_MASK ((1 << VDMA_CHANNEL_NUM_PROCESSED_WIDTH) - 1)
#define VDMA_CHANNEL_NUM_ONGOING_MASK VDMA_CHANNEL_NUM_PROCESSED_MASK

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

static void hailo_vdma_program_descriptor(struct hailo_vdma_descriptor *descriptor, u64 dma_address, size_t page_size,
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
    return (channel_index < MAX_VDMA_CHANNELS_PER_ENGINE) ? (channel_index & 0xF) : INVALID_VDMA_CHANNEL;
}

int hailo_vdma_program_descriptors_in_chunk(
    struct hailo_vdma_hw *vdma_hw,
    dma_addr_t chunk_addr,
    unsigned int chunk_size,
    struct hailo_vdma_descriptors_list *desc_list,
    u32 desc_index,
    u32 max_desc_index,
    u8 channel_index,
    u8 data_id)
{
    const u16 page_size = desc_list->desc_page_size;
    const u32 descs_to_program = DIV_ROUND_UP(chunk_size, page_size);
    const u32 starting_desc_index = desc_index;
    const u32 residue_size = chunk_size % page_size;
    struct hailo_vdma_descriptor *dma_desc = NULL;
    u64 encoded_addr = 0;

    if (descs_to_program == 0) {
        // Nothing to program
        return 0;
    }

    // We iterate through descriptors [desc_index, desc_index + descs_to_program)
    if (desc_index + descs_to_program > max_desc_index + 1) {
        return -ERANGE;
    }

    encoded_addr = vdma_hw->hw_ops.encode_desc_dma_address_range(chunk_addr, chunk_addr + chunk_size, page_size, get_channel_id(channel_index));
    if (INVALID_VDMA_ADDRESS == encoded_addr) {
        return -EFAULT;
    }

    // Program all descriptors except the last one
    for (desc_index = starting_desc_index; desc_index < starting_desc_index + descs_to_program - 1; desc_index++) {
        // 'desc_index & desc_list_len_mask' is used instead of modulo; see hailo_vdma_descriptors_list documentation.
        hailo_vdma_program_descriptor(
            &desc_list->desc_list[desc_index & desc_list->desc_count_mask],
            encoded_addr, page_size, data_id);
        encoded_addr += page_size;
    }

    // Handle the last descriptor outside of the loop
    // 'desc_index & desc_list_len_mask' is used instead of modulo; see hailo_vdma_descriptors_list documentation.
    dma_desc = &desc_list->desc_list[desc_index & desc_list->desc_count_mask];
    hailo_vdma_program_descriptor(dma_desc, encoded_addr,
        (residue_size == 0) ? page_size : (u16)residue_size, data_id);

    return (int)descs_to_program;
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

static int bind_and_program_descriptors_list(
    struct hailo_vdma_hw *vdma_hw,
    struct hailo_vdma_descriptors_list *desc_list,
    u32 starting_desc,
    struct hailo_vdma_mapped_transfer_buffer *buffer,
    u8 channel_index,
    enum hailo_vdma_interrupts_domain last_desc_interrupts,
    bool is_debug)
{
    int desc_programmed = 0;
    int descs_programmed_in_chunk = 0;
    u32 max_desc_index = 0;
    u32 chunk_size = 0;
    struct scatterlist *sg_entry = NULL;
    unsigned int i = 0;
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

        descs_programmed_in_chunk  = hailo_vdma_program_descriptors_in_chunk(vdma_hw, chunk_start_addr, chunk_size, desc_list,
            starting_desc, max_desc_index, channel_index, vdma_hw->ddr_data_id);
        if (descs_programmed_in_chunk < 0) {
            return descs_programmed_in_chunk;
        }

        desc_programmed += descs_programmed_in_chunk;
        starting_desc = starting_desc + descs_programmed_in_chunk;
        program_size -= chunk_size;
        buffer_current_offset += sg_dma_len(sg_entry);
    }

    if (program_size != 0) {
        // We didn't program all the buffer.
        return -EFAULT;
    }

    desc_list->desc_list[(starting_desc - 1) % desc_list->desc_count].PageSize_DescControl |=
        get_interrupts_bitmask(vdma_hw, last_desc_interrupts, is_debug);

    return desc_programmed;
}

static int program_last_desc(
    struct hailo_vdma_hw *vdma_hw,
    struct hailo_vdma_descriptors_list *desc_list,
    u32 starting_desc,
    struct hailo_vdma_mapped_transfer_buffer *transfer_buffer,
    enum hailo_vdma_interrupts_domain last_desc_interrupts,
    bool is_debug)
{
    u8 control = (u8)(DESCRIPTOR_DESC_CONTROL | get_interrupts_bitmask(vdma_hw, last_desc_interrupts, is_debug));
    u32 total_descs = DIV_ROUND_UP(transfer_buffer->size, desc_list->desc_page_size);
    u32 last_desc = (starting_desc + total_descs - 1) % desc_list->desc_count;
    u32 last_desc_size = transfer_buffer->size - (total_descs - 1) * desc_list->desc_page_size;

    // Configure only last descriptor with residue size
    desc_list->desc_list[last_desc].PageSize_DescControl = (u32)
        ((last_desc_size << DESCRIPTOR_PAGE_SIZE_SHIFT) + control);
    return (int)total_descs;
}

int hailo_vdma_program_descriptors_list(
    struct hailo_vdma_hw *vdma_hw,
    struct hailo_vdma_descriptors_list *desc_list,
    u32 starting_desc,
    struct hailo_vdma_mapped_transfer_buffer *buffer,
    bool should_bind,
    u8 channel_index,
    enum hailo_vdma_interrupts_domain last_desc_interrupts,
    bool is_debug)
{
    return should_bind ?
        bind_and_program_descriptors_list(vdma_hw, desc_list, starting_desc,
            buffer, channel_index, last_desc_interrupts, is_debug) :
        program_last_desc(vdma_hw, desc_list, starting_desc, buffer,
            last_desc_interrupts, is_debug);
}


static bool channel_control_reg_is_active(u8 control)
{
    return (control & VDMA_CHANNEL_CONTROL_START_ABORT_BITMASK) == VDMA_CHANNEL_CONTROL_START;
}

static int validate_channel_state(struct hailo_vdma_channel *channel)
{
    u32 host_regs_value = ioread32(channel->host_regs);
    const u8 control = READ_BITS_AT_OFFSET(BYTE_SIZE * BITS_IN_BYTE, CHANNEL_CONTROL_OFFSET * BITS_IN_BYTE, host_regs_value);
    const u16 hw_num_avail = READ_BITS_AT_OFFSET(WORD_SIZE * BITS_IN_BYTE, CHANNEL_NUM_AVAIL_OFFSET * BITS_IN_BYTE, host_regs_value);

    if (!channel_control_reg_is_active(control)) {
        return -ECONNRESET;
    }

    if (hw_num_avail != channel->state.num_avail) {
        pr_err("Channel %d hw state out of sync. num available is %d, expected %d\n",
            channel->index, hw_num_avail, channel->state.num_avail);
        return -EFAULT;
    }

    return 0;
}

void hailo_vdma_set_num_avail(u8 __iomem *regs, u16 num_avail)
{
    u32 regs_val = ioread32(regs);
    iowrite32(WRITE_BITS_AT_OFFSET(WORD_SIZE * BITS_IN_BYTE, CHANNEL_NUM_AVAIL_OFFSET * BITS_IN_BYTE, regs_val, num_avail),
        regs);
}

u16 hailo_vdma_get_num_proc(u8 __iomem *regs)
{
    return READ_BITS_AT_OFFSET(WORD_SIZE * BITS_IN_BYTE, 0, ioread32(regs + CHANNEL_NUM_PROC_OFFSET));
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

    ret = validate_channel_state(channel);
    if (ret < 0) {
        return ret;
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

    BUILD_BUG_ON_MSG((HAILO_MAX_BUFFERS_PER_SINGLE_TRANSFER + 1) != ARRAY_SIZE(ongoing_transfer.dirty_descs),
        "Unexpected amount of dirty descriptors");
    ongoing_transfer.dirty_descs_count = buffers_count + 1;
    ongoing_transfer.dirty_descs[0] = (u16)starting_desc;

    for (i = 0; i < buffers_count; i++) {
        ret = hailo_vdma_program_descriptors_list(vdma_hw, desc_list,
            starting_desc, &buffers[i], should_bind, channel->index,
            (i == (buffers_count - 1) ? last_desc_interrupts : HAILO_VDMA_INTERRUPTS_DOMAIN_NONE),
            is_debug);

        total_descs += ret;
        last_desc = (starting_desc + ret - 1) % desc_list->desc_count;
        starting_desc = (starting_desc + ret) % desc_list->desc_count;

        ongoing_transfer.dirty_descs[i+1] = (u16)last_desc;
        ongoing_transfer.buffers[i] = buffers[i];
    }
    ongoing_transfer.buffers_count = buffers_count;

    desc_list->desc_list[first_desc].PageSize_DescControl |=
        get_interrupts_bitmask(vdma_hw, first_interrupts_domain, is_debug);

    ongoing_transfer.last_desc = (u16)last_desc;
    ongoing_transfer.is_debug = is_debug;
    ret = ongoing_transfer_push(channel, &ongoing_transfer);
    if (ret < 0) {
        pr_err("Failed push ongoing transfer to channel %d\n", channel->index);
        return ret;
    }

    new_num_avail = (u16)((last_desc + 1) % desc_list->desc_count);
    channel->state.num_avail = new_num_avail;
    hailo_vdma_set_num_avail(channel->host_regs, new_num_avail);

    return (int)total_descs;
}

static void hailo_vdma_push_timestamp(struct hailo_vdma_channel *channel)
{
    struct hailo_channel_interrupt_timestamp_list *timestamp_list = &channel->timestamp_list;
    const u16 num_proc = hailo_vdma_get_num_proc(channel->host_regs);
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

static u8 __iomem *get_channel_regs(u8 __iomem *regs_base, u8 channel_index, bool is_host_side, u32 src_channels_bitmask)
{
    // Check if getting host side regs or device side
    u8 __iomem *channel_regs_base = regs_base + CHANNEL_BASE_OFFSET(channel_index);
    if (is_host_side) {
        return hailo_test_bit(channel_index, &src_channels_bitmask) ? channel_regs_base :
            (channel_regs_base + CHANNEL_DEST_REGS_OFFSET);
    } else {
        return hailo_test_bit(channel_index, &src_channels_bitmask) ? (channel_regs_base + CHANNEL_DEST_REGS_OFFSET) :
            channel_regs_base;
    }
}

void hailo_vdma_engine_init(struct hailo_vdma_engine *engine, u8 engine_index,
    const struct hailo_resource *channel_registers, u32 src_channels_bitmask)
{
    u8 channel_index = 0;
    struct hailo_vdma_channel *channel;

    engine->index = engine_index;
    engine->enabled_channels = 0x0;
    engine->interrupted_channels = 0x0;

    for_each_vdma_channel(engine, channel, channel_index) {
        u8 __iomem *regs_base = (u8 __iomem *)channel_registers->address;
        channel->host_regs = get_channel_regs(regs_base, channel_index, true, src_channels_bitmask);
        channel->device_regs = get_channel_regs(regs_base, channel_index, false, src_channels_bitmask);
        channel->index = channel_index;
        channel->timestamp_measure_enabled = false;

        channel_state_init(&channel->state);
        channel->last_desc_list = NULL;

        channel->ongoing_transfers.head = 0;
        channel->ongoing_transfers.tail = 0;
    }
}

/**
 * Enables the given channels bitmap in the given engine. Allows launching transfer
 * and reading interrupts from the channels.
 *
 * @param engine - dma engine.
 * @param bitmap - channels bitmap to enable.
 * @param measure_timestamp - if set, allow interrupts timestamp measure.
 */
void hailo_vdma_engine_enable_channels(struct hailo_vdma_engine *engine, u32 bitmap,
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

/**
 * Disables the given channels bitmap in the given engine.
 *
 * @param engine - dma engine.
 * @param bitmap - channels bitmap to enable.
 * @param measure_timestamp - if set, allow interrupts timestamp measure.
 */
void hailo_vdma_engine_disable_channels(struct hailo_vdma_engine *engine, u32 bitmap)
{
    struct hailo_vdma_channel *channel = NULL;
    u8 channel_index = 0;

    engine->enabled_channels &= ~bitmap;

    for_each_vdma_channel(engine, channel, channel_index) {
        if (hailo_test_bit(channel_index, &bitmap)) {
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
    struct hailo_vdma_engine *engine, struct hailo_vdma_channel *channel, u8 transfers_completed,
    bool validation_success)
{
    u8 host_control = READ_BITS_AT_OFFSET(BYTE_SIZE * BITS_IN_BYTE, CHANNEL_CONTROL_OFFSET * BITS_IN_BYTE, ioread32(channel->host_regs));
    u8 device_control = READ_BITS_AT_OFFSET(BYTE_SIZE * BITS_IN_BYTE, CHANNEL_CONTROL_OFFSET * BITS_IN_BYTE, ioread32(channel->device_regs));

    irq_data->engine_index = engine->index;
    irq_data->channel_index = channel->index;

    irq_data->is_active = channel_control_reg_is_active(host_control) &&
        channel_control_reg_is_active(device_control);

    irq_data->transfers_completed = transfers_completed;
    irq_data->host_error = READ_BITS_AT_OFFSET(BYTE_SIZE * BITS_IN_BYTE, 0, ioread32(channel->host_regs + CHANNEL_ERROR_OFFSET));
    irq_data->device_error = READ_BITS_AT_OFFSET(BYTE_SIZE * BITS_IN_BYTE, 0, ioread32(channel->device_regs + CHANNEL_ERROR_OFFSET));
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
        u8 transfers_completed = 0;
        u16 hw_num_proc = U16_MAX;

        BUILD_BUG_ON_MSG(HAILO_VDMA_MAX_ONGOING_TRANSFERS >= U8_MAX,
            "HAILO_VDMA_MAX_ONGOING_TRANSFERS must be less than U8_MAX to use transfers_completed as u8");

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
        hw_num_proc = hailo_vdma_get_num_proc(channel->host_regs) & channel->state.desc_count_mask;

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
            transfers_completed++;
        }

        fill_channel_irq_data(&irq_data->irq_data[irq_data->channels_count],
            engine, channel, transfers_completed, validation_success);
        irq_data->channels_count++;
    }

    return 0;
}

// For all these functions - best way to optimize might be to not call the function when need to pause and then abort,
// Rather read value once and maybe save 
// This function reads and writes the register - should try to make more optimized in future
static void start_vdma_control_register(u8 __iomem *host_regs)
{
    u32 host_regs_value = ioread32(host_regs);
    iowrite32(WRITE_BITS_AT_OFFSET(BYTE_SIZE * BITS_IN_BYTE, CHANNEL_CONTROL_OFFSET * BITS_IN_BYTE, host_regs_value,
        VDMA_CHANNEL_CONTROL_START_RESUME), host_regs);
}

static void hailo_vdma_channel_pause(u8 __iomem *host_regs)
{
    u32 host_regs_value = ioread32(host_regs);
    iowrite32(WRITE_BITS_AT_OFFSET(BYTE_SIZE * BITS_IN_BYTE, CHANNEL_CONTROL_OFFSET * BITS_IN_BYTE, host_regs_value,
        VDMA_CHANNEL_CONTROL_START_PAUSE), host_regs);
}

// This function reads and writes the register - should try to make more optimized in future
static void hailo_vdma_channel_abort(u8 __iomem *host_regs)
{
    u32 host_regs_value = ioread32(host_regs);
    iowrite32(WRITE_BITS_AT_OFFSET(BYTE_SIZE * BITS_IN_BYTE, CHANNEL_CONTROL_OFFSET * BITS_IN_BYTE, host_regs_value,
        VDMA_CHANNEL_CONTROL_ABORT), host_regs);
}

int hailo_vdma_start_channel(u8 __iomem *regs, uint64_t desc_dma_address, uint32_t desc_count,
    uint8_t data_id)
{
    u16 dma_address_l = 0;
    u32 dma_address_h = 0;
    u32 desc_depth_data_id = 0;
    u8 desc_depth = ceil_log2(desc_count);

    if (((desc_dma_address & 0xFFFF) != 0) || 
         (desc_depth > DESCRIPTOR_LIST_MAX_DEPTH)) {
        return -EINVAL;
    }

    // According to spec, depth 16 is equivalent to depth 0.
    if (DESCRIPTOR_LIST_MAX_DEPTH == desc_depth) {
        desc_depth = 0;
    }

    // Stop old channel state
    hailo_vdma_stop_channel(regs);

    // Configure address, depth and id
    dma_address_l = (uint16_t)((desc_dma_address >> 16) & 0xFFFF);
    iowrite32(WRITE_BITS_AT_OFFSET(WORD_SIZE * BITS_IN_BYTE, (VDMA_CHANNEL__ADDRESS_L_OFFSET -
        VDMA_CHANNEL__ALIGNED_ADDRESS_L_OFFSET) * BITS_IN_BYTE, ioread32(regs +
        VDMA_CHANNEL__ALIGNED_ADDRESS_L_OFFSET), dma_address_l), regs + VDMA_CHANNEL__ALIGNED_ADDRESS_L_OFFSET);

    dma_address_h = (uint32_t)(desc_dma_address >> 32);
    iowrite32(dma_address_h, regs + VDMA_CHANNEL__ADDRESS_H_OFFSET);

    desc_depth_data_id = (uint32_t)(desc_depth << VDMA_CHANNEL_DESC_DEPTH_SHIFT) | 
        (data_id << VDMA_CHANNEL_DATA_ID_SHIFT);
    iowrite32(desc_depth_data_id, regs);

    start_vdma_control_register(regs);

    return 0;
}

static bool hailo_vdma_channel_is_idle(u8 __iomem *host_regs, size_t host_side_max_desc_count)
{
    // Num processed and ongoing are next to each other in the memory. 
    // Reading them both in order to save BAR reads.
    u32 host_side_num_processed_ongoing = ioread32(host_regs + CHANNEL_NUM_PROC_OFFSET);
    u16 host_side_num_processed = (host_side_num_processed_ongoing & VDMA_CHANNEL_NUM_PROCESSED_MASK);
    u16 host_side_num_ongoing = (host_side_num_processed_ongoing >> VDMA_CHANNEL_NUM_PROCESSED_WIDTH) &
        VDMA_CHANNEL_NUM_ONGOING_MASK;

    if ((host_side_num_processed % host_side_max_desc_count) == (host_side_num_ongoing % host_side_max_desc_count)) {
        return true;
    }

    return false;
}

static int hailo_vdma_wait_until_channel_idle(u8 __iomem *host_regs)
{
    bool is_idle = false;
    uint32_t check_counter = 0;

    u8 depth = (uint8_t)(READ_BITS_AT_OFFSET(VDMA_CHANNEL_DESC_DEPTH_WIDTH, VDMA_CHANNEL_DESC_DEPTH_SHIFT,
        ioread32(host_regs)));
    size_t host_side_max_desc_count = (size_t)(1 << depth);

    for (check_counter = 0; check_counter < VDMA_CHANNEL__MAX_CHECKS_CHANNEL_IS_IDLE; check_counter++) {
        is_idle = hailo_vdma_channel_is_idle(host_regs, host_side_max_desc_count);
        if (is_idle) {
            return 0;
        }
    }

    return -ETIMEDOUT;
}

void hailo_vdma_stop_channel(u8 __iomem *regs)
{
    int err = 0;
    u8 host_side_channel_regs = READ_BITS_AT_OFFSET(BYTE_SIZE * BITS_IN_BYTE, CHANNEL_CONTROL_OFFSET * BITS_IN_BYTE, ioread32(regs));

    if ((host_side_channel_regs & VDMA_CHANNEL_CONTROL_START_ABORT_PAUSE_RESUME_BITMASK) == VDMA_CHANNEL_CONTROL_ABORT_PAUSE) {
        // The channel is aborted (we set the channel to VDMA_CHANNEL_CONTROL_ABORT_PAUSE at the end of this function)
        return;
    }

    // Pause the channel
    // The channel is paused to allow for "all transfers from fetched descriptors..." to be "...completed"
    // (from PLDA PCIe refernce manual, "9.2.5 Starting a Channel and Transferring Data")
    hailo_vdma_channel_pause(regs);

    // Even if channel is stuck and not idle, force abort and return error in the end
    err = hailo_vdma_wait_until_channel_idle(regs);
    // Success oriented - if error occured print error but still abort channel
    if (err < 0) {
        pr_err("Timeout occured while waiting for channel to become idle\n");
    }

    // Abort the channel (even of hailo_vdma_wait_until_channel_idle function fails)
    hailo_vdma_channel_abort(regs);
}

bool hailo_check_channel_index(u8 channel_index, u32 src_channels_bitmask, bool is_input_channel)
{
    return is_input_channel ? hailo_test_bit(channel_index, &src_channels_bitmask) :
        (!hailo_test_bit(channel_index, &src_channels_bitmask));
}