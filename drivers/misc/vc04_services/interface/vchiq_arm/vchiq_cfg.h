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

#ifndef VCHIQ_CFG_H
#define VCHIQ_CFG_H

#define VCHIQ_MAGIC              VCHIQ_MAKE_FOURCC('V','C','H','I')
/* The version of VCHIQ - change with any non-trivial change */
#define VCHIQ_VERSION            2
/* The minimum compatible version - update to match VCHIQ_VERSION with any incompatible change */
#define VCHIQ_VERSION_MIN        2

#define VCHIQ_MAX_SERVICES       4096
#define VCHIQ_MAX_SLOTS          128
#define VCHIQ_MAX_SLOTS_PER_SIDE 64

#define VCHIQ_NUM_CURRENT_BULKS        32
#define VCHIQ_NUM_SERVICE_BULKS        4

#ifndef VCHIQ_ENABLE_DEBUG
#define VCHIQ_ENABLE_DEBUG             1
#endif

#ifndef VCHIQ_ENABLE_STATS
#define VCHIQ_ENABLE_STATS             1
#endif

#endif /* VCHIQ_CFG_H */
