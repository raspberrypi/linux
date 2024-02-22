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

enum gunyah_mem_flags {
	GHMF_CLOEXEC = (1UL << 0),
	GHMF_ALLOW_HUGEPAGE = (1UL << 1),
};

/**
 * struct gunyah_create_mem_args - Description of guest memory to create
 * @flags: See GHMF_*.
 */
struct gunyah_create_mem_args {
	__u64 flags;
	__u64 size;
	__u64 reserved[6];
};

#define GUNYAH_CREATE_GUEST_MEM      \
	_IOW(GUNYAH_IOCTL_TYPE, 0x8, \
	     struct gunyah_create_mem_args) /* Returns a Gunyah memory fd */

/*
 * ioctls for gunyah-vm fds (returned by GUNYAH_CREATE_VM)
 */

/**
 * struct gunyah_vm_dtb_config - Set the location of the VM's devicetree blob
 * @guest_phys_addr: Address of the VM's devicetree in guest memory.
 * @size: Maximum size of the devicetree including space for overlays.
 *        Resource manager applies an overlay to the DTB and dtb_size should
 *        include room for the overlay. A page of memory is typicaly plenty.
 */
struct gunyah_vm_dtb_config {
	__u64 guest_phys_addr;
	__u64 size;
};
#define GUNYAH_VM_SET_DTB_CONFIG	_IOW(GUNYAH_IOCTL_TYPE, 0x2, struct gunyah_vm_dtb_config)

#define GUNYAH_VM_START		_IO(GUNYAH_IOCTL_TYPE, 0x3)

/**
 * enum gunyah_fn_type - Valid types of Gunyah VM functions
 * @GUNYAH_FN_VCPU: create a vCPU instance to control a vCPU
 *              &struct gunyah_fn_desc.arg is a pointer to &struct gunyah_fn_vcpu_arg
 *              Return: file descriptor to manipulate the vcpu.
 */
enum gunyah_fn_type {
	GUNYAH_FN_VCPU = 1,
};

#define GUNYAH_FN_MAX_ARG_SIZE		256

/**
 * struct gunyah_fn_vcpu_arg - Arguments to create a vCPU.
 * @id: vcpu id
 *
 * Create this function with &GUNYAH_VM_ADD_FUNCTION using type &GUNYAH_FN_VCPU.
 *
 * The vcpu type will register with the VM Manager to expect to control
 * vCPU number `vcpu_id`. It returns a file descriptor allowing interaction with
 * the vCPU. See the Gunyah vCPU API description sections for interacting with
 * the Gunyah vCPU file descriptors.
 */
struct gunyah_fn_vcpu_arg {
	__u32 id;
};

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

/**
 * enum gunyah_map_flags- Possible flags on &struct gunyah_map_mem_args
 * @GUNYAH_MEM_DEFAULT_SHARE: Use default host access for the VM type
 * @GUNYAH_MEM_FORCE_LEND: Force unmapping the memory once the guest starts to use
 * @GUNYAH_MEM_FORCE_SHARE: Allow host to continue accessing memory when guest starts to use
 * @GUNYAH_MEM_ALLOW_READ: Allow guest to read memory
 * @GUNYAH_MEM_ALLOW_WRITE: Allow guest to write to the memory
 * @GUNYAH_MEM_ALLOW_EXEC: Allow guest to execute instructions in the memory
 */
enum gunyah_map_flags {
	GUNYAH_MEM_DEFAULT_ACCESS = 0,
	GUNYAH_MEM_FORCE_LEND = 1,
	GUNYAH_MEM_FORCE_SHARE = 2,
#define GUNYAH_MEM_ACCESS_MASK 0x7

	GUNYAH_MEM_ALLOW_READ = 1UL << 4,
	GUNYAH_MEM_ALLOW_WRITE = 1UL << 5,
	GUNYAH_MEM_ALLOW_EXEC = 1UL << 6,
	GUNYAH_MEM_ALLOW_RWX =
		(GUNYAH_MEM_ALLOW_READ | GUNYAH_MEM_ALLOW_WRITE | GUNYAH_MEM_ALLOW_EXEC),

