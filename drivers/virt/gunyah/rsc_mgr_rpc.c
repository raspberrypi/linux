// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/error-injection.h>

#include "rsc_mgr.h"

/* Message IDs: Memory Management */
#define GUNYAH_RM_RPC_MEM_LEND 0x51000012
#define GUNYAH_RM_RPC_MEM_SHARE 0x51000013
#define GUNYAH_RM_RPC_MEM_RECLAIM 0x51000015
#define GUNYAH_RM_RPC_MEM_APPEND 0x51000018

/* Message IDs: VM Management */
/* clang-format off */
#define GUNYAH_RM_RPC_VM_ALLOC_VMID		0x56000001
#define GUNYAH_RM_RPC_VM_DEALLOC_VMID		0x56000002
#define GUNYAH_RM_RPC_VM_START			0x56000004
#define GUNYAH_RM_RPC_VM_STOP			0x56000005
#define GUNYAH_RM_RPC_VM_RESET			0x56000006
#define GUNYAH_RM_RPC_VM_CONFIG_IMAGE		0x56000009
#define GUNYAH_RM_RPC_VM_INIT			0x5600000B
#define GUNYAH_RM_RPC_VM_GET_HYP_RESOURCES	0x56000020
#define GUNYAH_RM_RPC_VM_GET_VMID		0x56000024
#define GUNYAH_RM_RPC_VM_SET_BOOT_CONTEXT	0x56000031
#define GUNYAH_RM_RPC_VM_SET_FIRMWARE_MEM	0x56000032
#define GUNYAH_RM_RPC_VM_SET_DEMAND_PAGING	0x56000033
#define GUNYAH_RM_RPC_VM_SET_ADDRESS_LAYOUT	0x56000034
/* clang-format on */

struct gunyah_rm_vm_common_vmid_req {
	__le16 vmid;
	__le16 _padding;
} __packed;

/* Call: MEM_LEND, MEM_SHARE */
#define GUNYAH_RM_MAX_MEM_ENTRIES 512

#define GUNYAH_MEM_SHARE_REQ_FLAGS_APPEND BIT(1)

struct gunyah_rm_mem_share_req_header {
	u8 mem_type;
	u8 _padding0;
	u8 flags;
	u8 _padding1;
	__le32 label;
} __packed;

struct gunyah_rm_mem_share_req_acl_section {
	__le16 n_entries;
	__le16 _padding;
	struct gunyah_rm_mem_acl_entry entries[];
} __packed;

struct gunyah_rm_mem_share_req_mem_section {
	__le16 n_entries;
	__le16 _padding;
	struct gunyah_rm_mem_entry entries[];
} __packed;

/* Call: MEM_RELEASE */
struct gunyah_rm_mem_release_req {
	__le32 mem_handle;
	u8 flags; /* currently not used */
	u8 _padding0;
	__le16 _padding1;
} __packed;

/* Call: MEM_APPEND */
#define GUNYAH_MEM_APPEND_REQ_FLAGS_END BIT(0)

struct gunyah_rm_mem_append_req_header {
	__le32 mem_handle;
	u8 flags;
	u8 _padding0;
	__le16 _padding1;
} __packed;

/* Call: VM_ALLOC */
struct gunyah_rm_vm_alloc_vmid_resp {
	__le16 vmid;
	__le16 _padding;
} __packed;

/* Call: VM_STOP */
#define GUNYAH_RM_VM_STOP_FLAG_FORCE_STOP BIT(0)

#define GUNYAH_RM_VM_STOP_REASON_FORCE_STOP 3

struct gunyah_rm_vm_stop_req {
	__le16 vmid;
	u8 flags;
	u8 _padding;
	__le32 stop_reason;
} __packed;

/* Call: VM_CONFIG_IMAGE */
struct gunyah_rm_vm_config_image_req {
	__le16 vmid;
	__le16 auth_mech;
	__le32 mem_handle;
	__le64 image_offset;
	__le64 image_size;
	__le64 dtb_offset;
	__le64 dtb_size;
} __packed;

/* Call: VM_SET_BOOT_CONTEXT */
struct gunyah_rm_vm_set_boot_context_req {
	__le16 vmid;
	u8 reg_set;
	u8 reg_index;
	__le32 _padding;
	__le64 value;
} __packed;

