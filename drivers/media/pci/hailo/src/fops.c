// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/

#include <linux/version.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/pagemap.h>
#include <linux/uaccess.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <asm/thread_info.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/signal.h>
#endif

#include "fops.h"
#include "vdma_common.h"
#include "utils/logs.h"
#include "vdma/memory.h"
#include "vdma/ioctl.h"
#include "utils/compact.h"
#include "nnc.h"
#include "soc.h"


#if LINUX_VERSION_CODE >= KERNEL_VERSION( 4, 13, 0 )
#define wait_queue_t wait_queue_entry_t
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION( 4, 15, 0 )
#define ACCESS_ONCE READ_ONCE
#endif

#ifndef VM_RESERVED
    #define VMEM_FLAGS (VM_IO | VM_DONTEXPAND | VM_DONTDUMP)
#else
    #define VMEM_FLAGS (VM_IO | VM_RESERVED)
#endif

#define IS_PO2_ALIGNED(size, alignment) (!(size & (alignment-1)))

// On pcie driver there is only one dma engine
#define DEFAULT_VDMA_ENGINE_INDEX       (0)


static struct hailo_file_context *create_file_context(struct hailo_pcie_board *board, struct file *filp)
{
    struct hailo_file_context *context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context) {
        hailo_err(board, "Failed to alloc file context (required size %zu)\n", sizeof(*context));
        return ERR_PTR(-ENOMEM);
    }

    context->filp = filp;
    hailo_vdma_file_context_init(&context->vdma_context);
    list_add(&context->open_files_list, &board->open_files_list);
    context->is_valid = true;
    return context;
}

static void release_file_context(struct hailo_file_context *context)
{
    context->is_valid = false;
    list_del(&context->open_files_list);
    kfree(context);
}

static struct hailo_file_context *find_file_context(struct hailo_pcie_board *board, struct file *filp)
{
    struct hailo_file_context *cur = NULL;
    list_for_each_entry(cur, &board->open_files_list, open_files_list) {
        if (cur->filp == filp) {
            return cur;
        }
    }
    return NULL;
}

int hailo_pcie_fops_open(struct inode *inode, struct file *filp)
{
    u32 major = MAJOR(inode->i_rdev);
    u32 minor = MINOR(inode->i_rdev);
    struct hailo_pcie_board *pBoard;
    int err = 0;
    pci_power_t previous_power_state = PCI_UNKNOWN;
    bool interrupts_enabled_by_filp = false;
    struct hailo_file_context *context = NULL;

    pr_debug(DRIVER_NAME ": (%d: %d-%d): fops_open\n", current->tgid, major, minor);

    // allow multiple processes to open a device, count references in hailo_pcie_get_board_index.
    if (!(pBoard = hailo_pcie_get_board_index(minor))) {
        pr_err(DRIVER_NAME ": fops_open: PCIe board not found for /dev/hailo%d node.\n", minor);
        err = -ENODEV;
        goto l_exit;
    }

    filp->private_data = pBoard;

    if (down_interruptible(&pBoard->mutex)) {
        hailo_err(pBoard, "fops_open down_interruptible fail tgid:%d\n", current->tgid);
        err = -ERESTARTSYS;
        goto l_decrease_ref_count;
    }

    context = create_file_context(pBoard, filp);
    if (IS_ERR(context)) {
        err = PTR_ERR(context);
        goto l_release_mutex;
    }

    previous_power_state = pBoard->pDev->current_state;
    if (PCI_D0 != previous_power_state) {
        hailo_info(pBoard, "Waking up board change state from %d to PCI_D0\n", previous_power_state);
        err = pci_set_power_state(pBoard->pDev, PCI_D0);
        if (err < 0) {
            hailo_err(pBoard, "Failed waking up board %d", err);
            goto l_free_context;
        }
    }

    if (!hailo_pcie_is_device_connected(&pBoard->pcie_resources)) {
        hailo_err(pBoard, "Device disconnected while opening device\n");
        err = -ENXIO;
        goto l_revert_power_state;
    }

    // enable interrupts
    if (!pBoard->interrupts_enabled) {
        err = hailo_enable_interrupts(pBoard);
        if (err < 0) {
            hailo_err(pBoard, "Failed Enabling interrupts %d\n", err);
            goto l_revert_power_state;
        }
        interrupts_enabled_by_filp = true;
    }

    if (pBoard->pcie_resources.accelerator_type == HAILO_ACCELERATOR_TYPE_NNC) {
        err = hailo_nnc_file_context_init(pBoard, context);
    } else {
        err = hailo_soc_file_context_init(pBoard, context);
    }
    if (err < 0) {
        goto l_release_irq;
    }

    hailo_dbg(pBoard, "(%d: %d-%d): fops_open: SUCCESS on /dev/hailo%d\n", current->tgid,
        major, minor, minor);

    up(&pBoard->mutex);
    return 0;

l_release_irq:
    if (interrupts_enabled_by_filp) {
        hailo_disable_interrupts(pBoard);
    }

l_revert_power_state:
    if (pBoard->pDev->current_state != previous_power_state) {
        hailo_info(pBoard, "Power changing state from %d to %d\n", previous_power_state, pBoard->pDev->current_state);
        if (pci_set_power_state(pBoard->pDev, previous_power_state) < 0) {
            hailo_err(pBoard, "Failed setting power state back to %d\n", (int)previous_power_state);
        }
    }
l_free_context:
    release_file_context(context);
l_release_mutex:
    up(&pBoard->mutex);
l_decrease_ref_count:
    atomic_dec(&pBoard->ref_count);
l_exit:
    return err;
}