	GUNYAH_MEM_UNMAP = 1UL << 8,
};

/**
 * struct gunyah_map_mem_args - Description to provide guest memory into a VM
 * @guest_addr: Location in guest address space to place the memory
 * @flags: See &enum gunyah_map_flags.
 * @guest_mem_fd: File descriptor created by GUNYAH_CREATE_GUEST_MEM
 * @offset: Offset into the guest memory file
 */
struct gunyah_map_mem_args {
	__u64 guest_addr;
	__u32 flags;
	__u32 guest_mem_fd;
	__u64 offset;
	__u64 size;
};

#define GUNYAH_VM_MAP_MEM _IOW(GUNYAH_IOCTL_TYPE, 0x9, struct gunyah_map_mem_args)

enum gunyah_vm_boot_context_reg {
	REG_SET_X		= 0,
	REG_SET_PC		= 1,
	REG_SET_SP		= 2,
};

#define GUNYAH_VM_BOOT_CONTEXT_REG_SHIFT	8
#define GUNYAH_VM_BOOT_CONTEXT_REG(reg, idx) (((reg & 0xff) << GUNYAH_VM_BOOT_CONTEXT_REG_SHIFT) |\
					      (idx & 0xff))

/**
 * struct gunyah_vm_boot_context - Set an initial register for the VM
 * @reg: Register to set. See GUNYAH_VM_BOOT_CONTEXT_REG_* macros
 * @reserved: reserved for alignment
 * @value: value to fill in the register
 */
struct gunyah_vm_boot_context {
	__u32 reg;
	__u32 reserved;
	__u64 value;
};
#define GUNYAH_VM_SET_BOOT_CONTEXT	_IOW(GUNYAH_IOCTL_TYPE, 0xa, struct gunyah_vm_boot_context)

/*
 * ioctls for vCPU fds
 */

/**
 * enum gunyah_vm_status - Stores status reason why VM is not runnable (exited).
 * @GUNYAH_VM_STATUS_LOAD_FAILED: VM didn't start because it couldn't be loaded.
 * @GUNYAH_VM_STATUS_EXITED: VM requested shutdown/reboot.
 *                       Use &struct gunyah_vm_exit_info.reason for further details.
 * @GUNYAH_VM_STATUS_CRASHED: VM state is unknown and has crashed.
 */
enum gunyah_vm_status {
	GUNYAH_VM_STATUS_LOAD_FAILED	= 1,
	GUNYAH_VM_STATUS_EXITED		= 2,
	GUNYAH_VM_STATUS_CRASHED		= 3,
};

/*
 * Gunyah presently sends max 4 bytes of exit_reason.
 * If that changes, this macro can be safely increased without breaking
 * userspace so long as struct gunyah_vcpu_run < PAGE_SIZE.
 */
#define GUNYAH_VM_MAX_EXIT_REASON_SIZE	8u

/**
 * struct gunyah_vm_exit_info - Reason for VM exit as reported by Gunyah
 * See Gunyah documentation for values.
 * @type: Describes how VM exited
 * @padding: padding bytes
 * @reason_size: Number of bytes valid for `reason`
 * @reason: See Gunyah documentation for interpretation. Note: these values are
 *          not interpreted by Linux and need to be converted from little-endian
 *          as applicable.
 */
struct gunyah_vm_exit_info {
	__u16 type;
	__u16 padding;
	__u32 reason_size;
	__u8 reason[GUNYAH_VM_MAX_EXIT_REASON_SIZE];
};

/**
 * enum gunyah_vcpu_exit - Stores reason why &GUNYAH_VCPU_RUN ioctl recently exited with status 0
 * @GUNYAH_VCPU_EXIT_UNKNOWN: Not used, status != 0
 * @GUNYAH_VCPU_EXIT_MMIO: vCPU performed a read or write that could not be handled
 *                     by hypervisor or Linux. Use @struct gunyah_vcpu_run.mmio for
 *                     details of the read/write.
 * @GUNYAH_VCPU_EXIT_STATUS: vCPU not able to run because the VM has exited.
 *                       Use @struct gunyah_vcpu_run.status for why VM has exited.
 * @GUNYAH_VCPU_EXIT_PAGE_FAULT: vCPU tried to execute an instruction at an address
 *                               for which memory hasn't been provided. Use
 *                               @struct gunyah_vcpu_run.page_fault for details.
 */
