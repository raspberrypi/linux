/*
 *  linux/arch/arm/mach-bcm2708/bcm2708.c
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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/broadcom/vc_cma.h>

#include <asm/mach/arch.h>
#include <asm/system_info.h>

/* command line parameters */
static unsigned boardrev, serial;

static void __init bcm2708_init(void)
{
	vc_cma_early_init();

	system_rev = boardrev;
	system_serial_low = serial;
}

static void __init board_reserve(void)
{
	vc_cma_reserve();
}

static const char * const bcm2708_compat[] = {
	"brcm,bcm2708",
	NULL
};

DT_MACHINE_START(BCM2708, "BCM2708")
    /* Maintainer: Broadcom Europe Ltd. */
	.init_machine = bcm2708_init,
	.reserve = board_reserve,
	.dt_compat = bcm2708_compat,
MACHINE_END

module_param(boardrev, uint, 0644);
module_param(serial, uint, 0644);
