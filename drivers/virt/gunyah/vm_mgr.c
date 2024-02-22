// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah_vm_mgr: " fmt

#include <linux/anon_inodes.h>
#include <linux/compat.h>
#include <linux/file.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/xarray.h>

#include <uapi/linux/gunyah.h>

#include "rsc_mgr.h"
#include "vm_mgr.h"

#define GUNYAH_VM_ADDRSPACE_LABEL 0
// "To" extent for memory private to guest
#define GUNYAH_VM_MEM_EXTENT_GUEST_PRIVATE_LABEL 0
// "From" extent for memory shared with guest
#define GUNYAH_VM_MEM_EXTENT_HOST_SHARED_LABEL 1
// "To" extent for memory shared with the guest
#define GUNYAH_VM_MEM_EXTENT_GUEST_SHARED_LABEL 3
// "From" extent for memory private to guest
#define GUNYAH_VM_MEM_EXTENT_HOST_PRIVATE_LABEL 2

static DEFINE_XARRAY(gunyah_vm_functions);

static void gunyah_vm_put_function(struct gunyah_vm_function *fn)
{
	module_put(fn->mod);
}

static struct gunyah_vm_function *gunyah_vm_get_function(u32 type)
{
	struct gunyah_vm_function *fn;

	fn = xa_load(&gunyah_vm_functions, type);
	if (!fn) {
		request_module("ghfunc:%d", type);

		fn = xa_load(&gunyah_vm_functions, type);
	}

	if (!fn || !try_module_get(fn->mod))
		fn = ERR_PTR(-ENOENT);

	return fn;
}

static void
gunyah_vm_remove_function_instance(struct gunyah_vm_function_instance *inst)
	__must_hold(&inst->ghvm->fn_lock)
{
	inst->fn->unbind(inst);
	list_del(&inst->vm_list);
	gunyah_vm_put_function(inst->fn);
	kfree(inst->argp);
	kfree(inst);
}

static void gunyah_vm_remove_functions(struct gunyah_vm *ghvm)
{
	struct gunyah_vm_function_instance *inst, *iiter;

	mutex_lock(&ghvm->fn_lock);
	list_for_each_entry_safe(inst, iiter, &ghvm->functions, vm_list) {
		gunyah_vm_remove_function_instance(inst);
	}
	mutex_unlock(&ghvm->fn_lock);
}

static long gunyah_vm_add_function_instance(struct gunyah_vm *ghvm,
					    struct gunyah_fn_desc *f)
{
	struct gunyah_vm_function_instance *inst;
	void __user *argp;
	long r = 0;

	if (f->arg_size > GUNYAH_FN_MAX_ARG_SIZE) {
		dev_err_ratelimited(ghvm->parent, "%s: arg_size > %d\n",
				    __func__, GUNYAH_FN_MAX_ARG_SIZE);
		return -EINVAL;
	}

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	inst->arg_size = f->arg_size;
	if (inst->arg_size) {
		inst->argp = kzalloc(inst->arg_size, GFP_KERNEL);
		if (!inst->argp) {
			r = -ENOMEM;
			goto free;
		}

		argp = u64_to_user_ptr(f->arg);
		if (copy_from_user(inst->argp, argp, f->arg_size)) {
			r = -EFAULT;
			goto free_arg;
		}
	}

	inst->fn = gunyah_vm_get_function(f->type);
	if (IS_ERR(inst->fn)) {
		r = PTR_ERR(inst->fn);
		goto free_arg;
	}

	inst->ghvm = ghvm;
	inst->rm = ghvm->rm;

	mutex_lock(&ghvm->fn_lock);
	r = inst->fn->bind(inst);
	if (r < 0) {
		mutex_unlock(&ghvm->fn_lock);
		gunyah_vm_put_function(inst->fn);
		goto free_arg;
	}

	list_add(&inst->vm_list, &ghvm->functions);
	mutex_unlock(&ghvm->fn_lock);

	return r;
free_arg:
	kfree(inst->argp);
free:
	kfree(inst);
	return r;
}

