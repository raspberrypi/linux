/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _UAPI_LINUX_GUNYAH_H
#define _UAPI_LINUX_GUNYAH_H

/*
 * Userspace interface for /dev/gunyah - gunyah based virtual machine
 */

#include <linux/types.h>
#include <linux/ioctl.h>

#define GUNYAH_IOCTL_TYPE 'G'

/*
 * ioctls for /dev/gunyah fds:
 */
#define GUNYAH_CREATE_VM _IO(GUNYAH_IOCTL_TYPE, 0x0) /* Returns a Gunyah VM fd */

/*
 * ioctls for gunyah-vm fds (returned by GUNYAH_CREATE_VM)
 */
#define GUNYAH_VM_START		_IO(GUNYAH_IOCTL_TYPE, 0x3)

#define GUNYAH_FN_MAX_ARG_SIZE		256

/**
 * struct gunyah_fn_desc - Arguments to create a VM function
 * @type: Type of the function. See &enum gunyah_fn_type.
 * @arg_size: Size of argument to pass to the function. arg_size <= GUNYAH_FN_MAX_ARG_SIZE
 * @arg: Pointer to argument given to the function. See &enum gunyah_fn_type for expected
 *       arguments for a function type.
 */
struct gunyah_fn_desc {
	__u32 type;
	__u32 arg_size;
	__u64 arg;
};

#define GUNYAH_VM_ADD_FUNCTION	_IOW(GUNYAH_IOCTL_TYPE, 0x4, struct gunyah_fn_desc)
#define GUNYAH_VM_REMOVE_FUNCTION	_IOW(GUNYAH_IOCTL_TYPE, 0x7, struct gunyah_fn_desc)

#endif
