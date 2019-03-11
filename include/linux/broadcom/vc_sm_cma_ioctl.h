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

/* IOCTL numbers */
#define VC_SM_CMA_IOCTL_MEM_ALLOC\
	_IOR(VC_SM_CMA_MAGIC_TYPE, VC_SM_CMA_CMD_ALLOC,\
	 struct vc_sm_cma_ioctl_alloc)

#define VC_SM_CMA_IOCTL_MEM_IMPORT_DMABUF\
	_IOR(VC_SM_CMA_MAGIC_TYPE, VC_SM_CMA_CMD_IMPORT_DMABUF,\
	 struct vc_sm_cma_ioctl_import_dmabuf)

#endif /* __VC_SM_CMA_IOCTL_H */
