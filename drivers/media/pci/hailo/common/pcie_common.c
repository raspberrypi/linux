// SPDX-License-Identifier: MIT
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/

#include "pcie_common.h"
#include "fw_operation.h"
#include "soc_structs.h"

#include <linux/errno.h>
#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/device.h>


#define BSC_IMASK_HOST (0x0188)
#define BCS_ISTATUS_HOST (0x018C)
#define BCS_SOURCE_INTERRUPT_PER_CHANNEL (0x400)
#define BCS_DESTINATION_INTERRUPT_PER_CHANNEL (0x500)

#define PO2_ROUND_UP(size, alignment) ((size + alignment-1) & ~(alignment-1))

#define ATR_PARAM               (0x17)
#define ATR_SRC_ADDR            (0x0)
#define ATR_TRSL_PARAM          (6)
#define ATR_TABLE_SIZE          (0x1000u)
#define ATR_TABLE_SIZE_MASK     (0x1000u - 1)

#define ATR0_PCIE_BRIDGE_OFFSET (0x700)

#define MAXIMUM_APP_FIRMWARE_CODE_SIZE (0x40000)
#define MAXIMUM_CORE_FIRMWARE_CODE_SIZE (0x20000)

#define FIRMWARE_LOAD_WAIT_MAX_RETRIES (100)
#define FIRMWARE_LOAD_SLEEP_MS         (50)

#define PCIE_REQUEST_SIZE_OFFSET (0x640)

#define PCIE_CONFIG_VENDOR_OFFSET (0x0098)

#define HAILO_PCIE_HOST_DMA_DATA_ID (0)
#define HAILO_PCIE_DMA_DEVICE_INTERRUPTS_BITMASK    (1 << 4)
#define HAILO_PCIE_DMA_HOST_INTERRUPTS_BITMASK      (1 << 5)
#define HAILO_PCIE_DMA_SRC_CHANNELS_BITMASK         (0x0000FFFF)

#define HAILO_PCIE_MAX_ATR_TABLE_INDEX (3)

#define MAX_FILES_PER_STAGE (4)

#define BOOT_STATUS_UNINITIALIZED (0x1)

struct hailo_fw_addresses {
    u32 boot_fw_header;
    u32 app_fw_code_ram_base;
    u32 boot_key_cert;
    u32 boot_cont_cert;
    u32 core_code_ram_base;
    u32 core_fw_header;
    u32 atr0_trsl_addr1;
    u32 raise_ready_offset;
    u32 boot_status;
};

struct loading_stage {
    const struct hailo_file_batch *batch;
    u32 trigger_address;
};

struct hailo_board_compatibility {
    struct hailo_fw_addresses fw_addresses;
    const struct loading_stage stages[MAX_LOADING_STAGES];
};

static const struct hailo_file_batch hailo10h_files_stg1[] = {
    {
        .filename = "hailo/hailo10h/customer_certificate.bin",
        .address = 0xA0000,
        .max_size = 0x8004,
        .is_mandatory = true,
        .has_header = false,
        .has_core = false
    },
    {
        .filename = "hailo/hailo10h/u-boot.dtb.signed",
        .address = 0xA8004,
        .max_size = 0x20000,
        .is_mandatory = true,
        .has_header = false,
        .has_core = false
    },
    {
        .filename = "hailo/hailo10h/scu_fw.bin",
        .address = 0x20000,
        .max_size = 0x40000,
        .is_mandatory = true,
        .has_header = true,
        .has_core = false
    },
    {
        .filename = NULL,
        .address = 0x00,
        .max_size = 0x00,
        .is_mandatory = false,
        .has_header = false,
        .has_core = false
    }
};

static const struct hailo_file_batch hailo10h_files_stg2[] = {
    {
        .filename = "hailo/hailo10h/u-boot-spl.bin",
        .address = 0x85000000,
        .max_size = 0x1000000,
        .is_mandatory = true,
        .has_header = false,
        .has_core = false
    },
    {
        .filename = "hailo/hailo10h/u-boot-tfa.itb",
        .address = 0x86000000,
        .max_size = 0x1000000,
        .is_mandatory = true,
        .has_header = false,
        .has_core = false
    },
    {
        .filename = "hailo/hailo10h/fitImage",
        .address = 0x87000000,
        .max_size = 0x1000000,
        .is_mandatory = true,
        .has_header = false,
        .has_core = false
    },
    {
        .filename = "hailo/hailo10h/core-image-minimal-hailo10-m2.ext4.gz",
        .address = 0x88000000,
        .max_size = 0x20000000, // Max size 512MB
        .is_mandatory = true,
        .has_header = false,
        .has_core = false
    },
};

