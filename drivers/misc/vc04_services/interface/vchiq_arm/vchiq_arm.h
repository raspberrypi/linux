/*
 * Copyright (c) 2010-2011 Broadcom Corporation. All rights reserved.
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

#ifndef VCHIQ_ARM_H
#define VCHIQ_ARM_H

#include "vchiq_core.h"

extern VCOS_LOG_CAT_T vchiq_arm_log_category;

extern int __init
vchiq_platform_vcos_init(void);

extern int __init
vchiq_platform_init(VCHIQ_STATE_T *state);

extern void __exit
vchiq_platform_exit(VCHIQ_STATE_T *state);

extern VCHIQ_STATE_T *
vchiq_get_state(void);

#endif /* VCHIQ_ARM_H */
