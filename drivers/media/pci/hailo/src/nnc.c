// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/
/**
 * A Hailo PCIe NNC device is a device contains a NNC (neural network core) and some basic FW.
 * The device supports sending controls, receiving notification and reading the FW log.
 */

#include "nnc.h"
#include "hailo_ioctl_common.h"

#include "utils/logs.h"
#include "utils/compact.h"

#include <linux/uaccess.h>

#if !defined(HAILO_EMULATOR)
#define DEFAULT_SHUTDOWN_TIMEOUT_MS (5)
#else /* !defined(HAILO_EMULATOR) */
#define DEFAULT_SHUTDOWN_TIMEOUT_MS (1000)
#endif /* !defined(HAILO_EMULATOR) */

void hailo_nnc_init(struct hailo_pcie_nnc *nnc)
{
    sema_init(&nnc->fw_control.mutex, 1);
    spin_lock_init(&nnc->notification_read_spinlock);
    init_completion(&nnc->fw_control.completion);
    INIT_LIST_HEAD(&nnc->notification_wait_list);
    memset(&nnc->notification_cache, 0, sizeof(nnc->notification_cache));
}

void hailo_nnc_finalize(struct hailo_pcie_nnc *nnc)
{
    struct hailo_notification_wait *cursor = NULL;

    // Lock rcu_read_lock and send notification_completion to wake anyone waiting on the notification_wait_list when removed
    rcu_read_lock();
    list_for_each_entry_rcu(cursor, &nnc->notification_wait_list, notification_wait_list) {
        cursor->is_disabled = true;
        complete(&cursor->notification_completion);
    }
    rcu_read_unlock();
}

static int hailo_fw_control(struct hailo_pcie_board *board, unsigned long arg, bool* should_up_board_mutex)
{
    struct hailo_fw_control *command = &board->nnc.fw_control.command;
    long completion_result = 0;
    int err = 0;

    up(&board->mutex);
    *should_up_board_mutex = false;

    if (down_interruptible(&board->nnc.fw_control.mutex)) {
        hailo_info(board, "hailo_fw_control down_interruptible fail tgid:%d (process was interrupted or killed)\n", current->tgid);
        return -ERESTARTSYS;
    }

    if (copy_from_user(command, (void __user*)arg, sizeof(*command))) {
        hailo_err(board, "hailo_fw_control, copy_from_user fail\n");
        err = -ENOMEM;
        goto l_exit;
    }

    reinit_completion(&board->nnc.fw_control.completion);

    err = hailo_pcie_write_firmware_control(&board->pcie_resources, command);
    if (err < 0) {
        hailo_err(board, "Failed writing fw control to pcie\n");
        goto l_exit;
    }

    // Wait for response
    completion_result = wait_for_completion_interruptible_timeout(&board->nnc.fw_control.completion, msecs_to_jiffies(command->timeout_ms));
    if (completion_result <= 0) {
        if (0 == completion_result) {
            hailo_err(board, "hailo_fw_control, timeout waiting for control (timeout_ms=%d)\n", command->timeout_ms);
            err = -ETIMEDOUT;
        } else {
            hailo_info(board, "hailo_fw_control, wait for completion failed with err=%ld (process was interrupted or killed)\n", completion_result);
            err = -EINTR;
        }
        goto l_exit;
    }

    err = hailo_pcie_read_firmware_control(&board->pcie_resources, command);
    if (err < 0) {
        hailo_err(board, "Failed reading fw control from pcie\n");
        goto l_exit;
    }

    if (copy_to_user((void __user*)arg, command, sizeof(*command))) {
        hailo_err(board, "hailo_fw_control, copy_to_user fail\n");
        err = -ENOMEM;
        goto l_exit;
    }

l_exit:
    up(&board->nnc.fw_control.mutex);
    return err;
}

static long hailo_get_notification_wait_thread(struct hailo_pcie_board *board, struct file *filp,
    struct hailo_notification_wait **current_waiting_thread)
{
    struct hailo_notification_wait *cursor = NULL;
    // note: safe to access without rcu because the notification_wait_list is closed only on file release
    list_for_each_entry(cursor, &board->nnc.notification_wait_list, notification_wait_list)
    {
        if ((current->tgid == cursor->tgid) && (filp == cursor->filp)) {
            *current_waiting_thread = cursor;
            return 0;
        }
    }

    return -EFAULT;
}

