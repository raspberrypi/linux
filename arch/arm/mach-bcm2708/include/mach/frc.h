/*
 *  arch/arm/mach-bcm2708/include/mach/timex.h
 *
 *  BCM2708 free running counter (timer)
 *
 *  Copyright (C) 2010 Broadcom
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

#ifndef _MACH_FRC_H
#define _MACH_FRC_H

#define FRC_TICK_RATE		(1000000)

/*! Free running counter incrementing at the CLOCK_TICK_RATE
    (slightly faster than frc_clock_ticks63()
 */
extern unsigned long frc_clock_ticks32(void);

/*! Free running counter incrementing at the CLOCK_TICK_RATE
 *  Note - top bit should be ignored (see cnt32_to_63)
 */
extern unsigned long long frc_clock_ticks63(void);

#endif
