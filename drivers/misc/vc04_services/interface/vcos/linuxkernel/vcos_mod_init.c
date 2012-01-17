/*****************************************************************************
* Copyright 2006 - 2008 Broadcom Corporation.  All rights reserved.
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
****************************************************************************/

/* ---- Include Files ---------------------------------------------------- */

#include "interface/vcos/vcos.h"
#include <linux/module.h>

/* ---- Public Variables ------------------------------------------------- */

/* ---- Private Constants and Types -------------------------------------- */

/* ---- Private Variables ------------------------------------------------ */

/* ---- Private Function Prototypes -------------------------------------- */

/* ---- Functions -------------------------------------------------------- */

/****************************************************************************
*
*   Called to perform module initialization when the module is loaded
*
***************************************************************************/

static int __init vcos_mod_init( void )
{
    printk( KERN_INFO "VCOS Module\n" );

    vcos_init();
    return 0;
}

/****************************************************************************
*
*   Called to perform module cleanup when the module is unloaded.
*
***************************************************************************/

static void __exit vcos_mod_exit( void )
{
    vcos_deinit();
}

/****************************************************************************/

module_init( vcos_mod_init );
module_exit( vcos_mod_exit );

MODULE_AUTHOR("Broadcom");
MODULE_DESCRIPTION( "VCOS Module Functions" );
MODULE_LICENSE( "GPL" );
MODULE_VERSION( "1.0" );

