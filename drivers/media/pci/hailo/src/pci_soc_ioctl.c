// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/
#include "pci_soc_ioctl.h"

#include "utils.h"
#include "vdma_common.h"
#include "utils/logs.h"
#include "vdma/memory.h"

#define PCI_SOC_VDMA_ENGINE_INDEX           (0)
#define PCI_SOC_WAIT_FOR_CONNECT_TIMEOUT_MS (10000)

long hailo_soc_ioctl(struct hailo_pcie_board *board, struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case HAILO_SOC_CONNECT:
        return hailo_soc_connect_ioctl(board, context, controller, arg);
    case HAILO_SOC_CLOSE:
        return hailo_soc_close_ioctl(board, controller, arg);
    default:
        hailo_err(board, "Invalid pcie EP ioctl code 0x%x (nr: %d)\n", cmd, _IOC_NR(cmd));
        return -ENOTTY;
    }
}

long hailo_soc_connect_ioctl(struct hailo_pcie_board *board, struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_soc_connect_params params;
    struct hailo_vdma_channel *input_channel = NULL;
    struct hailo_vdma_channel *output_channel = NULL;
    struct hailo_vdma_engine *vdma_engine = NULL;
    struct hailo_descriptors_list_buffer *input_descriptors_buffer = NULL;
    struct hailo_descriptors_list_buffer *output_descriptors_buffer = NULL;
    uint8_t depth = 0;
    int err = 0;
    long completion_result = 0;

    if (copy_from_user(&params, (void *)arg, sizeof(params))) {
        hailo_err(board, "copy_from_user fail\n");
        return -ENOMEM;
    }

    // TODO: have pci_ep choose the channel indexes the soc will use - for now use 0 and 16
    params.input_channel_index = 0;
    params.output_channel_index = 16;

    reinit_completion(&board->soc_connect_accepted);
    hailo_soc_write_soc_connect(&board->pcie_resources);

    // Wait for completion
    completion_result = wait_for_completion_interruptible_timeout(&board->soc_connect_accepted,
        msecs_to_jiffies(PCI_SOC_WAIT_FOR_CONNECT_TIMEOUT_MS));
    if (0 > completion_result) {
        if (0 == completion_result) {
            hailo_err(board, "Timeout waiting for connect to be accepted (timeout_ms=%d)\n", PCI_SOC_WAIT_FOR_CONNECT_TIMEOUT_MS);
            return -ETIMEDOUT;
        } else {
            hailo_info(board, "soc connect failed with err=%ld (process was interrupted or killed)\n",
                completion_result);
            return -EINTR;
        }
    }

    vdma_engine = &controller->vdma_engines[PCI_SOC_VDMA_ENGINE_INDEX];
    input_channel = &vdma_engine->channels[params.input_channel_index];
    output_channel = &vdma_engine->channels[params.output_channel_index];

    input_descriptors_buffer = hailo_vdma_find_descriptors_buffer(context, params.input_desc_handle);
    output_descriptors_buffer = hailo_vdma_find_descriptors_buffer(context, params.output_desc_handle);
    if (NULL == input_descriptors_buffer || NULL == output_descriptors_buffer) {
        hailo_dev_err(&board->pDev->dev, "input / output descriptors buffer not found \n");
        return -EINVAL;
    }

    // Make sure channels that we are accepting are not already enabled
    if (0 != (vdma_engine->enabled_channels & params.input_channel_index) ||
        0 != (vdma_engine->enabled_channels & params.output_channel_index)) {
        hailo_dev_err(&board->pDev->dev, "Trying to accept already enabled channels\n");
        return -EINVAL;
    }

    if (!is_powerof2((size_t)input_descriptors_buffer->desc_list.desc_count) ||
        !is_powerof2((size_t)output_descriptors_buffer->desc_list.desc_count)) {
        hailo_dev_err(&board->pDev->dev, "Invalid desc list size\n");
        return -EINVAL;
    }

    // configure and start input channel
    depth = ceil_log2(input_descriptors_buffer->desc_list.desc_count);
    // DMA Direction is only to get channel index - so 
    err = hailo_vdma_start_channel(input_channel->host_regs, input_descriptors_buffer->dma_address, depth,
        board->vdma.hw->ddr_data_id);
    if (err < 0) {
        hailo_dev_err(&board->pDev->dev, "Error starting vdma input channel index %u\n", params.input_channel_index);
        return -EINVAL;
    }
    
    // configure and start output channel
    depth = ceil_log2(output_descriptors_buffer->desc_list.desc_count);
    // DMA Direction is only to get channel index - so 
    err = hailo_vdma_start_channel(output_channel->host_regs, output_descriptors_buffer->dma_address, depth,
        board->vdma.hw->ddr_data_id);
    if (err < 0) {
        hailo_dev_err(&board->pDev->dev, "Error starting vdma output channel index %u\n", params.output_channel_index);
        // Close input channel
        hailo_vdma_stop_channel(input_channel->host_regs);
        return -EINVAL;
    }

    if (copy_to_user((void *)arg, &params, sizeof(params))) {
        hailo_dev_err(&board->pDev->dev, "copy_to_user fail\n");
        return -ENOMEM;
    }

    return 0;
}

long hailo_soc_close_ioctl(struct hailo_pcie_board *board, struct hailo_vdma_controller *controller, unsigned long arg)
{
    struct hailo_soc_close_params params;
    struct hailo_vdma_channel *input_channel = NULL;
    struct hailo_vdma_channel *output_channel = NULL;
    struct hailo_vdma_engine *vdma_engine = NULL;

    if (copy_from_user(&params, (void *)arg, sizeof(params))) {
        hailo_dev_err(&board->pDev->dev, "copy_from_user fail\n");
        return -ENOMEM;
    }

    vdma_engine = &controller->vdma_engines[PCI_SOC_VDMA_ENGINE_INDEX];

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

    // Close channels
    hailo_vdma_stop_channel(input_channel->host_regs);
    hailo_vdma_stop_channel(output_channel->host_regs);

    hailo_pcie_write_firmware_driver_shutdown(&board->pcie_resources);
    return 0;
}