enum gunyah_vcpu_exit {
	GUNYAH_VCPU_EXIT_UNKNOWN,
	GUNYAH_VCPU_EXIT_MMIO,
	GUNYAH_VCPU_EXIT_STATUS,
	GUNYAH_VCPU_EXIT_PAGE_FAULT,
};

/**
 * enum gunyah_vcpu_resume_action - Provide resume action after an MMIO or page fault
 * @GUNYAH_VCPU_RESUME_HANDLED: The mmio or page fault has been handled, continue
 *                              normal operation of vCPU
 * @GUNYAH_VCPU_RESUME_FAULT: The mmio or page fault could not be satisfied and
 *                            inject the original fault back to the guest.
 * @GUNYAH_VCPU_RESUME_RETRY: Retry the faulting instruction. Perhaps you added
 *                            memory binding to satisfy the request.
 */
enum gunyah_vcpu_resume_action {
	GUNYAH_VCPU_RESUME_HANDLED = 0,
	GUNYAH_VCPU_RESUME_FAULT,
	GUNYAH_VCPU_RESUME_RETRY,
};

/**
 * struct gunyah_vcpu_run - Application code obtains a pointer to the gunyah_vcpu_run
 *                      structure by mmap()ing a vcpu fd.
 * @immediate_exit: polled when scheduling the vcpu. If set, immediately returns -EINTR.
 * @padding: padding bytes
 * @exit_reason: Set when GUNYAH_VCPU_RUN returns successfully and gives reason why
 *               GUNYAH_VCPU_RUN has stopped running the vCPU. See &enum gunyah_vcpu_exit.
 * @mmio: Used when exit_reason == GUNYAH_VCPU_EXIT_MMIO
 *        The guest has faulted on an memory-mapped I/O that
 *        couldn't be satisfied by gunyah.
 * @mmio.phys_addr: Address guest tried to access
 * @mmio.data: the value that was written if `is_write == 1`. Filled by
 *        user for reads (`is_write == 0`).
 * @mmio.len: Length of write. Only the first `len` bytes of `data`
 *       are considered by Gunyah.
 * @mmio.is_write: 1 if VM tried to perform a write, 0 for a read
 * @mmio.resume_action: See &enum gunyah_vcpu_resume_action
 * @status: Used when exit_reason == GUNYAH_VCPU_EXIT_STATUS.
 *          The guest VM is no longer runnable. This struct informs why.
 * @status.status: See &enum gunyah_vm_status for possible values
 * @status.exit_info: Used when status == GUNYAH_VM_STATUS_EXITED
 * @page_fault: Used when EXIT_REASON == GUNYAH_VCPU_EXIT_PAGE_FAULT
 *              The guest has faulted on a region that can only be provided
 *              by mapping memory at phys_addr.
 * @page_fault.phys_addr: Address guest tried to access.
 * @page_fault.attempt: Error code why Linux wasn't able to handle fault itself
 *                      Typically, if no memory was mapped: -ENOENT,
 *                      If permission bits weren't what the VM wanted: -EPERM
 * @page_fault.resume_action: See &enum gunyah_vcpu_resume_action
 */
struct gunyah_vcpu_run {
	/* in */
	__u8 immediate_exit;
	__u8 padding[7];

	/* out */
	__u32 exit_reason;

	union {
		struct {
			__u64 phys_addr;
			__u8  data[8];
			__u32 len;
			__u8  is_write;
			__u8  resume_action;
		} mmio;

		struct {
			enum gunyah_vm_status status;
			struct gunyah_vm_exit_info exit_info;
		} status;

		struct {
			__u64 phys_addr;
			__s32 attempt;
			__u8  resume_action;
		} page_fault;
	};
};

#define GUNYAH_VCPU_RUN		_IO(GUNYAH_IOCTL_TYPE, 0x5)
#define GUNYAH_VCPU_MMAP_SIZE	_IO(GUNYAH_IOCTL_TYPE, 0x6)

#endif
