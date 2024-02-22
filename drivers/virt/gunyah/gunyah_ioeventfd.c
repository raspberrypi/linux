// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/eventfd.h>
#include <linux/device/driver.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gunyah.h>
#include <linux/module.h>
#include <linux/printk.h>

#include <uapi/linux/gunyah.h>

struct gunyah_ioeventfd {
	struct gunyah_vm_function_instance *f;
	struct gunyah_vm_io_handler io_handler;

	struct eventfd_ctx *ctx;
};

static int gunyah_write_ioeventfd(struct gunyah_vm_io_handler *io_dev, u64 addr,
				  u32 len, u64 data)
{
	struct gunyah_ioeventfd *iofd =
		container_of(io_dev, struct gunyah_ioeventfd, io_handler);

	eventfd_signal(iofd->ctx, 1);
	return 0;
}

static struct gunyah_vm_io_handler_ops io_ops = {
	.write = gunyah_write_ioeventfd,
};

static long gunyah_ioeventfd_bind(struct gunyah_vm_function_instance *f)
{
	const struct gunyah_fn_ioeventfd_arg *args = f->argp;
	struct gunyah_ioeventfd *iofd;
	struct eventfd_ctx *ctx;
	int ret;

	if (f->arg_size != sizeof(*args))
		return -EINVAL;

	/* All other flag bits are reserved for future use */
	if (args->flags & ~GUNYAH_IOEVENTFD_FLAGS_DATAMATCH)
		return -EINVAL;

	/* must be natural-word sized, or 0 to ignore length */
	switch (args->len) {
	case 0:
	case 1:
	case 2:
	case 4:
	case 8:
		break;
	default:
		return -EINVAL;
	}

	/* check for range overflow */
	if (overflows_type(args->addr + args->len, u64))
		return -EINVAL;

	/* ioeventfd with no length can't be combined with DATAMATCH */
	if (!args->len && (args->flags & GUNYAH_IOEVENTFD_FLAGS_DATAMATCH))
		return -EINVAL;

	ctx = eventfd_ctx_fdget(args->fd);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	iofd = kzalloc(sizeof(*iofd), GFP_KERNEL);
	if (!iofd) {
		ret = -ENOMEM;
		goto err_eventfd;
	}

	f->data = iofd;
	iofd->f = f;

	iofd->ctx = ctx;

	if (args->flags & GUNYAH_IOEVENTFD_FLAGS_DATAMATCH) {
		iofd->io_handler.datamatch = true;
		iofd->io_handler.len = args->len;
		iofd->io_handler.data = args->datamatch;
	}
	iofd->io_handler.addr = args->addr;
	iofd->io_handler.ops = &io_ops;

	ret = gunyah_vm_add_io_handler(f->ghvm, &iofd->io_handler);
	if (ret)
		goto err_io_dev_add;

	return 0;

err_io_dev_add:
	kfree(iofd);
err_eventfd:
	eventfd_ctx_put(ctx);
	return ret;
}

static void gunyah_ioevent_unbind(struct gunyah_vm_function_instance *f)
{
	struct gunyah_ioeventfd *iofd = f->data;

	gunyah_vm_remove_io_handler(iofd->f->ghvm, &iofd->io_handler);
	eventfd_ctx_put(iofd->ctx);
	kfree(iofd);
}

static bool gunyah_ioevent_compare(const struct gunyah_vm_function_instance *f,
				   const void *arg, size_t size)
{
	const struct gunyah_fn_ioeventfd_arg *instance = f->argp, *other = arg;

	if (sizeof(*other) != size)
		return false;

	if (instance->addr != other->addr || instance->len != other->len ||
	    instance->flags != other->flags)
		return false;

	if ((instance->flags & GUNYAH_IOEVENTFD_FLAGS_DATAMATCH) &&
	    instance->datamatch != other->datamatch)
		return false;

	return true;
}

DECLARE_GUNYAH_VM_FUNCTION_INIT(ioeventfd, GUNYAH_FN_IOEVENTFD, 3,
				gunyah_ioeventfd_bind, gunyah_ioevent_unbind,
				gunyah_ioevent_compare);
MODULE_DESCRIPTION("Gunyah ioeventfd VM Function");
MODULE_LICENSE("GPL");
