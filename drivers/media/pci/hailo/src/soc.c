// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/
/**
 * A Hailo PCIe NNC device is a device contains a full SoC over PCIe. The SoC contains NNC (neural network core) and
 * some application processor (pci_ep).
 */

#include "soc.h"

#include "vdma_common.h"
#include "utils/logs.h"
#include "vdma/memory.h"
#include "pcie_common.h"

#include <linux/uaccess.h>

#ifndef HAILO_EMULATOR
#define PCI_SOC_CONTROL_CONNECT_TIMEOUT_MS (1000)
#else
#define PCI_SOC_CONTROL_CONNECT_TIMEOUT_MS (1000000)
#endif /* ifndef HAILO_EMULATOR */

void hailo_soc_init(struct hailo_pcie_soc *soc)
{
    init_completion(&soc->control_resp_ready);
}

long hailo_soc_ioctl(struct hailo_pcie_board *board, struct hailo_file_context *context,
    struct hailo_vdma_controller *controller, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case HAILO_SOC_CONNECT:
        return hailo_soc_connect_ioctl(board, context, controller, arg);
    case HAILO_SOC_CLOSE:
        return hailo_soc_close_ioctl(board, controller, context, arg);
    default:
        hailo_err(board, "Invalid pcie EP ioctl code 0x%x (nr: %d)\n", cmd, _IOC_NR(cmd));
        return -ENOTTY;
    }
}

static int soc_control(struct hailo_pcie_board *board,
    const struct hailo_pcie_soc_request *request,
    struct hailo_pcie_soc_response *response)
{
    int ret = 0;
    reinit_completion(&board->soc.control_resp_ready);

    hailo_pcie_soc_write_request(&board->pcie_resources, request);

    ret = wait_for_completion_interruptible_timeout(&board->soc.control_resp_ready,
        msecs_to_jiffies(PCI_SOC_CONTROL_CONNECT_TIMEOUT_MS));
    if (ret <= 0) {
        if (0 == ret) {
            hailo_err(board, "Timeout waiting for soc control (timeout_ms=%d)\n", PCI_SOC_CONTROL_CONNECT_TIMEOUT_MS);
            return -ETIMEDOUT;
        } else {
            hailo_info(board, "soc control failed with err=%d (process was interrupted or killed)\n",
                ret);
            return ret;
        }
    }

    hailo_pcie_soc_read_response(&board->pcie_resources, response);
    
    if (response->status < 0) {
        hailo_err(board, "soc control failed with status=%d\n", response->status);
        return response->status;
    }

    if (response->control_code != request->control_code) {
        hailo_err(board, "Invalid response control code %d (expected %d)\n",
            response->control_code, request->control_code);
        return -EINVAL;
    }

    return 0;
}

