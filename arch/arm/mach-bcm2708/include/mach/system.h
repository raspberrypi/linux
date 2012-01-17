/*
 *  arch/arm/mach-bcm2708/include/mach/system.h
 *
 *  Copyright (C) 2010 Broadcom
 *  Copyright (C) 2003 ARM Limited
 *  Copyright (C) 2000 Deep Blue Solutions Ltd
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
#ifndef __ASM_ARCH_SYSTEM_H
#define __ASM_ARCH_SYSTEM_H

#include <linux/io.h>
#include <mach/hardware.h>
#include <mach/platform.h>

static inline void arch_idle(void)
{
	/*
	 * This should do all the clock switching
	 * and wait for interrupt tricks
	 */
	cpu_do_idle();
}

static inline void arch_reset(char mode, const char *cmd)
{
	uint32_t pm_rstc, pm_wdog;
	uint32_t timeout = 10;

	/* Setup watchdog for reset */
	pm_rstc = readl(IO_ADDRESS(PM_RSTC));

	pm_wdog = PM_PASSWORD | (timeout & PM_WDOG_TIME_SET); // watchdog timer = timer clock / 16; need password (31:16) + value (11:0)
	pm_rstc = PM_PASSWORD | (pm_rstc & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET;

	writel(pm_wdog, IO_ADDRESS(PM_WDOG));
	writel(pm_rstc, IO_ADDRESS(PM_RSTC));
}

#endif
