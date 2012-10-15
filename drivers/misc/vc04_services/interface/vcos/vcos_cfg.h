/*****************************************************************************
* Copyright 2009 - 2011 Broadcom Corporation.  All rights reserved.
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

#if !defined( VCOS_CFG_H )
#define VCOS_CFG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"
#include "vcos_platform.h"

typedef struct opaque_vcos_cfg_buf_t    *VCOS_CFG_BUF_T;
typedef struct opaque_vcos_cfg_entry_t  *VCOS_CFG_ENTRY_T;

/** \file vcos_file.h
  *
  * API for accessing configuration/statistics information. This
  * is loosely modelled on the linux proc entries.
  */

typedef void (*VCOS_CFG_SHOW_FPTR)( VCOS_CFG_BUF_T buf, void *data );
typedef void (*VCOS_CFG_PARSE_FPTR)( VCOS_CFG_BUF_T buf, void *data );

/** Create a configuration directory.
  *
  * @param entry        Place to store the created config entry.
  * @param parent       Parent entry (for directory like config 
  *                     options).
  * @param entryName    Name of the directory.
  */

VCOS_STATUS_T vcos_cfg_mkdir( VCOS_CFG_ENTRY_T *entry,
                              VCOS_CFG_ENTRY_T *parent,
                              const char *dirName );           

/** Create a configuration entry.
  *
  * @param entry        Place to store the created config entry.
  * @param parent       Parent entry (for directory like config 
  *                     options).
  * @param entryName    Name of the configuration entry.
  * @param showFunc     Function pointer to show configuration 
  *                     data.
  * @param parseFunc    Function pointer to parse new data. 
  */

VCOS_STATUS_T vcos_cfg_create_entry( VCOS_CFG_ENTRY_T *entry,
                                     VCOS_CFG_ENTRY_T *parent,
                                     const char *entryName,
                                     VCOS_CFG_SHOW_FPTR showFunc,
                                     VCOS_CFG_PARSE_FPTR parseFunc,
                                     void *data );

/** Determines if a configuration entry has been created or not.
  *
  * @param entry        Configuration entry to query.
  */

int vcos_cfg_is_entry_created( VCOS_CFG_ENTRY_T entry );

/** Returns the name of a configuration entry.
  *
  * @param entry        Configuration entry to query.
  */

const char *vcos_cfg_get_entry_name( VCOS_CFG_ENTRY_T entry );

/** Removes a configuration entry.
  *
  * @param entry        Configuration entry to remove.
  */

VCOS_STATUS_T vcos_cfg_remove_entry( VCOS_CFG_ENTRY_T *entry );


/** Writes data into a configuration buffer. Only valid inside
  * the show function. 
  *
  * @param buf      Buffer to write data into.
  * @param fmt      printf style format string. 
  */

void vcos_cfg_buf_printf( VCOS_CFG_BUF_T buf, const char *fmt, ... );

/** Retrieves a null terminated string of the data associated
  * with the buffer. Only valid inside the parse function.
  *
  * @param buf      Buffer to get data from.
  * @param fmt      printf style format string. 
  */

char *vcos_cfg_buf_get_str( VCOS_CFG_BUF_T buf );

void *vcos_cfg_get_proc_entry( VCOS_CFG_ENTRY_T entry );

#ifdef __cplusplus
}
#endif
#endif

