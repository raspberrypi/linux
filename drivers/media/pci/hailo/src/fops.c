// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
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

#include "hailo_pcie_version.h"
#include "utils.h"
#include "fops.h"
#include "vdma_common.h"
#include "utils/logs.h"
#include "vdma/memory.h"
#include "vdma/ioctl.h"
#include "utils/compact.h"


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

#if !defined(HAILO_EMULATOR)
#define DEFAULT_SHUTDOWN_TIMEOUT_MS (5)
#else /* !defined(HAILO_EMULATOR) */
#define DEFAULT_SHUTDOWN_TIMEOUT_MS (1000)
#endif /* !defined(HAILO_EMULATOR) */

static long hailo_add_notification_wait(struct hailo_pcie_board *board, struct file *filp);

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
        hailo_info(pBoard, "Waking up board");
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

    err = hailo_add_notification_wait(pBoard, filp);
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

int hailo_pcie_driver_down(struct hailo_pcie_board *board)
{
    long completion_result = 0;
    int err = 0;

    reinit_completion(&board->driver_down.reset_completed);

    hailo_pcie_write_firmware_driver_shutdown(&board->pcie_resources);

    // Wait for response
    completion_result =
        wait_for_completion_timeout(&board->driver_down.reset_completed, msecs_to_jiffies(DEFAULT_SHUTDOWN_TIMEOUT_MS));
    if (completion_result <= 0) {
        if (0 == completion_result) {
            hailo_err(board, "hailo_pcie_driver_down, timeout waiting for shutdown response (timeout_ms=%d)\n", DEFAULT_SHUTDOWN_TIMEOUT_MS);
            err = -ETIMEDOUT;
        } else {
            hailo_info(board, "hailo_pcie_driver_down, wait for completion failed with err=%ld (process was interrupted or killed)\n",
                completion_result);
            err = completion_result;
        }
        goto l_exit;
    }

l_exit:
    return err;
}

