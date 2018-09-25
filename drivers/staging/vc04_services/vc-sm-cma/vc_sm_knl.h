/* SPDX-License-Identifier: GPL-2.0 */

/*
 * VideoCore Shared Memory CMA allocator
 *
 * Copyright: 2018, Raspberry Pi (Trading) Ltd
 *
 * Based on vc_sm_defs.h from the vmcs_sm driver Copyright Broadcom Corporation.
 *
 */

#ifndef __VC_SM_KNL_H__INCLUDED__
#define __VC_SM_KNL_H__INCLUDED__

#if !defined(__KERNEL__)
#error "This interface is for kernel use only..."
#endif

/* Free a previously allocated or imported shared memory handle and block. */
int vc_sm_cma_free(int handle);

/* Get an internal resource handle mapped from the external one. */
int vc_sm_cma_int_handle(int handle);

/* Import a block of memory into the GPU space. */
int vc_sm_cma_import_dmabuf(struct dma_buf *dmabuf, int *handle);

#endif /* __VC_SM_KNL_H__INCLUDED__ */
