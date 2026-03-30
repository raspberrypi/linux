/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */
#ifndef _AIE_H_
#define _AIE_H_

#include "amdxdna_pci_drv.h"
#include "amdxdna_mailbox.h"

struct aie_device {
	struct amdxdna_dev *xdna;
	struct mailbox_channel *mgmt_chann;
	struct xdna_mailbox_chann_res mgmt_x2i;
	struct xdna_mailbox_chann_res mgmt_i2x;
	u32 mgmt_chan_idx;
	u32 mgmt_prot_major;
	u32 mgmt_prot_minor;
	unsigned long feature_mask;
};

#define DECLARE_AIE_MSG(name, op) \
	DECLARE_XDNA_MSG_COMMON(name, op, -1)
#define AIE_FEATURE_ON(aie, feature) test_bit(feature, &(aie)->feature_mask)

void aie_dump_mgmt_chann_debug(struct aie_device *aie);
void aie_destroy_chann(struct aie_device *aie, struct mailbox_channel **chann);
int aie_send_mgmt_msg_wait(struct aie_device *aie, struct xdna_mailbox_msg *msg);
int aie_check_protocol(struct aie_device *aie, u32 fw_major, u32 fw_minor);

#endif /* _AIE_H_ */
