/*=============================================================================
Copyright (c) 2011 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  chip driver

FILE DESCRIPTION
VideoCore OS Abstraction Layer - 'once'
=============================================================================*/

#ifndef VCOS_ONCE_H
#define VCOS_ONCE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"
#include "vcos_platform.h"

/**
 * \file vcos_once.h
 *
 * Ensure something is called only once.
 *
 * Initialize once_control to VCOS_ONCE_INIT. The first
 * time this is called, the init_routine will be called. Thereafter
 * it won't.
 *
 * \sa pthread_once()
 *
 */

VCOS_STATUS_T vcos_once(VCOS_ONCE_T *once_control,
                        void (*init_routine)(void));

#ifdef __cplusplus
}
#endif
#endif