int hailo_pcie_fops_release(struct inode *inode, struct file *filp)
{
    struct hailo_pcie_board *board = (struct hailo_pcie_board *)filp->private_data;
    struct hailo_file_context *context = NULL;

    u32 major = MAJOR(inode->i_rdev);
    u32 minor = MINOR(inode->i_rdev);

    if (board) {
        hailo_info(board, "(%d: %d-%d): fops_release\n", current->tgid, major, minor);


        down(&board->mutex);

        context = find_file_context(board, filp);
        if (NULL == context) {
            hailo_err(board, "Invalid driver state, file context does not exist\n");
            up(&board->mutex);
            return -EINVAL;
        }

        if (false == context->is_valid) {
            // File context is invalid, but open. It's OK to continue finalize and release it.
            hailo_err(board, "Invalid file context\n");
        }

        if (board->pcie_resources.accelerator_type == HAILO_ACCELERATOR_TYPE_NNC) {
            hailo_nnc_file_context_finalize(board, context);
        } else {
            hailo_soc_file_context_finalize(board, context);
        }

        hailo_vdma_file_context_finalize(&context->vdma_context, &board->vdma, filp);
        release_file_context(context);

        if (atomic_dec_and_test(&board->ref_count)) {
            // Disable interrupts
            hailo_disable_interrupts(board);

            if (power_mode_enabled()) {
                hailo_info(board, "Power change state to PCI_D3hot\n");
                if (board->pDev && pci_set_power_state(board->pDev, PCI_D3hot) < 0) {
                    hailo_err(board, "Failed setting power state to D3hot");
                }
            }

            // deallocate board if already removed
            if (!board->pDev) {
                hailo_dbg(board, "fops_release, freed board\n");
                up(&board->mutex);
                kfree(board);
                board = NULL;
            } else {
                hailo_dbg(board, "fops_release, released resources for board\n");
                up(&board->mutex);
            }
        } else {
            up(&board->mutex);
        }

        hailo_dbg(board, "(%d: %d-%d): fops_release: SUCCESS on /dev/hailo%d\n", current->tgid,
            major, minor, minor);
    }

    return 0;
}

static long hailo_memory_transfer_ioctl(struct hailo_pcie_board *board, unsigned long arg)
{
    long err = 0;
    struct hailo_memory_transfer_params* transfer = &board->memory_transfer_params;

    hailo_dbg(board, "Start memory transfer ioctl\n");

    if (copy_from_user(transfer, (void __user*)arg, sizeof(*transfer))) {
        hailo_err(board, "copy_from_user fail\n");
        return -ENOMEM;
    }

    err = hailo_pcie_memory_transfer(&board->pcie_resources, transfer);
    if (err < 0) {
        hailo_err(board, "memory transfer failed %ld", err);
    }

    if (copy_to_user((void __user*)arg, transfer, sizeof(*transfer))) {
        hailo_err(board, "copy_to_user fail\n");
        return -ENOMEM;
    }

    return err;
}