static long hailo_read_notification_ioctl(struct hailo_pcie_board *board, unsigned long arg, struct file *filp,
    bool* should_up_board_mutex)
{
    long err = 0;
    struct hailo_notification_wait *current_waiting_thread = NULL;
    struct hailo_d2h_notification *notification = &board->nnc.notification_to_user;
    unsigned long irq_saved_flags;

    err = hailo_get_notification_wait_thread(board, filp, &current_waiting_thread);
    if (0 != err) {
        goto l_exit;
    }
    up(&board->mutex);

    if (0 > (err = wait_for_completion_interruptible(&current_waiting_thread->notification_completion))) {
        hailo_info(board,
            "HAILO_READ_NOTIFICATION - wait_for_completion_interruptible error. err=%ld. tgid=%d (process was interrupted or killed)\n",
            err, current_waiting_thread->tgid);
        *should_up_board_mutex = false;
        goto l_exit;
    }

    if (down_interruptible(&board->mutex)) {
        hailo_info(board, "HAILO_READ_NOTIFICATION - down_interruptible error (process was interrupted or killed)\n");
        *should_up_board_mutex = false;
        err = -ERESTARTSYS;
        goto l_exit;
    }

    // Check if was disabled
    if (current_waiting_thread->is_disabled) {
        hailo_info(board, "HAILO_READ_NOTIFICATION, can't find notification wait for tgid=%d\n", current->tgid);
        err = -EINVAL;
        goto l_exit;
    }

    reinit_completion(&current_waiting_thread->notification_completion);

    spin_lock_irqsave(&board->nnc.notification_read_spinlock, irq_saved_flags);
    notification->buffer_len = board->nnc.notification_cache.buffer_len;
    memcpy(notification->buffer, board->nnc.notification_cache.buffer, notification->buffer_len);
    spin_unlock_irqrestore(&board->nnc.notification_read_spinlock, irq_saved_flags);

    if (copy_to_user((void __user*)arg, notification, sizeof(*notification))) {
        hailo_err(board, "HAILO_READ_NOTIFICATION copy_to_user fail\n");
        err = -ENOMEM;
        goto l_exit;
    }

l_exit:
    return err;
}

static long hailo_disable_notification(struct hailo_pcie_board *board, struct file *filp)
{
    struct hailo_notification_wait *cursor = NULL;

    hailo_info(board, "HAILO_DISABLE_NOTIFICATION: disable notification");
    rcu_read_lock();
    list_for_each_entry_rcu(cursor, &board->nnc.notification_wait_list, notification_wait_list) {
        if ((current->tgid == cursor->tgid) && (filp == cursor->filp)) {
            cursor->is_disabled = true;
            complete(&cursor->notification_completion);
            break;
        }
    }
    rcu_read_unlock();

    return 0;
}

static long hailo_read_log_ioctl(struct hailo_pcie_board *board, unsigned long arg)
{
    long err = 0;
    struct hailo_read_log_params params;

    if (copy_from_user(&params, (void __user*)arg, sizeof(params))) {
        hailo_err(board, "HAILO_READ_LOG, copy_from_user fail\n");
        return -ENOMEM;
    }

    if (0 > (err = hailo_pcie_read_firmware_log(&board->pcie_resources.fw_access, &params))) {
        hailo_err(board, "HAILO_READ_LOG, reading from log failed with error: %ld \n", err);
        return err;
    }

    if (copy_to_user((void*)arg, &params, sizeof(params))) {
        return -ENOMEM;
    }

    return 0;
}

long hailo_nnc_ioctl(struct hailo_pcie_board *board, unsigned int cmd, unsigned long arg,
    struct file *filp, bool *should_up_board_mutex)
{
    switch (cmd) {
    case HAILO_FW_CONTROL:
        return hailo_fw_control(board, arg, should_up_board_mutex);
    case HAILO_READ_NOTIFICATION:
        return hailo_read_notification_ioctl(board, arg, filp, should_up_board_mutex);
    case HAILO_DISABLE_NOTIFICATION:
        return hailo_disable_notification(board, filp);
    case HAILO_READ_LOG:
        return hailo_read_log_ioctl(board, arg);
    default:
        hailo_err(board, "Invalid nnc ioctl code 0x%x (nr: %d)\n", cmd, _IOC_NR(cmd));
        return -ENOTTY;
    }
}


static int add_notification_wait(struct hailo_pcie_board *board, struct file *filp)
{
    struct hailo_notification_wait *wait = kmalloc(sizeof(*wait), GFP_KERNEL);
    if (!wait) {
        hailo_err(board, "Failed to allocate notification wait structure.\n");
        return -ENOMEM;
    }
    wait->tgid = current->tgid;
    wait->filp = filp;
    wait->is_disabled = false;
    init_completion(&wait->notification_completion);
    list_add_rcu(&wait->notification_wait_list, &board->nnc.notification_wait_list);
    return 0;
}

int hailo_nnc_file_context_init(struct hailo_pcie_board *board, struct hailo_file_context *context)
{
    return add_notification_wait(board, context->filp);
}

static void clear_notification_wait_list(struct hailo_pcie_board *board, struct file *filp)
{
    struct hailo_notification_wait *cur = NULL, *next = NULL;
    list_for_each_entry_safe(cur, next, &board->nnc.notification_wait_list, notification_wait_list) {
        if (cur->filp == filp) {
            list_del_rcu(&cur->notification_wait_list);
            synchronize_rcu();
            kfree(cur);
        }
    }
}

int hailo_nnc_driver_down(struct hailo_pcie_board *board)
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
            hailo_err(board, "hailo_nnc_driver_down, timeout waiting for shutdown response (timeout_ms=%d)\n", DEFAULT_SHUTDOWN_TIMEOUT_MS);
            err = -ETIMEDOUT;
        } else {
            hailo_info(board, "hailo_nnc_driver_down, wait for completion failed with err=%ld (process was interrupted or killed)\n",
                completion_result);
            err = completion_result;
        }
        goto l_exit;
    }

l_exit:
    return err;
}

void hailo_nnc_file_context_finalize(struct hailo_pcie_board *board, struct hailo_file_context *context)
{
    clear_notification_wait_list(board, context->filp);

    if (context->filp == board->vdma.used_by_filp) {
        hailo_nnc_driver_down(board);
    }
}