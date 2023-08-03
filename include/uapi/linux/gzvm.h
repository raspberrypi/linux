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

#define GZVM_CAP_VM_GPA_SIZE	0xa5
#define GZVM_CAP_PROTECTED_VM	0xffbadab1

/* sub-commands put in args[0] for GZVM_CAP_PROTECTED_VM */
#define GZVM_CAP_PVM_SET_PVMFW_GPA		0
#define GZVM_CAP_PVM_GET_PVMFW_SIZE		1
/* GZVM_CAP_PVM_SET_PROTECTED_VM only sets protected but not load pvmfw */
#define GZVM_CAP_PVM_SET_PROTECTED_VM		2

/* GZVM ioctls */
#define GZVM_IOC_MAGIC			0x92	/* gz */

/* ioctls for /dev/gzvm fds */
#define GZVM_CREATE_VM             _IO(GZVM_IOC_MAGIC,   0x01) /* Returns a Geniezone VM fd */

/*
 * Check if the given capability is supported or not.
 * The argument is capability. Ex. GZVM_CAP_PROTECTED_VM or GZVM_CAP_VM_GPA_SIZE
 * return is 0 (supported, no error)
 * return is -EOPNOTSUPP (unsupported)
 * return is -EFAULT (failed to get the argument from userspace)
 */
#define GZVM_CHECK_EXTENSION       _IO(GZVM_IOC_MAGIC,   0x03)

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

/**
 * struct gzvm_enable_cap: The `capability support` on GenieZone hypervisor
 * @cap: `GZVM_CAP_ARM_PROTECTED_VM` or `GZVM_CAP_ARM_VM_IPA_SIZE`
 * @args: x3-x7 registers can be used for additional args
 */
struct gzvm_enable_cap {
	__u64 cap;
	__u64 args[5];
};

#define GZVM_ENABLE_CAP            _IOW(GZVM_IOC_MAGIC,  0xa3, \
					struct gzvm_enable_cap)

#endif /* __GZVM_H__ */
