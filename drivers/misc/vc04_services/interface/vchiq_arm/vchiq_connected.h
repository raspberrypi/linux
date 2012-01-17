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

#ifndef VCHIQ_CONNECTED_H
#define VCHIQ_CONNECTED_H

/* ---- Include Files ----------------------------------------------------- */

/* ---- Constants and Types ---------------------------------------------- */

typedef void (*VCHIQ_CONNECTED_CALLBACK_T)( void );

/* ---- Variable Externs ------------------------------------------------- */

/* ---- Function Prototypes ---------------------------------------------- */

void vchiq_add_connected_callback( VCHIQ_CONNECTED_CALLBACK_T callback );
void vchiq_call_connected_callbacks( void );

#endif /* VCHIQ_CONNECTED_H */