// If loading linux from EMMC - only need few files from second batch (u-boot-spl.bin and u-boot-tfa.itb)
static const struct hailo_file_batch hailo10h_files_stg2_linux_in_emmc[] = {
    {
        .filename = "hailo/hailo10h/u-boot-spl.bin",
        .address = 0x85000000,
        .max_size = 0x1000000,
        .is_mandatory = true,
        .has_header = false,
        .has_core = false
    },
    {
        .filename = "hailo/hailo10h/u-boot-tfa.itb",
        .address = 0x86000000,
        .max_size = 0x1000000,
        .is_mandatory = true,
        .has_header = false,
        .has_core = false
    },
    {
        .filename = NULL,
        .address = 0x00,
        .max_size = 0x00,
        .is_mandatory = false,
        .has_header = false,
        .has_core = false
    },
};

static const struct hailo_file_batch hailo8_files_stg1[] = {
    {
        .filename = "hailo/hailo8_fw.4.19.0.bin",
        .address = 0x20000,
        .max_size = 0x50000,
        .is_mandatory = true,
        .has_header = true,
        .has_core = true
    },
    {
        .filename = "hailo/hailo8_board_cfg.bin",
        .address = 0x60001000,
        .max_size = PCIE_HAILO8_BOARD_CFG_MAX_SIZE,
        .is_mandatory = false,
        .has_header = false,
        .has_core = false
    },
    {
        .filename = "hailo/hailo8_fw_cfg.bin",
        .address = 0x60001500,
        .max_size = PCIE_HAILO8_FW_CFG_MAX_SIZE,
        .is_mandatory = false,
        .has_header = false,
        .has_core = false
    },
    {
        .filename = NULL,
        .address = 0x00,
        .max_size = 0x00,
        .is_mandatory = false,
        .has_header = false,
        .has_core = false
    }
};

static const struct hailo_file_batch hailo10h_legacy_files_stg1[] = {
    {
        .filename = "hailo/hailo15_fw.bin",
        .address = 0x20000,
        .max_size = 0x100000,
        .is_mandatory = true,
        .has_header = true,
        .has_core = true
    },
    {
        .filename = NULL,
        .address = 0x00,
        .max_size = 0x00,
        .is_mandatory = false,
        .has_header = false,
        .has_core = false
    }
};

static const struct hailo_file_batch pluto_files_stg1[] = {
    {
        .filename = "hailo/pluto_fw.bin",
        .address = 0x20000,
        .max_size = 0x100000,
        .is_mandatory = true,
        .has_header = true,
        .has_core = true
    },
    {
        .filename = NULL,
        .address = 0x00,
        .max_size = 0x00,
        .is_mandatory = false,
        .has_header = false,
        .has_core = false
    }
};

