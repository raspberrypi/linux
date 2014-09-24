/*****************************************************************************
* Copyright 2012 Broadcom Corporation.  All rights reserved.
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
*****************************************************************************/

#if !defined(VC_LMK_H)
#define VC_LMK_H

#if defined(__KERNEL__)
#include <linux/types.h>	/* Needed for standard types */
#else
#include <stdint.h>
#endif

#include <linux/ioctl.h>

#define VC_LMK_IOC_MAGIC  'L'

struct vclmk_ioctl_killpid {
	int pid;
	unsigned int reclaim;

};

struct vclmk_ioctl_lmk_candidate {
	int pid;
	int candidate;

};

struct vclmk_ioctl_lmk_hmem {
	int pid;
	int num_pages;
	int page_size;

};

#define VC_LMK_IOC_KILL_PID \
	_IOR(VC_LMK_IOC_MAGIC, 0, struct vclmk_ioctl_killpid)
#define VC_LMK_IOC_CAND_PID \
	_IOR(VC_LMK_IOC_MAGIC, 1, struct vclmk_ioctl_lmk_candidate)
#define VC_LMK_IOC_HMEM_PID \
	_IOR(VC_LMK_IOC_MAGIC, 2, struct vclmk_ioctl_lmk_hmem)

#endif /* VC_LMK_H */
