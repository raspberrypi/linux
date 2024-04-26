/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef __GUNYAH_RSC_MGR_PRIV_H
#define __GUNYAH_RSC_MGR_PRIV_H

#include <linux/gunyah.h>
#include <linux/notifier.h>
#include <linux/types.h>

#define GUNYAH_VMID_INVAL U16_MAX
#define GUNYAH_MEM_HANDLE_INVAL U32_MAX

struct gunyah_rm;

int gunyah_rm_notifier_register(struct gunyah_rm *rm,
				struct notifier_block *nb);
int gunyah_rm_notifier_unregister(struct gunyah_rm *rm,
				  struct notifier_block *nb);
struct device *gunyah_rm_get(struct gunyah_rm *rm);
void gunyah_rm_put(struct gunyah_rm *rm);

struct gunyah_rm_vm_exited_payload {
	__le16 vmid;
	__le16 exit_type;
	__le32 exit_reason_size;
	u8 exit_reason[];
} __packed;

enum gunyah_rm_notification_id {
	/* clang-format off */
	GUNYAH_RM_NOTIFICATION_VM_EXITED		 = 0x56100001,
	GUNYAH_RM_NOTIFICATION_VM_STATUS		 = 0x56100008,
	/* clang-format on */
};

enum gunyah_rm_vm_status {
	/* clang-format off */
	GUNYAH_RM_VM_STATUS_NO_STATE		= 0,
	GUNYAH_RM_VM_STATUS_INIT		= 1,
	GUNYAH_RM_VM_STATUS_READY		= 2,
	GUNYAH_RM_VM_STATUS_RUNNING		= 3,
	GUNYAH_RM_VM_STATUS_PAUSED		= 4,
	GUNYAH_RM_VM_STATUS_LOAD		= 5,
	GUNYAH_RM_VM_STATUS_AUTH		= 6,
	GUNYAH_RM_VM_STATUS_INIT_FAILED		= 8,
	GUNYAH_RM_VM_STATUS_EXITED		= 9,
	GUNYAH_RM_VM_STATUS_RESETTING		= 10,
	GUNYAH_RM_VM_STATUS_RESET		= 11,
	/* clang-format on */
};

struct gunyah_rm_vm_status_payload {
	__le16 vmid;
	u16 reserved;
	u8 vm_status;
	u8 os_status;
	__le16 app_status;
} __packed;

/* RPC Calls */
int gunyah_rm_mem_share(struct gunyah_rm *rm,
			struct gunyah_rm_mem_parcel *parcel);
int gunyah_rm_mem_reclaim(struct gunyah_rm *rm,
			  struct gunyah_rm_mem_parcel *parcel);

int gunyah_rm_alloc_vmid(struct gunyah_rm *rm, u16 vmid);
int gunyah_rm_dealloc_vmid(struct gunyah_rm *rm, u16 vmid);
int gunyah_rm_vm_reset(struct gunyah_rm *rm, u16 vmid);
int gunyah_rm_vm_start(struct gunyah_rm *rm, u16 vmid);
int gunyah_rm_vm_stop(struct gunyah_rm *rm, u16 vmid);

enum gunyah_rm_vm_auth_mechanism {
	/* clang-format off */
	GUNYAH_RM_VM_AUTH_NONE			= 0,
	GUNYAH_RM_VM_AUTH_QCOM_PIL_ELF		= 1,
	GUNYAH_RM_VM_AUTH_QCOM_ANDROID_PVM	= 2,
	/* clang-format on */
};

int gunyah_rm_vm_configure(struct gunyah_rm *rm, u16 vmid,
			   enum gunyah_rm_vm_auth_mechanism auth_mechanism,
			   u32 mem_handle, u64 image_offset, u64 image_size,
			   u64 dtb_offset, u64 dtb_size);
int gunyah_rm_vm_init(struct gunyah_rm *rm, u16 vmid);
int gunyah_rm_vm_set_boot_context(struct gunyah_rm *rm, u16 vmid, u8 reg_set,
				  u8 reg_index, u64 value);

struct gunyah_rm_hyp_resource {
	u8 type;
	u8 reserved;
	__le16 partner_vmid;
	__le32 resource_handle;
	__le32 resource_label;
	__le64 cap_id;
	__le32 virq_handle;
	__le32 virq;
	__le64 base;
	__le64 size;
} __packed;

struct gunyah_rm_hyp_resources {
	__le32 n_entries;
	struct gunyah_rm_hyp_resource entries[];
} __packed;

int gunyah_rm_get_hyp_resources(struct gunyah_rm *rm, u16 vmid,
				struct gunyah_rm_hyp_resources **resources);

int gunyah_rm_get_vmid(struct gunyah_rm *rm, u16 *vmid);

int gunyah_rm_vm_set_demand_paging(struct gunyah_rm *rm, u16 vmid, u32 count,
				   struct gunyah_rm_mem_entry *mem_entries);

enum gunyah_rm_range_id {
	GUNYAH_RM_RANGE_ID_IMAGE = 0,
	GUNYAH_RM_RANGE_ID_FIRMWARE = 1,
};

int gunyah_rm_vm_set_firmware_mem(struct gunyah_rm *rm, u16 vmid, struct gunyah_rm_mem_parcel *parcel,
				u64 fw_offset, u64 fw_size);

int gunyah_rm_vm_set_address_layout(struct gunyah_rm *rm, u16 vmid,
				    enum gunyah_rm_range_id range_id,
				    u64 base_address, u64 size);

struct gunyah_resource *
gunyah_rm_alloc_resource(struct gunyah_rm *rm,
			 struct gunyah_rm_hyp_resource *hyp_resource);
void gunyah_rm_free_resource(struct gunyah_resource *ghrsc);

int gunyah_rm_call(struct gunyah_rm *rsc_mgr, u32 message_id,
		   const void *req_buf, size_t req_buf_size, void **resp_buf,
		   size_t *resp_buf_size);

int gunyah_rm_platform_pre_mem_share(struct gunyah_rm *rm,
				     struct gunyah_rm_mem_parcel *mem_parcel);
int gunyah_rm_platform_post_mem_reclaim(
	struct gunyah_rm *rm, struct gunyah_rm_mem_parcel *mem_parcel);

int gunyah_rm_platform_pre_demand_page(struct gunyah_rm *rm, u16 vmid,
				       u32 flags, struct folio *folio);
int gunyah_rm_platform_reclaim_demand_page(struct gunyah_rm *rm, u16 vmid,
					   u32 flags, struct folio *folio);

#endif
