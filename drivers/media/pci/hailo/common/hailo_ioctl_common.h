// SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) AND MIT
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef _HAILO_IOCTL_COMMON_H_
#define _HAILO_IOCTL_COMMON_H_


// This value is not easily changeable.
// For example: the channel interrupts ioctls assume we have up to 32 channels
#define MAX_VDMA_CHANNELS_PER_ENGINE    (32)
#define MAX_VDMA_ENGINES                (3)
#define SIZE_OF_VDMA_DESCRIPTOR         (16)
#define VDMA_DEST_CHANNELS_START        (16)

#define HAILO_VDMA_MAX_ONGOING_TRANSFERS (128)
#define HAILO_VDMA_MAX_ONGOING_TRANSFERS_MASK (HAILO_VDMA_MAX_ONGOING_TRANSFERS - 1)

#define CHANNEL_IRQ_TIMESTAMPS_SIZE (HAILO_VDMA_MAX_ONGOING_TRANSFERS * 2)
#define CHANNEL_IRQ_TIMESTAMPS_SIZE_MASK (CHANNEL_IRQ_TIMESTAMPS_SIZE - 1)

#define INVALID_DRIVER_HANDLE_VALUE     ((uintptr_t)-1)

// Used by windows and unix driver to raise the right CPU control handle to the FW. The same as in pcie_service FW
#define FW_ACCESS_CORE_CPU_CONTROL_SHIFT (1)
#define FW_ACCESS_CORE_CPU_CONTROL_MASK  (1 << FW_ACCESS_CORE_CPU_CONTROL_SHIFT)
#define FW_ACCESS_CONTROL_INTERRUPT_SHIFT (0)
#define FW_ACCESS_APP_CPU_CONTROL_MASK (1 << FW_ACCESS_CONTROL_INTERRUPT_SHIFT)
#define FW_ACCESS_DRIVER_SHUTDOWN_SHIFT (2)
#define FW_ACCESS_DRIVER_SHUTDOWN_MASK (1 << FW_ACCESS_DRIVER_SHUTDOWN_SHIFT)

#define INVALID_VDMA_CHANNEL (0xff)

#if !defined(__cplusplus) && defined(NTDDI_VERSION)
#include <wdm.h>
typedef ULONG uint32_t;
typedef UCHAR uint8_t;
typedef USHORT uint16_t;
typedef ULONGLONG uint64_t;
#endif /*  !defined(__cplusplus) && defined(NTDDI_VERSION) */


#ifdef _MSC_VER

#include <initguid.h>

#if !defined(bool) && !defined(__cplusplus)
typedef uint8_t bool;
#endif // !defined(bool) && !defined(__cplusplus)

#if !defined(INT_MAX)
#define INT_MAX 0x7FFFFFFF
#endif // !defined(INT_MAX)


// {d88d31f1-fede-4e71-ac2a-6ce0018c1501}
DEFINE_GUID (GUID_DEVINTERFACE_HailoKM,
    0xd88d31f1,0xfede,0x4e71,0xac,0x2a,0x6c,0xe0,0x01,0x8c,0x15,0x01);

#define HAILO_GENERAL_IOCTL_MAGIC 0
#define HAILO_VDMA_IOCTL_MAGIC    1
#define HAILO_NON_LINUX_IOCTL_MAGIC 2

#define HAILO_IOCTL_COMPATIBLE                  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)


typedef struct tCompatibleHailoIoctlParam
{
    union {
        struct {
            ULONG Size : 16;
            ULONG Code : 8;
            ULONG Type : 6;
            ULONG Read : 1;
            ULONG Write : 1;
        } bits;
        ULONG value;
    } u;
} tCompatibleHailoIoctlParam;

static ULONG FORCEINLINE _IOC_(ULONG nr, ULONG type, ULONG size, bool read, bool write)
{
    struct tCompatibleHailoIoctlParam param;
    param.u.bits.Code = nr;
    param.u.bits.Size = size;
    param.u.bits.Type = type;
    param.u.bits.Read = read ? 1 : 0;
    param.u.bits.Write = write ? 1 : 0;
    return param.u.value;
}