static const struct hailo_board_compatibility compat[HAILO_BOARD_TYPE_COUNT] = {
    [HAILO_BOARD_TYPE_HAILO8] = {
        .fw_addresses = {
            .boot_fw_header = 0xE0030,
            .boot_key_cert = 0xE0048,
            .boot_cont_cert = 0xE0390,
            .app_fw_code_ram_base = 0x60000,
            .core_code_ram_base = 0xC0000,
            .core_fw_header = 0xA0000,
            .atr0_trsl_addr1 = 0x60000000,
            .raise_ready_offset = 0x1684,
            .boot_status = 0xe0000,
        },
        .stages = {
            {
                .batch = hailo8_files_stg1,
                .trigger_address = 0xE0980
            },
        },
    },
    [HAILO_BOARD_TYPE_HAILO10H_LEGACY] = {
        .fw_addresses = {
            .boot_fw_header = 0x88000,
            .boot_key_cert = 0x88018,
            .boot_cont_cert = 0x886a8,
            .app_fw_code_ram_base = 0x20000,
            .core_code_ram_base = 0x60000,
            .core_fw_header = 0xC0000,
            .atr0_trsl_addr1 = 0x000BE000,
            .raise_ready_offset = 0x1754,
            .boot_status = 0x80000,
        },
        .stages = {
            {
                .batch = hailo10h_legacy_files_stg1,
                .trigger_address = 0x88c98
            },
        },
    },
    [HAILO_BOARD_TYPE_HAILO10H] = {
        .fw_addresses = {
            .boot_fw_header = 0x88000,
            .boot_key_cert = 0x88018,
            .boot_cont_cert = 0x886a8,
            .app_fw_code_ram_base = 0x20000,
            .core_code_ram_base = 0,
            .core_fw_header = 0,
            .atr0_trsl_addr1 = 0x000BE000,
            .raise_ready_offset = 0x1754,
            .boot_status = 0x80000,
        },
        .stages = {
            {
                .batch = hailo10h_files_stg1,
                .trigger_address = 0x88c98
            },
            {
                .batch = hailo10h_files_stg2,
                .trigger_address = 0x84000000
            },
            {
                .batch = hailo10h_files_stg2_linux_in_emmc,
                .trigger_address = 0x84000000
            },
        },
    },
    // HRT-11344 : none of these matter except raise_ready_offset seeing as we load fw seperately - not through driver
    // After implementing bootloader put correct values here
    [HAILO_BOARD_TYPE_PLUTO] = {
        .fw_addresses = {
            .boot_fw_header = 0x88000,
            .boot_key_cert = 0x88018,
            .boot_cont_cert = 0x886a8,
            .app_fw_code_ram_base = 0x20000,
            .core_code_ram_base = 0x60000,
            .core_fw_header = 0xC0000,
            .atr0_trsl_addr1 = 0x000BE000,
            // NOTE: After they update hw consts - check register fw_access_interrupt_w1s of pcie_config
            .raise_ready_offset = 0x174c,
            .boot_status = 0x80000,
        },
        .stages = {
            {
                .batch = pluto_files_stg1,
                .trigger_address = 0x88c98
            },
        },
    }
};


bool hailo_pcie_read_interrupt(struct hailo_pcie_resources *resources, struct hailo_pcie_interrupt_source *source)
{
    u32 channel_data_source = 0;
    u32 channel_data_dest = 0;
    memset(source, 0, sizeof(*source));

    source->interrupt_bitmask = hailo_resource_read32(&resources->config, BCS_ISTATUS_HOST);
    if (0 == source->interrupt_bitmask) {
        return false;
    }

    // clear signal
    hailo_resource_write32(&resources->config, BCS_ISTATUS_HOST, source->interrupt_bitmask);

    if (source->interrupt_bitmask & BCS_ISTATUS_HOST_VDMA_SRC_IRQ_MASK) {
        channel_data_source = hailo_resource_read32(&resources->config, BCS_SOURCE_INTERRUPT_PER_CHANNEL);
        hailo_resource_write32(&resources->config, BCS_SOURCE_INTERRUPT_PER_CHANNEL, channel_data_source);
    }
    if (source->interrupt_bitmask & BCS_ISTATUS_HOST_VDMA_DEST_IRQ_MASK) {
        channel_data_dest = hailo_resource_read32(&resources->config, BCS_DESTINATION_INTERRUPT_PER_CHANNEL);
        hailo_resource_write32(&resources->config, BCS_DESTINATION_INTERRUPT_PER_CHANNEL, channel_data_dest);
    }
    source->vdma_channels_bitmap = channel_data_source | channel_data_dest;

    return true;
}

int hailo_pcie_write_firmware_control(struct hailo_pcie_resources *resources, const struct hailo_fw_control *command)
{
    int err = 0;
    u32 request_size = 0;
    u8 fw_access_value = FW_ACCESS_APP_CPU_CONTROL_MASK;
    const struct hailo_fw_addresses *fw_addresses = &(compat[resources->board_type].fw_addresses);

    if (!hailo_pcie_is_firmware_loaded(resources)) {
        return -ENODEV;
    }

    // Copy md5 + buffer_len + buffer
    request_size = sizeof(command->expected_md5) + sizeof(command->buffer_len) + command->buffer_len;
    err = hailo_resource_write_buffer(&resources->fw_access, 0, PO2_ROUND_UP(request_size, FW_CODE_SECTION_ALIGNMENT),
        command);
    if (err < 0) {
        return err;
    }

    // Raise the bit for the CPU that will handle the control
    fw_access_value = (command->cpu_id == HAILO_CPU_ID_CPU1) ? FW_ACCESS_CORE_CPU_CONTROL_MASK :
        FW_ACCESS_APP_CPU_CONTROL_MASK;
    
    // Raise ready flag to FW
    hailo_resource_write32(&resources->fw_access, fw_addresses->raise_ready_offset, (u32)fw_access_value);
    return 0;
}

