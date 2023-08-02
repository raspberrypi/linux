/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

/**
 * DOC: UAPI of GenieZone Hypervisor
 *
 * This file declares common data structure shared among user space,
 * kernel space, and GenieZone hypervisor.
 */
#ifndef __GZVM_H__
#define __GZVM_H__

#include <linux/const.h>
#include <linux/types.h>
#include <linux/ioctl.h>

/* GZVM ioctls */
#define GZVM_IOC_MAGIC			0x92	/* gz */

/* ioctls for /dev/gzvm fds */
#define GZVM_CREATE_VM             _IO(GZVM_IOC_MAGIC,   0x01) /* Returns a Geniezone VM fd */

/* ioctls for VM fds */
/* for GZVM_SET_MEMORY_REGION */
struct gzvm_memory_region {
	__u32 slot;
	__u32 flags;
	__u64 guest_phys_addr;
	__u64 memory_size; /* bytes */
};

#define GZVM_SET_MEMORY_REGION     _IOW(GZVM_IOC_MAGIC,  0x40, \
					struct gzvm_memory_region)

/* for GZVM_SET_USER_MEMORY_REGION */
struct gzvm_userspace_memory_region {
	__u32 slot;
	__u32 flags;
	__u64 guest_phys_addr;
	/* bytes */
	__u64 memory_size;
	/* start of the userspace allocated memory */
	__u64 userspace_addr;
};

#define GZVM_SET_USER_MEMORY_REGION _IOW(GZVM_IOC_MAGIC, 0x46, \
					 struct gzvm_userspace_memory_region)

#endif /* __GZVM_H__ */
