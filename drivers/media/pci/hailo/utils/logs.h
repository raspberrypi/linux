// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef _COMMON_LOGS_H_
#define _COMMON_LOGS_H_

#include <linux/kern_levels.h>

// Should be used only by "module_param".
// Specify the current debug level for the logs
extern int o_dbg;


// Logging, same interface as dev_*, uses o_dbg to filter
// log messages
#define hailo_printk(level, dev, fmt, ...)              \
    do {                                                \
        int __level = (level[1] - '0');                 \
        if (__level <= o_dbg) {    \
            dev_printk((level), dev, fmt, ##__VA_ARGS__); \
        }                                               \
    } while (0)

#define hailo_emerg(board, fmt, ...) hailo_printk(KERN_EMERG, &(board)->pDev->dev, fmt, ##__VA_ARGS__)
#define hailo_alert(board, fmt, ...) hailo_printk(KERN_ALERT, &(board)->pDev->dev, fmt, ##__VA_ARGS__)
#define hailo_crit(board, fmt, ...)  hailo_printk(KERN_CRIT, &(board)->pDev->dev, fmt, ##__VA_ARGS__)
#define hailo_err(board, fmt, ...) hailo_printk(KERN_ERR, &(board)->pDev->dev, fmt, ##__VA_ARGS__)
#define hailo_warn(board, fmt, ...) hailo_printk(KERN_WARNING, &(board)->pDev->dev, fmt, ##__VA_ARGS__)
#define hailo_notice(board, fmt, ...) hailo_printk(KERN_NOTICE, &(board)->pDev->dev, fmt, ##__VA_ARGS__)
#define hailo_info(board, fmt, ...) hailo_printk(KERN_INFO, &(board)->pDev->dev, fmt, ##__VA_ARGS__)
#define hailo_dbg(board, fmt, ...) hailo_printk(KERN_DEBUG, &(board)->pDev->dev, fmt, ##__VA_ARGS__)

#define hailo_dev_emerg(dev, fmt, ...) hailo_printk(KERN_EMERG, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_alert(dev, fmt, ...) hailo_printk(KERN_ALERT, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_crit(dev, fmt, ...)  hailo_printk(KERN_CRIT, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_err(dev, fmt, ...) hailo_printk(KERN_ERR, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_warn(dev, fmt, ...) hailo_printk(KERN_WARNING, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_notice(dev, fmt, ...) hailo_printk(KERN_NOTICE, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_info(dev, fmt, ...) hailo_printk(KERN_INFO, dev, fmt, ##__VA_ARGS__)
#define hailo_dev_dbg(dev, fmt, ...) hailo_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__)


#endif //_COMMON_LOGS_H_