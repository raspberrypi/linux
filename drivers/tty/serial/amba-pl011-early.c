// SPDX-License-Identifier: GPL-2.0
/*
 * Early serial console for Pl011 devices
 *
 * (c) Copyright 2004 Hewlett-Packard Development Company, L.P.
 *	Bjorn Helgaas <bjorn.helgaas@hp.com>
 *
 * Based on the Pl011.c serial driver, Copyright (C) 2001 Russell King,
 * and on early_printk.c by Andi Kleen.
 *
 * This is for use before the serial driver has initialized, in
 * particular, before the UARTs have been discovered and named.
 * Instead of specifying the console device as, e.g., "ttyS0",
 * we locate the device directly by its MMIO or I/O port address.
 *
 * The user can specify the device directly, e.g.,
 *	earlycon=pl011,mmio32,0xfe201000,115200n8
 */

#include <linux/tty.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/amba/serial.h>
#include <linux/serial_core.h>

static void pl011_putc(struct uart_port *port, unsigned char c)
{
	while (readl(port->membase + UART01x_FR) & UART01x_FR_TXFF)
		cpu_relax();
	if (port->iotype == UPIO_MEM32)
		writel(c, port->membase + UART01x_DR);
	else
		writeb(c, port->membase + UART01x_DR);
	while (readl(port->membase + UART01x_FR) & UART01x_FR_BUSY)
		cpu_relax();
}

static void pl011_early_write(struct console *con, const char *s, unsigned n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, pl011_putc);
}

#ifdef CONFIG_CONSOLE_POLL
static int pl011_getc(struct uart_port *port)
{
	if (readl(port->membase + UART01x_FR) & UART01x_FR_RXFE)
		return NO_POLL_CHAR;

	if (port->iotype == UPIO_MEM32)
		return readl(port->membase + UART01x_DR);
	else
		return readb(port->membase + UART01x_DR);
}

static int pl011_early_read(struct console *con, char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;
	int ch, num_read = 0;

	while (num_read < n) {
		ch = pl011_getc(&dev->port);
		if (ch == NO_POLL_CHAR)
			break;

		s[num_read++] = ch;
	}

	return num_read;
}
#else
#define pl011_early_read NULL
#endif

/*
 * On non-ACPI systems, earlycon is enabled by specifying
 * "earlycon=pl011,<address>" on the kernel command line.
 *
 * On ACPI ARM64 systems, an "early" console is enabled via the SPCR table,
 * by specifying only "earlycon" on the command line.  Because it requires
 * SPCR, the console starts after ACPI is parsed, which is later than a
 * traditional early console.
 *
 * To get the traditional early console that starts before ACPI is parsed,
 * specify the full "earlycon=pl011,<address>" option.
 */
static int __init pl011_early_console_setup(struct earlycon_device *device,
					    const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = pl011_early_write;
	device->con->read = pl011_early_read;

	return 0;
}
OF_EARLYCON_DECLARE(pl011, "arm,pl011", pl011_early_console_setup);
