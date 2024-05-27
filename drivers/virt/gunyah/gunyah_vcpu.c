// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/gunyah.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "vm_mgr.h"

#include <uapi/linux/gunyah.h>

#define MAX_VCPU_NAME 20 /* gh-vcpu:strlen(U32::MAX)+NUL */

/**
 * struct gunyah_vcpu - Track an instance of gunyah vCPU
 * @f: Function instance (how we get associated with the main VM)
 * @rsc: Pointer to the Gunyah vCPU resource, will be NULL until VM starts
 * @run_lock: One userspace thread at a time should run the vCPU
 * @ghvm: Pointer to the main VM struct; quicker look up than going through
 *        @f->ghvm
 * @vcpu_run: Pointer to page shared with userspace to communicate vCPU state
 * @state: Our copy of the state of the vCPU, since userspace could trick
 *         kernel to behave incorrectly if we relied on @vcpu_run
 * @mmio_read_len: Our copy of @vcpu_run->mmio.len; see also @state
 * @mmio_addr: Our copy of @vcpu_run->mmio.phys_addr; see also @state
 * @ready: if vCPU goes to sleep, hypervisor reports to us that it's sleeping
 *         and will signal interrupt (from @rsc) when it's time to wake up.
 *         This completion signals that we can run vCPU again.
 * @nb: When VM exits, the status of VM is reported via @vcpu_run->status.
 *      We need to track overall VM status, and the nb gives us the updates from
 *      Resource Manager.
 * @ticket: resource ticket to claim vCPU# for the VM
 * @kref: Reference counter
 */
struct gunyah_vcpu {
	struct gunyah_vm_function_instance *f;
	struct gunyah_resource *rsc;
	struct mutex run_lock;
	struct gunyah_vm *ghvm;

	struct gunyah_vcpu_run *vcpu_run;

	/**
	 * Track why the vcpu_run hypercall returned. This mirrors the vcpu_run
	 * structure shared with userspace, except is used internally to avoid
	 * trusting userspace to not modify the vcpu_run structure.
	 */
	enum {
		GUNYAH_VCPU_RUN_STATE_UNKNOWN = 0,
		GUNYAH_VCPU_RUN_STATE_READY,
		GUNYAH_VCPU_RUN_STATE_MMIO_READ,
		GUNYAH_VCPU_RUN_STATE_MMIO_WRITE,
		GUNYAH_VCPU_RUN_STATE_SYSTEM_DOWN,
	} state;
	u8 mmio_read_len;
	u64 mmio_addr;

	struct completion ready;

	struct notifier_block nb;
	struct gunyah_vm_resource_ticket ticket;
	struct kref kref;
};

static void vcpu_release(struct kref *kref)
{
	struct gunyah_vcpu *vcpu = container_of(kref, struct gunyah_vcpu, kref);

	free_page((unsigned long)vcpu->vcpu_run);
	kfree(vcpu);
}

/*
 * When hypervisor allows us to schedule vCPU again, it gives us an interrupt
 */
static irqreturn_t gunyah_vcpu_irq_handler(int irq, void *data)
{
	struct gunyah_vcpu *vcpu = data;

	complete(&vcpu->ready);
	return IRQ_HANDLED;
}

static bool gunyah_handle_page_fault(
	struct gunyah_vcpu *vcpu,
	const struct gunyah_hypercall_vcpu_run_resp *vcpu_run_resp)
{
	u64 addr = vcpu_run_resp->state_data[0];
	bool write = !!vcpu_run_resp->state_data[1];
	int ret = 0;

	ret = gunyah_gup_demand_page(vcpu->ghvm, addr, write);
	if (!ret || ret == -EAGAIN)
		return true;

	vcpu->vcpu_run->page_fault.resume_action = GUNYAH_VCPU_RESUME_FAULT;
	vcpu->vcpu_run->page_fault.attempt = ret;
	vcpu->vcpu_run->page_fault.phys_addr = addr;
	vcpu->vcpu_run->exit_reason = GUNYAH_VCPU_EXIT_PAGE_FAULT;
	return false;
}