int hailo_pcie_read_firmware_control(struct hailo_pcie_resources *resources, struct hailo_fw_control *command)
{
    u32 response_header_size = 0;

    // Copy response md5 + buffer_len
    response_header_size = sizeof(command->expected_md5) + sizeof(command->buffer_len);

    hailo_resource_read_buffer(&resources->fw_access, PCIE_REQUEST_SIZE_OFFSET, response_header_size, command);

    if (sizeof(command->buffer) < command->buffer_len) {
        return -EINVAL;
    }

    // Copy response buffer
    hailo_resource_read_buffer(&resources->fw_access, PCIE_REQUEST_SIZE_OFFSET + (size_t)response_header_size,
        command->buffer_len, &command->buffer);

    return 0;
}

void hailo_pcie_write_firmware_driver_shutdown(struct hailo_pcie_resources *resources)
{
    const struct hailo_fw_addresses *fw_addresses = &(compat[resources->board_type].fw_addresses);
    const u32 fw_access_value = FW_ACCESS_DRIVER_SHUTDOWN_MASK;

    // Write shutdown flag to FW
    hailo_resource_write32(&resources->fw_access, fw_addresses->raise_ready_offset, fw_access_value);
}

int hailo_pcie_configure_atr_table(struct hailo_resource *bridge_config, u64 trsl_addr, u32 atr_index)
{
    size_t offset = 0;
    struct hailo_atr_config atr = {
        .atr_param = (ATR_PARAM | (atr_index << 12)),
        .atr_src = ATR_SRC_ADDR,
        .atr_trsl_addr_1 = (u32)(trsl_addr & 0xFFFFFFFF),
        .atr_trsl_addr_2 = (u32)(trsl_addr >> 32),
        .atr_trsl_param = ATR_TRSL_PARAM
    };

    BUG_ON(HAILO_PCIE_MAX_ATR_TABLE_INDEX < atr_index);
    offset = ATR0_PCIE_BRIDGE_OFFSET + (atr_index * 0x20);

    return hailo_resource_write_buffer(bridge_config, offset, sizeof(atr), (void*)&atr);
}

void hailo_pcie_read_atr_table(struct hailo_resource *bridge_config, struct hailo_atr_config *atr, u32 atr_index)
{
    size_t offset = 0;

    BUG_ON(HAILO_PCIE_MAX_ATR_TABLE_INDEX < atr_index);
    offset = ATR0_PCIE_BRIDGE_OFFSET + (atr_index * 0x20);
    
    hailo_resource_read_buffer(bridge_config, offset, sizeof(*atr), (void*)atr);
}

static void write_memory_chunk(struct hailo_pcie_resources *resources,
    hailo_ptr_t dest, u32 dest_offset, const void *src, u32 len)
{
    u32 ATR_INDEX = 0;
    BUG_ON(dest_offset + len > (u32)resources->fw_access.size);

    (void)hailo_pcie_configure_atr_table(&resources->config, dest, ATR_INDEX);
    (void)hailo_resource_write_buffer(&resources->fw_access, dest_offset, len, src);
}

static void read_memory_chunk(
    struct hailo_pcie_resources *resources, hailo_ptr_t src, u32 src_offset, void *dest, u32 len)
{
    u32 ATR_INDEX = 0;
    BUG_ON(src_offset + len > (u32)resources->fw_access.size);

    (void)hailo_pcie_configure_atr_table(&resources->config, src, ATR_INDEX);
    (void)hailo_resource_read_buffer(&resources->fw_access, src_offset, len, dest);
}