static long gunyah_vm_rm_function_instance(struct gunyah_vm *ghvm,
					   struct gunyah_fn_desc *f)
{
	struct gunyah_vm_function_instance *inst, *iter;
	void __user *user_argp;
	void *argp __free(kfree) = NULL;
	long r = 0;

	if (f->arg_size) {
		argp = kzalloc(f->arg_size, GFP_KERNEL);
		if (!argp)
			return -ENOMEM;

		user_argp = u64_to_user_ptr(f->arg);
		if (copy_from_user(argp, user_argp, f->arg_size))
			return -EFAULT;
	}

	r = mutex_lock_interruptible(&ghvm->fn_lock);
	if (r)
		return r;

	r = -ENOENT;
	list_for_each_entry_safe(inst, iter, &ghvm->functions, vm_list) {
		if (inst->fn->type == f->type &&
		    inst->fn->compare(inst, argp, f->arg_size)) {
			gunyah_vm_remove_function_instance(inst);
			r = 0;
		}
	}

	mutex_unlock(&ghvm->fn_lock);
	return r;
}

int gunyah_vm_function_register(struct gunyah_vm_function *fn)
{
	if (!fn->bind || !fn->unbind)
		return -EINVAL;

	return xa_err(xa_store(&gunyah_vm_functions, fn->type, fn, GFP_KERNEL));
}
EXPORT_SYMBOL_GPL(gunyah_vm_function_register);

void gunyah_vm_function_unregister(struct gunyah_vm_function *fn)
{
	/* Expecting unregister to only come when unloading a module */
	WARN_ON(fn->mod && module_refcount(fn->mod));
	xa_erase(&gunyah_vm_functions, fn->type);
}
EXPORT_SYMBOL_GPL(gunyah_vm_function_unregister);

static bool gunyah_vm_resource_ticket_populate_noop(
	struct gunyah_vm_resource_ticket *ticket, struct gunyah_resource *ghrsc)
{
	return true;
}
static void gunyah_vm_resource_ticket_unpopulate_noop(
	struct gunyah_vm_resource_ticket *ticket, struct gunyah_resource *ghrsc)
{
}

int gunyah_vm_add_resource_ticket(struct gunyah_vm *ghvm,
				  struct gunyah_vm_resource_ticket *ticket)
{
	struct gunyah_vm_resource_ticket *iter;
	struct gunyah_resource *ghrsc, *rsc_iter;
	int ret = 0;

	mutex_lock(&ghvm->resources_lock);
	list_for_each_entry(iter, &ghvm->resource_tickets, vm_list) {
		if (iter->resource_type == ticket->resource_type &&
		    iter->label == ticket->label) {
			ret = -EEXIST;
			goto out;
		}
	}

	if (!try_module_get(ticket->owner)) {
		ret = -ENODEV;
		goto out;
	}

	list_add(&ticket->vm_list, &ghvm->resource_tickets);
	INIT_LIST_HEAD(&ticket->resources);

	list_for_each_entry_safe(ghrsc, rsc_iter, &ghvm->resources, list) {
		if (ghrsc->type == ticket->resource_type &&
		    ghrsc->rm_label == ticket->label) {
			if (ticket->populate(ticket, ghrsc))
				list_move(&ghrsc->list, &ticket->resources);
		}
	}
out:
	mutex_unlock(&ghvm->resources_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_vm_add_resource_ticket);

static void
__gunyah_vm_remove_resource_ticket(struct gunyah_vm *ghvm,
				   struct gunyah_vm_resource_ticket *ticket)
{
	struct gunyah_resource *ghrsc, *iter;

	list_for_each_entry_safe(ghrsc, iter, &ticket->resources, list) {
		ticket->unpopulate(ticket, ghrsc);
		list_move(&ghrsc->list, &ghvm->resources);
	}

	module_put(ticket->owner);
	list_del(&ticket->vm_list);
}

void gunyah_vm_remove_resource_ticket(struct gunyah_vm *ghvm,
				      struct gunyah_vm_resource_ticket *ticket)
{
	mutex_lock(&ghvm->resources_lock);
	__gunyah_vm_remove_resource_ticket(ghvm, ticket);
	mutex_unlock(&ghvm->resources_lock);
}
EXPORT_SYMBOL_GPL(gunyah_vm_remove_resource_ticket);

static void gunyah_vm_add_resource(struct gunyah_vm *ghvm,
				   struct gunyah_resource *ghrsc)
{
	struct gunyah_vm_resource_ticket *ticket;