static bool
gunyah_handle_mmio(struct gunyah_vcpu *vcpu, unsigned long resume_data[3],
		   const struct gunyah_hypercall_vcpu_run_resp *vcpu_run_resp)
{
	u64 addr = vcpu_run_resp->state_data[0],
	    len = vcpu_run_resp->state_data[1],
	    data = vcpu_run_resp->state_data[2];
	int ret;

	if (WARN_ON(len > sizeof(u64)))
		len = sizeof(u64);

	ret = gunyah_gup_demand_page(vcpu->ghvm, addr,
					vcpu->vcpu_run->mmio.is_write);
	if (!ret || ret == -EAGAIN) {
		resume_data[1] = GUNYAH_ADDRSPACE_VMMIO_ACTION_RETRY;
		return true;
	}

	if (vcpu_run_resp->state == GUNYAH_VCPU_ADDRSPACE_VMMIO_READ) {
		vcpu->vcpu_run->mmio.is_write = 0;
		/* Record that we need to give vCPU user's supplied value next gunyah_vcpu_run() */
		vcpu->state = GUNYAH_VCPU_RUN_STATE_MMIO_READ;
		vcpu->mmio_read_len = len;
	} else { /* GUNYAH_VCPU_ADDRSPACE_VMMIO_WRITE */
		if (!gunyah_vm_mmio_write(vcpu->ghvm, addr, len, data)) {
			resume_data[0] = GUNYAH_ADDRSPACE_VMMIO_ACTION_EMULATE;
			return true;
		}
		vcpu->vcpu_run->mmio.is_write = 1;
		memcpy(vcpu->vcpu_run->mmio.data, &data, len);
		vcpu->state = GUNYAH_VCPU_RUN_STATE_MMIO_WRITE;
	}

	/* Assume userspace is okay and handles the access due to existing userspace */
	vcpu->vcpu_run->mmio.resume_action = GUNYAH_VCPU_RESUME_HANDLED;
	vcpu->mmio_addr = vcpu->vcpu_run->mmio.phys_addr = addr;
	vcpu->vcpu_run->mmio.len = len;
	vcpu->vcpu_run->exit_reason = GUNYAH_VCPU_EXIT_MMIO;

	return false;
}