long hailo_soc_connect_ioctl(struct hailo_pcie_board *board, struct hailo_file_context *context,
    struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_pcie_soc_request request = {0};
    struct hailo_pcie_soc_response response = {0};
    struct hailo_soc_connect_params params;
    struct hailo_vdma_channel *input_channel = NULL;
    struct hailo_vdma_channel *output_channel = NULL;
    struct hailo_vdma_engine *vdma_engine = &controller->vdma_engines[PCI_VDMA_ENGINE_INDEX];
    struct hailo_descriptors_list_buffer *input_descriptors_buffer = NULL;
    struct hailo_descriptors_list_buffer *output_descriptors_buffer = NULL;
    int err = 0;

    if (copy_from_user(&params, (void *)arg, sizeof(params))) {
        hailo_err(board, "copy_from_user fail\n");
        return -ENOMEM;
    }

    request = (struct hailo_pcie_soc_request) {
        .control_code = HAILO_PCIE_SOC_CONTROL_CODE_CONNECT,
        .connect = {
            .port = params.port_number
        }
    };
    err = soc_control(board, &request, &response);
    if (err < 0) {
        return err;
    }

    params.input_channel_index = response.connect.input_channel_index;
    params.output_channel_index = response.connect.output_channel_index;

    if (!hailo_check_channel_index(params.input_channel_index, controller->hw->src_channels_bitmask, true)) {
        hailo_dev_err(&board->pDev->dev, "Invalid input channel index %u\n", params.input_channel_index);
        return -EINVAL;
    }

    if (!hailo_check_channel_index(params.output_channel_index, controller->hw->src_channels_bitmask, false)) {
        hailo_dev_err(&board->pDev->dev, "Invalid output channel index %u\n", params.output_channel_index);
        return -EINVAL;
    }

    input_channel = &vdma_engine->channels[params.input_channel_index];
    output_channel = &vdma_engine->channels[params.output_channel_index];

    input_descriptors_buffer = hailo_vdma_find_descriptors_buffer(&context->vdma_context, params.input_desc_handle);
    output_descriptors_buffer = hailo_vdma_find_descriptors_buffer(&context->vdma_context, params.output_desc_handle);
    if (NULL == input_descriptors_buffer || NULL == output_descriptors_buffer) {
        hailo_dev_err(&board->pDev->dev, "input / output descriptors buffer not found \n");
        return -EINVAL;
    }

    if (!is_powerof2((size_t)input_descriptors_buffer->desc_list.desc_count) ||
        !is_powerof2((size_t)output_descriptors_buffer->desc_list.desc_count)) {
        hailo_dev_err(&board->pDev->dev, "Invalid desc list size\n");
        return -EINVAL;
    }

    // configure and start input channel
    // DMA Direction is only to get channel index - so 
    err = hailo_vdma_start_channel(input_channel->host_regs, input_descriptors_buffer->dma_address, input_descriptors_buffer->desc_list.desc_count,
        board->vdma.hw->ddr_data_id);
    if (err < 0) {
        hailo_dev_err(&board->pDev->dev, "Error starting vdma input channel index %u\n", params.input_channel_index);
        return -EINVAL;
    }

    // Store the input channels state in bitmap (open)
    hailo_set_bit(params.input_channel_index, &context->soc_used_channels_bitmap);
    
    // configure and start output channel
    // DMA Direction is only to get channel index - so 
    err = hailo_vdma_start_channel(output_channel->host_regs, output_descriptors_buffer->dma_address, output_descriptors_buffer->desc_list.desc_count,
        board->vdma.hw->ddr_data_id);
    if (err < 0) {
        hailo_dev_err(&board->pDev->dev, "Error starting vdma output channel index %u\n", params.output_channel_index);
        // Close input channel
        hailo_vdma_stop_channel(input_channel->host_regs);
        return -EINVAL;
    }

    // Store the output channels state in bitmap (open)
    hailo_set_bit(params.output_channel_index, &context->soc_used_channels_bitmap);

    if (copy_to_user((void *)arg, &params, sizeof(params))) {
        hailo_dev_err(&board->pDev->dev, "copy_to_user fail\n");
        return -ENOMEM;
    }

    return 0;
}

static int close_channels(struct hailo_pcie_board *board, u32 channels_bitmap)
{
    struct hailo_pcie_soc_request request = {0};
    struct hailo_pcie_soc_response response = {0};
    struct hailo_vdma_engine *engine = &board->vdma.vdma_engines[PCI_VDMA_ENGINE_INDEX];
    struct hailo_vdma_channel *channel = NULL;
    u8 channel_index = 0;

    hailo_info(board, "Closing channels bitmap 0x%x\n", channels_bitmap);
    for_each_vdma_channel(engine, channel, channel_index) {
        if (hailo_test_bit(channel_index, &channels_bitmap)) {
            hailo_vdma_stop_channel(channel->host_regs);
        }
    }

    request = (struct hailo_pcie_soc_request) {
        .control_code = HAILO_PCIE_SOC_CONTROL_CODE_CLOSE,
        .close = {
            .channels_bitmap = channels_bitmap
        }
    };
    return soc_control(board, &request, &response);
}

long hailo_soc_close_ioctl(struct hailo_pcie_board *board, struct hailo_vdma_controller *controller, 
    struct hailo_file_context *context, unsigned long arg)
{
    struct hailo_soc_close_params params;
    u32 channels_bitmap = 0;
    int err = 0;

    if (copy_from_user(&params, (void *)arg, sizeof(params))) {
        hailo_dev_err(&board->pDev->dev, "copy_from_user fail\n");
        return -ENOMEM;
    }

    // TOOD: check channels are connected

    channels_bitmap = (1 << params.input_channel_index) | (1 << params.output_channel_index);

    err = close_channels(board, channels_bitmap);
    if (0 != err) {
        hailo_dev_err(&board->pDev->dev, "Error closing channels\n");
        return err;
    }

    // Store the channel state in bitmap (closed)
    hailo_clear_bit(params.input_channel_index, &context->soc_used_channels_bitmap);
    hailo_clear_bit(params.output_channel_index, &context->soc_used_channels_bitmap);

    return err;
}

int hailo_soc_file_context_init(struct hailo_pcie_board *board, struct hailo_file_context *context)
{
    // Nothing to init yet
    return 0;
}

void hailo_soc_file_context_finalize(struct hailo_pcie_board *board, struct hailo_file_context *context)
{
    // close only channels connected by this (by bitmap)
    if (context->soc_used_channels_bitmap != 0) {
        close_channels(board, context->soc_used_channels_bitmap);
    }
}

int hailo_soc_driver_down(struct hailo_pcie_board *board)
{
    return close_channels(board, 0xFFFFFFFF);
}