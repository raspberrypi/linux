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
VCOS - abstraction over dynamic library opening
=============================================================================*/

#ifndef VCOS_DLFCN_H
#define VCOS_DLFCN_H

#include "interface/vcos/vcos_types.h"
#include "vcos_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VCOS_DL_LAZY 1
#define VCOS_DL_NOW  2

/**
 * \file
 *
 * Loading dynamic libraries. See also dlfcn.h.
 */

/** Open a dynamic library.
  *
  * @param name  name of the library
  * @param mode  Load lazily or immediately (VCOS_DL_LAZY, VCOS_DL_NOW).
  *
  * @return A handle for use in subsequent calls.
  */
VCOSPRE_ void * VCOSPOST_ vcos_dlopen(const char *name, int mode);

/** Look up a symbol.
  *
  * @param handle Handle to open
  * @param name   Name of function
  *
  * @return Function pointer, or NULL.
  */
VCOSPRE_ void VCOSPOST_ (*vcos_dlsym(void *handle, const char *name))(void);

/** Close a library
  *
  * @param handle Handle to close
  */
VCOSPRE_ int VCOSPOST_ vcos_dlclose (void *handle);

/** Return error message from library.
  *
  * @param err  On return, set to non-zero if an error has occurred
  * @param buf  Buffer to write error to
  * @param len  Size of buffer (including terminating NUL).
  */
VCOSPRE_ int VCOSPOST_ vcos_dlerror(int *err, char *buf, size_t buflen);


#ifdef __cplusplus
}
#endif
#endif