static void firmware_notification_irq_handler(struct hailo_pcie_board *board)
{
    struct hailo_notification_wait *notif_wait_cursor = NULL;
    int err = 0;
    unsigned long irq_saved_flags = 0;

    spin_lock_irqsave(&board->nnc.notification_read_spinlock, irq_saved_flags);
    err = hailo_pcie_read_firmware_notification(&board->pcie_resources.fw_access, &board->nnc.notification_cache);
    spin_unlock_irqrestore(&board->nnc.notification_read_spinlock, irq_saved_flags);

    if (err < 0) {
        hailo_err(board, "Failed reading firmware notification");
    }
    else {
        // TODO: HRT-14502 move interrupt handling to nnc
        rcu_read_lock();
        list_for_each_entry_rcu(notif_wait_cursor, &board->nnc.notification_wait_list, notification_wait_list)
        {
            complete(&notif_wait_cursor->notification_completion);
        }
        rcu_read_unlock();
    }
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,22)
irqreturn_t hailo_irqhandler(int irq, void *dev_id, struct pt_regs *regs)
#else
irqreturn_t hailo_irqhandler(int irq, void *dev_id)
#endif
{
    irqreturn_t return_value = IRQ_NONE;
    struct hailo_pcie_board *board = (struct hailo_pcie_board *)dev_id;
    bool got_interrupt = false;
    struct hailo_pcie_interrupt_source irq_source = {0};

    hailo_dbg(board, "hailo_irqhandler\n");

    while (true) {
        if (!hailo_pcie_is_device_connected(&board->pcie_resources)) {
            hailo_err(board, "Device disconnected while handling irq\n");
            break;
        }

        got_interrupt = hailo_pcie_read_interrupt(&board->pcie_resources, &irq_source);
        if (!got_interrupt) {
            break;
        }

        return_value = IRQ_HANDLED;

        // wake fw_control if needed
        if (irq_source.interrupt_bitmask & FW_CONTROL) {
            complete(&board->nnc.fw_control.completion);
        }

        // wake driver_down if needed
        if (irq_source.interrupt_bitmask & DRIVER_DOWN) {
            complete(&board->driver_down.reset_completed);
        }

        if (irq_source.interrupt_bitmask & FW_NOTIFICATION) {
            if (!completion_done(&board->fw_loaded_completion)) {
                // Complete firmware loaded completion
                complete_all(&board->fw_loaded_completion);
            } else {
                firmware_notification_irq_handler(board);
            }
        }

        if (irq_source.interrupt_bitmask & SOC_CONNECT_ACCEPTED) {
            complete_all(&board->soc.control_resp_ready);
        }

        if (irq_source.interrupt_bitmask & SOC_CLOSED_IRQ) {
            hailo_info(board, "hailo_irqhandler - SOC_CLOSED_IRQ\n");
            // always use bitmap=0xFFFFFFFF - it is ok to wake all interrupts since each handler will check if the stream was aborted or not. 
            hailo_vdma_wakeup_interrupts(&board->vdma, &board->vdma.vdma_engines[DEFAULT_VDMA_ENGINE_INDEX],
                0xFFFFFFFF);
        }

        if (0 != irq_source.vdma_channels_bitmap) {
            hailo_vdma_irq_handler(&board->vdma, DEFAULT_VDMA_ENGINE_INDEX,
                irq_source.vdma_channels_bitmap);
        }
    }

    return return_value;
}

static long hailo_query_device_properties(struct hailo_pcie_board *board, unsigned long arg)
{
    struct hailo_device_properties props = {
        .desc_max_page_size = board->desc_max_page_size,
        .board_type = board->pcie_resources.board_type,
        .allocation_mode = board->allocation_mode,
        .dma_type = HAILO_DMA_TYPE_PCIE,
        .dma_engines_count = board->vdma.vdma_engines_count,
        .is_fw_loaded = hailo_pcie_is_firmware_loaded(&board->pcie_resources),
    };

    hailo_info(board, "HAILO_QUERY_DEVICE_PROPERTIES: desc_max_page_size=%u\n", props.desc_max_page_size);

    if (copy_to_user((void __user*)arg, &props, sizeof(props))) {
        hailo_err(board, "HAILO_QUERY_DEVICE_PROPERTIES, copy_to_user failed\n");
        return -ENOMEM;
    }

    return 0;
}

static long hailo_query_driver_info(struct hailo_pcie_board *board, unsigned long arg)
{
    struct hailo_driver_info info = {
        .major_version = HAILO_DRV_VER_MAJOR,
        .minor_version = HAILO_DRV_VER_MINOR,
        .revision_version = HAILO_DRV_VER_REVISION
    };

    hailo_info(board, "HAILO_QUERY_DRIVER_INFO: major=%u, minor=%u, revision=%u\n",
        info.major_version, info.minor_version, info.revision_version);

    if (copy_to_user((void __user*)arg, &info, sizeof(info))) {
        hailo_err(board, "HAILO_QUERY_DRIVER_INFO, copy_to_user failed\n");
        return -ENOMEM;
    }

    return 0;
}

static long hailo_general_ioctl(struct hailo_pcie_board *board, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case HAILO_MEMORY_TRANSFER:
        return hailo_memory_transfer_ioctl(board, arg);
    case HAILO_QUERY_DEVICE_PROPERTIES:
        return hailo_query_device_properties(board, arg);
    case HAILO_QUERY_DRIVER_INFO:
        return hailo_query_driver_info(board, arg);
    default:
        hailo_err(board, "Invalid general ioctl code 0x%x (nr: %d)\n", cmd, _IOC_NR(cmd));
        return -ENOTTY;
    }
}

