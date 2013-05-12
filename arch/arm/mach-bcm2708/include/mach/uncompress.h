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
#include <linux/amba/serial.h>
#include <mach/platform.h>

#define UART_BAUD 115200

#define BCM2708_UART_DR   __io(UART0_BASE + UART01x_DR)
#define BCM2708_UART_FR   __io(UART0_BASE + UART01x_FR)
#define BCM2708_UART_IBRD __io(UART0_BASE + UART011_IBRD)
#define BCM2708_UART_FBRD __io(UART0_BASE + UART011_FBRD)
#define BCM2708_UART_LCRH __io(UART0_BASE + UART011_LCRH)
#define BCM2708_UART_CR   __io(UART0_BASE + UART011_CR)

/*
 * This does not append a newline
 */
static inline void putc(int c)
{
	while (__raw_readl(BCM2708_UART_FR) & UART01x_FR_TXFF)
		barrier();

	__raw_writel(c, BCM2708_UART_DR);
}

static inline void flush(void)
{
	int fr;

	do {
		fr = __raw_readl(BCM2708_UART_FR);
		barrier();
	} while ((fr & (UART011_FR_TXFE | UART01x_FR_BUSY)) != UART011_FR_TXFE);
}

static inline void arch_decomp_setup(void)
{
	int temp, div, rem, frac;

	temp = 16 * UART_BAUD;
	div = UART0_CLOCK / temp;
	rem = UART0_CLOCK % temp;
	temp = (8 * rem) / UART_BAUD;
	frac = (temp >> 1) + (temp & 1);

	/* Make sure the UART is disabled before we start */
	__raw_writel(0, BCM2708_UART_CR);

	/* Set the baud rate */
	__raw_writel(div, BCM2708_UART_IBRD);
	__raw_writel(frac, BCM2708_UART_FBRD);

	/* Set the UART to 8n1, FIFO enabled */
	__raw_writel(UART01x_LCRH_WLEN_8 | UART01x_LCRH_FEN, BCM2708_UART_LCRH);

	/* Enable the UART */
	__raw_writel(UART01x_CR_UARTEN | UART011_CR_TXE | UART011_CR_RXE,
			BCM2708_UART_CR);
}

/*
 * nothing to do
 */
#define arch_decomp_wdog()
