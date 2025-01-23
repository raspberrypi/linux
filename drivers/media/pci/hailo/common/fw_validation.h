// SPDX-License-Identifier: MIT
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef PCIE_COMMON_FIRMWARE_HEADER_UTILS_H_
#define PCIE_COMMON_FIRMWARE_HEADER_UTILS_H_

#include "hailo_ioctl_common.h"
#include <linux/types.h>

#define FIRMWARE_HEADER_MAGIC_HAILO8    (0x1DD89DE0)
#define FIRMWARE_HEADER_MAGIC_HAILO15   (0xE905DAAB)
#define FIRMWARE_HEADER_MAGIC_HAILO15L  (0xF94739AB)

typedef enum {
    FIRMWARE_HEADER_VERSION_INITIAL = 0,

    /* MUST BE LAST */
    FIRMWARE_HEADER_VERSION_COUNT
} firmware_header_version_t;

typedef struct {
    u32 magic;
    u32 header_version;
    u32 firmware_major;
    u32 firmware_minor;
    u32 firmware_revision;
    u32 code_size;
} firmware_header_t;


#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4200)
#endif /* _MSC_VER */

typedef struct {
    u32 key_size;
    u32 content_size;
} secure_boot_certificate_header_t;

#ifdef _MSC_VER
#pragma warning(pop)
#endif /* _MSC_VER */

#define MINIMUM_FIRMWARE_CODE_SIZE (20*4)
#define MAXIMUM_FIRMWARE_CERT_KEY_SIZE (0x1000)
#define MAXIMUM_FIRMWARE_CERT_CONTENT_SIZE (0x1000)

int FW_VALIDATION__validate_fw_header(uintptr_t firmware_base_address,
    size_t firmware_size, u32 max_code_size, u32 *outer_consumed_firmware_offset,
    firmware_header_t **out_firmware_header, enum hailo_board_type board_type);

int FW_VALIDATION__validate_cert_header(uintptr_t firmware_base_address,
    size_t firmware_size, u32 *outer_consumed_firmware_offset, secure_boot_certificate_header_t **out_firmware_cert);

#endif