#define _IOW_(type,nr,size) _IOC_(nr, type, sizeof(size), true, false)
#define _IOR_(type,nr,size) _IOC_(nr, type, sizeof(size), false, true)
#define _IOWR_(type,nr,size) _IOC_(nr, type, sizeof(size), true, true)
#define _IO_(type,nr) _IOC_(nr, type, 0, false, false)

#elif defined(__linux__) // #ifdef _MSC_VER
#ifndef __KERNEL__
// include the userspace headers only if this file is included by user space program
// It is discourged to include them when compiling the driver (https://lwn.net/Articles/113349/)
#include <stdint.h>
#include <sys/types.h>
#else
#include <linux/types.h>
#include <linux/limits.h>
#include <linux/kernel.h>
#endif // ifndef __KERNEL__

#include <linux/ioctl.h>

#define _IOW_       _IOW
#define _IOR_       _IOR
#define _IOWR_      _IOWR
#define _IO_        _IO

#define HAILO_GENERAL_IOCTL_MAGIC 'g'
#define HAILO_VDMA_IOCTL_MAGIC 'v'
#define HAILO_NON_LINUX_IOCTL_MAGIC 'w'

#elif defined(__QNX__) // #ifdef _MSC_VER
#include <devctl.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <stdbool.h>

// defines for devctl
#define _IOW_   __DIOF
#define _IOR_   __DIOT
#define _IOWR_  __DIOTF
#define _IO_    __DION
#define HAILO_GENERAL_IOCTL_MAGIC   _DCMD_ALL
#define HAILO_VDMA_IOCTL_MAGIC      _DCMD_MISC
#define HAILO_NON_LINUX_IOCTL_MAGIC _DCMD_PROC

#else // #ifdef _MSC_VER
#error "unsupported platform!"
#endif

#pragma pack(push, 1)

struct hailo_channel_interrupt_timestamp {
    uint64_t timestamp_ns;
    uint16_t desc_num_processed;
};

typedef struct {
    uint16_t is_buffer_in_use;
    uint16_t buffer_len;
} hailo_d2h_buffer_details_t;

// This struct is the same as `enum dma_data_direction` (defined in linux/dma-direction)
enum hailo_dma_data_direction {
    HAILO_DMA_BIDIRECTIONAL = 0,
    HAILO_DMA_TO_DEVICE = 1,
    HAILO_DMA_FROM_DEVICE = 2,
    HAILO_DMA_NONE = 3,

    /** Max enum value to maintain ABI Integrity */
    HAILO_DMA_MAX_ENUM = INT_MAX,
};

// Enum that determines if buffer should be allocated from user space or from driver
enum hailo_allocation_mode {
    HAILO_ALLOCATION_MODE_USERSPACE = 0,
    HAILO_ALLOCATION_MODE_DRIVER    = 1,

    /** Max enum value to maintain ABI Integrity */
    HAILO_ALLOCATION_MODE_MAX_ENUM = INT_MAX,
};

/* structure used in ioctl HAILO_VDMA_BUFFER_MAP */
struct hailo_vdma_buffer_map_params {
#if defined(__linux__) || defined(_MSC_VER)
    void* user_address;                             // in
#elif defined(__QNX__)
    shm_handle_t shared_memory_handle;              // in
#else
#error "unsupported platform!"
#endif // __linux__
    size_t size;                                    // in
    enum hailo_dma_data_direction data_direction;   // in
    uintptr_t allocated_buffer_handle;              // in
    size_t mapped_handle;                           // out
};

/* structure used in ioctl HAILO_VDMA_BUFFER_UNMAP */
struct hailo_vdma_buffer_unmap_params {
    size_t mapped_handle;
};

/* structure used in ioctl HAILO_DESC_LIST_CREATE */
struct hailo_desc_list_create_params {
    size_t desc_count;          // in
    uint16_t desc_page_size;    // in
    bool is_circular;           // in
    uintptr_t desc_handle;      // out
    uint64_t dma_address;       // out
};

/* structure used in ioctl HAILO_DESC_LIST_RELEASE */
struct hailo_desc_list_release_params {
    uintptr_t desc_handle;      // in
};

