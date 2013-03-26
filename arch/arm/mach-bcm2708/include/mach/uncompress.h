/*
 *  arch/arm/mach-bcn2708/include/mach/uncompress.h
 *
 *  Copyright (C) 2010 Broadcom
 *  Copyright (C) 2003 ARM Limited
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

#include <linux/io.h>
#include <mach/hardware.h>

#define BCM2708_UART_DR	__io_address(UART0_BASE + 0x00)
#define BCM2708_UART_FR	__io_address(UART0_BASE + 0x18)

/*
 * This does not append a newline
 */
static inline void putc(int c)
{
	while (readl(BCM2708_UART_FR) & (1 << 5))
		barrier();

	writel(c, BCM2708_UART_DR);
}

static inline void flush(void)
{
	while (readl(BCM2708_UART_FR) & (1 << 3))
		barrier();
}

/*
 * nothing to do
 */
#define arch_decomp_setup()
#define arch_decomp_wdog()
