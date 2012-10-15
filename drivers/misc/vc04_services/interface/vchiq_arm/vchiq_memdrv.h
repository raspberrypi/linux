/*****************************************************************************
* Copyright 2001 - 2010 Broadcom Corporation.  All rights reserved.
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

#ifndef VCHIQ_MEMDRV_H
#define VCHIQ_MEMDRV_H

/* ---- Include Files ----------------------------------------------------- */

#include <linux/kernel.h>
#include "vchiq_if.h"

/* ---- Constants and Types ---------------------------------------------- */

typedef struct
{
    void                   *armSharedMemVirt;
    dma_addr_t              armSharedMemPhys;
    size_t                  armSharedMemSize;

    void                   *vcSharedMemVirt;
    dma_addr_t              vcSharedMemPhys;
    size_t                  vcSharedMemSize;

} VCHIQ_SHARED_MEM_INFO_T;

/* ---- Variable Externs ------------------------------------------------- */

/* ---- Function Prototypes ---------------------------------------------- */

void vchiq_get_shared_mem_info( VCHIQ_SHARED_MEM_INFO_T *info );

VCHIQ_STATUS_T vchiq_memdrv_initialise(void);

#endif
