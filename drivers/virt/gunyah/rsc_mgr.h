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

struct gunyah_rm;

int gunyah_rm_notifier_register(struct gunyah_rm *rm,
				struct notifier_block *nb);
int gunyah_rm_notifier_unregister(struct gunyah_rm *rm,
				  struct notifier_block *nb);
struct device *gunyah_rm_get(struct gunyah_rm *rm);
void gunyah_rm_put(struct gunyah_rm *rm);


int gunyah_rm_call(struct gunyah_rm *rsc_mgr, u32 message_id,
		   const void *req_buf, size_t req_buf_size, void **resp_buf,
		   size_t *resp_buf_size);

#endif
