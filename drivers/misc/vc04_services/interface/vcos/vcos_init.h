/*
 * Copyright (c) 2010-2011 Broadcom. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*=============================================================================
VideoCore OS Abstraction Layer - initialization routines
=============================================================================*/


#include "interface/vcos/vcos_types.h"
#include "vcos_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \file
  *
  * Some OS support libraries need some initialization. To support this, call this
  * function at the start of day.
  */

VCOSPRE_ VCOS_STATUS_T VCOSPOST_ vcos_init(void);
VCOSPRE_ void VCOSPOST_ vcos_deinit(void);
VCOSPRE_ void VCOSPOST_ vcos_global_lock(void);
VCOSPRE_ void VCOSPOST_ vcos_global_unlock(void);

/** Pass in the argv/argc arguments passed to main() */
VCOSPRE_ void VCOSPOST_ vcos_set_args(int argc, const char **argv);

/** Return argc. */
VCOSPRE_ int VCOSPOST_ vcos_get_argc(void);

/** Return argv. */
VCOSPRE_ const char ** VCOSPOST_ vcos_get_argv(void);

#ifdef __cplusplus
}
#endif