// Note: this function modify the device ATR table (that is also used by the firmware for control and vdma).
// Use with caution, and restore the original atr if needed.
static void write_memory(struct hailo_pcie_resources *resources, hailo_ptr_t dest, const void *src, u32 len)
{
    struct hailo_atr_config previous_atr = {0};
    hailo_ptr_t base_address = (dest & ~ATR_TABLE_SIZE_MASK);
    u32 chunk_len = 0;
    u32 offset = 0;
    u32 ATR_INDEX = 0;

    // Store previous ATR (Read/write modify the ATR).
    hailo_pcie_read_atr_table(&resources->config, &previous_atr, ATR_INDEX);

    if (base_address != dest) {
        // Data is not aligned, write the first chunk
        chunk_len = min((u32)(base_address + ATR_TABLE_SIZE - dest), len);
        write_memory_chunk(resources, base_address, (u32)(dest - base_address), src, chunk_len);
        offset += chunk_len;
    }

    while (offset < len) {
        chunk_len = min(len - offset, ATR_TABLE_SIZE);
        write_memory_chunk(resources, dest + offset, 0, (const u8*)src + offset, chunk_len);
        offset += chunk_len;
    }

    (void)hailo_pcie_configure_atr_table(&resources->config,
        (((u64)(previous_atr.atr_trsl_addr_2) << 32) | previous_atr.atr_trsl_addr_1), ATR_INDEX);
}

// Note: this function modify the device ATR table (that is also used by the firmware for control and vdma).
// Use with caution, and restore the original atr if needed.
static void read_memory(struct hailo_pcie_resources *resources, hailo_ptr_t src, void *dest, u32 len)
{
    struct hailo_atr_config previous_atr = {0};
    hailo_ptr_t base_address = (src & ~ATR_TABLE_SIZE_MASK);
    u32 chunk_len = 0;
    u32 offset = 0;
    u32 ATR_INDEX = 0;

    // Store previous ATR (Read/write modify the ATR).
    hailo_pcie_read_atr_table(&resources->config, &previous_atr, ATR_INDEX);

    if (base_address != src) {
        // Data is not aligned, write the first chunk
        chunk_len = min((u32)(base_address + ATR_TABLE_SIZE - src), len);
        read_memory_chunk(resources, base_address, (u32)(src - base_address), dest, chunk_len);
        offset += chunk_len;
    }

    while (offset < len) {
        chunk_len = min(len - offset, ATR_TABLE_SIZE);
        read_memory_chunk(resources, src + offset, 0, (u8*)dest + offset, chunk_len);
        offset += chunk_len;
    }

    (void)hailo_pcie_configure_atr_table(&resources->config,
        (((u64)(previous_atr.atr_trsl_addr_2) << 32) | previous_atr.atr_trsl_addr_1), ATR_INDEX);
}

static void hailo_write_app_firmware(struct hailo_pcie_resources *resources, firmware_header_t *fw_header,
    secure_boot_certificate_header_t *fw_cert)
{
    const struct hailo_fw_addresses *fw_addresses = &(compat[resources->board_type].fw_addresses);
    u8 *fw_code = ((u8*)fw_header + sizeof(firmware_header_t));
    u8 *key_data = ((u8*)fw_cert + sizeof(secure_boot_certificate_header_t));
    u8 *content_data = key_data + fw_cert->key_size;

    write_memory(resources, fw_addresses->boot_fw_header, fw_header, sizeof(firmware_header_t));

    write_memory(resources, fw_addresses->app_fw_code_ram_base, fw_code, fw_header->code_size);

    write_memory(resources, fw_addresses->boot_key_cert, key_data, fw_cert->key_size);
    write_memory(resources, fw_addresses->boot_cont_cert, content_data, fw_cert->content_size);
}

static void hailo_write_core_firmware(struct hailo_pcie_resources *resources, firmware_header_t *fw_header)
{
    const struct hailo_fw_addresses *fw_addresses = &(compat[resources->board_type].fw_addresses);
    void *fw_code = (void*)((u8*)fw_header + sizeof(firmware_header_t));

    write_memory(resources, fw_addresses->core_code_ram_base, fw_code, fw_header->code_size);
    write_memory(resources, fw_addresses->core_fw_header, fw_header, sizeof(firmware_header_t));
}

void hailo_trigger_firmware_boot(struct hailo_pcie_resources *resources, u32 address)
{
    u32 pcie_finished = 1;

    write_memory(resources, address, (void*)&pcie_finished, sizeof(pcie_finished));
}

