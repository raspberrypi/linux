// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah_vm_mgr: " fmt

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include <uapi/linux/gunyah.h>

#include "rsc_mgr.h"
#include "vm_mgr.h"

static __must_check struct gunyah_vm *gunyah_vm_alloc(struct gunyah_rm *rm)
{
	struct gunyah_vm *ghvm;

	ghvm = kzalloc(sizeof(*ghvm), GFP_KERNEL);
	if (!ghvm)
		return ERR_PTR(-ENOMEM);

	ghvm->parent = gunyah_rm_get(rm);
	ghvm->rm = rm;

	return ghvm;
}

static int gunyah_vm_release(struct inode *inode, struct file *filp)
{
	struct gunyah_vm *ghvm = filp->private_data;

	gunyah_rm_put(ghvm->rm);
	kfree(ghvm);
	return 0;
}

static const struct file_operations gunyah_vm_fops = {
	.owner = THIS_MODULE,
	.release = gunyah_vm_release,
	.llseek = noop_llseek,
};

static long gunyah_dev_ioctl_create_vm(struct gunyah_rm *rm, unsigned long arg)
{
	struct gunyah_vm *ghvm;
	struct file *file;
	int fd, err;

	/* arg reserved for future use. */
	if (arg)
		return -EINVAL;

	ghvm = gunyah_vm_alloc(rm);
	if (IS_ERR(ghvm))
		return PTR_ERR(ghvm);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		err = fd;
		goto err_destroy_vm;
	}

	file = anon_inode_getfile("gunyah-vm", &gunyah_vm_fops, ghvm, O_RDWR);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_put_fd;
	}

	fd_install(fd, file);

	return fd;

err_put_fd:
	put_unused_fd(fd);
err_destroy_vm:
	gunyah_rm_put(ghvm->rm);
	kfree(ghvm);
	return err;
}

long gunyah_dev_vm_mgr_ioctl(struct gunyah_rm *rm, unsigned int cmd,
			     unsigned long arg)
{
	switch (cmd) {
	case GUNYAH_CREATE_VM:
		return gunyah_dev_ioctl_create_vm(rm, arg);
	default:
		return -ENOTTY;
	}
}