	mutex_lock(&ghvm->resources_lock);
	list_for_each_entry(ticket, &ghvm->resource_tickets, vm_list) {
		if (ghrsc->type == ticket->resource_type &&
		    ghrsc->rm_label == ticket->label) {
			if (ticket->populate(ticket, ghrsc))
				list_add(&ghrsc->list, &ticket->resources);
			else
				list_add(&ghrsc->list, &ghvm->resources);
			/* unconditonal -- we prevent multiple identical
			 * resource tickets so there will not be some other
			 * ticket elsewhere in the list if populate() failed.
			 */
			goto found;
		}
	}
	list_add(&ghrsc->list, &ghvm->resources);
found:
	mutex_unlock(&ghvm->resources_lock);
}

static void gunyah_vm_clean_resources(struct gunyah_vm *ghvm)
{
	struct gunyah_vm_resource_ticket *ticket, *titer;
	struct gunyah_resource *ghrsc, *riter;

	mutex_lock(&ghvm->resources_lock);
	if (!list_empty(&ghvm->resource_tickets)) {
		dev_warn(ghvm->parent, "Dangling resource tickets:\n");
		list_for_each_entry_safe(ticket, titer, &ghvm->resource_tickets,
					 vm_list) {
			dev_warn(ghvm->parent, "  %pS\n", ticket->populate);
			__gunyah_vm_remove_resource_ticket(ghvm, ticket);
		}
	}

	list_for_each_entry_safe(ghrsc, riter, &ghvm->resources, list) {
		gunyah_rm_free_resource(ghrsc);
	}
	mutex_unlock(&ghvm->resources_lock);
}

static int gunyah_vm_rm_notification_status(struct gunyah_vm *ghvm, void *data)
{
	struct gunyah_rm_vm_status_payload *payload = data;

	if (le16_to_cpu(payload->vmid) != ghvm->vmid)
		return NOTIFY_OK;

	/* All other state transitions are synchronous to a corresponding RM call */
	if (payload->vm_status == GUNYAH_RM_VM_STATUS_RESET) {
		down_write(&ghvm->status_lock);
		ghvm->vm_status = payload->vm_status;
		up_write(&ghvm->status_lock);
		wake_up(&ghvm->vm_status_wait);
	}

	return NOTIFY_DONE;
}

static int gunyah_vm_rm_notification_exited(struct gunyah_vm *ghvm, void *data)
{
	struct gunyah_rm_vm_exited_payload *payload = data;

	if (le16_to_cpu(payload->vmid) != ghvm->vmid)
		return NOTIFY_OK;

	down_write(&ghvm->status_lock);
	ghvm->vm_status = GUNYAH_RM_VM_STATUS_EXITED;
	ghvm->exit_info.type = le16_to_cpu(payload->exit_type);
	ghvm->exit_info.reason_size = le32_to_cpu(payload->exit_reason_size);
	memcpy(&ghvm->exit_info.reason, payload->exit_reason,
	       min(GUNYAH_VM_MAX_EXIT_REASON_SIZE,
		   ghvm->exit_info.reason_size));
	up_write(&ghvm->status_lock);
	wake_up(&ghvm->vm_status_wait);

	return NOTIFY_DONE;
}

static int gunyah_vm_rm_notification(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	struct gunyah_vm *ghvm = container_of(nb, struct gunyah_vm, nb);

	switch (action) {
	case GUNYAH_RM_NOTIFICATION_VM_STATUS:
		return gunyah_vm_rm_notification_status(ghvm, data);
	case GUNYAH_RM_NOTIFICATION_VM_EXITED:
		return gunyah_vm_rm_notification_exited(ghvm, data);
	default:
		return NOTIFY_OK;
	}
}

static void gunyah_vm_stop(struct gunyah_vm *ghvm)
{
	int ret;

	if (ghvm->vm_status == GUNYAH_RM_VM_STATUS_RUNNING) {
		ret = gunyah_rm_vm_stop(ghvm->rm, ghvm->vmid);
		if (ret)
			dev_warn(ghvm->parent, "Failed to stop VM: %d\n", ret);
	}

	wait_event(ghvm->vm_status_wait,
		   ghvm->vm_status != GUNYAH_RM_VM_STATUS_RUNNING);
}

static inline void setup_extent_ticket(struct gunyah_vm *ghvm,
				       struct gunyah_vm_resource_ticket *ticket,
				       u32 label)
{
	ticket->resource_type = GUNYAH_RESOURCE_TYPE_MEM_EXTENT;
	ticket->label = label;
	ticket->populate = gunyah_vm_resource_ticket_populate_noop;
	ticket->unpopulate = gunyah_vm_resource_ticket_unpopulate_noop;
	gunyah_vm_add_resource_ticket(ghvm, ticket);
}

static __must_check struct gunyah_vm *gunyah_vm_alloc(struct gunyah_rm *rm)
{
	struct gunyah_vm *ghvm;

	ghvm = kzalloc(sizeof(*ghvm), GFP_KERNEL);
	if (!ghvm)
		return ERR_PTR(-ENOMEM);

	ghvm->parent = gunyah_rm_get(rm);
	ghvm->vmid = GUNYAH_VMID_INVAL;
	ghvm->rm = rm;

	init_rwsem(&ghvm->status_lock);
	init_waitqueue_head(&ghvm->vm_status_wait);
	kref_init(&ghvm->kref);
	ghvm->vm_status = GUNYAH_RM_VM_STATUS_NO_STATE;

	INIT_LIST_HEAD(&ghvm->functions);
	mutex_init(&ghvm->fn_lock);
	mutex_init(&ghvm->resources_lock);
	INIT_LIST_HEAD(&ghvm->resources);
	INIT_LIST_HEAD(&ghvm->resource_tickets);

	mt_init(&ghvm->mm);
	mt_init(&ghvm->bindings);
	init_rwsem(&ghvm->bindings_lock);

	ghvm->addrspace_ticket.resource_type = GUNYAH_RESOURCE_TYPE_ADDR_SPACE;
	ghvm->addrspace_ticket.label = GUNYAH_VM_ADDRSPACE_LABEL;
	ghvm->addrspace_ticket.populate =
		gunyah_vm_resource_ticket_populate_noop;
	ghvm->addrspace_ticket.unpopulate =
		gunyah_vm_resource_ticket_unpopulate_noop;
	gunyah_vm_add_resource_ticket(ghvm, &ghvm->addrspace_ticket);

	setup_extent_ticket(ghvm, &ghvm->host_private_extent_ticket,
			    GUNYAH_VM_MEM_EXTENT_HOST_PRIVATE_LABEL);
	setup_extent_ticket(ghvm, &ghvm->host_shared_extent_ticket,
			    GUNYAH_VM_MEM_EXTENT_HOST_SHARED_LABEL);
	setup_extent_ticket(ghvm, &ghvm->guest_private_extent_ticket,
			    GUNYAH_VM_MEM_EXTENT_GUEST_PRIVATE_LABEL);
	setup_extent_ticket(ghvm, &ghvm->guest_shared_extent_ticket,
			    GUNYAH_VM_MEM_EXTENT_GUEST_SHARED_LABEL);

	return ghvm;
}

static int gunyah_vm_start(struct gunyah_vm *ghvm)
{
	struct gunyah_rm_hyp_resources *resources;
	struct gunyah_resource *ghrsc;
	int ret, i, n;

	down_write(&ghvm->status_lock);
	if (ghvm->vm_status != GUNYAH_RM_VM_STATUS_NO_STATE) {
		up_write(&ghvm->status_lock);
		return 0;
	}

	ghvm->nb.notifier_call = gunyah_vm_rm_notification;
	ret = gunyah_rm_notifier_register(ghvm->rm, &ghvm->nb);
	if (ret)
		goto err;

	ret = gunyah_rm_alloc_vmid(ghvm->rm, 0);
	if (ret < 0) {
		gunyah_rm_notifier_unregister(ghvm->rm, &ghvm->nb);
		goto err;
	}
	ghvm->vmid = ret;
	ghvm->vm_status = GUNYAH_RM_VM_STATUS_LOAD;

	ret = gunyah_rm_vm_configure(ghvm->rm, ghvm->vmid, ghvm->auth, 0, 0, 0,
				     0, 0);
	if (ret) {
		dev_warn(ghvm->parent, "Failed to configure VM: %d\n", ret);
		goto err;
	}

	ret = gunyah_rm_vm_init(ghvm->rm, ghvm->vmid);
	if (ret) {
		ghvm->vm_status = GUNYAH_RM_VM_STATUS_INIT_FAILED;
		dev_warn(ghvm->parent, "Failed to initialize VM: %d\n", ret);
		goto err;
	}
	ghvm->vm_status = GUNYAH_RM_VM_STATUS_READY;

	ret = gunyah_rm_get_hyp_resources(ghvm->rm, ghvm->vmid, &resources);
	if (ret) {
		dev_warn(ghvm->parent,
			 "Failed to get hypervisor resources for VM: %d\n",
			 ret);
		goto err;
	}

	for (i = 0, n = le32_to_cpu(resources->n_entries); i < n; i++) {
		ghrsc = gunyah_rm_alloc_resource(ghvm->rm,
						 &resources->entries[i]);
		if (!ghrsc) {
			ret = -ENOMEM;
			goto err;
		}

		gunyah_vm_add_resource(ghvm, ghrsc);
	}

	ret = gunyah_rm_vm_start(ghvm->rm, ghvm->vmid);
	if (ret) {
		dev_warn(ghvm->parent, "Failed to start VM: %d\n", ret);
		goto err;
	}

	ghvm->vm_status = GUNYAH_RM_VM_STATUS_RUNNING;
	up_write(&ghvm->status_lock);
	return ret;
err:
	/* gunyah_vm_free will handle releasing resources and reclaiming memory */
	up_write(&ghvm->status_lock);
	return ret;
}

static int gunyah_vm_ensure_started(struct gunyah_vm *ghvm)
{
	int ret;

	ret = down_read_interruptible(&ghvm->status_lock);
	if (ret)
		return ret;

	/* Unlikely because VM is typically started */
	if (unlikely(ghvm->vm_status == GUNYAH_RM_VM_STATUS_NO_STATE)) {
		up_read(&ghvm->status_lock);
		ret = gunyah_vm_start(ghvm);
		if (ret)
			return ret;
		ret = down_read_interruptible(&ghvm->status_lock);
		if (ret)
			return ret;
	}

	/* Unlikely because VM is typically running */
	if (unlikely(ghvm->vm_status != GUNYAH_RM_VM_STATUS_RUNNING))
		ret = -ENODEV;

	up_read(&ghvm->status_lock);
	return ret;
}

static long gunyah_vm_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct gunyah_vm *ghvm = filp->private_data;
	void __user *argp = (void __user *)arg;
	long r;