u32 hailo_get_boot_status(struct hailo_pcie_resources *resources)
{
    u32 boot_status = 0;
    const struct hailo_fw_addresses *fw_addresses = &(compat[resources->board_type].fw_addresses);

    read_memory(resources, fw_addresses->boot_status, &boot_status, sizeof(boot_status));

    return boot_status;
}

/**
* Validates the FW headers.
* @param[in] address                    Address of the firmware.
* @param[in] firmware_size              Size of the firmware.
* @param[out] out_app_firmware_header   (optional) App firmware header
* @param[out] out_core_firmware_header  (optional) Core firmware header
* @param[out] out_firmware_cert         (optional) Firmware certificate header
*/
static int FW_VALIDATION__validate_fw_headers(uintptr_t firmware_base_address, size_t firmware_size,
    firmware_header_t **out_app_firmware_header, firmware_header_t **out_core_firmware_header,
    secure_boot_certificate_header_t **out_firmware_cert, enum hailo_board_type board_type)
{
    firmware_header_t *app_firmware_header = NULL;
    firmware_header_t *core_firmware_header = NULL;
    secure_boot_certificate_header_t *firmware_cert = NULL;
    int err = -EINVAL;
    u32 consumed_firmware_offset = 0;

    err = FW_VALIDATION__validate_fw_header(firmware_base_address, firmware_size, MAXIMUM_APP_FIRMWARE_CODE_SIZE,
        &consumed_firmware_offset, &app_firmware_header, board_type);
    if (0 != err) {
        err = -EINVAL;
        goto exit;
    }

    err = FW_VALIDATION__validate_cert_header(firmware_base_address, firmware_size,
        &consumed_firmware_offset, &firmware_cert);
    if (0 != err) {
        err = -EINVAL;
        goto exit;
    }

    // Not validating with HAILO10H since core firmware doesn't loaded over pcie
    if (HAILO_BOARD_TYPE_HAILO10H != board_type) {
        err = FW_VALIDATION__validate_fw_header(firmware_base_address, firmware_size, MAXIMUM_CORE_FIRMWARE_CODE_SIZE,
            &consumed_firmware_offset, &core_firmware_header, board_type);
        if (0 != err) {
            err = -EINVAL;
            goto exit;
        }
    }

    if (consumed_firmware_offset != firmware_size) {
        /* it is an error if there is leftover data after the last firmware header */
        err = -EINVAL;
        goto exit;
    }

    /* the out params are all optional */
    if (NULL != out_app_firmware_header) {
        *out_app_firmware_header = app_firmware_header;
    }
    if (NULL != out_firmware_cert) {
        *out_firmware_cert = firmware_cert;
    }
    if (NULL != out_core_firmware_header) {
        *out_core_firmware_header = core_firmware_header;
    }
    err = 0;

exit:
    return err;
}

static int write_single_file(struct hailo_pcie_resources *resources, const struct hailo_file_batch *file_info, struct device *dev)
{
    const struct firmware *firmware = NULL;
    firmware_header_t *app_firmware_header = NULL;
    secure_boot_certificate_header_t *firmware_cert = NULL;
    firmware_header_t *core_firmware_header = NULL;
    int err = 0;

    err = request_firmware_direct(&firmware, file_info->filename, dev);
    if (err < 0) {
        return err;
    }

    if (firmware->size > file_info->max_size) {
        release_firmware(firmware);
        return -EFBIG;
    }

    if (file_info->has_header) {
        err = FW_VALIDATION__validate_fw_headers((uintptr_t)firmware->data, firmware->size,
            &app_firmware_header, &core_firmware_header, &firmware_cert, resources->board_type);
        if (err < 0) {
            release_firmware(firmware);
            return err;
        }

        hailo_write_app_firmware(resources, app_firmware_header, firmware_cert);
        if (file_info->has_core) {
            hailo_write_core_firmware(resources, core_firmware_header);
        }
    } else {
        write_memory(resources, file_info->address, (void*)firmware->data, firmware->size);
    }

    release_firmware(firmware);

    return 0;
}