static int gunyah_handle_mmio_resume(struct gunyah_vcpu *vcpu,
				     unsigned long resume_data[3])
{
	switch (vcpu->vcpu_run->mmio.resume_action) {
	case GUNYAH_VCPU_RESUME_HANDLED:
		if (vcpu->state == GUNYAH_VCPU_RUN_STATE_MMIO_READ) {
			if (unlikely(vcpu->mmio_read_len >
				     sizeof(resume_data[0])))
				vcpu->mmio_read_len = sizeof(resume_data[0]);
			memcpy(&resume_data[0], vcpu->vcpu_run->mmio.data,
			       vcpu->mmio_read_len);
		}
		resume_data[1] = GUNYAH_ADDRSPACE_VMMIO_ACTION_EMULATE;
		break;
	case GUNYAH_VCPU_RESUME_FAULT:
		resume_data[1] = GUNYAH_ADDRSPACE_VMMIO_ACTION_FAULT;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int gunyah_vcpu_rm_notification(struct notifier_block *nb,
				       unsigned long action, void *data)
{
	struct gunyah_vcpu *vcpu = container_of(nb, struct gunyah_vcpu, nb);
	struct gunyah_rm_vm_exited_payload *exit_payload = data;

	/* Wake up userspace waiting for the vCPU to be runnable again */
	if (action == GUNYAH_RM_NOTIFICATION_VM_EXITED &&
	    le16_to_cpu(exit_payload->vmid) == vcpu->ghvm->vmid)
		complete(&vcpu->ready);

	return NOTIFY_OK;
}

static inline enum gunyah_vm_status
remap_vm_status(enum gunyah_rm_vm_status rm_status)
{
	switch (rm_status) {
	case GUNYAH_RM_VM_STATUS_INIT_FAILED:
		return GUNYAH_VM_STATUS_LOAD_FAILED;
	case GUNYAH_RM_VM_STATUS_EXITED:
		return GUNYAH_VM_STATUS_EXITED;
	default:
		return GUNYAH_VM_STATUS_CRASHED;
	}
}

/**
 * gunyah_vcpu_check_system() - Check whether VM as a whole is running
 * @vcpu: Pointer to gunyah_vcpu
 *
 * Returns true if the VM is alive.
 * Returns false if the vCPU is the VM is not alive (can only be that VM is shutting down).
 */
static bool gunyah_vcpu_check_system(struct gunyah_vcpu *vcpu)
	__must_hold(&vcpu->run_lock)
{
	bool ret = true;

	down_read(&vcpu->ghvm->status_lock);
	if (likely(vcpu->ghvm->vm_status == GUNYAH_RM_VM_STATUS_RUNNING))
		goto out;

	vcpu->vcpu_run->status.status = remap_vm_status(vcpu->ghvm->vm_status);
	vcpu->vcpu_run->status.exit_info = vcpu->ghvm->exit_info;
	vcpu->vcpu_run->exit_reason = GUNYAH_VCPU_EXIT_STATUS;
	vcpu->state = GUNYAH_VCPU_RUN_STATE_SYSTEM_DOWN;
	ret = false;
out:
	up_read(&vcpu->ghvm->status_lock);
	return ret;
}

/**
 * gunyah_vcpu_run() - Request Gunyah to begin scheduling this vCPU.
 * @vcpu: The client descriptor that was obtained via gunyah_vcpu_alloc()
 */
static int gunyah_vcpu_run(struct gunyah_vcpu *vcpu)
{
	struct gunyah_hypercall_vcpu_run_resp vcpu_run_resp;
	unsigned long resume_data[3] = { 0 };
	enum gunyah_error gunyah_error;
	int ret = 0;

	if (!vcpu->f)
		return -ENODEV;

	if (mutex_lock_interruptible(&vcpu->run_lock))
		return -ERESTARTSYS;

	if (!vcpu->rsc) {
		ret = -ENODEV;
		goto out;
	}

	switch (vcpu->state) {
	case GUNYAH_VCPU_RUN_STATE_UNKNOWN:
		if (vcpu->ghvm->vm_status != GUNYAH_RM_VM_STATUS_RUNNING) {
			/**
			 * Check if VM is up. If VM is starting, will block
			 * until VM is fully up since that thread does
			 * down_write.
			 */
			if (!gunyah_vcpu_check_system(vcpu))
				goto out;
		}
		vcpu->state = GUNYAH_VCPU_RUN_STATE_READY;
		break;
	case GUNYAH_VCPU_RUN_STATE_MMIO_READ:
	case GUNYAH_VCPU_RUN_STATE_MMIO_WRITE:
		ret = gunyah_handle_mmio_resume(vcpu, resume_data);
		if (ret)
			goto out;
		vcpu->state = GUNYAH_VCPU_RUN_STATE_READY;
		break;
	case GUNYAH_VCPU_RUN_STATE_SYSTEM_DOWN:
		goto out;
	default:
		break;
	}

	if (current->mm != vcpu->ghvm->mm_s) {
		ret = -EPERM;
		goto out;
	}

	while (!ret && !signal_pending(current)) {
		if (vcpu->vcpu_run->immediate_exit) {
			ret = -EINTR;
			goto out;
		}

		gunyah_error = gunyah_hypercall_vcpu_run(
			vcpu->rsc->capid, resume_data, &vcpu_run_resp);
		if (gunyah_error == GUNYAH_ERROR_OK) {
			memset(resume_data, 0, sizeof(resume_data));
			switch (vcpu_run_resp.state) {
			case GUNYAH_VCPU_STATE_READY:
				if (need_resched())
					schedule();
				break;
			case GUNYAH_VCPU_STATE_POWERED_OFF:
				/**
				 * vcpu might be off because the VM is shut down
				 * If so, it won't ever run again
				 */
				if (!gunyah_vcpu_check_system(vcpu))
					goto out;
				/**
				 * Otherwise, another vcpu will turn it on (e.g.
				 * by PSCI) and hyp sends an interrupt to wake
				 * Linux up.
				 */
				fallthrough;
			case GUNYAH_VCPU_STATE_EXPECTS_WAKEUP:
				ret = wait_for_completion_interruptible(
					&vcpu->ready);
				/**
				 * reinitialize completion before next
				 * hypercall. If we reinitialize after the
				 * hypercall, interrupt may have already come
				 * before re-initializing the completion and
				 * then end up waiting for event that already
				 * happened.
				 */
				reinit_completion(&vcpu->ready);
				/**
				 * Check VM status again. Completion
				 * might've come from VM exiting
				 */
				if (!ret && !gunyah_vcpu_check_system(vcpu))
					goto out;
				break;
			case GUNYAH_VCPU_STATE_BLOCKED:
				schedule();
				break;
			case GUNYAH_VCPU_ADDRSPACE_VMMIO_READ:
			case GUNYAH_VCPU_ADDRSPACE_VMMIO_WRITE:
				if (!gunyah_handle_mmio(vcpu, resume_data,
							&vcpu_run_resp))
					goto out;
				break;
			case GUNYAH_VCPU_ADDRSPACE_PAGE_FAULT:
				if (!gunyah_handle_page_fault(vcpu,
							      &vcpu_run_resp))
					goto out;
				break;
			default:
				pr_warn_ratelimited(
					"Unknown vCPU state: %llx\n",
					vcpu_run_resp.sized_state);
				schedule();
				break;
			}
		} else if (gunyah_error == GUNYAH_ERROR_RETRY) {
			schedule();
		} else {
			ret = gunyah_error_remap(gunyah_error);
		}
	}

out:
	mutex_unlock(&vcpu->run_lock);

	if (signal_pending(current))
		return -ERESTARTSYS;

	return ret;
}

static long gunyah_vcpu_ioctl(struct file *filp, unsigned int cmd,
			      unsigned long arg)
{
	struct gunyah_vcpu *vcpu = filp->private_data;
	long ret = -ENOTTY;

	switch (cmd) {
	case GUNYAH_VCPU_RUN:
		ret = gunyah_vcpu_run(vcpu);
		break;
	case GUNYAH_VCPU_MMAP_SIZE:
		ret = PAGE_SIZE;
		break;
	default:
		break;
	}
	return ret;
}

static int gunyah_vcpu_release(struct inode *inode, struct file *filp)
{
	struct gunyah_vcpu *vcpu = filp->private_data;

	gunyah_vm_put(vcpu->ghvm);
	kref_put(&vcpu->kref, vcpu_release);
	return 0;
}

static vm_fault_t gunyah_vcpu_fault(struct vm_fault *vmf)
{
	struct gunyah_vcpu *vcpu = vmf->vma->vm_file->private_data;
	struct page *page;

	if (vmf->pgoff)
		return VM_FAULT_SIGBUS;

	page = virt_to_page(vcpu->vcpu_run);
	get_page(page);
	vmf->page = page;
	return 0;
}

static const struct vm_operations_struct gunyah_vcpu_ops = {
	.fault = gunyah_vcpu_fault,
};

static int gunyah_vcpu_mmap(struct file *file, struct vm_area_struct *vma)
{
	vma->vm_ops = &gunyah_vcpu_ops;
	return 0;
}

static const struct file_operations gunyah_vcpu_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gunyah_vcpu_ioctl,
	.release = gunyah_vcpu_release,
	.llseek = noop_llseek,
	.mmap = gunyah_vcpu_mmap,
};

static bool gunyah_vcpu_populate(struct gunyah_vm_resource_ticket *ticket,
				 struct gunyah_resource *ghrsc)
{
	struct gunyah_vcpu *vcpu =
		container_of(ticket, struct gunyah_vcpu, ticket);
	int ret;

	mutex_lock(&vcpu->run_lock);
	if (vcpu->rsc) {
		pr_warn("vcpu%d already got a Gunyah resource. Check if multiple resources with same label were configured.\n",
			vcpu->ticket.label);
		ret = -EEXIST;
		goto out;
	}

	vcpu->rsc = ghrsc;

	ret = request_irq(vcpu->rsc->irq, gunyah_vcpu_irq_handler,
			  IRQF_TRIGGER_RISING, "gunyah_vcpu", vcpu);
	if (ret) {
		pr_warn("Failed to request vcpu irq %d: %d", vcpu->rsc->irq,
			ret);
		goto out;
	}

	enable_irq_wake(vcpu->rsc->irq);

out:
	mutex_unlock(&vcpu->run_lock);
	return !ret;
}

static void gunyah_vcpu_unpopulate(struct gunyah_vm_resource_ticket *ticket,
				   struct gunyah_resource *ghrsc)
{
	struct gunyah_vcpu *vcpu =
		container_of(ticket, struct gunyah_vcpu, ticket);

	vcpu->vcpu_run->immediate_exit = true;
	complete_all(&vcpu->ready);
	mutex_lock(&vcpu->run_lock);
	free_irq(vcpu->rsc->irq, vcpu);
	vcpu->rsc = NULL;
	mutex_unlock(&vcpu->run_lock);
}

static long gunyah_vcpu_bind(struct gunyah_vm_function_instance *f)
{
	struct gunyah_fn_vcpu_arg *arg = f->argp;
	struct gunyah_vcpu *vcpu;
	char name[MAX_VCPU_NAME];
	struct file *file;
	struct page *page;
	int fd;
	long r;

	if (f->arg_size != sizeof(*arg))
		return -EINVAL;

	vcpu = kzalloc(sizeof(*vcpu), GFP_KERNEL);
	if (!vcpu)
		return -ENOMEM;

	vcpu->f = f;
	f->data = vcpu;
	mutex_init(&vcpu->run_lock);
	kref_init(&vcpu->kref);
	init_completion(&vcpu->ready);

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		r = -ENOMEM;
		goto err_destroy_vcpu;
	}
	vcpu->vcpu_run = page_address(page);

	vcpu->ticket.resource_type = GUNYAH_RESOURCE_TYPE_VCPU;
	vcpu->ticket.label = arg->id;
	vcpu->ticket.owner = THIS_MODULE;
	vcpu->ticket.populate = gunyah_vcpu_populate;
	vcpu->ticket.unpopulate = gunyah_vcpu_unpopulate;

	r = gunyah_vm_add_resource_ticket(f->ghvm, &vcpu->ticket);
	if (r)
		goto err_destroy_page;

	if (!gunyah_vm_get(f->ghvm)) {
		r = -ENODEV;
		goto err_remove_resource_ticket;
	}
	vcpu->ghvm = f->ghvm;

	vcpu->nb.notifier_call = gunyah_vcpu_rm_notification;
	/**
	 * Ensure we run after the vm_mgr handles the notification and does
	 * any necessary state changes.
	 */
	vcpu->nb.priority = -1;
	r = gunyah_rm_notifier_register(f->rm, &vcpu->nb);
	if (r)
		goto err_put_gunyah_vm;

	kref_get(&vcpu->kref);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		r = fd;
		goto err_notifier;
	}

	snprintf(name, sizeof(name), "gh-vcpu:%u", vcpu->ticket.label);
	file = anon_inode_getfile(name, &gunyah_vcpu_fops, vcpu, O_RDWR);
	if (IS_ERR(file)) {
		r = PTR_ERR(file);
		goto err_put_fd;
	}

	fd_install(fd, file);

	return fd;
