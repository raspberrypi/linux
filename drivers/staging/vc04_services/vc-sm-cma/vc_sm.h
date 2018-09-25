/* SPDX-License-Identifier: GPL-2.0 */

/*
 * VideoCore Shared Memory driver using CMA.
 *
 * Copyright: 2018, Raspberry Pi (Trading) Ltd
 *
 */

#ifndef VC_SM_H
#define VC_SM_H

#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/kref.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/shrinker.h>
#include <linux/types.h>
#include <linux/miscdevice.h>

#define VC_SM_MAX_NAME_LEN 32

enum vc_sm_vpu_mapping_state {
	VPU_NOT_MAPPED,
	VPU_MAPPED,
	VPU_UNMAPPING
};

struct vc_sm_buffer {
	struct list_head global_buffer_list;	/* Global list of buffers. */

	size_t size;

	/* Lock over all the following state for this buffer */
	struct mutex lock;
	struct sg_table *sg_table;
	struct list_head attachments;

	char name[VC_SM_MAX_NAME_LEN];

	int in_use:1;	/* Kernel is still using this resource */

	enum vc_sm_vpu_mapping_state vpu_state;
	u32 vc_handle;	/* VideoCore handle for this buffer */

	/* DMABUF related fields */
	struct dma_buf *import_dma_buf;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t dma_addr;

	struct vc_sm_privdata_t *private;
};

#endif
