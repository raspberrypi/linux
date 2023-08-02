// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/kdev_t.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gzvm_drv.h>

static DEFINE_MUTEX(gzvm_list_lock);
static LIST_HEAD(gzvm_list);

static void gzvm_destroy_vm(struct gzvm *gzvm)
{
	pr_debug("VM-%u is going to be destroyed\n", gzvm->vm_id);

	mutex_lock(&gzvm->lock);

	gzvm_arch_destroy_vm(gzvm->vm_id);

	mutex_lock(&gzvm_list_lock);
	list_del(&gzvm->vm_list);
	mutex_unlock(&gzvm_list_lock);

	mutex_unlock(&gzvm->lock);

	kfree(gzvm);
}

static int gzvm_vm_release(struct inode *inode, struct file *filp)
{
	struct gzvm *gzvm = filp->private_data;

	gzvm_destroy_vm(gzvm);
	return 0;
}

static const struct file_operations gzvm_vm_fops = {
	.release        = gzvm_vm_release,
	.llseek		= noop_llseek,
};

static struct gzvm *gzvm_create_vm(unsigned long vm_type)
{
	int ret;
	struct gzvm *gzvm;

	gzvm = kzalloc(sizeof(*gzvm), GFP_KERNEL);
	if (!gzvm)
		return ERR_PTR(-ENOMEM);

	ret = gzvm_arch_create_vm(vm_type);
	if (ret < 0) {
		kfree(gzvm);
		return ERR_PTR(ret);
	}

	gzvm->vm_id = ret;
	gzvm->mm = current->mm;
	mutex_init(&gzvm->lock);

	mutex_lock(&gzvm_list_lock);
	list_add(&gzvm->vm_list, &gzvm_list);
	mutex_unlock(&gzvm_list_lock);

	pr_debug("VM-%u is created\n", gzvm->vm_id);

	return gzvm;
}

/**
 * gzvm_dev_ioctl_create_vm - Create vm fd
 * @vm_type: VM type. Only supports Linux VM now.
 *
 * Return: fd of vm, negative if error
 */
int gzvm_dev_ioctl_create_vm(unsigned long vm_type)
{
	struct gzvm *gzvm;

	gzvm = gzvm_create_vm(vm_type);
	if (IS_ERR(gzvm))
		return PTR_ERR(gzvm);

	return anon_inode_getfd("gzvm-vm", &gzvm_vm_fops, gzvm,
			       O_RDWR | O_CLOEXEC);
}

void gzvm_destroy_all_vms(void)
{
	struct gzvm *gzvm, *tmp;

	mutex_lock(&gzvm_list_lock);
	if (list_empty(&gzvm_list))
		goto out;

	list_for_each_entry_safe(gzvm, tmp, &gzvm_list, vm_list)
		gzvm_destroy_vm(gzvm);

out:
	mutex_unlock(&gzvm_list_lock);
}
