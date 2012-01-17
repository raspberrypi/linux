/*=============================================================================
Copyright (c) 2010 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver

FILE DESCRIPTION
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