/* Call: VM_SET_DEMAND_PAGING */
struct gunyah_rm_vm_set_demand_paging_req {
	__le16 vmid;
	__le16 _padding;
	__le32 range_count;
	DECLARE_FLEX_ARRAY(struct gunyah_rm_mem_entry, ranges);
} __packed;

/* Call: VM_SET_ADDRESS_LAYOUT */
struct gunyah_rm_vm_set_address_layout_req {
	__le16 vmid;
	__le16 _padding;
	__le32 range_id;
	__le64 range_base;
	__le64 range_size;
} __packed;

/* Call: VM_SET_FIRMWARE_MEM */
struct gunyah_vm_set_firmware_mem_req {
	__le16 vmid;
	__le16 reserved;
	__le32 mem_handle;
	__le64 fw_offset;
	__le64 fw_size;
} __packed;

/*
 * Several RM calls take only a VMID as a parameter and give only standard
 * response back. Deduplicate boilerplate code by using this common call.
 */
static int gunyah_rm_common_vmid_call(struct gunyah_rm *rm, u32 message_id,
				      u16 vmid)
{
	struct gunyah_rm_vm_common_vmid_req req_payload = {
		.vmid = cpu_to_le16(vmid),
	};

	return gunyah_rm_call(rm, message_id, &req_payload, sizeof(req_payload),
			      NULL, NULL);
}