int hailo_pcie_fops_release(struct inode *inode, struct file *filp)
{
    struct hailo_pcie_board *pBoard = (struct hailo_pcie_board *)filp->private_data;
    struct hailo_file_context *context = NULL;

    u32 major = MAJOR(inode->i_rdev);
    u32 minor = MINOR(inode->i_rdev);

    if (pBoard) {
        hailo_info(pBoard, "(%d: %d-%d): fops_release\n", current->tgid, major, minor);

        if (down_interruptible(&pBoard->mutex)) {
            hailo_err(pBoard, "fops_release down_interruptible failed");
            return -ERESTARTSYS;
        }

        context = find_file_context(pBoard, filp);
        if (NULL == context) {
            hailo_err(pBoard, "Invalid driver state, file context does not exist\n");
            up(&pBoard->mutex);
            return -EINVAL;
        }

        if (false == context->is_valid) {
            // File context is invalid, but open. It's OK to continue finalize and release it.
            hailo_err(pBoard, "Invalid file context\n");
        }

        hailo_pcie_clear_notification_wait_list(pBoard, filp);

        if (filp == pBoard->vdma.used_by_filp) {
            if (hailo_pcie_driver_down(pBoard)) {
                hailo_err(pBoard, "Failed sending FW shutdown event");
            }
        }

        hailo_vdma_file_context_finalize(&context->vdma_context, &pBoard->vdma, filp);
        release_file_context(context);

        if (atomic_dec_and_test(&pBoard->ref_count)) {
            // Disable interrupts
            hailo_disable_interrupts(pBoard);

            if (power_mode_enabled()) {
                if (pBoard->pDev && pci_set_power_state(pBoard->pDev, PCI_D3hot) < 0) {
                    hailo_err(pBoard, "Failed setting power state to D3hot");
                }
            }

            // deallocate board if already removed
            if (!pBoard->pDev) {
                hailo_dbg(pBoard, "fops_close, freed board\n");
                up(&pBoard->mutex);
                kfree(pBoard);
                pBoard = NULL;
            } else {

                hailo_dbg(pBoard, "fops_close, released resources for board\n");
                up(&pBoard->mutex);
            }
        } else {
            up(&pBoard->mutex);
        }

        hailo_dbg(pBoard, "(%d: %d-%d): fops_close: SUCCESS on /dev/hailo%d\n", current->tgid,
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

static long hailo_read_log_ioctl(struct hailo_pcie_board *pBoard, unsigned long arg)
{
    long err = 0;
    struct hailo_read_log_params params;

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_err(pBoard, "HAILO_READ_LOG, copy_from_user fail\n");
        return -ENOMEM;
    }

    if (0 > (err = hailo_pcie_read_firmware_log(&pBoard->pcie_resources, &params))) {
        hailo_err(pBoard, "HAILO_READ_LOG, reading from log failed with error: %ld \n", err);
        return err;
    }

    if (copy_to_user((void*)arg, &params, sizeof(params))) {
        return -ENOMEM;
    }

    return 0;
}

static void firmware_notification_irq_handler(struct hailo_pcie_board *board)
{
    struct hailo_notification_wait *notif_wait_cursor = NULL;
    int err = 0;
    unsigned long irq_saved_flags = 0;

    spin_lock_irqsave(&board->notification_read_spinlock, irq_saved_flags);
    err = hailo_pcie_read_firmware_notification(&board->pcie_resources, &board->notification_cache);
    spin_unlock_irqrestore(&board->notification_read_spinlock, irq_saved_flags);

    if (err < 0) {
        hailo_err(board, "Failed reading firmware notification");
    }
    else {
        rcu_read_lock();
        list_for_each_entry_rcu(notif_wait_cursor, &board->notification_wait_list, notification_wait_list)
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
            complete(&board->fw_control.completion);
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

        if (0 != irq_source.vdma_channels_bitmap) {
            hailo_vdma_irq_handler(&board->vdma, DEFAULT_VDMA_ENGINE_INDEX,
                irq_source.vdma_channels_bitmap);
        }
    }

    return return_value;
}

static long hailo_get_notification_wait_thread(struct hailo_pcie_board *pBoard, struct file *filp,
    struct hailo_notification_wait **current_waiting_thread)
{
    struct hailo_notification_wait *cursor = NULL;
    // note: safe to access without rcu because the notification_wait_list is closed only on file release
    list_for_each_entry(cursor, &pBoard->notification_wait_list, notification_wait_list)
    {
        if ((current->tgid == cursor->tgid) && (filp == cursor->filp)) {
            *current_waiting_thread = cursor;
            return 0;
        }
    }

    return -EFAULT;
}

static long hailo_add_notification_wait(struct hailo_pcie_board *board, struct file *filp)
{
    struct hailo_notification_wait *new_notification_wait = NULL;
    if (!(new_notification_wait = kmalloc(sizeof(*new_notification_wait), GFP_KERNEL))) {
        hailo_err(board, "Failed to allocate notification wait structure.\n");
        return -ENOMEM;
    }
    new_notification_wait->tgid = current->tgid;
    new_notification_wait->filp = filp;
    new_notification_wait->is_disabled = false;
    init_completion(&new_notification_wait->notification_completion);
    list_add_rcu(&new_notification_wait->notification_wait_list, &board->notification_wait_list);
    return 0;
}

static long hailo_read_notification_ioctl(struct hailo_pcie_board *pBoard, unsigned long arg, struct file *filp,
    bool* should_up_board_mutex)
{
    long err = 0;
    struct hailo_notification_wait *current_waiting_thread = NULL;
    struct hailo_d2h_notification *notification = &pBoard->notification_to_user;
    unsigned long irq_saved_flags;

    err = hailo_get_notification_wait_thread(pBoard, filp, &current_waiting_thread);
    if (0 != err) {
        goto l_exit;
    }
    up(&pBoard->mutex);

    if (0 > (err = wait_for_completion_interruptible(&current_waiting_thread->notification_completion))) {
        hailo_info(pBoard,
            "HAILO_READ_NOTIFICATION - wait_for_completion_interruptible error. err=%ld. tgid=%d (process was interrupted or killed)\n",
            err, current_waiting_thread->tgid);
        *should_up_board_mutex = false;
        goto l_exit;
    }

    if (down_interruptible(&pBoard->mutex)) {
        hailo_info(pBoard, "HAILO_READ_NOTIFICATION - down_interruptible error (process was interrupted or killed)\n");
        *should_up_board_mutex = false;
        err = -ERESTARTSYS;
        goto l_exit;
    }

    // Check if was disabled
    if (current_waiting_thread->is_disabled) {
        hailo_info(pBoard, "HAILO_READ_NOTIFICATION, can't find notification wait for tgid=%d\n", current->tgid);
        err = -EINVAL;
        goto l_exit;
    }

    reinit_completion(&current_waiting_thread->notification_completion);
    
    spin_lock_irqsave(&pBoard->notification_read_spinlock, irq_saved_flags);
    notification->buffer_len = pBoard->notification_cache.buffer_len;
    memcpy(notification->buffer, pBoard->notification_cache.buffer, notification->buffer_len);
    spin_unlock_irqrestore(&pBoard->notification_read_spinlock, irq_saved_flags);

    if (copy_to_user((void __user*)arg, notification, sizeof(*notification))) {
        hailo_err(pBoard, "HAILO_READ_NOTIFICATION copy_to_user fail\n");
        err = -ENOMEM;
        goto l_exit;
    }

l_exit:
    return err;
}

static long hailo_disable_notification(struct hailo_pcie_board *pBoard, struct file *filp)
{
    struct hailo_notification_wait *cursor = NULL;

    hailo_info(pBoard, "HAILO_DISABLE_NOTIFICATION: disable notification");
    rcu_read_lock();
    list_for_each_entry_rcu(cursor, &pBoard->notification_wait_list, notification_wait_list) {
        if ((current->tgid == cursor->tgid) && (filp == cursor->filp)) {
            cursor->is_disabled = true;
            complete(&cursor->notification_completion);
            break;
        }
    }
    rcu_read_unlock();

    return 0;
}

static int hailo_fw_control(struct hailo_pcie_board *pBoard, unsigned long arg, bool* should_up_board_mutex)
{
    struct hailo_fw_control *command = &pBoard->fw_control.command;
    long completion_result = 0;
    int err = 0;

    up(&pBoard->mutex);
    *should_up_board_mutex = false;

    if (down_interruptible(&pBoard->fw_control.mutex)) {
        hailo_info(pBoard, "hailo_fw_control down_interruptible fail tgid:%d (process was interrupted or killed)\n", current->tgid);
        return -ERESTARTSYS;
    }

    if (copy_from_user(command, (void __user*)arg, sizeof(*command))) {
        hailo_err(pBoard, "hailo_fw_control, copy_from_user fail\n");
        err = -ENOMEM;
        goto l_exit;
    }

    reinit_completion(&pBoard->fw_control.completion);

    err = hailo_pcie_write_firmware_control(&pBoard->pcie_resources, command);
    if (err < 0) {
        hailo_err(pBoard, "Failed writing fw control to pcie\n");
        goto l_exit;
    }

    // Wait for response
    completion_result = wait_for_completion_interruptible_timeout(&pBoard->fw_control.completion, msecs_to_jiffies(command->timeout_ms));
    if (completion_result <= 0) {
        if (0 == completion_result) {
            hailo_err(pBoard, "hailo_fw_control, timeout waiting for control (timeout_ms=%d)\n", command->timeout_ms);
            err = -ETIMEDOUT;
        } else {
            hailo_info(pBoard, "hailo_fw_control, wait for completion failed with err=%ld (process was interrupted or killed)\n", completion_result);
            err = -EINTR;
        }
        goto l_exit;
    }

    err = hailo_pcie_read_firmware_control(&pBoard->pcie_resources, command);
    if (err < 0) {
        hailo_err(pBoard, "Failed reading fw control from pcie\n");
        goto l_exit;
    }

    if (copy_to_user((void __user*)arg, command, sizeof(*command))) {
        hailo_err(pBoard, "hailo_fw_control, copy_to_user fail\n");
        err = -ENOMEM;
        goto l_exit;
    }

l_exit:
    up(&pBoard->fw_control.mutex);
    return err;
}

static long hailo_query_device_properties(struct hailo_pcie_board *board, unsigned long arg)
{
    struct hailo_device_properties props = {
        .desc_max_page_size = board->desc_max_page_size,
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

static long hailo_general_ioctl(struct hailo_file_context *context, struct hailo_pcie_board *board,
    unsigned int cmd, unsigned long arg, struct file *filp, bool *should_up_board_mutex)
{
    switch (cmd) {
    case HAILO_MEMORY_TRANSFER:
        return hailo_memory_transfer_ioctl(board, arg);
    case HAILO_FW_CONTROL:
        return hailo_fw_control(board, arg, should_up_board_mutex);
    case HAILO_READ_NOTIFICATION:
        return hailo_read_notification_ioctl(board, arg, filp, should_up_board_mutex);
    case HAILO_DISABLE_NOTIFICATION:
        return hailo_disable_notification(board, filp);
    case HAILO_QUERY_DEVICE_PROPERTIES:
        return hailo_query_device_properties(board, arg);
    case HAILO_QUERY_DRIVER_INFO:
        return hailo_query_driver_info(board, arg);
    case HAILO_READ_LOG:
        return hailo_read_log_ioctl(board, arg);
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
        err = hailo_general_ioctl(context, board, cmd, arg, filp, &should_up_board_mutex);
        break;
    case HAILO_VDMA_IOCTL_MAGIC:
        err = hailo_vdma_ioctl(&context->vdma_context, &board->vdma, cmd, arg, filp, &board->mutex,
            &should_up_board_mutex);
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
