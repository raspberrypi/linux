/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _LINUX_GUNYAH_H
#define _LINUX_GUNYAH_H

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/types.h>

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

	struct list_head list;
	u32 rm_label;
};

struct gunyah_vm;

/**
 * struct gunyah_vm_resource_ticket - Represents a ticket to reserve access to VM resource(s)
 * @vm_list: for @gunyah_vm->resource_tickets
 * @resources: List of resource(s) associated with this ticket
 *             (members are from @gunyah_resource->list)
 * @resource_type: Type of resource this ticket reserves
 * @label: Label of the resource from resource manager this ticket reserves.
 * @owner: owner of the ticket
 * @populate: callback provided by the ticket owner and called when a resource is found that
 *            matches @resource_type and @label. Note that this callback could be called
 *            multiple times if userspace created mutliple resources with the same type/label.
 *            This callback may also have significant delay after gunyah_vm_add_resource_ticket()
 *            since gunyah_vm_add_resource_ticket() could be called before the VM starts.
 * @unpopulate: callback provided by the ticket owner and called when the ticket owner should no
 *              longer use the resource provided in the argument. When unpopulate() returns,
 *              the ticket owner should not be able to use the resource any more as the resource
 *              might being freed.
 */
struct gunyah_vm_resource_ticket {
	struct list_head vm_list;
	struct list_head resources;
	enum gunyah_resource_type resource_type;
	u32 label;

	struct module *owner;
	bool (*populate)(struct gunyah_vm_resource_ticket *ticket,
			 struct gunyah_resource *ghrsc);
	void (*unpopulate)(struct gunyah_vm_resource_ticket *ticket,
			   struct gunyah_resource *ghrsc);
};

int gunyah_vm_add_resource_ticket(struct gunyah_vm *ghvm,
				  struct gunyah_vm_resource_ticket *ticket);
void gunyah_vm_remove_resource_ticket(struct gunyah_vm *ghvm,
				      struct gunyah_vm_resource_ticket *ticket);

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

enum gunyah_api_feature {
	/* clang-format off */
	GUNYAH_FEATURE_DOORBELL		= 1,
	GUNYAH_FEATURE_MSGQUEUE		= 2,
	GUNYAH_FEATURE_VCPU		= 5,
	GUNYAH_FEATURE_MEMEXTENT	= 6,
	/* clang-format on */
};

bool arch_is_gunyah_guest(void);

#define GUNYAH_API_V1 1

/* Other bits reserved for future use and will be zero */
/* clang-format off */
#define GUNYAH_API_INFO_API_VERSION_MASK	GENMASK_ULL(13, 0)
#define GUNYAH_API_INFO_BIG_ENDIAN		BIT_ULL(14)
#define GUNYAH_API_INFO_IS_64BIT		BIT_ULL(15)
#define GUNYAH_API_INFO_VARIANT_MASK 		GENMASK_ULL(63, 56)
/* clang-format on */

struct gunyah_hypercall_hyp_identify_resp {
	u64 api_info;
	u64 flags[3];
};

static inline u16
gunyah_api_version(const struct gunyah_hypercall_hyp_identify_resp *gunyah_api)
{
	return FIELD_GET(GUNYAH_API_INFO_API_VERSION_MASK,
			 gunyah_api->api_info);
}

void gunyah_hypercall_hyp_identify(
	struct gunyah_hypercall_hyp_identify_resp *hyp_identity);

/* Immediately raise RX vIRQ on receiver VM */
#define GUNYAH_HYPERCALL_MSGQ_TX_FLAGS_PUSH BIT(0)

enum gunyah_error gunyah_hypercall_msgq_send(u64 capid, size_t size, void *buff,
					     u64 tx_flags, bool *ready);
enum gunyah_error gunyah_hypercall_msgq_recv(u64 capid, void *buff, size_t size,
					     size_t *recv_size, bool *ready);

#endif
