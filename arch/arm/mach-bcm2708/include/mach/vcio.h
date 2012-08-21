/*
 *  arch/arm/mach-bcm2708/include/mach/vcio.h
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
#ifndef _MACH_BCM2708_VCIO_H
#define _MACH_BCM2708_VCIO_H

/* Routines to handle I/O via the VideoCore "ARM control" registers
 * (semaphores, doorbells, mailboxes)
 */

#define BCM_VCIO_DRIVER_NAME "bcm2708_vcio"

/* Constants shared with the ARM identifying separate mailbox channels */
#define MBOX_CHAN_POWER   0 /* for use by the power management interface */
#define MBOX_CHAN_FB      1 /* for use by the frame buffer */
#define MBOX_CHAN_VUART   2 /* for use by the virtual UART */
#define MBOX_CHAN_VCHIQ   3 /* for use by the VCHIQ interface */
#define MBOX_CHAN_LEDS    4 /* for use by the leds interface */
#define MBOX_CHAN_BUTTONS 5 /* for use by the buttons interface */
#define MBOX_CHAN_TOUCH   6 /* for use by the touchscreen interface */
#define MBOX_CHAN_PROPERTY 8 /* for use by the property channel */
#define MBOX_CHAN_COUNT   9

extern int /*rc*/ bcm_mailbox_read(unsigned chan, uint32_t *data28);
extern int /*rc*/ bcm_mailbox_write(unsigned chan, uint32_t data28);

#endif