/* structure used in ioctl HAILO_NON_LINUX_DESC_LIST_MMAP */
struct hailo_non_linux_desc_list_mmap_params {
    uintptr_t desc_handle;  // in
    size_t size;            // in
    void* user_address;     // out
};

/* structure used in ioctl HAILO_DESC_LIST_BIND_VDMA_BUFFER */
struct hailo_desc_list_bind_vdma_buffer_params {
    size_t buffer_handle;       // in
    size_t buffer_size;         // in
    size_t buffer_offset;       // in
    uintptr_t desc_handle;      // in
    uint8_t channel_index;      // in
    uint32_t starting_desc;     // in
};

/* structure used in ioctl HAILO_VDMA_INTERRUPTS_ENABLE */
struct hailo_vdma_interrupts_enable_params {
    uint32_t channels_bitmap_per_engine[MAX_VDMA_ENGINES];  // in
    bool enable_timestamps_measure;                         // in
};

/* structure used in ioctl HAILO_VDMA_INTERRUPTS_DISABLE */
struct hailo_vdma_interrupts_disable_params {
    uint32_t channels_bitmap_per_engine[MAX_VDMA_ENGINES];  // in
};

/* structure used in ioctl HAILO_VDMA_INTERRUPTS_WAIT */
struct hailo_vdma_interrupts_channel_data {
    uint8_t engine_index;
    uint8_t channel_index;
    bool is_active;                 // If not activate, num_processed is ignored.
    uint16_t host_num_processed;
    uint8_t host_error;             // Channel errors bits on source side
    uint8_t device_error;           // Channel errors bits on dest side
    bool validation_success;        // If the validation of the channel was successful
};

struct hailo_vdma_interrupts_wait_params {
    uint32_t channels_bitmap_per_engine[MAX_VDMA_ENGINES];          // in
    uint8_t channels_count;                                         // out
    struct hailo_vdma_interrupts_channel_data
        irq_data[MAX_VDMA_CHANNELS_PER_ENGINE * MAX_VDMA_ENGINES];  // out
};

/* structure used in ioctl HAILO_VDMA_INTERRUPTS_READ_TIMESTAMPS */
struct hailo_vdma_interrupts_read_timestamp_params {
    uint8_t engine_index;                                                               // in
    uint8_t channel_index;                                                              // in
    uint32_t timestamps_count;                                                          // out
    struct hailo_channel_interrupt_timestamp timestamps[CHANNEL_IRQ_TIMESTAMPS_SIZE];   // out
};

/* structure used in ioctl HAILO_FW_CONTROL */
#define MAX_CONTROL_LENGTH  (1500)
#define PCIE_EXPECTED_MD5_LENGTH (16)


/* structure used in ioctl	HAILO_FW_CONTROL and HAILO_READ_LOG */
enum hailo_cpu_id {
    HAILO_CPU_ID_CPU0 = 0,
    HAILO_CPU_ID_CPU1,
    HAILO_CPU_ID_NONE,

    /** Max enum value to maintain ABI Integrity */
    HAILO_CPU_MAX_ENUM = INT_MAX,
};

struct hailo_fw_control {
    // expected_md5+buffer_len+buffer must be in this order at the start of the struct
    uint8_t   expected_md5[PCIE_EXPECTED_MD5_LENGTH];
    uint32_t  buffer_len;
    uint8_t   buffer[MAX_CONTROL_LENGTH];
    uint32_t timeout_ms;
    enum hailo_cpu_id cpu_id;
};

/* structure used in ioctl HAILO_MEMORY_TRANSFER */
// Max bar transfer size gotten from ATR0_TABLE_SIZE
#define MAX_MEMORY_TRANSFER_LENGTH  (4096)

enum hailo_transfer_direction {
    TRANSFER_READ = 0,
    TRANSFER_WRITE,

    /** Max enum value to maintain ABI Integrity */
    TRANSFER_MAX_ENUM = INT_MAX,
};

enum hailo_transfer_memory_type {
    HAILO_TRANSFER_DEVICE_DIRECT_MEMORY,