long hailo_pcie_fops_unlockedioctl(struct file* filp, unsigned int cmd, unsigned long arg)
{
    long err = 0;
    struct hailo_pcie_board* board = (struct hailo_pcie_board*) filp->private_data;
    struct hailo_file_context *context = NULL;
    bool should_up_board_mutex = true;


    if (!board || !board->pDev) return -ENODEV;

    hailo_dbg(board, "(%d): fops_unlockedioctl. cmd:%d\n", current->tgid, _IOC_NR(cmd));

    if (_IOC_DIR(cmd) & _IOC_READ)
    {
        err = !compatible_access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
    }
    else if (_IOC_DIR(cmd) & _IOC_WRITE)
    {
        err =  !compatible_access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
    }

    if (err) {
        hailo_err(board, "Invalid ioctl parameter access 0x%x", cmd);
        return -EFAULT;
    }

    if (down_interruptible(&board->mutex)) {
        hailo_err(board, "unlockedioctl down_interruptible failed");
        return -ERESTARTSYS;
    }
    BUG_ON(board->mutex.count != 0);

    context = find_file_context(board, filp);
    if (NULL == context) {
        hailo_err(board, "Invalid driver state, file context does not exist\n");
        up(&board->mutex);
        return -EINVAL;
    }

    if (false == context->is_valid) {
        hailo_err(board, "Invalid file context\n");
        up(&board->mutex);
        return -EINVAL;
    }

    switch (_IOC_TYPE(cmd)) {
    case HAILO_GENERAL_IOCTL_MAGIC:
        err = hailo_general_ioctl(board, cmd, arg);
        break;
    case HAILO_VDMA_IOCTL_MAGIC:
        err = hailo_vdma_ioctl(&context->vdma_context, &board->vdma, cmd, arg, filp, &board->mutex,
            &should_up_board_mutex);
        break;
    case HAILO_SOC_IOCTL_MAGIC:
        if (HAILO_ACCELERATOR_TYPE_SOC != board->pcie_resources.accelerator_type) {
            hailo_err(board, "Ioctl %d is not supported on this accelerator type\n", _IOC_TYPE(cmd));
            err = -EINVAL;
        } else {
            err = hailo_soc_ioctl(board, context, &board->vdma, cmd, arg);
        }
        break;
    case HAILO_NNC_IOCTL_MAGIC:
        if (HAILO_ACCELERATOR_TYPE_NNC != board->pcie_resources.accelerator_type) {
            hailo_err(board, "Ioctl %d is not supported on this accelerator type\n", _IOC_TYPE(cmd));
            err = -EINVAL;
        } else {
            err = hailo_nnc_ioctl(board, cmd, arg, filp, &should_up_board_mutex);
        }
        break;
    default:
        hailo_err(board, "Invalid ioctl type %d\n", _IOC_TYPE(cmd));
        err = -ENOTTY;
    }

    if (should_up_board_mutex) {
        up(&board->mutex);
    }

    hailo_dbg(board, "(%d): fops_unlockedioct: SUCCESS\n", current->tgid);
    return err;

}

int hailo_pcie_fops_mmap(struct file* filp, struct vm_area_struct *vma)
{
    int err = 0;

    uintptr_t vdma_handle   = vma->vm_pgoff << PAGE_SHIFT;

    struct hailo_pcie_board* board = (struct hailo_pcie_board*)filp->private_data;
    struct hailo_file_context *context = NULL;

    BUILD_BUG_ON_MSG(sizeof(vma->vm_pgoff) < sizeof(vdma_handle),
        "If this expression fails to compile it means the target HW is not compatible with our approach to use "
         "the page offset paramter of 'mmap' to pass the driver the 'handle' of the desired descriptor");

    vma->vm_pgoff = 0; // vm_pgoff contains vdma_handle page offset, the actual offset from the phys addr is 0

    hailo_info(board, "%d fops_mmap\n", current->tgid);

    if (!board || !board->pDev) return -ENODEV;

    if (down_interruptible(&board->mutex)) {
        hailo_err(board, "hailo_pcie_fops_mmap down_interruptible fail tgid:%d\n", current->tgid);
        return -ERESTARTSYS;
    }

    context = find_file_context(board, filp);
    if (NULL == context) {
        up(&board->mutex);
        hailo_err(board, "Invalid driver state, file context does not exist\n");
        return -EINVAL;
    }

    if (false == context->is_valid) {
        up(&board->mutex);
        hailo_err(board, "Invalid file context\n");
        return -EINVAL;
    }

    err = hailo_vdma_mmap(&context->vdma_context, &board->vdma, vma, vdma_handle);
    up(&board->mutex);
    return err;
}
