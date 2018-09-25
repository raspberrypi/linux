/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright 2019 Raspberry Pi (Trading) Ltd.  All rights reserved.
 *
 * Based on vmcs_sm_ioctl.h Copyright Broadcom Corporation.
 */

#ifndef __VC_SM_CMA_IOCTL_H
#define __VC_SM_CMA_IOCTL_H

/* ---- Include Files ---------------------------------------------------- */

#if defined(__KERNEL__)
#include <linux/types.h>	/* Needed for standard types */
#else
#include <stdint.h>
#endif

#include <linux/ioctl.h>

/* ---- Constants and Types ---------------------------------------------- */

#define VC_SM_CMA_RESOURCE_NAME               32
#define VC_SM_CMA_RESOURCE_NAME_DEFAULT       "sm-host-resource"

/* Type define used to create unique IOCTL number */
#define VC_SM_CMA_MAGIC_TYPE                  'J'

/* IOCTL commands on /dev/vc-sm-cma */
enum vc_sm_cma_cmd_e {
	VC_SM_CMA_CMD_ALLOC = 0x5A,	/* Start at 0x5A arbitrarily */

	VC_SM_CMA_CMD_IMPORT_DMABUF,

	VC_SM_CMA_CMD_CLEAN_INVALID2,

	VC_SM_CMA_CMD_LAST	/* Do not delete */
};

/* Cache type supported, conveniently matches the user space definition in
 * user-vcsm.h.
 */
enum vc_sm_cma_cache_e {
	VC_SM_CMA_CACHE_NONE,
	VC_SM_CMA_CACHE_HOST,
	VC_SM_CMA_CACHE_VC,
	VC_SM_CMA_CACHE_BOTH,
};

/* IOCTL Data structures */
struct vc_sm_cma_ioctl_alloc {
	/* user -> kernel */
	__u32 size;
	__u32 num;
	__u32 cached;		/* enum vc_sm_cma_cache_e */
	__u32 pad;
	__u8 name[VC_SM_CMA_RESOURCE_NAME];

	/* kernel -> user */
	__s32 handle;
	__u32 vc_handle;
	__u64 dma_addr;
};

struct vc_sm_cma_ioctl_import_dmabuf {
	/* user -> kernel */
	__s32 dmabuf_fd;
	__u32 cached;		/* enum vc_sm_cma_cache_e */
	__u8 name[VC_SM_CMA_RESOURCE_NAME];

	/* kernel -> user */
	__s32 handle;
	__u32 vc_handle;
	__u32 size;
	__u32 pad;
	__u64 dma_addr;
};

/*
 * Cache functions to be set to struct vc_sm_cma_ioctl_clean_invalid2
 * invalidate_mode.
 */
#define VC_SM_CACHE_OP_NOP       0x00
#define VC_SM_CACHE_OP_INV       0x01
#define VC_SM_CACHE_OP_CLEAN     0x02
#define VC_SM_CACHE_OP_FLUSH     0x03

struct vc_sm_cma_ioctl_clean_invalid2 {
	__u32 op_count;
	__u32 pad;
	struct vc_sm_cma_ioctl_clean_invalid_block {
		__u32 invalidate_mode;
		__u32 block_count;
		void *  __user start_address;
		__u32 block_size;
		__u32 inter_block_stride;
	} s[0];
};

/* IOCTL numbers */
#define VC_SM_CMA_IOCTL_MEM_ALLOC\
	_IOR(VC_SM_CMA_MAGIC_TYPE, VC_SM_CMA_CMD_ALLOC,\
	 struct vc_sm_cma_ioctl_alloc)

#define VC_SM_CMA_IOCTL_MEM_IMPORT_DMABUF\
	_IOR(VC_SM_CMA_MAGIC_TYPE, VC_SM_CMA_CMD_IMPORT_DMABUF,\
	 struct vc_sm_cma_ioctl_import_dmabuf)

#define VC_SM_CMA_IOCTL_MEM_CLEAN_INVALID2\
	_IOR(VC_SM_CMA_MAGIC_TYPE, VC_SM_CMA_CMD_CLEAN_INVALID2,\
	 struct vc_sm_cma_ioctl_clean_invalid2)

#endif /* __VC_SM_CMA_IOCTL_H */
