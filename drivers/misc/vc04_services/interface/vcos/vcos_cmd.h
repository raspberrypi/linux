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

#if !defined( VCOS_CMD_H )
#define VCOS_CMD_H

/* ---- Include Files ----------------------------------------------------- */

#include "interface/vcos/vcos.h"
#include "interface/vcos/vcos_stdint.h"


/* ---- Constants and Types ---------------------------------------------- */

struct VCOS_CMD_S;
typedef struct VCOS_CMD_S VCOS_CMD_T;

typedef struct
{
    int         argc;           /* Number of arguments (includes the command/sub-command) */
    char      **argv;           /* Array of arguments */
    char      **argv_orig;      /* Original array of arguments */

    VCOS_CMD_T *cmd_entry;
    VCOS_CMD_T *cmd_parent_entry;

    int         use_log;        /* Output being logged? */
    size_t      result_size;    /* Size of result buffer. */
    char       *result_ptr;     /* Next place to put output. */
    char       *result_buf;     /* Start of the buffer. */

} VCOS_CMD_PARAM_T;

typedef VCOS_STATUS_T (*VCOS_CMD_FUNC_T)( VCOS_CMD_PARAM_T *param );

struct VCOS_CMD_S
{
    const char         *name;
    const char         *args;
    VCOS_CMD_FUNC_T     cmd_fn;
    VCOS_CMD_T         *sub_cmd_entry;
    const char         *descr;

};

/* ---- Variable Externs ------------------------------------------------- */

/* ---- Function Prototypes ---------------------------------------------- */

/*
 * Common printing routine for generating command output.
 */
VCOSPRE_ void VCOSPOST_ vcos_cmd_error( VCOS_CMD_PARAM_T *param, const char *fmt, ... ) VCOS_FORMAT_ATTR_(printf, 2, 3);
VCOSPRE_ void VCOSPOST_ vcos_cmd_printf( VCOS_CMD_PARAM_T *param, const char *fmt, ... ) VCOS_FORMAT_ATTR_(printf, 2, 3);
VCOSPRE_ void VCOSPOST_ vcos_cmd_vprintf( VCOS_CMD_PARAM_T *param, const char *fmt, va_list args ) VCOS_FORMAT_ATTR_(printf, 2, 0);

/*
 * Cause vcos_cmd_error, printf and vprintf to always log to the provided
 * category. When this call is made, the results buffer passed into
 * vcos_cmd_execute is used as a line buffer and does not need to be
 * output by the caller.
 */
VCOSPRE_ void VCOSPOST_ vcos_cmd_always_log_output( VCOS_LOG_CAT_T *log_category );

/*
 * Prints command usage for the current command.
 */
VCOSPRE_ void VCOSPOST_ vcos_cmd_usage( VCOS_CMD_PARAM_T *param );

/*
 * Register commands to be processed
 */
VCOSPRE_ VCOS_STATUS_T VCOSPOST_ vcos_cmd_register( VCOS_CMD_T *cmd_entry );

/*
 * Registers multiple commands to be processed. The array should
 * be terminated by an entry with all zeros.
 */
VCOSPRE_ VCOS_STATUS_T VCOSPOST_ vcos_cmd_register_multiple( VCOS_CMD_T *cmd_entry );

/*
 * Executes a command based on a command line.
 */
VCOSPRE_ VCOS_STATUS_T VCOSPOST_ vcos_cmd_execute( int argc, char **argv, size_t result_size, char *result_buf );

#endif /* VCOS_CMD_H */