    // vDMA memories
    HAILO_TRANSFER_MEMORY_VDMA0 = 0x100,
    HAILO_TRANSFER_MEMORY_VDMA1,
    HAILO_TRANSFER_MEMORY_VDMA2,

    // PCIe driver memories
    HAILO_TRANSFER_MEMORY_PCIE_BAR0 = 0x200,
    HAILO_TRANSFER_MEMORY_PCIE_BAR2 = 0x202,
    HAILO_TRANSFER_MEMORY_PCIE_BAR4 = 0x204,

    // DRAM DMA driver memories
    HAILO_TRANSFER_MEMORY_DMA_ENGINE0 = 0x300,
    HAILO_TRANSFER_MEMORY_DMA_ENGINE1,
    HAILO_TRANSFER_MEMORY_DMA_ENGINE2,

    /** Max enum value to maintain ABI Integrity */
    HAILO_TRANSFER_MEMORY_MAX_ENUM = INT_MAX,
};

struct hailo_memory_transfer_params {
    enum hailo_transfer_direction transfer_direction;   // in
    enum hailo_transfer_memory_type memory_type;        // in
    uint64_t address;                                   // in
    size_t count;                                       // in
    uint8_t buffer[MAX_MEMORY_TRANSFER_LENGTH];         // in/out
};

/* structure used in ioctl HAILO_VDMA_BUFFER_SYNC */
enum hailo_vdma_buffer_sync_type {
    HAILO_SYNC_FOR_CPU,
    HAILO_SYNC_FOR_DEVICE,

    /** Max enum value to maintain ABI Integrity */
    HAILO_SYNC_MAX_ENUM = INT_MAX,
};

struct hailo_vdma_buffer_sync_params {
    size_t handle;                              // in
    enum hailo_vdma_buffer_sync_type sync_type; // in
    size_t offset;                              // in
    size_t count;                               // in
};

/* structure used in ioctl HAILO_READ_NOTIFICATION */
#define MAX_NOTIFICATION_LENGTH  (1500)

struct hailo_d2h_notification {
    size_t buffer_len;                  // out
    uint8_t buffer[MAX_NOTIFICATION_LENGTH]; // out
};

enum hailo_board_type {
    HAILO_BOARD_TYPE_HAILO8 = 0,
    HAILO_BOARD_TYPE_HAILO15,
    HAILO_BOARD_TYPE_PLUTO,
    HAILO_BOARD_TYPE_COUNT,

    /** Max enum value to maintain ABI Integrity */
    HAILO_BOARD_TYPE_MAX_ENUM = INT_MAX
};

enum hailo_dma_type {
    HAILO_DMA_TYPE_PCIE,
    HAILO_DMA_TYPE_DRAM,

    /** Max enum value to maintain ABI Integrity */
    HAILO_DMA_TYPE_MAX_ENUM = INT_MAX,
};

struct hailo_device_properties {
    uint16_t                     desc_max_page_size;
    enum hailo_board_type        board_type;
    enum hailo_allocation_mode   allocation_mode;
    enum hailo_dma_type          dma_type;
    size_t                       dma_engines_count;
    bool                         is_fw_loaded;
#ifdef __QNX__
    pid_t                        resource_manager_pid;
#endif // __QNX__
};

struct hailo_driver_info {
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t revision_version;
};

/* structure used in ioctl HAILO_READ_LOG */
#define MAX_FW_LOG_BUFFER_LENGTH  (512)

struct hailo_read_log_params {
    enum hailo_cpu_id cpu_id;                   // in
    uint8_t buffer[MAX_FW_LOG_BUFFER_LENGTH];   // out
    size_t buffer_size;                         // in
    size_t read_bytes;                          // out
};

/* structure used in ioctl HAILO_VDMA_LOW_MEMORY_BUFFER_ALLOC */
struct hailo_allocate_low_memory_buffer_params {
    size_t      buffer_size;    // in
    uintptr_t   buffer_handle;  // out
};

/* structure used in ioctl HAILO_VDMA_LOW_MEMORY_BUFFER_FREE */
struct hailo_free_low_memory_buffer_params {
    uintptr_t  buffer_handle;  // in
};

struct hailo_mark_as_in_use_params {
    bool in_use;           // out
};

