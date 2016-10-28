/*
 * Copyright 2010 - 2011 Broadcom Corporation.  All rights reserved.
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
 */

#ifndef _VC_MEM_H
#define _VC_MEM_H

#include <linux/ioctl.h>

#define VC_MEM_IOC_MAGIC  'v'

#define VC_MEM_IOC_MEM_PHYS_ADDR    _IOR(VC_MEM_IOC_MAGIC, 0, unsigned long)
#define VC_MEM_IOC_MEM_SIZE         _IOR(VC_MEM_IOC_MAGIC, 1, unsigned int)
#define VC_MEM_IOC_MEM_BASE         _IOR(VC_MEM_IOC_MAGIC, 2, unsigned int)
#define VC_MEM_IOC_MEM_LOAD         _IOR(VC_MEM_IOC_MAGIC, 3, unsigned int)

#ifdef __KERNEL__
#define VC_MEM_TO_ARM_ADDR_MASK 0x3FFFFFFF

extern unsigned long mm_vc_mem_phys_addr;
extern unsigned int  mm_vc_mem_size;
extern int vc_mem_get_current_size(void);
#endif

#ifdef CONFIG_COMPAT
#define VC_MEM_IOC_MEM_PHYS_ADDR32  _IOR(VC_MEM_IOC_MAGIC, 0, compat_ulong_t)
#endif

#endif  /* _VC_MEM_H */
