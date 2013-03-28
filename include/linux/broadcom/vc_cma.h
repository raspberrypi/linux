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

#if !defined( VC_CMA_H )
#define VC_CMA_H

#include <linux/ioctl.h>

#define VC_CMA_IOC_MAGIC 0xc5

#define VC_CMA_IOC_RESERVE _IO(VC_CMA_IOC_MAGIC, 0)

#ifdef __KERNEL__
extern void __init vc_cma_early_init(void);
extern void __init vc_cma_reserve(void);
#endif

#endif /* VC_CMA_H */