/* structure used in ioctl HAILO_VDMA_CONTINUOUS_BUFFER_ALLOC */
struct hailo_allocate_continuous_buffer_params {
    size_t buffer_size;         // in
    uintptr_t buffer_handle;    // out
    uint64_t dma_address;       // out
};

/* structure used in ioctl HAILO_VDMA_CONTINUOUS_BUFFER_FREE */
struct hailo_free_continuous_buffer_params {
    uintptr_t buffer_handle;    // in
};

/* structures used in ioctl HAILO_VDMA_LAUNCH_TRANSFER */
struct hailo_vdma_transfer_buffer {
    size_t mapped_buffer_handle;       // in
    uint32_t offset;                   // in
    uint32_t size;                     // in
};

enum hailo_vdma_interrupts_domain {
    HAILO_VDMA_INTERRUPTS_DOMAIN_NONE   = 0,
    HAILO_VDMA_INTERRUPTS_DOMAIN_DEVICE = (1 << 0),
    HAILO_VDMA_INTERRUPTS_DOMAIN_HOST   = (1 << 1),

    /** Max enum value to maintain ABI Integrity */
    HAILO_VDMA_INTERRUPTS_DOMAIN_MAX_ENUM = INT_MAX,
};

// We allow maximum 2 buffers per transfer since we may have an extra buffer 
// to make sure each buffer is aligned to page size.
#define HAILO_MAX_BUFFERS_PER_SINGLE_TRANSFER (2)

struct hailo_vdma_launch_transfer_params {
    uint8_t engine_index;                                               // in
    uint8_t channel_index;                                              // in

    uintptr_t desc_handle;                                              // in
    uint32_t starting_desc;                                             // in

    bool should_bind;                                                   // in, if false, assumes buffer already bound.
    uint8_t buffers_count;                                              // in
    struct hailo_vdma_transfer_buffer
        buffers[HAILO_MAX_BUFFERS_PER_SINGLE_TRANSFER];                 // in

    enum hailo_vdma_interrupts_domain first_interrupts_domain;          // in
    enum hailo_vdma_interrupts_domain last_interrupts_domain;           // in

    bool is_debug;                                                      // in, if set program hw to send
                                                                        // more info (e.g desc complete status)

    uint32_t descs_programed;                                           // out, amount of descriptors programed.
};

#ifdef _MSC_VER
struct tCompatibleHailoIoctlData
{
    tCompatibleHailoIoctlParam Parameters;
    ULONG_PTR Value;
    union {
        struct hailo_memory_transfer_params MemoryTransfer;
        struct hailo_vdma_interrupts_enable_params VdmaInterruptsEnable;
        struct hailo_vdma_interrupts_disable_params VdmaInterruptsDisable;
        struct hailo_vdma_interrupts_read_timestamp_params VdmaInterruptsReadTimestamps;
        struct hailo_vdma_interrupts_wait_params VdmaInterruptsWait;
        struct hailo_vdma_buffer_sync_params VdmaBufferSync;
        struct hailo_fw_control FirmwareControl;
        struct hailo_vdma_buffer_map_params VdmaBufferMap;
        struct hailo_vdma_buffer_unmap_params VdmaBufferUnmap;
        struct hailo_desc_list_create_params DescListCreate;
        struct hailo_desc_list_release_params DescListReleaseParam;
        struct hailo_desc_list_bind_vdma_buffer_params DescListBind;
        struct hailo_d2h_notification D2HNotification;
        struct hailo_device_properties DeviceProperties;
        struct hailo_driver_info DriverInfo;
        struct hailo_non_linux_desc_list_mmap_params DescListMmap;
        struct hailo_read_log_params ReadLog;
        struct hailo_mark_as_in_use_params MarkAsInUse;
        struct hailo_vdma_launch_transfer_params LaunchTransfer;
    } Buffer;
};
#endif // _MSC_VER

#pragma pack(pop)