int hailo_pcie_write_firmware_batch(struct device *dev, struct hailo_pcie_resources *resources, u32 stage)
{
    const struct hailo_file_batch *files_batch = compat[resources->board_type].stages[stage].batch;
    int file_index = 0;
    int err = 0;

    for (file_index = 0; file_index < MAX_FILES_PER_STAGE; file_index++)
    {
        if (NULL == files_batch[file_index].filename) {
            break;
        }

        dev_notice(dev, "Writing file %s\n", files_batch[file_index].filename);

        err = write_single_file(resources, &files_batch[file_index], dev);
        if (err < 0) {
            pr_warn("Failed to write file %s\n", files_batch[file_index].filename);
            if (files_batch[file_index].is_mandatory) {
                return err;
            }
        }

        dev_notice(dev, "File %s written successfully\n", files_batch[file_index].filename);
    }

    hailo_trigger_firmware_boot(resources, compat[resources->board_type].stages[stage].trigger_address);

    return 0;
}

// TODO: HRT-14147 - remove this function
static bool hailo_pcie_is_device_ready_for_boot(struct hailo_pcie_resources *resources)
{
    return hailo_get_boot_status(resources) == BOOT_STATUS_UNINITIALIZED;
}

bool hailo_pcie_is_firmware_loaded(struct hailo_pcie_resources *resources)
{
    u32 offset;
    u32 atr_value;

    // TODO: HRT-14147
    if (HAILO_BOARD_TYPE_HAILO10H == resources->board_type) {
        return !hailo_pcie_is_device_ready_for_boot(resources);
    }

    offset = ATR0_PCIE_BRIDGE_OFFSET + offsetof(struct hailo_atr_config, atr_trsl_addr_1);
    atr_value = hailo_resource_read32(&resources->config, offset);

    return atr_value == compat[resources->board_type].fw_addresses.atr0_trsl_addr1;
}

bool hailo_pcie_wait_for_firmware(struct hailo_pcie_resources *resources)
{
    size_t retries;
    for (retries = 0; retries < FIRMWARE_LOAD_WAIT_MAX_RETRIES; retries++) {
        if (hailo_pcie_is_firmware_loaded(resources)) {
            return true;
        }

        msleep(FIRMWARE_LOAD_SLEEP_MS);
    }

    return false;
}

void hailo_pcie_update_channel_interrupts_mask(struct hailo_pcie_resources* resources, u32 channels_bitmap)
{
    size_t i = 0;
    u32 mask = hailo_resource_read32(&resources->config, BSC_IMASK_HOST);

    // Clear old channel interrupts
    mask &= ~BCS_ISTATUS_HOST_VDMA_SRC_IRQ_MASK;
    mask &= ~BCS_ISTATUS_HOST_VDMA_DEST_IRQ_MASK;
    // Set interrupt by the bitmap
    for (i = 0; i < MAX_VDMA_CHANNELS_PER_ENGINE; ++i) {
        if (hailo_test_bit(i, &channels_bitmap)) {
            // based on 18.5.2 "vDMA Interrupt Registers" in PLDA documentation
            u32 offset = (i & 16) ? 8 : 0;
            hailo_set_bit((((int)i*8) / MAX_VDMA_CHANNELS_PER_ENGINE) + offset, &mask);
        }
    }
    hailo_resource_write32(&resources->config, BSC_IMASK_HOST, mask);
}

void hailo_pcie_enable_interrupts(struct hailo_pcie_resources *resources)
{
    u32 mask = hailo_resource_read32(&resources->config, BSC_IMASK_HOST);

    hailo_resource_write32(&resources->config, BCS_ISTATUS_HOST, 0xFFFFFFFF);
    hailo_resource_write32(&resources->config, BCS_DESTINATION_INTERRUPT_PER_CHANNEL, 0xFFFFFFFF);
    hailo_resource_write32(&resources->config, BCS_SOURCE_INTERRUPT_PER_CHANNEL, 0xFFFFFFFF);

    mask |= (BCS_ISTATUS_HOST_FW_IRQ_CONTROL_MASK | BCS_ISTATUS_HOST_FW_IRQ_NOTIFICATION |
        BCS_ISTATUS_HOST_DRIVER_DOWN | BCS_ISTATUS_SOC_CONNECT_ACCEPTED | BCS_ISTATUS_SOC_CLOSED_IRQ);
    hailo_resource_write32(&resources->config, BSC_IMASK_HOST, mask);
}