err_put_fd:
	put_unused_fd(fd);
err_notifier:
	gunyah_rm_notifier_unregister(f->rm, &vcpu->nb);
err_put_gunyah_vm:
	gunyah_vm_put(vcpu->ghvm);
err_remove_resource_ticket:
	gunyah_vm_remove_resource_ticket(f->ghvm, &vcpu->ticket);
err_destroy_page:
	free_page((unsigned long)vcpu->vcpu_run);
err_destroy_vcpu:
	kfree(vcpu);
	return r;
}

static void gunyah_vcpu_unbind(struct gunyah_vm_function_instance *f)
{
	struct gunyah_vcpu *vcpu = f->data;

	gunyah_rm_notifier_unregister(f->rm, &vcpu->nb);
	gunyah_vm_remove_resource_ticket(vcpu->ghvm, &vcpu->ticket);
	vcpu->f = NULL;

	kref_put(&vcpu->kref, vcpu_release);
}

static bool gunyah_vcpu_compare(const struct gunyah_vm_function_instance *f,
				const void *arg, size_t size)
{
	const struct gunyah_fn_vcpu_arg *instance = f->argp, *other = arg;

	if (sizeof(*other) != size)
		return false;

	return instance->id == other->id;
}

DECLARE_GUNYAH_VM_FUNCTION_INIT(vcpu, GUNYAH_FN_VCPU, 1, gunyah_vcpu_bind,
				gunyah_vcpu_unbind, gunyah_vcpu_compare);
MODULE_DESCRIPTION("Gunyah vCPU Function");
MODULE_LICENSE("GPL");
