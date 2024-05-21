// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef _HAILO_PCI_NNC_H_
#define _HAILO_PCI_NNC_H_

#include "pcie.h"

void hailo_nnc_init(struct hailo_pcie_nnc *nnc);
void hailo_nnc_finalize(struct hailo_pcie_nnc *nnc);

long hailo_nnc_ioctl(struct hailo_pcie_board *board, unsigned int cmd, unsigned long arg,
    struct file *filp, bool *should_up_board_mutex);

int hailo_nnc_file_context_init(struct hailo_pcie_board *board, struct hailo_file_context *context);
void hailo_nnc_file_context_finalize(struct hailo_pcie_board *board, struct hailo_file_context *context);

int hailo_nnc_driver_down(struct hailo_pcie_board *board);

#endif /* _HAILO_PCI_NNC_H_ */