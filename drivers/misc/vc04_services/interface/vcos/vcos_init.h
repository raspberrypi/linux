/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver

FILE DESCRIPTION
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

