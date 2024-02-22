// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/error-injection.h>

#include "rsc_mgr.h"

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
/* clang-format on */

struct gunyah_rm_vm_common_vmid_req {
	__le16 vmid;
	__le16 _padding;
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
