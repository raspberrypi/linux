/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _LINUX_GUNYAH_H
#define _LINUX_GUNYAH_H

#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/limits.h>

/* Matches resource manager's resource types for VM_GET_HYP_RESOURCES RPC */
enum gunyah_resource_type {
	/* clang-format off */
	GUNYAH_RESOURCE_TYPE_BELL_TX	= 0,
	GUNYAH_RESOURCE_TYPE_BELL_RX	= 1,
	GUNYAH_RESOURCE_TYPE_MSGQ_TX	= 2,
	GUNYAH_RESOURCE_TYPE_MSGQ_RX	= 3,
	GUNYAH_RESOURCE_TYPE_VCPU	= 4,
	GUNYAH_RESOURCE_TYPE_MEM_EXTENT	= 9,
	GUNYAH_RESOURCE_TYPE_ADDR_SPACE	= 10,
	/* clang-format on */
};

struct gunyah_resource {
	enum gunyah_resource_type type;
	u64 capid;
	unsigned int irq;
};

/******************************************************************************/
/* Common arch-independent definitions for Gunyah hypercalls                  */
#define GUNYAH_CAPID_INVAL U64_MAX
#define GUNYAH_VMID_ROOT_VM 0xff

enum gunyah_error {
	/* clang-format off */
	GUNYAH_ERROR_OK				= 0,
	GUNYAH_ERROR_UNIMPLEMENTED		= -1,
	GUNYAH_ERROR_RETRY			= -2,

	GUNYAH_ERROR_ARG_INVAL			= 1,
	GUNYAH_ERROR_ARG_SIZE			= 2,
	GUNYAH_ERROR_ARG_ALIGN			= 3,

	GUNYAH_ERROR_NOMEM			= 10,

	GUNYAH_ERROR_ADDR_OVFL			= 20,
	GUNYAH_ERROR_ADDR_UNFL			= 21,
	GUNYAH_ERROR_ADDR_INVAL			= 22,

	GUNYAH_ERROR_DENIED			= 30,
	GUNYAH_ERROR_BUSY			= 31,
	GUNYAH_ERROR_IDLE			= 32,

	GUNYAH_ERROR_IRQ_BOUND			= 40,
	GUNYAH_ERROR_IRQ_UNBOUND		= 41,

	GUNYAH_ERROR_CSPACE_CAP_NULL		= 50,
	GUNYAH_ERROR_CSPACE_CAP_REVOKED		= 51,
	GUNYAH_ERROR_CSPACE_WRONG_OBJ_TYPE	= 52,
	GUNYAH_ERROR_CSPACE_INSUF_RIGHTS	= 53,
	GUNYAH_ERROR_CSPACE_FULL		= 54,

	GUNYAH_ERROR_MSGQUEUE_EMPTY		= 60,
	GUNYAH_ERROR_MSGQUEUE_FULL		= 61,
	/* clang-format on */
};

/**
 * gunyah_error_remap() - Remap Gunyah hypervisor errors into a Linux error code
 * @gunyah_error: Gunyah hypercall return value
 */
static inline int gunyah_error_remap(enum gunyah_error gunyah_error)
{
	switch (gunyah_error) {
	case GUNYAH_ERROR_OK:
		return 0;
	case GUNYAH_ERROR_NOMEM:
		return -ENOMEM;
	case GUNYAH_ERROR_DENIED:
	case GUNYAH_ERROR_CSPACE_CAP_NULL:
	case GUNYAH_ERROR_CSPACE_CAP_REVOKED:
	case GUNYAH_ERROR_CSPACE_WRONG_OBJ_TYPE:
	case GUNYAH_ERROR_CSPACE_INSUF_RIGHTS:
		return -EACCES;
	case GUNYAH_ERROR_CSPACE_FULL:
	case GUNYAH_ERROR_BUSY:
	case GUNYAH_ERROR_IDLE:
		return -EBUSY;
	case GUNYAH_ERROR_IRQ_BOUND:
	case GUNYAH_ERROR_IRQ_UNBOUND:
	case GUNYAH_ERROR_MSGQUEUE_FULL:
	case GUNYAH_ERROR_MSGQUEUE_EMPTY:
		return -EIO;
	case GUNYAH_ERROR_UNIMPLEMENTED:
		return -EOPNOTSUPP;
	case GUNYAH_ERROR_RETRY:
		return -EAGAIN;
	default:
		return -EINVAL;
	}
}

#endif
