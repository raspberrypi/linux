// SPDX-License-Identifier: MIT
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/
/**
 * Contains definitions for pcie soc to pcie ep communication
 */

#ifndef __HAILO_COMMON_SOC_STRUCTS__
#define __HAILO_COMMON_SOC_STRUCTS__

#include <linux/types.h>

#pragma pack(push, 1)

struct hailo_pcie_soc_connect_request {
    u16 port;
};

struct hailo_pcie_soc_connect_response {
    u8 input_channel_index;
    u8 output_channel_index;
};


struct hailo_pcie_soc_close_request {
    u32 channels_bitmap;
};

struct hailo_pcie_soc_close_response {
    u8 reserved;
};

enum hailo_pcie_soc_control_code {
    // Start from big initial value to ensure the right code was used (using 0
    // as initiale may cause confusion if the code was not set correctly).
    HAILO_PCIE_SOC_CONTROL_CODE_CONNECT = 0x100,
    HAILO_PCIE_SOC_CONTROL_CODE_CLOSE,
    HAILO_PCIE_SOC_CONTROL_CODE_INVALID,
};

#define HAILO_PCIE_SOC_MAX_REQUEST_SIZE_BYTES  (16)
#define HAILO_PCIE_SOC_MAX_RESPONSE_SIZE_BYTES (16)

// IRQ to signal the PCIe that the EP was closed/released
#define PCI_EP_SOC_CLOSED_IRQ       (0x00000020)
#define PCI_EP_SOC_CONNECT_RESPONSE (0x00000010)

struct hailo_pcie_soc_request {
    u32 control_code;
    union {
        struct hailo_pcie_soc_connect_request connect;
        struct hailo_pcie_soc_close_request close;
        u8 pad[HAILO_PCIE_SOC_MAX_REQUEST_SIZE_BYTES];
    };
};

struct hailo_pcie_soc_response {
    u32 control_code;
    s32 status;
    union {
        struct hailo_pcie_soc_connect_response connect;
        struct hailo_pcie_soc_close_response close;
        u8 pad[HAILO_PCIE_SOC_MAX_RESPONSE_SIZE_BYTES];
    };
};

#pragma pack(pop)

// Compile time validate function. Don't need to call it.
static inline void __validate_soc_struct_sizes(void)
{
    BUILD_BUG_ON_MSG(sizeof(struct hailo_pcie_soc_request) !=
        sizeof(u32) + HAILO_PCIE_SOC_MAX_REQUEST_SIZE_BYTES, "Invalid request size");
    BUILD_BUG_ON_MSG(sizeof(struct hailo_pcie_soc_response) !=
        sizeof(u32) + sizeof(s32) + HAILO_PCIE_SOC_MAX_RESPONSE_SIZE_BYTES, "Invalid response size");
}

#endif /* __HAILO_COMMON_SOC_STRUCTS__ */