static int gunyah_rm_mem_append(struct gunyah_rm *rm, u32 mem_handle,
				struct gunyah_rm_mem_entry *entries,
				size_t n_entries)
{
	struct gunyah_rm_mem_append_req_header *req __free(kfree) = NULL;
	struct gunyah_rm_mem_share_req_mem_section *mem;
	int ret = 0;
	size_t n;

	req = kzalloc(sizeof(*req) + struct_size(mem, entries, GUNYAH_RM_MAX_MEM_ENTRIES),
		      GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->mem_handle = cpu_to_le32(mem_handle);
	mem = (void *)(req + 1);

	while (n_entries) {
		req->flags = 0;
		if (n_entries > GUNYAH_RM_MAX_MEM_ENTRIES) {
			n = GUNYAH_RM_MAX_MEM_ENTRIES;
		} else {
			req->flags |= GUNYAH_MEM_APPEND_REQ_FLAGS_END;
			n = n_entries;
		}

		mem->n_entries = cpu_to_le16(n);
		memcpy(mem->entries, entries, sizeof(*entries) * n);

		ret = gunyah_rm_call(rm, GUNYAH_RM_RPC_MEM_APPEND, req,
				     sizeof(*req) + struct_size(mem, entries, n),
				     NULL, NULL);
		if (ret)
			break;

		entries += n;
		n_entries -= n;
	}

	return ret;
}

/**
 * gunyah_rm_mem_share() - Share memory with other virtual machines.
 * @rm: Handle to a Gunyah resource manager
 * @p: Information about the memory to be shared.
 *
 * Sharing keeps Linux's access to the memory while the memory parcel is shared.
 */
int gunyah_rm_mem_share(struct gunyah_rm *rm, struct gunyah_rm_mem_parcel *p)
{
	u32 message_id = p->n_acl_entries == 1 ? GUNYAH_RM_RPC_MEM_LEND :
						 GUNYAH_RM_RPC_MEM_SHARE;
	size_t msg_size, initial_mem_entries = p->n_mem_entries, resp_size;
	struct gunyah_rm_mem_share_req_acl_section *acl;
	struct gunyah_rm_mem_share_req_mem_section *mem;
	struct gunyah_rm_mem_share_req_header *req_header;
	size_t acl_size, mem_size;
	u32 *attr_section;
	bool need_append = false;
	__le32 *resp;
	void *msg;
	int ret;

	if (!p->acl_entries || !p->n_acl_entries || !p->mem_entries ||
	    !p->n_mem_entries || p->n_acl_entries > U8_MAX ||
	    p->mem_handle != GUNYAH_MEM_HANDLE_INVAL)
		return -EINVAL;

	if (initial_mem_entries > GUNYAH_RM_MAX_MEM_ENTRIES) {
		initial_mem_entries = GUNYAH_RM_MAX_MEM_ENTRIES;
		need_append = true;
	}

	acl_size = struct_size(acl, entries, p->n_acl_entries);
	mem_size = struct_size(mem, entries, initial_mem_entries);

	/* The format of the message goes:
	 * request header
	 * ACL entries (which VMs get what kind of access to this memory parcel)
	 * Memory entries (list of memory regions to share)
	 * Memory attributes (currently unused, we'll hard-code the size to 0)
	 */
	msg_size = sizeof(struct gunyah_rm_mem_share_req_header) + acl_size +
		   mem_size +
		   sizeof(u32); /* for memory attributes, currently unused */

	msg = kzalloc(msg_size, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	ret = gunyah_rm_platform_pre_mem_share(rm, p);
	if (ret) {
		kfree(msg);
		return ret;
	}

	req_header = msg;
	acl = (void *)req_header + sizeof(*req_header);
	mem = (void *)acl + acl_size;
	attr_section = (void *)mem + mem_size;

	req_header->mem_type = p->mem_type;
	if (need_append)
		req_header->flags |= GUNYAH_MEM_SHARE_REQ_FLAGS_APPEND;
	req_header->label = cpu_to_le32(p->label);

	acl->n_entries = cpu_to_le32(p->n_acl_entries);
	memcpy(acl->entries, p->acl_entries,
	       flex_array_size(acl, entries, p->n_acl_entries));

	mem->n_entries = cpu_to_le16(initial_mem_entries);
	memcpy(mem->entries, p->mem_entries,
	       flex_array_size(mem, entries, initial_mem_entries));

	/* Set n_entries for memory attribute section to 0 */
	*attr_section = 0;

	ret = gunyah_rm_call(rm, message_id, msg, msg_size, (void **)&resp,
			     &resp_size);
	kfree(msg);

	if (ret) {
		gunyah_rm_platform_post_mem_reclaim(rm, p);
		return ret;
	}

	p->mem_handle = le32_to_cpu(*resp);
	kfree(resp);

	if (need_append) {
		ret = gunyah_rm_mem_append(
			rm, p->mem_handle, &p->mem_entries[initial_mem_entries],
			p->n_mem_entries - initial_mem_entries);
		if (ret) {
			gunyah_rm_mem_reclaim(rm, p);
			p->mem_handle = GUNYAH_MEM_HANDLE_INVAL;
		}
	}

	return ret;
}
ALLOW_ERROR_INJECTION(gunyah_rm_mem_share, ERRNO);

/**
 * gunyah_rm_mem_reclaim() - Reclaim a memory parcel
 * @rm: Handle to a Gunyah resource manager
 * @parcel: Information about the memory to be reclaimed.
 *
 * RM maps the associated memory back into the stage-2 page tables of the owner VM.
 */
int gunyah_rm_mem_reclaim(struct gunyah_rm *rm,
			  struct gunyah_rm_mem_parcel *parcel)
{
	struct gunyah_rm_mem_release_req req = {
		.mem_handle = cpu_to_le32(parcel->mem_handle),
	};
	int ret;

	ret = gunyah_rm_call(rm, GUNYAH_RM_RPC_MEM_RECLAIM, &req, sizeof(req),
			     NULL, NULL);
	/* Only call platform mem reclaim hooks if we reclaimed the memory */
	if (ret)
		return ret;

	return gunyah_rm_platform_post_mem_reclaim(rm, parcel);
}
ALLOW_ERROR_INJECTION(gunyah_rm_mem_reclaim, ERRNO);

/**
 * gunyah_rm_alloc_vmid() - Allocate a new VM in Gunyah. Returns the VM identifier.
 * @rm: Handle to a Gunyah resource manager
 * @vmid: Use 0 to dynamically allocate a VM. A reserved VMID can be supplied
 *        to request allocation of a platform-defined VM.
 *
 * Return: the allocated VMID or negative value on error
 */
int gunyah_rm_alloc_vmid(struct gunyah_rm *rm, u16 vmid)
{
	struct gunyah_rm_vm_common_vmid_req req_payload = {
		.vmid = cpu_to_le16(vmid),
	};
	struct gunyah_rm_vm_alloc_vmid_resp *resp_payload;
	size_t resp_size;
	void *resp;
	int ret;

	ret = gunyah_rm_call(rm, GUNYAH_RM_RPC_VM_ALLOC_VMID, &req_payload,
			     sizeof(req_payload), &resp, &resp_size);
	if (ret)
		return ret;

	if (!vmid) {
		resp_payload = resp;
		ret = le16_to_cpu(resp_payload->vmid);
		kfree(resp);
	}

	return ret;
}
ALLOW_ERROR_INJECTION(gunyah_rm_alloc_vmid, ERRNO);

/**
 * gunyah_rm_dealloc_vmid() - Dispose of a VMID
 * @rm: Handle to a Gunyah resource manager
 * @vmid: VM identifier allocated with gunyah_rm_alloc_vmid
 */
int gunyah_rm_dealloc_vmid(struct gunyah_rm *rm, u16 vmid)
{
	return gunyah_rm_common_vmid_call(rm, GUNYAH_RM_RPC_VM_DEALLOC_VMID,
					  vmid);
}
ALLOW_ERROR_INJECTION(gunyah_rm_dealloc_vmid, ERRNO);

/**
 * gunyah_rm_vm_reset() - Reset a VM's resources
 * @rm: Handle to a Gunyah resource manager
 * @vmid: VM identifier allocated with gunyah_rm_alloc_vmid
 *
 * As part of tearing down the VM, request RM to clean up all the VM resources
 * associated with the VM. Only after this, Linux can clean up all the
 * references it maintains to resources.
 */
int gunyah_rm_vm_reset(struct gunyah_rm *rm, u16 vmid)
{
	return gunyah_rm_common_vmid_call(rm, GUNYAH_RM_RPC_VM_RESET, vmid);
}
ALLOW_ERROR_INJECTION(gunyah_rm_vm_reset, ERRNO);

/**
 * gunyah_rm_vm_start() - Move a VM into "ready to run" state
 * @rm: Handle to a Gunyah resource manager
 * @vmid: VM identifier allocated with gunyah_rm_alloc_vmid
 *
 * On VMs which use proxy scheduling, vcpu_run is needed to actually run the VM.
 * On VMs which use Gunyah's scheduling, the vCPUs start executing in accordance with Gunyah
 * scheduling policies.
 */
int gunyah_rm_vm_start(struct gunyah_rm *rm, u16 vmid)
{
	return gunyah_rm_common_vmid_call(rm, GUNYAH_RM_RPC_VM_START, vmid);
}
ALLOW_ERROR_INJECTION(gunyah_rm_vm_start, ERRNO);

/**
 * gunyah_rm_vm_stop() - Send a request to Resource Manager VM to forcibly stop a VM.
 * @rm: Handle to a Gunyah resource manager
 * @vmid: VM identifier allocated with gunyah_rm_alloc_vmid
 */
int gunyah_rm_vm_stop(struct gunyah_rm *rm, u16 vmid)
{
	struct gunyah_rm_vm_stop_req req_payload = {
		.vmid = cpu_to_le16(vmid),
		.flags = GUNYAH_RM_VM_STOP_FLAG_FORCE_STOP,
		.stop_reason = cpu_to_le32(GUNYAH_RM_VM_STOP_REASON_FORCE_STOP),
	};

	return gunyah_rm_call(rm, GUNYAH_RM_RPC_VM_STOP, &req_payload,
			      sizeof(req_payload), NULL, NULL);
}
ALLOW_ERROR_INJECTION(gunyah_rm_vm_stop, ERRNO);

/**
 * gunyah_rm_vm_configure() - Prepare a VM to start and provide the common
 *			  configuration needed by RM to configure a VM
 * @rm: Handle to a Gunyah resource manager
 * @vmid: VM identifier allocated with gunyah_rm_alloc_vmid
 * @auth_mechanism: Authentication mechanism used by resource manager to verify
 *                  the virtual machine
 * @mem_handle: Handle to a previously shared memparcel that contains all parts
 *              of the VM image subject to authentication.
 * @image_offset: Start address of VM image, relative to the start of memparcel
 * @image_size: Size of the VM image
 * @dtb_offset: Start address of the devicetree binary with VM configuration,
 *              relative to start of memparcel.
 * @dtb_size: Maximum size of devicetree binary.
 */
int gunyah_rm_vm_configure(struct gunyah_rm *rm, u16 vmid,
			   enum gunyah_rm_vm_auth_mechanism auth_mechanism,
			   u32 mem_handle, u64 image_offset, u64 image_size,
			   u64 dtb_offset, u64 dtb_size)
{
	struct gunyah_rm_vm_config_image_req req_payload = {
		.vmid = cpu_to_le16(vmid),
		.auth_mech = cpu_to_le16(auth_mechanism),
		.mem_handle = cpu_to_le32(mem_handle),
		.image_offset = cpu_to_le64(image_offset),
		.image_size = cpu_to_le64(image_size),
		.dtb_offset = cpu_to_le64(dtb_offset),
		.dtb_size = cpu_to_le64(dtb_size),
	};

	return gunyah_rm_call(rm, GUNYAH_RM_RPC_VM_CONFIG_IMAGE, &req_payload,
			      sizeof(req_payload), NULL, NULL);
}
ALLOW_ERROR_INJECTION(gunyah_rm_vm_configure, ERRNO);

/**
 * gunyah_rm_vm_init() - Move the VM to initialized state.
 * @rm: Handle to a Gunyah resource manager
 * @vmid: VM identifier
 *
 * RM will allocate needed resources for the VM.
 */
int gunyah_rm_vm_init(struct gunyah_rm *rm, u16 vmid)
{
	return gunyah_rm_common_vmid_call(rm, GUNYAH_RM_RPC_VM_INIT, vmid);
}
ALLOW_ERROR_INJECTION(gunyah_rm_vm_init, ERRNO);

/**
 * gunyah_rm_vm_set_boot_context() - set the initial boot context of the primary vCPU
 * @rm: Handle to a Gunyah resource manager
 * @vmid: VM identifier
 * @reg_set: See &enum gunyah_vm_boot_context_reg
 * @reg_index: Which register to set; must be 0 for REG_SET_PC
 * @value: Value to set in the register
 */
int gunyah_rm_vm_set_boot_context(struct gunyah_rm *rm, u16 vmid, u8 reg_set,
				  u8 reg_index, u64 value)
{
	struct gunyah_rm_vm_set_boot_context_req req_payload = {
		.vmid = cpu_to_le16(vmid),
		.reg_set = reg_set,
		.reg_index = reg_index,
		.value = cpu_to_le64(value),
	};

	return gunyah_rm_call(rm, GUNYAH_RM_RPC_VM_SET_BOOT_CONTEXT,
			      &req_payload, sizeof(req_payload), NULL, NULL);
}
ALLOW_ERROR_INJECTION(gunyah_rm_vm_set_boot_context, ERRNO);

/**
 * gunyah_rm_get_hyp_resources() - Retrieve hypervisor resources (capabilities) associated with a VM
 * @rm: Handle to a Gunyah resource manager
 * @vmid: VMID of the other VM to get the resources of
 * @resources: Set by gunyah_rm_get_hyp_resources and contains the returned hypervisor resources.
 *             Caller must free the resources pointer if successful.
 */
int gunyah_rm_get_hyp_resources(struct gunyah_rm *rm, u16 vmid,
				struct gunyah_rm_hyp_resources **resources)
{
	struct gunyah_rm_vm_common_vmid_req req_payload = {
		.vmid = cpu_to_le16(vmid),
	};
	struct gunyah_rm_hyp_resources *resp;
	size_t resp_size;
	int ret;

	ret = gunyah_rm_call(rm, GUNYAH_RM_RPC_VM_GET_HYP_RESOURCES,
			     &req_payload, sizeof(req_payload), (void **)&resp,
			     &resp_size);
	if (ret)
		return ret;

	if (!resp_size)
		return -EBADMSG;

	if (resp_size < struct_size(resp, entries, 0) ||
	    resp_size !=
		    struct_size(resp, entries, le32_to_cpu(resp->n_entries))) {
		kfree(resp);
		return -EBADMSG;
	}

	*resources = resp;
	return 0;
}
ALLOW_ERROR_INJECTION(gunyah_rm_get_hyp_resources, ERRNO);

/**
 * gunyah_rm_get_vmid() - Retrieve VMID of this virtual machine
 * @rm: Handle to a Gunyah resource manager
 * @vmid: Filled with the VMID of this VM
 */
int gunyah_rm_get_vmid(struct gunyah_rm *rm, u16 *vmid)
{
	static u16 cached_vmid = GUNYAH_VMID_INVAL;
	size_t resp_size;
	__le32 *resp;
	int ret;

	if (cached_vmid != GUNYAH_VMID_INVAL) {
		*vmid = cached_vmid;
		return 0;
	}

	ret = gunyah_rm_call(rm, GUNYAH_RM_RPC_VM_GET_VMID, NULL, 0,
			     (void **)&resp, &resp_size);
	if (ret)
		return ret;

	*vmid = cached_vmid = lower_16_bits(le32_to_cpu(*resp));
	kfree(resp);

	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_rm_get_vmid);

/**
 * gunyah_rm_vm_set_demand_paging() - Enable demand paging of memory regions
 * @rm: Handle to a Gunyah resource manager
 * @vmid: VMID of the other VM
 * @count: Number of demand paged memory regions
 * @entries: Array of the regions
 */
int gunyah_rm_vm_set_demand_paging(struct gunyah_rm *rm, u16 vmid, u32 count,
				   struct gunyah_rm_mem_entry *entries)
{
	struct gunyah_rm_vm_set_demand_paging_req *req __free(kfree) = NULL;
	size_t req_size;

	req_size = struct_size(req, ranges, count);
	if (req_size == SIZE_MAX)
		return -EINVAL;

	req = kzalloc(req_size, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->vmid = cpu_to_le16(vmid);
	req->range_count = cpu_to_le32(count);
	memcpy(req->ranges, entries, sizeof(*entries) * count);

	return gunyah_rm_call(rm, GUNYAH_RM_RPC_VM_SET_DEMAND_PAGING, req,
			      req_size, NULL, NULL);
}
ALLOW_ERROR_INJECTION(gunyah_rm_vm_set_demand_paging, ERRNO);

/**
 * gunyah_rm_vm_set_address_layout() - Set the start address of images
 * @rm: Handle to a Gunyah resource manager
 * @vmid: VMID of the other VM
 * @range_id: Which image to set
 * @base_address: Base address
 * @size: Size
 */
int gunyah_rm_vm_set_address_layout(struct gunyah_rm *rm, u16 vmid,
				    enum gunyah_rm_range_id range_id,
				    u64 base_address, u64 size)
{
	struct gunyah_rm_vm_set_address_layout_req req = {
		.vmid = cpu_to_le16(vmid),
		.range_id = cpu_to_le32(range_id),
		.range_base = cpu_to_le64(base_address),
		.range_size = cpu_to_le64(size),
	};

	return gunyah_rm_call(rm, GUNYAH_RM_RPC_VM_SET_ADDRESS_LAYOUT, &req,
			      sizeof(req), NULL, NULL);
}
ALLOW_ERROR_INJECTION(gunyah_rm_vm_set_address_layout, ERRNO);

/**
 * gunyah_rm_vm_set_firmware_mem() - Set the location of firmware for GH_RM_VM_AUTH_QCOM_ANDROID_PVM VMs
 * @rm: Handle to a Gunyah resource manager.
 * @vmid: VM identifier allocated with gh_rm_alloc_vmid.
 * @parcel: Memory parcel where the firmware should be loaded.
 * @fw_offset: offset into the memory parcel where the firmware should be loaded.
 * @fw_size: Maxmimum size of the fw that can be loaded.
 */
int gunyah_rm_vm_set_firmware_mem(struct gunyah_rm *rm, u16 vmid, struct gunyah_rm_mem_parcel *parcel,
				u64 fw_offset, u64 fw_size)
{
	struct gunyah_vm_set_firmware_mem_req req = {
		.vmid = cpu_to_le16(vmid),
		.mem_handle = cpu_to_le32(parcel->mem_handle),
		.fw_offset = cpu_to_le64(fw_offset),
		.fw_size = cpu_to_le64(fw_size),
	};

	return gunyah_rm_call(rm, GUNYAH_RM_RPC_VM_SET_FIRMWARE_MEM, &req, sizeof(req), NULL, NULL);
}
ALLOW_ERROR_INJECTION(gunyah_rm_vm_set_firmware_mem, ERRNO);