enum hailo_general_ioctl_code {
    HAILO_MEMORY_TRANSFER_CODE,
    HAILO_FW_CONTROL_CODE,
    HAILO_READ_NOTIFICATION_CODE,
    HAILO_DISABLE_NOTIFICATION_CODE,
    HAILO_QUERY_DEVICE_PROPERTIES_CODE,
    HAILO_QUERY_DRIVER_INFO_CODE,
    HAILO_READ_LOG_CODE,
    HAILO_RESET_NN_CORE_CODE,

    // Must be last
    HAILO_GENERAL_IOCTL_MAX_NR,
};

#define HAILO_MEMORY_TRANSFER           _IOWR_(HAILO_GENERAL_IOCTL_MAGIC,  HAILO_MEMORY_TRANSFER_CODE,            struct hailo_memory_transfer_params)
#define HAILO_FW_CONTROL                _IOWR_(HAILO_GENERAL_IOCTL_MAGIC,  HAILO_FW_CONTROL_CODE,                 struct hailo_fw_control)
#define HAILO_READ_NOTIFICATION         _IOW_(HAILO_GENERAL_IOCTL_MAGIC,   HAILO_READ_NOTIFICATION_CODE,          struct hailo_d2h_notification)
#define HAILO_DISABLE_NOTIFICATION      _IO_(HAILO_GENERAL_IOCTL_MAGIC,    HAILO_DISABLE_NOTIFICATION_CODE)
#define HAILO_QUERY_DEVICE_PROPERTIES   _IOW_(HAILO_GENERAL_IOCTL_MAGIC,   HAILO_QUERY_DEVICE_PROPERTIES_CODE,    struct hailo_device_properties)
#define HAILO_QUERY_DRIVER_INFO         _IOW_(HAILO_GENERAL_IOCTL_MAGIC,   HAILO_QUERY_DRIVER_INFO_CODE,          struct hailo_driver_info)
#define HAILO_READ_LOG                  _IOWR_(HAILO_GENERAL_IOCTL_MAGIC,  HAILO_READ_LOG_CODE,                   struct hailo_read_log_params)
#define HAILO_RESET_NN_CORE             _IO_(HAILO_GENERAL_IOCTL_MAGIC,    HAILO_RESET_NN_CORE_CODE)

enum hailo_vdma_ioctl_code {
    HAILO_VDMA_INTERRUPTS_ENABLE_CODE,
    HAILO_VDMA_INTERRUPTS_DISABLE_CODE,
    HAILO_VDMA_INTERRUPTS_WAIT_CODE,
    HAILO_VDMA_INTERRUPTS_READ_TIMESTAMPS_CODE,
    HAILO_VDMA_BUFFER_MAP_CODE,
    HAILO_VDMA_BUFFER_UNMAP_CODE,
    HAILO_VDMA_BUFFER_SYNC_CODE,
    HAILO_DESC_LIST_CREATE_CODE,
    HAILO_DESC_LIST_RELEASE_CODE,
    HAILO_DESC_LIST_BIND_VDMA_BUFFER_CODE,
    HAILO_VDMA_LOW_MEMORY_BUFFER_ALLOC_CODE,
    HAILO_VDMA_LOW_MEMORY_BUFFER_FREE_CODE,
    HAILO_MARK_AS_IN_USE_CODE,
    HAILO_VDMA_CONTINUOUS_BUFFER_ALLOC_CODE,
    HAILO_VDMA_CONTINUOUS_BUFFER_FREE_CODE,
    HAILO_VDMA_LAUNCH_TRANSFER_CODE,

    // Must be last
    HAILO_VDMA_IOCTL_MAX_NR,
};

#define HAILO_VDMA_INTERRUPTS_ENABLE          _IOR_(HAILO_VDMA_IOCTL_MAGIC,  HAILO_VDMA_INTERRUPTS_ENABLE_CODE,            struct hailo_vdma_interrupts_enable_params)
#define HAILO_VDMA_INTERRUPTS_DISABLE         _IOR_(HAILO_VDMA_IOCTL_MAGIC,  HAILO_VDMA_INTERRUPTS_DISABLE_CODE,           struct hailo_vdma_interrupts_disable_params)
#define HAILO_VDMA_INTERRUPTS_WAIT            _IOWR_(HAILO_VDMA_IOCTL_MAGIC, HAILO_VDMA_INTERRUPTS_WAIT_CODE,              struct hailo_vdma_interrupts_wait_params)
#define HAILO_VDMA_INTERRUPTS_READ_TIMESTAMPS _IOWR_(HAILO_VDMA_IOCTL_MAGIC, HAILO_VDMA_INTERRUPTS_READ_TIMESTAMPS_CODE,   struct hailo_vdma_interrupts_read_timestamp_params)

