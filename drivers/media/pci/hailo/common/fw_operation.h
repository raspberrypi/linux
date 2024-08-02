// SPDX-License-Identifier: MIT
/**
 * Copyright (c) 2022 Hailo Technologies Ltd. All rights reserved.
**/

#ifndef _HAILO_COMMON_FIRMWARE_OPERATION_H_
#define _HAILO_COMMON_FIRMWARE_OPERATION_H_

#include "hailo_resource.h"

#define DEBUG_BUFFER_TOTAL_SIZE (4*1024)

#ifdef __cplusplus
extern "C" {
#endif

int hailo_read_firmware_notification(struct hailo_resource *resource, struct hailo_d2h_notification *notification);

long hailo_read_firmware_log(struct hailo_resource *fw_logger_resource, struct hailo_read_log_params *params);

#ifdef __cplusplus
}
#endif

#endif /* _HAILO_COMMON_FIRMWARE_OPERATION_H_ */
