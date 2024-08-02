// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef _HAILO_PCI_SOC_IOCTL_H_
#define _HAILO_PCI_SOC_IOCTL_H_

#include "vdma/ioctl.h"
#include "pcie.h"


long hailo_soc_ioctl(struct hailo_pcie_board *board, struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, unsigned int cmd, unsigned long arg);
long hailo_soc_connect_ioctl(struct hailo_pcie_board *board, struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller, unsigned long arg);
long hailo_soc_close_ioctl(struct hailo_pcie_board *board, struct hailo_vdma_controller *controller, unsigned long arg);

#endif // _HAILO_PCI_SOC_IOCTL_H_