// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/arm-smccc.h>
#include <linux/err.h>
#include <linux/uaccess.h>

#include <linux/gzvm.h>
#include <linux/gzvm_drv.h>
#include "gzvm_arch_common.h"

#define PAR_PA47_MASK ((((1UL << 48) - 1) >> 12) << 12)

/**
 * gzvm_hypcall_wrapper() - the wrapper for hvc calls
 * @a0: arguments passed in registers 0
 * @a1: arguments passed in registers 1
 * @a2: arguments passed in registers 2
 * @a3: arguments passed in registers 3
 * @a4: arguments passed in registers 4
 * @a5: arguments passed in registers 5
 * @a6: arguments passed in registers 6
 * @a7: arguments passed in registers 7
 * @res: result values from registers 0 to 3
 *
 * Return: The wrapper helps caller to convert geniezone errno to Linux errno.
 */
int gzvm_hypcall_wrapper(unsigned long a0, unsigned long a1,
			 unsigned long a2, unsigned long a3,
			 unsigned long a4, unsigned long a5,
			 unsigned long a6, unsigned long a7,
			 struct arm_smccc_res *res)
{
	arm_smccc_hvc(a0, a1, a2, a3, a4, a5, a6, a7, res);
	return gzvm_err_to_errno(res->a0);
}

int gzvm_arch_probe(void)
{
	struct arm_smccc_res res;
	int ret;

	ret = gzvm_hypcall_wrapper(MT_HVC_GZVM_PROBE, 0, 0, 0, 0, 0, 0, 0, &res);
	if (ret)
		return -ENXIO;

	return 0;
}

int gzvm_arch_set_memregion(u16 vm_id, size_t buf_size,
			    phys_addr_t region)
{
	struct arm_smccc_res res;

	return gzvm_hypcall_wrapper(MT_HVC_GZVM_SET_MEMREGION, vm_id,
				    buf_size, region, 0, 0, 0, 0, &res);
}

static int gzvm_cap_vm_gpa_size(void __user *argp)
{
	__u64 value = CONFIG_ARM64_PA_BITS;

	if (copy_to_user(argp, &value, sizeof(__u64)))
		return -EFAULT;

	return 0;
}

int gzvm_arch_check_extension(struct gzvm *gzvm, __u64 cap, void __user *argp)
{
	int ret;

	switch (cap) {
	case GZVM_CAP_PROTECTED_VM: {
		__u64 success = 1;

		if (copy_to_user(argp, &success, sizeof(__u64)))
			return -EFAULT;

		return 0;
	}
	case GZVM_CAP_VM_GPA_SIZE: {
		ret = gzvm_cap_vm_gpa_size(argp);
		return ret;
	}
	default:
		break;
	}

	return -EOPNOTSUPP;
}

/**
 * gzvm_arch_create_vm() - create vm
 * @vm_type: VM type. Only supports Linux VM now.
 *
 * Return:
 * * positive value	- VM ID
 * * -ENOMEM		- Memory not enough for storing VM data
 */
int gzvm_arch_create_vm(unsigned long vm_type)
{
	struct arm_smccc_res res;
	int ret;

	ret = gzvm_hypcall_wrapper(MT_HVC_GZVM_CREATE_VM, vm_type, 0, 0, 0, 0,
				   0, 0, &res);
	return ret ? ret : res.a1;
}

int gzvm_arch_destroy_vm(u16 vm_id)
{
	struct arm_smccc_res res;

	return gzvm_hypcall_wrapper(MT_HVC_GZVM_DESTROY_VM, vm_id, 0, 0, 0, 0,
				    0, 0, &res);
}

static int gzvm_vm_arch_enable_cap(struct gzvm *gzvm,
				   struct gzvm_enable_cap *cap,
				   struct arm_smccc_res *res)
{
	return gzvm_hypcall_wrapper(MT_HVC_GZVM_ENABLE_CAP, gzvm->vm_id,
				    cap->cap, cap->args[0], cap->args[1],
				    cap->args[2], cap->args[3], cap->args[4],
				    res);
}

/**
 * gzvm_vm_ioctl_get_pvmfw_size() - Get pvmfw size from hypervisor, return
 *				    in x1, and return to userspace in args
 * @gzvm: Pointer to struct gzvm.
 * @cap: Pointer to struct gzvm_enable_cap.
 * @argp: Pointer to struct gzvm_enable_cap in user space.
 *
 * Return:
 * * 0			- Succeed
 * * -EINVAL		- Hypervisor return invalid results
 * * -EFAULT		- Fail to copy back to userspace buffer
 */
static int gzvm_vm_ioctl_get_pvmfw_size(struct gzvm *gzvm,
					struct gzvm_enable_cap *cap,
					void __user *argp)
{
	struct arm_smccc_res res = {0};

	if (gzvm_vm_arch_enable_cap(gzvm, cap, &res) != 0)
		return -EINVAL;

	cap->args[1] = res.a1;
	if (copy_to_user(argp, cap, sizeof(*cap)))
		return -EFAULT;

	return 0;
}

/**
 * gzvm_vm_ioctl_cap_pvm() - Proceed GZVM_CAP_PROTECTED_VM's subcommands
 * @gzvm: Pointer to struct gzvm.
 * @cap: Pointer to struct gzvm_enable_cap.
 * @argp: Pointer to struct gzvm_enable_cap in user space.
 *
 * Return:
 * * 0			- Succeed
 * * -EINVAL		- Invalid subcommand or arguments
 */
static int gzvm_vm_ioctl_cap_pvm(struct gzvm *gzvm,
				 struct gzvm_enable_cap *cap,
				 void __user *argp)
{
	struct arm_smccc_res res = {0};
	int ret;

	switch (cap->args[0]) {
	case GZVM_CAP_PVM_SET_PVMFW_GPA:
		fallthrough;
	case GZVM_CAP_PVM_SET_PROTECTED_VM:
		ret = gzvm_vm_arch_enable_cap(gzvm, cap, &res);
		return ret;
	case GZVM_CAP_PVM_GET_PVMFW_SIZE:
		ret = gzvm_vm_ioctl_get_pvmfw_size(gzvm, cap, argp);
		return ret;
	default:
		break;
	}

	return -EINVAL;
}

int gzvm_vm_ioctl_arch_enable_cap(struct gzvm *gzvm,
				  struct gzvm_enable_cap *cap,
				  void __user *argp)
{
	int ret;

	switch (cap->cap) {
	case GZVM_CAP_PROTECTED_VM:
		ret = gzvm_vm_ioctl_cap_pvm(gzvm, cap, argp);
		return ret;
	default:
		break;
	}

	return -EINVAL;
}
