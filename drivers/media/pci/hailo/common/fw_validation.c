// SPDX-License-Identifier: MIT
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
 **/

#include "fw_validation.h"
#include <linux/errno.h>
#include <linux/types.h>



/* when reading the firmware we don't want to read past the firmware_size,
   so we have a consumed_firmware_offset that is updated _before_ accessing data at that offset
   of firmware_base_address */
#define CONSUME_FIRMWARE(__size, __err) do {                                                    \
        consumed_firmware_offset += (u32) (__size);                                        \
        if ((firmware_size < (__size)) || (firmware_size < consumed_firmware_offset)) {         \
            err = __err;                                                                        \
            goto exit;                                                                          \
        }                                                                                       \
    } while(0)

int FW_VALIDATION__validate_fw_header(uintptr_t firmware_base_address,
    size_t firmware_size, u32 max_code_size, u32 *outer_consumed_firmware_offset,
    firmware_header_t **out_firmware_header, enum hailo_board_type board_type)
{
    int err = -EINVAL;
    firmware_header_t *firmware_header = NULL;
    u32 consumed_firmware_offset = *outer_consumed_firmware_offset;
    u32 expected_firmware_magic = 0;

    firmware_header = (firmware_header_t *) (firmware_base_address + consumed_firmware_offset);
    CONSUME_FIRMWARE(sizeof(firmware_header_t), -EINVAL);

    switch (board_type) {
    case HAILO_BOARD_TYPE_HAILO8:
        expected_firmware_magic = FIRMWARE_HEADER_MAGIC_HAILO8;
        break;
    case HAILO_BOARD_TYPE_HAILO10H_LEGACY:
    case HAILO_BOARD_TYPE_HAILO15:
    case HAILO_BOARD_TYPE_HAILO10H:
        expected_firmware_magic = FIRMWARE_HEADER_MAGIC_HAILO15;
        break;
    case HAILO_BOARD_TYPE_PLUTO:
        expected_firmware_magic = FIRMWARE_HEADER_MAGIC_PLUTO;
        break;
    default:
        err = -EINVAL;
        goto exit;
    }

    if (expected_firmware_magic != firmware_header->magic) {
        err = -EINVAL;
        goto exit;
    }

    /* Validate that the firmware header version is supported */
    switch(firmware_header->header_version) {
        case FIRMWARE_HEADER_VERSION_INITIAL:
            break;
        default:
            err = -EINVAL;
            goto exit;
            break;
    }

    if (MINIMUM_FIRMWARE_CODE_SIZE > firmware_header->code_size) {
        err = -EINVAL;
        goto exit;
    }

    if (max_code_size < firmware_header->code_size) {
        err = -EINVAL;
        goto exit;
    }

    CONSUME_FIRMWARE(firmware_header->code_size, -EINVAL);

    *outer_consumed_firmware_offset = consumed_firmware_offset;
    *out_firmware_header = firmware_header;
    err = 0;

exit:
    return err;
}

int FW_VALIDATION__validate_cert_header(uintptr_t firmware_base_address,
    size_t firmware_size, u32 *outer_consumed_firmware_offset, secure_boot_certificate_t **out_firmware_cert)
{

    secure_boot_certificate_t *firmware_cert = NULL;
    int err = -EINVAL;
    u32 consumed_firmware_offset = *outer_consumed_firmware_offset;

    firmware_cert = (secure_boot_certificate_t *) (firmware_base_address + consumed_firmware_offset);
    CONSUME_FIRMWARE(sizeof(secure_boot_certificate_t), -EINVAL);

    if ((MAXIMUM_FIRMWARE_CERT_KEY_SIZE < firmware_cert->key_size) ||
        (MAXIMUM_FIRMWARE_CERT_CONTENT_SIZE < firmware_cert->content_size)) {
        err = -EINVAL;
        goto exit;
    }

    CONSUME_FIRMWARE(firmware_cert->key_size, -EINVAL);
    CONSUME_FIRMWARE(firmware_cert->content_size, -EINVAL);

    *outer_consumed_firmware_offset = consumed_firmware_offset;
    *out_firmware_cert = firmware_cert;
    err = 0;

exit:
    return err;
}