	switch (cmd) {
	case GUNYAH_VM_START: {
		r = gunyah_vm_ensure_started(ghvm);
		break;
	}
	case GUNYAH_VM_ADD_FUNCTION: {
		struct gunyah_fn_desc f;

		if (copy_from_user(&f, argp, sizeof(f)))
			return -EFAULT;

		r = gunyah_vm_add_function_instance(ghvm, &f);
		break;
	}
	case GUNYAH_VM_REMOVE_FUNCTION: {
		struct gunyah_fn_desc f;

		if (copy_from_user(&f, argp, sizeof(f)))
			return -EFAULT;

		r = gunyah_vm_rm_function_instance(ghvm, &f);
		break;
	}
	case GUNYAH_VM_MAP_MEM: {
		struct gunyah_map_mem_args args;

		if (copy_from_user(&args, argp, sizeof(args)))
			return -EFAULT;

		return gunyah_gmem_modify_mapping(ghvm, &args);
	}
	default:
		r = -ENOTTY;
		break;
	}

	return r;
}

int __must_check gunyah_vm_get(struct gunyah_vm *ghvm)
{
	return kref_get_unless_zero(&ghvm->kref);
}
EXPORT_SYMBOL_GPL(gunyah_vm_get);

static void _gunyah_vm_put(struct kref *kref)
{
	struct gunyah_vm *ghvm = container_of(kref, struct gunyah_vm, kref);
	struct gunyah_gmem_binding *b;
	unsigned long idx = 0;
	int ret;

	/**
	 * We might race with a VM exit notification, but that's ok:
	 * gh_rm_vm_stop() will just return right away.
	 */
	if (ghvm->vm_status == GUNYAH_RM_VM_STATUS_RUNNING)
		gunyah_vm_stop(ghvm);

	gunyah_vm_remove_functions(ghvm);

	down_write(&ghvm->bindings_lock);
	mt_for_each(&ghvm->bindings, b, idx, ULONG_MAX) {
		gunyah_gmem_remove_binding(b);
	}
	up_write(&ghvm->bindings_lock);
	WARN_ON(!mtree_empty(&ghvm->bindings));
	mtree_destroy(&ghvm->bindings);
	/**
	 * If this fails, we're going to lose the memory for good and is
	 * BUG_ON-worthy, but not unrecoverable (we just lose memory).
	 * This call should always succeed though because the VM is in not
	 * running and RM will let us reclaim all the memory.
	 */
	WARN_ON(gunyah_vm_reclaim_range(ghvm, 0, U64_MAX));

	/* clang-format off */
	gunyah_vm_remove_resource_ticket(ghvm, &ghvm->addrspace_ticket);
	gunyah_vm_remove_resource_ticket(ghvm, &ghvm->host_shared_extent_ticket);
	gunyah_vm_remove_resource_ticket(ghvm, &ghvm->host_private_extent_ticket);
	gunyah_vm_remove_resource_ticket(ghvm, &ghvm->guest_shared_extent_ticket);
	gunyah_vm_remove_resource_ticket(ghvm, &ghvm->guest_private_extent_ticket);
	/* clang-format on */

	gunyah_vm_clean_resources(ghvm);

	if (ghvm->vm_status == GUNYAH_RM_VM_STATUS_EXITED ||
	    ghvm->vm_status == GUNYAH_RM_VM_STATUS_READY ||
	    ghvm->vm_status == GUNYAH_RM_VM_STATUS_INIT_FAILED) {
		ret = gunyah_rm_vm_reset(ghvm->rm, ghvm->vmid);
		/* clang-format off */
		if (!ret)
			wait_event(ghvm->vm_status_wait,
				   ghvm->vm_status == GUNYAH_RM_VM_STATUS_RESET);
		else
			dev_err(ghvm->parent, "Failed to reset the vm: %d\n",ret);
		/* clang-format on */
	}

	WARN_ON(!mtree_empty(&ghvm->mm));
	mtree_destroy(&ghvm->mm);

	if (ghvm->vm_status > GUNYAH_RM_VM_STATUS_NO_STATE) {
		gunyah_rm_notifier_unregister(ghvm->rm, &ghvm->nb);

		ret = gunyah_rm_dealloc_vmid(ghvm->rm, ghvm->vmid);
		if (ret)
			dev_warn(ghvm->parent,
				 "Failed to deallocate vmid: %d\n", ret);
	}

	gunyah_rm_put(ghvm->rm);
	kfree(ghvm);
}

void gunyah_vm_put(struct gunyah_vm *ghvm)
{
	kref_put(&ghvm->kref, _gunyah_vm_put);
}
EXPORT_SYMBOL_GPL(gunyah_vm_put);

static int gunyah_vm_release(struct inode *inode, struct file *filp)
{
	struct gunyah_vm *ghvm = filp->private_data;

	gunyah_vm_put(ghvm);
	return 0;
}

static const struct file_operations gunyah_vm_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gunyah_vm_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
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
	case GUNYAH_CREATE_GUEST_MEM: {
		struct gunyah_create_mem_args args;

		if (copy_from_user(&args, (const void __user *)arg,
				   sizeof(args)))
			return -EFAULT;

		return gunyah_guest_mem_create(&args);
	}
	default:
		return -ENOTTY;
	}
}