void hailo_pcie_disable_interrupts(struct hailo_pcie_resources* resources)
{
    hailo_resource_write32(&resources->config, BSC_IMASK_HOST, 0);
}

static int direct_memory_transfer(struct hailo_pcie_resources *resources,
    struct hailo_memory_transfer_params *params)
{
    switch (params->transfer_direction) {
    case TRANSFER_READ:
        read_memory(resources, params->address, params->buffer, (u32)params->count);
        break;
    case TRANSFER_WRITE:
        write_memory(resources, params->address, params->buffer, (u32)params->count);
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

int hailo_pcie_memory_transfer(struct hailo_pcie_resources *resources, struct hailo_memory_transfer_params *params)
{
    if (params->count > ARRAY_SIZE(params->buffer)) {
        return -EINVAL;
    }

    switch (params->memory_type) {
    case HAILO_TRANSFER_DEVICE_DIRECT_MEMORY:
        return direct_memory_transfer(resources, params);
    case HAILO_TRANSFER_MEMORY_PCIE_BAR0:
        return hailo_resource_transfer(&resources->config, params);
    case HAILO_TRANSFER_MEMORY_PCIE_BAR2:
    case HAILO_TRANSFER_MEMORY_VDMA0:
        return hailo_resource_transfer(&resources->vdma_registers, params);
    case HAILO_TRANSFER_MEMORY_PCIE_BAR4:
        return hailo_resource_transfer(&resources->fw_access, params);
    default:
        return -EINVAL;
    }
}

bool hailo_pcie_is_device_connected(struct hailo_pcie_resources *resources)
{
    return PCI_VENDOR_ID_HAILO == hailo_resource_read16(&resources->config, PCIE_CONFIG_VENDOR_OFFSET);
}

int hailo_set_device_type(struct hailo_pcie_resources *resources)
{
    switch(resources->board_type) {
    case HAILO_BOARD_TYPE_HAILO8:
    case HAILO_BOARD_TYPE_HAILO10H_LEGACY:
    case HAILO_BOARD_TYPE_PLUTO:
        resources->accelerator_type = HAILO_ACCELERATOR_TYPE_NNC;
        break;
    case HAILO_BOARD_TYPE_HAILO10H:
        resources->accelerator_type = HAILO_ACCELERATOR_TYPE_SOC;
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

// On PCIe, just return the start address
u64 hailo_pcie_encode_desc_dma_address_range(dma_addr_t dma_address_start, dma_addr_t dma_address_end, u32 step, u8 channel_id)
{
    (void)channel_id;
    (void)dma_address_end;
    (void)step;
    return (u64)dma_address_start;
}

struct hailo_vdma_hw hailo_pcie_vdma_hw = {
    .hw_ops = {
        .encode_desc_dma_address_range = hailo_pcie_encode_desc_dma_address_range,
    },
    .ddr_data_id = HAILO_PCIE_HOST_DMA_DATA_ID,
    .device_interrupts_bitmask = HAILO_PCIE_DMA_DEVICE_INTERRUPTS_BITMASK,
    .host_interrupts_bitmask = HAILO_PCIE_DMA_HOST_INTERRUPTS_BITMASK,
    .src_channels_bitmask = HAILO_PCIE_DMA_SRC_CHANNELS_BITMASK,
};

void hailo_pcie_soc_write_request(struct hailo_pcie_resources *resources,
    const struct hailo_pcie_soc_request *request)
{
    const struct hailo_fw_addresses *fw_addresses = &(compat[resources->board_type].fw_addresses);
    BUILD_BUG_ON_MSG((sizeof(*request) % sizeof(u32)) != 0, "Request must be a multiple of 4 bytes");

    hailo_resource_write_buffer(&resources->fw_access, 0, sizeof(*request), (void*)request);
    hailo_resource_write32(&resources->fw_access, fw_addresses->raise_ready_offset, FW_ACCESS_SOC_CONTROL_MASK);
}

void hailo_pcie_soc_read_response(struct hailo_pcie_resources *resources,
    struct hailo_pcie_soc_response *response)
{
    BUILD_BUG_ON_MSG((sizeof(*response) % sizeof(u32)) != 0, "Request must be a multiple of 4 bytes");
    hailo_resource_read_buffer(&resources->fw_access, 0, sizeof(*response), response);
}
