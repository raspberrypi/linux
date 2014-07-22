/*
 ****************************************************************************
 * Copyright 2011 Broadcom Corporation.  All rights reserved.
 *
 * Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2, available at
 * http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
 *
 * Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a
 * license other than the GPL, without Broadcom's express prior written
 * consent.
 ****************************************************************************
 */

#ifndef __VC_SM_KNL_H__INCLUDED__
#define __VC_SM_KNL_H__INCLUDED__

#if !defined(__KERNEL__)
#error "This interface is for kernel use only..."
#endif

/* Type of memory to be locked (ie mapped) */
enum vc_sm_lock_cache_mode {
	VC_SM_LOCK_CACHED,
	VC_SM_LOCK_NON_CACHED,
};

/* Allocate a shared memory handle and block. */
int vc_sm_alloc(struct vc_sm_alloc_t *alloc, int *handle);

/* Free a previously allocated shared memory handle and block. */
int vc_sm_free(int handle);

/* Lock a memory handle for use by kernel. */
int vc_sm_lock(int handle, enum vc_sm_lock_cache_mode mode,
	       unsigned long *data);

/* Unlock a memory handle in use by kernel. */
int vc_sm_unlock(int handle, int flush, int no_vc_unlock);

/* Get an internal resource handle mapped from the external one. */
int vc_sm_int_handle(int handle);

/* Map a shared memory region for use by kernel. */
int vc_sm_map(int handle, unsigned int sm_addr,
	      enum vc_sm_lock_cache_mode mode, unsigned long *data);

/* Import a block of memory into the GPU space. */
int vc_sm_import_dmabuf(struct dma_buf *dmabuf, int *handle);

#endif /* __VC_SM_KNL_H__INCLUDED__ */