#define HAILO_VDMA_BUFFER_MAP                 _IOWR_(HAILO_VDMA_IOCTL_MAGIC, HAILO_VDMA_BUFFER_MAP_CODE,                    struct hailo_vdma_buffer_map_params)
#define HAILO_VDMA_BUFFER_UNMAP               _IOR_(HAILO_VDMA_IOCTL_MAGIC,  HAILO_VDMA_BUFFER_UNMAP_CODE,                  struct hailo_vdma_buffer_unmap_params)
#define HAILO_VDMA_BUFFER_SYNC                _IOR_(HAILO_VDMA_IOCTL_MAGIC,  HAILO_VDMA_BUFFER_SYNC_CODE,                   struct hailo_vdma_buffer_sync_params)

#define HAILO_DESC_LIST_CREATE                _IOWR_(HAILO_VDMA_IOCTL_MAGIC, HAILO_DESC_LIST_CREATE_CODE,                   struct hailo_desc_list_create_params)
#define HAILO_DESC_LIST_RELEASE               _IOR_(HAILO_VDMA_IOCTL_MAGIC,  HAILO_DESC_LIST_RELEASE_CODE,                  struct hailo_desc_list_release_params)
#define HAILO_DESC_LIST_BIND_VDMA_BUFFER      _IOR_(HAILO_VDMA_IOCTL_MAGIC,  HAILO_DESC_LIST_BIND_VDMA_BUFFER_CODE,         struct hailo_desc_list_bind_vdma_buffer_params)

#define HAILO_VDMA_LOW_MEMORY_BUFFER_ALLOC    _IOWR_(HAILO_VDMA_IOCTL_MAGIC, HAILO_VDMA_LOW_MEMORY_BUFFER_ALLOC_CODE,       struct hailo_allocate_low_memory_buffer_params)
#define HAILO_VDMA_LOW_MEMORY_BUFFER_FREE     _IOR_(HAILO_VDMA_IOCTL_MAGIC,   HAILO_VDMA_LOW_MEMORY_BUFFER_FREE_CODE,       struct hailo_free_low_memory_buffer_params)

#define HAILO_MARK_AS_IN_USE                  _IOW_(HAILO_VDMA_IOCTL_MAGIC,  HAILO_MARK_AS_IN_USE_CODE,                     struct hailo_mark_as_in_use_params)

#define HAILO_VDMA_CONTINUOUS_BUFFER_ALLOC    _IOWR_(HAILO_VDMA_IOCTL_MAGIC, HAILO_VDMA_CONTINUOUS_BUFFER_ALLOC_CODE,       struct hailo_allocate_continuous_buffer_params)
#define HAILO_VDMA_CONTINUOUS_BUFFER_FREE     _IOR_(HAILO_VDMA_IOCTL_MAGIC,   HAILO_VDMA_CONTINUOUS_BUFFER_FREE_CODE,       struct hailo_free_continuous_buffer_params)

#define HAILO_VDMA_LAUNCH_TRANSFER           _IOWR_(HAILO_VDMA_IOCTL_MAGIC, HAILO_VDMA_LAUNCH_TRANSFER_CODE,                struct hailo_vdma_launch_transfer_params)


enum hailo_non_linux_ioctl_code {
    HAILO_NON_LINUX_DESC_LIST_MMAP_CODE,

    // Must be last
    HAILO_NON_LINUX_IOCTL_MAX_NR,
};

#define HAILO_NON_LINUX_DESC_LIST_MMAP _IOWR_(HAILO_NON_LINUX_IOCTL_MAGIC, HAILO_NON_LINUX_DESC_LIST_MMAP_CODE, struct hailo_non_linux_desc_list_mmap_params)


#endif /* _HAILO_IOCTL_COMMON_H_ */
