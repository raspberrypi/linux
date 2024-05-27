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
 * @GUNYAH_FN_IRQFD: register eventfd to assert a Gunyah doorbell
 *               &struct gunyah_fn_desc.arg is a pointer to &struct gunyah_fn_irqfd_arg
 * @GUNYAH_FN_IOEVENTFD: register ioeventfd to trigger when VM faults on parameter
 *                   &struct gunyah_fn_desc.arg is a pointer to &struct gunyah_fn_ioeventfd_arg
 */
enum gunyah_fn_type {
	GUNYAH_FN_VCPU = 1,
	GUNYAH_FN_IRQFD,
	GUNYAH_FN_IOEVENTFD,
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
 * enum gunyah_irqfd_flags - flags for use in gunyah_fn_irqfd_arg
 * @GUNYAH_IRQFD_FLAGS_LEVEL: make the interrupt operate like a level triggered
 *                        interrupt on guest side. Triggering IRQFD before
 *                        guest handles the interrupt causes interrupt to
 *                        stay asserted.
 */
enum gunyah_irqfd_flags {
	GUNYAH_IRQFD_FLAGS_LEVEL		= 1UL << 0,
};

/**
 * struct gunyah_fn_irqfd_arg - Arguments to create an irqfd function.
 *
 * Create this function with &GUNYAH_VM_ADD_FUNCTION using type &GUNYAH_FN_IRQFD.
 *
 * Allows setting an eventfd to directly trigger a guest interrupt.
 * irqfd.fd specifies the file descriptor to use as the eventfd.
 * irqfd.label corresponds to the doorbell label used in the guest VM's devicetree.
 *
 * @fd: an eventfd which when written to will raise a doorbell
 * @label: Label of the doorbell created on the guest VM
 * @flags: see &enum gunyah_irqfd_flags
 * @padding: padding bytes
 */
struct gunyah_fn_irqfd_arg {
	__u32 fd;
	__u32 label;
	__u32 flags;
	__u32 padding;
};

/**
 * enum gunyah_ioeventfd_flags - flags for use in gunyah_fn_ioeventfd_arg
 * @GUNYAH_IOEVENTFD_FLAGS_DATAMATCH: the event will be signaled only if the
 *                                written value to the registered address is
 *                                equal to &struct gunyah_fn_ioeventfd_arg.datamatch
 */
enum gunyah_ioeventfd_flags {
	GUNYAH_IOEVENTFD_FLAGS_DATAMATCH	= 1UL << 0,
};

/**
 * struct gunyah_fn_ioeventfd_arg - Arguments to create an ioeventfd function
 * @datamatch: data used when GUNYAH_IOEVENTFD_DATAMATCH is set
 * @addr: Address in guest memory
 * @len: Length of access
 * @fd: When ioeventfd is matched, this eventfd is written
 * @flags: See &enum gunyah_ioeventfd_flags
 * @padding: padding bytes
 *
 * Create this function with &GUNYAH_VM_ADD_FUNCTION using type &GUNYAH_FN_IOEVENTFD.
 *
 * Attaches an ioeventfd to a legal mmio address within the guest. A guest write
 * in the registered address will signal the provided event instead of triggering
 * an exit on the GUNYAH_VCPU_RUN ioctl.
 */
struct gunyah_fn_ioeventfd_arg {
	__u64 datamatch;
	__u64 addr;        /* legal mmio address */
	__u32 len;         /* 1, 2, 4, or 8 bytes; or 0 to ignore length */
	__s32 fd;
	__u32 flags;
	__u32 padding;
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
	GUNYAH_MEM_ALLOW_READ = 1UL << 0,
	GUNYAH_MEM_ALLOW_WRITE = 1UL << 1,
	GUNYAH_MEM_ALLOW_EXEC = 1UL << 2,
	GUNYAH_MEM_ALLOW_RWX =
		(GUNYAH_MEM_ALLOW_READ | GUNYAH_MEM_ALLOW_WRITE | GUNYAH_MEM_ALLOW_EXEC),
	GUNYAH_MEM_DEFAULT_ACCESS = 0x00,
	GUNYAH_MEM_FORCE_LEND = 0x10,
	GUNYAH_MEM_FORCE_SHARE = 0x20,
#define GUNYAH_MEM_ACCESS_MASK 0x70

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

/**
 * struct gunyah_userspace_memory_region - Userspace memory descripion for GH_VM_SET_USER_MEM_REGION
 * @label: Identifer to the region which is unique to the VM.
 * @flags: Flags for memory parcel behavior. See &enum gh_mem_flags.
 * @guest_phys_addr: Location of the memory region in guest's memory space (page-aligned)
 * @memory_size: Size of the region (page-aligned)
 * @userspace_addr: Location of the memory region in caller (userspace)'s memory
 *
 * See Documentation/virt/gunyah/vm-manager.rst for further details.
 */
struct gunyah_userspace_memory_region {
	__u32 label;
	__u32 flags;
	__u64 guest_phys_addr;
	__u64 memory_size;
	__u64 userspace_addr;
};

#define GH_VM_SET_USER_MEM_REGION	_IOW(GUNYAH_IOCTL_TYPE, 0x1, \
						struct gunyah_userspace_memory_region)
#define GH_ANDROID_IOCTL_TYPE		'A'

#define GH_VM_ANDROID_LEND_USER_MEM	_IOW(GH_ANDROID_IOCTL_TYPE, 0x11, \
						struct gunyah_userspace_memory_region)

struct gunyah_vm_firmware_config {
	__u64 guest_phys_addr;
	__u64 size;
};

#define GH_VM_ANDROID_SET_FW_CONFIG	_IOW(GH_ANDROID_IOCTL_TYPE, 0x12, \
						struct gunyah_vm_firmware_config)
#endif
