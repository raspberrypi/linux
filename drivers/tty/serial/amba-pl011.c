// SPDX-License-Identifier: GPL-2.0+
/*
 *  Driver for AMBA serial ports
 *
 *  Based on drivers/char/serial.c, by Linus Torvalds, Theodore Ts'o.
 *
 *  Copyright 1999 ARM Limited
 *  Copyright (C) 2000 Deep Blue Solutions Ltd.
 *  Copyright (C) 2010 ST-Ericsson SA
 *
 * This is a generic driver for ARM AMBA-type serial ports.  They
 * have a lot of 16550-like features, but are not register compatible.
 * Note that although they do have CTS, DCD and DSR inputs, they do
 * not have an RI input, nor do they have DTR or RTS outputs.  If
 * required, these have to be supplied via some other means (eg, GPIO)
 * and hooked into this driver.
 */

#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/platform_device.h>
#include <linux/sysrq.h>
#include <linux/device.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/amba/bus.h>
#include <linux/amba/serial.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/sizes.h>
#include <linux/io.h>
#include <linux/acpi.h>

#define UART_NR			14

#define SERIAL_AMBA_MAJOR	204
#define SERIAL_AMBA_MINOR	64
#define SERIAL_AMBA_NR		UART_NR

#define AMBA_ISR_PASS_LIMIT	256

#define UART_DR_ERROR		(UART011_DR_OE|UART011_DR_BE|UART011_DR_PE|UART011_DR_FE)
#define UART_DUMMY_DR_RX	(1 << 16)

enum {
	REG_DR,
	REG_ST_DMAWM,
	REG_ST_TIMEOUT,
	REG_FR,
	REG_LCRH_RX,
	REG_LCRH_TX,
	REG_IBRD,
	REG_FBRD,
	REG_CR,
	REG_IFLS,
	REG_IMSC,
	REG_RIS,
	REG_MIS,
	REG_ICR,
	REG_DMACR,
	REG_ST_XFCR,
	REG_ST_XON1,
	REG_ST_XON2,
	REG_ST_XOFF1,
	REG_ST_XOFF2,
	REG_ST_ITCR,
	REG_ST_ITIP,
	REG_ST_ABCR,
	REG_ST_ABIMSC,

	/* The size of the array - must be last */
	REG_ARRAY_SIZE,
};

static u16 pl011_std_offsets[REG_ARRAY_SIZE] = {
	[REG_DR] = UART01x_DR,
	[REG_FR] = UART01x_FR,
	[REG_LCRH_RX] = UART011_LCRH,
	[REG_LCRH_TX] = UART011_LCRH,
	[REG_IBRD] = UART011_IBRD,
	[REG_FBRD] = UART011_FBRD,
	[REG_CR] = UART011_CR,
	[REG_IFLS] = UART011_IFLS,
	[REG_IMSC] = UART011_IMSC,
	[REG_RIS] = UART011_RIS,
	[REG_MIS] = UART011_MIS,
	[REG_ICR] = UART011_ICR,
	[REG_DMACR] = UART011_DMACR,
};

/* There is by now at least one vendor with differing details, so handle it */
struct vendor_data {
	const u16		*reg_offset;
	unsigned int		ifls;
	unsigned int		fr_busy;
	unsigned int		fr_dsr;
	unsigned int		fr_cts;
	unsigned int		fr_ri;
	unsigned int		inv_fr;
	bool			access_32b;
	bool			oversampling;
	bool			dma_threshold;
	bool			cts_event_workaround;
	bool			always_enabled;
	bool			fixed_options;

	unsigned int (*get_fifosize)(struct amba_device *dev);
};

static unsigned int get_fifosize_arm(struct amba_device *dev)
{
	return amba_rev(dev) < 3 ? 16 : 32;
}

static struct vendor_data vendor_arm = {
	.reg_offset		= pl011_std_offsets,
	.ifls			= UART011_IFLS_RX4_8|UART011_IFLS_TX4_8,
	.fr_busy		= UART01x_FR_BUSY,
	.fr_dsr			= UART01x_FR_DSR,
	.fr_cts			= UART01x_FR_CTS,
	.fr_ri			= UART011_FR_RI,
	.oversampling		= false,
	.dma_threshold		= false,
	.cts_event_workaround	= false,
	.always_enabled		= false,
	.fixed_options		= false,
	.get_fifosize		= get_fifosize_arm,
};

/*
 * We wrap our port structure around the generic uart_port.
 */
struct uart_amba_port {
	struct uart_port	port;
	const u16		*reg_offset;
	struct clk		*clk;
	const struct vendor_data *vendor;
	unsigned int		im;		/* interrupt mask */
	unsigned int		old_status;
	unsigned int		fifosize;	/* vendor-specific */
	unsigned int		fixed_baud;	/* vendor-set fixed baud rate */
	char			type[12];
	bool			rs485_tx_started;
	unsigned int		rs485_tx_drain_interval; /* usecs */
};

static unsigned int pl011_tx_empty(struct uart_port *port);

static unsigned int pl011_reg_to_offset(const struct uart_amba_port *uap,
	unsigned int reg)
{
	return uap->reg_offset[reg];
}

static unsigned int pl011_read(const struct uart_amba_port *uap,
	unsigned int reg)
{
	void __iomem *addr = uap->port.membase + pl011_reg_to_offset(uap, reg);

	return (uap->port.iotype == UPIO_MEM32) ?
		readl_relaxed(addr) : readw_relaxed(addr);
}

static void pl011_write(unsigned int val, const struct uart_amba_port *uap,
	unsigned int reg)
{
	void __iomem *addr = uap->port.membase + pl011_reg_to_offset(uap, reg);

	if (uap->port.iotype == UPIO_MEM32)
		writel_relaxed(val, addr);
	else
		writew_relaxed(val, addr);
}

/*
 * Reads up to 256 characters from the FIFO or until it's empty and
 * inserts them into the TTY layer. Returns the number of characters
 * read from the FIFO.
 */
static int pl011_fifo_to_tty(struct uart_amba_port *uap)
{
	unsigned int ch, fifotaken;
	int sysrq;
	u16 status;
	u8 flag;

	for (fifotaken = 0; fifotaken != 256; fifotaken++) {
		status = pl011_read(uap, REG_FR);
		if (status & UART01x_FR_RXFE)
			break;

		/* Take chars from the FIFO and update status */
		ch = pl011_read(uap, REG_DR) | UART_DUMMY_DR_RX;
		flag = TTY_NORMAL;
		uap->port.icount.rx++;

		if (unlikely(ch & UART_DR_ERROR)) {
			if (ch & UART011_DR_BE) {
				ch &= ~(UART011_DR_FE | UART011_DR_PE);
				uap->port.icount.brk++;
				if (uart_handle_break(&uap->port))
					continue;
			} else if (ch & UART011_DR_PE)
				uap->port.icount.parity++;
			else if (ch & UART011_DR_FE)
				uap->port.icount.frame++;
			if (ch & UART011_DR_OE)
				uap->port.icount.overrun++;

			ch &= uap->port.read_status_mask;

			if (ch & UART011_DR_BE)
				flag = TTY_BREAK;
			else if (ch & UART011_DR_PE)
				flag = TTY_PARITY;
			else if (ch & UART011_DR_FE)
				flag = TTY_FRAME;
		}

		spin_unlock(&uap->port.lock);
		sysrq = uart_handle_sysrq_char(&uap->port, ch & 255);
		spin_lock(&uap->port.lock);

		if (!sysrq)
			uart_insert_char(&uap->port, ch, UART011_DR_OE, ch, flag);
	}

	return fifotaken;
}

static void pl011_rs485_tx_stop(struct uart_amba_port *uap)
{
	/*
	 * To be on the safe side only time out after twice as many iterations
	 * as fifo size.
	 */
	const int MAX_TX_DRAIN_ITERS = uap->port.fifosize * 2;
	struct uart_port *port = &uap->port;
	int i = 0;
	u32 cr;

	/* Wait until hardware tx queue is empty */
	while (!pl011_tx_empty(port)) {
		if (i > MAX_TX_DRAIN_ITERS) {
			dev_warn(port->dev,
				 "timeout while draining hardware tx queue\n");
			break;
		}

		udelay(uap->rs485_tx_drain_interval);
		i++;
	}

	if (port->rs485.delay_rts_after_send)
		mdelay(port->rs485.delay_rts_after_send);

	cr = pl011_read(uap, REG_CR);

	if (port->rs485.flags & SER_RS485_RTS_AFTER_SEND)
		cr &= ~UART011_CR_RTS;
	else
		cr |= UART011_CR_RTS;

	/* Disable the transmitter and reenable the transceiver */
	cr &= ~UART011_CR_TXE;
	cr |= UART011_CR_RXE;
	pl011_write(cr, uap, REG_CR);

	uap->rs485_tx_started = false;
}

static void pl011_stop_tx(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	uap->im &= ~UART011_TXIM;
	pl011_write(uap->im, uap, REG_IMSC);
}

static bool pl011_tx_chars(struct uart_amba_port *uap, bool from_irq);

/* Start TX with programmed I/O only (no DMA) */
static void pl011_start_tx_pio(struct uart_amba_port *uap)
{
	if (pl011_tx_chars(uap, false)) {
		uap->im |= UART011_TXIM;
		pl011_write(uap->im, uap, REG_IMSC);
	}
}

static void pl011_rs485_tx_start(struct uart_amba_port *uap)
{
	struct uart_port *port = &uap->port;
	u32 cr;

	/* Enable transmitter */
	cr = pl011_read(uap, REG_CR);
	cr |= UART011_CR_TXE;

	/* Disable receiver if half-duplex */
	if (!(port->rs485.flags & SER_RS485_RX_DURING_TX))
		cr &= ~UART011_CR_RXE;

	if (port->rs485.flags & SER_RS485_RTS_ON_SEND)
		cr &= ~UART011_CR_RTS;
	else
		cr |= UART011_CR_RTS;

	pl011_write(cr, uap, REG_CR);

	if (port->rs485.delay_rts_before_send)
		mdelay(port->rs485.delay_rts_before_send);

	uap->rs485_tx_started = true;
}

static void pl011_start_tx(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	if ((uap->port.rs485.flags & SER_RS485_ENABLED) &&
	    !uap->rs485_tx_started)
		pl011_rs485_tx_start(uap);

	pl011_start_tx_pio(uap);
}

static void pl011_stop_rx(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	uap->im &= ~(UART011_RXIM|UART011_RTIM|UART011_FEIM|
		     UART011_PEIM|UART011_BEIM|UART011_OEIM);
	pl011_write(uap->im, uap, REG_IMSC);
}

static void pl011_throttle_rx(struct uart_port *port)
{
	unsigned long flags;

	spin_lock_irqsave(&port->lock, flags);
	pl011_stop_rx(port);
	spin_unlock_irqrestore(&port->lock, flags);
}

static void pl011_enable_ms(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	uap->im |= UART011_RIMIM|UART011_CTSMIM|UART011_DCDMIM|UART011_DSRMIM;
	pl011_write(uap->im, uap, REG_IMSC);
}

static void pl011_rx_chars(struct uart_amba_port *uap)
__releases(&uap->port.lock)
__acquires(&uap->port.lock)
{
	pl011_fifo_to_tty(uap);

	spin_unlock(&uap->port.lock);
	tty_flip_buffer_push(&uap->port.state->port);
	spin_lock(&uap->port.lock);
}

static bool pl011_tx_char(struct uart_amba_port *uap, unsigned char c,
			  bool from_irq)
{
	if (unlikely(!from_irq) &&
	    pl011_read(uap, REG_FR) & UART01x_FR_TXFF)
		return false; /* unable to transmit character */

	pl011_write(c, uap, REG_DR);
	mb();
	uap->port.icount.tx++;

	return true;
}

/* Returns true if tx interrupts have to be (kept) enabled  */
static bool pl011_tx_chars(struct uart_amba_port *uap, bool from_irq)
{
	struct circ_buf *xmit = &uap->port.state->xmit;
	int count = uap->fifosize >> 1;

	if (uap->port.x_char) {
		if (!pl011_tx_char(uap, uap->port.x_char, from_irq))
			return true;
		uap->port.x_char = 0;
		--count;
	}
	if (uart_circ_empty(xmit) || uart_tx_stopped(&uap->port)) {
		pl011_stop_tx(&uap->port);
		return false;
	}

	do {
		if (likely(from_irq) && count-- == 0)
			break;

		if (likely(from_irq) && count == 0 &&
		    pl011_read(uap, REG_FR) & UART01x_FR_TXFF)
			break;

		if (!pl011_tx_char(uap, xmit->buf[xmit->tail], from_irq))
			break;

		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
	} while (!uart_circ_empty(xmit));

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(&uap->port);

	if (uart_circ_empty(xmit)) {
		pl011_stop_tx(&uap->port);
		return false;
	}
	return true;
}

static void pl011_modem_status(struct uart_amba_port *uap)
{
	unsigned int status, delta;

	status = pl011_read(uap, REG_FR) & UART01x_FR_MODEM_ANY;

	delta = status ^ uap->old_status;
	uap->old_status = status;

	if (!delta)
		return;

	if (delta & UART01x_FR_DCD)
		uart_handle_dcd_change(&uap->port, status & UART01x_FR_DCD);

	if (delta & uap->vendor->fr_dsr)
		uap->port.icount.dsr++;

	if (delta & uap->vendor->fr_cts)
		uart_handle_cts_change(&uap->port,
				       status & uap->vendor->fr_cts);

	wake_up_interruptible(&uap->port.state->port.delta_msr_wait);
}

static void check_apply_cts_event_workaround(struct uart_amba_port *uap)
{
	if (!uap->vendor->cts_event_workaround)
		return;

	/* workaround to make sure that all bits are unlocked.. */
	pl011_write(0x00, uap, REG_ICR);

	/*
	 * WA: introduce 26ns(1 uart clk) delay before W1C;
	 * single apb access will incur 2 pclk(133.12Mhz) delay,
	 * so add 2 dummy reads
	 */
	pl011_read(uap, REG_ICR);
	pl011_read(uap, REG_ICR);
}

static irqreturn_t pl011_int(int irq, void *dev_id)
{
	struct uart_amba_port *uap = dev_id;
	unsigned long flags;
	unsigned int status, pass_counter = AMBA_ISR_PASS_LIMIT;
	int handled = 0;

	spin_lock_irqsave(&uap->port.lock, flags);
	status = pl011_read(uap, REG_RIS) & uap->im;
	if (status) {
		do {
			check_apply_cts_event_workaround(uap);

			pl011_write(status & ~(UART011_TXIS|UART011_RTIS|
					       UART011_RXIS),
				    uap, REG_ICR);

			if (status & (UART011_RTIS|UART011_RXIS)) {
				pl011_rx_chars(uap);
			}
			if (status & (UART011_DSRMIS|UART011_DCDMIS|
				      UART011_CTSMIS|UART011_RIMIS))
				pl011_modem_status(uap);
			if (status & UART011_TXIS)
				pl011_tx_chars(uap, true);

			if (pass_counter-- == 0)
				break;

			status = pl011_read(uap, REG_RIS) & uap->im;
		} while (status != 0);
		handled = 1;
	}

	spin_unlock_irqrestore(&uap->port.lock, flags);

	return IRQ_RETVAL(handled);
}

static unsigned int pl011_tx_empty(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	/* Allow feature register bits to be inverted to work around errata */
	unsigned int status = pl011_read(uap, REG_FR) ^ uap->vendor->inv_fr;

	return status & (uap->vendor->fr_busy | UART01x_FR_TXFF) ?
							0 : TIOCSER_TEMT;
}

static unsigned int pl011_get_mctrl(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int result = 0;
	unsigned int status = pl011_read(uap, REG_FR);

#define TIOCMBIT(uartbit, tiocmbit)	\
	if (status & uartbit)		\
		result |= tiocmbit

	TIOCMBIT(UART01x_FR_DCD, TIOCM_CAR);
	TIOCMBIT(uap->vendor->fr_dsr, TIOCM_DSR);
	TIOCMBIT(uap->vendor->fr_cts, TIOCM_CTS);
	TIOCMBIT(uap->vendor->fr_ri, TIOCM_RNG);
#undef TIOCMBIT
	return result;
}

static void pl011_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int cr;

	cr = pl011_read(uap, REG_CR);

#define	TIOCMBIT(tiocmbit, uartbit)		\
	if (mctrl & tiocmbit)		\
		cr |= uartbit;		\
	else				\
		cr &= ~uartbit

	TIOCMBIT(TIOCM_RTS, UART011_CR_RTS);
	TIOCMBIT(TIOCM_DTR, UART011_CR_DTR);
	TIOCMBIT(TIOCM_OUT1, UART011_CR_OUT1);
	TIOCMBIT(TIOCM_OUT2, UART011_CR_OUT2);
	TIOCMBIT(TIOCM_LOOP, UART011_CR_LBE);

	if (port->status & UPSTAT_AUTORTS) {
		/* We need to disable auto-RTS if we want to turn RTS off */
		TIOCMBIT(TIOCM_RTS, UART011_CR_RTSEN);
	}
#undef TIOCMBIT

	pl011_write(cr, uap, REG_CR);
}

static void pl011_break_ctl(struct uart_port *port, int break_state)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned long flags;
	unsigned int lcr_h;

	spin_lock_irqsave(&uap->port.lock, flags);
	lcr_h = pl011_read(uap, REG_LCRH_TX);
	if (break_state == -1)
		lcr_h |= UART01x_LCRH_BRK;
	else
		lcr_h &= ~UART01x_LCRH_BRK;
	pl011_write(lcr_h, uap, REG_LCRH_TX);
	spin_unlock_irqrestore(&uap->port.lock, flags);
}

#ifdef CONFIG_CONSOLE_POLL

static void pl011_quiesce_irqs(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	pl011_write(pl011_read(uap, REG_MIS), uap, REG_ICR);
	/*
	 * There is no way to clear TXIM as this is "ready to transmit IRQ", so
	 * we simply mask it. start_tx() will unmask it.
	 *
	 * Note we can race with start_tx(), and if the race happens, the
	 * polling user might get another interrupt just after we clear it.
	 * But it should be OK and can happen even w/o the race, e.g.
	 * controller immediately got some new data and raised the IRQ.
	 *
	 * And whoever uses polling routines assumes that it manages the device
	 * (including tx queue), so we're also fine with start_tx()'s caller
	 * side.
	 */
	pl011_write(pl011_read(uap, REG_IMSC) & ~UART011_TXIM, uap,
		    REG_IMSC);
}

static int pl011_get_poll_char(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int status;

	/*
	 * The caller might need IRQs lowered, e.g. if used with KDB NMI
	 * debugger.
	 */
	pl011_quiesce_irqs(port);

	status = pl011_read(uap, REG_FR);
	if (status & UART01x_FR_RXFE)
		return NO_POLL_CHAR;

	return pl011_read(uap, REG_DR);
}

static void pl011_put_poll_char(struct uart_port *port,
			 unsigned char ch)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	while (pl011_read(uap, REG_FR) & UART01x_FR_TXFF)
		cpu_relax();

	pl011_write(ch, uap, REG_DR);
}

#endif /* CONFIG_CONSOLE_POLL */

static int pl011_hwinit(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	int retval;

	/* Optionaly enable pins to be muxed in and configured */
	pinctrl_pm_select_default_state(port->dev);

	/*
	 * Try to enable the clock producer.
	 */
	retval = clk_prepare_enable(uap->clk);
	if (retval)
		return retval;

	uap->port.uartclk = clk_get_rate(uap->clk);
	printk(KERN_INFO "====Serial: uartclk is %d", uap->port.uartclk);

	/* Clear pending error and receive interrupts */
	pl011_write(UART011_OEIS | UART011_BEIS | UART011_PEIS |
		    UART011_FEIS | UART011_RTIS | UART011_RXIS,
		    uap, REG_ICR);

	/*
	 * Save interrupts enable mask, and enable RX interrupts in case if
	 * the interrupt is used for NMI entry.
	 */
	uap->im = pl011_read(uap, REG_IMSC);
	pl011_write(UART011_RTIM | UART011_RXIM, uap, REG_IMSC);

	if (dev_get_platdata(uap->port.dev)) {
		struct amba_pl011_data *plat;

		plat = dev_get_platdata(uap->port.dev);
		if (plat->init)
			plat->init();
	}
	return 0;
}

static bool pl011_split_lcrh(const struct uart_amba_port *uap)
{
	return pl011_reg_to_offset(uap, REG_LCRH_RX) !=
	       pl011_reg_to_offset(uap, REG_LCRH_TX);
}

static void pl011_write_lcr_h(struct uart_amba_port *uap, unsigned int lcr_h)
{
	pl011_write(lcr_h, uap, REG_LCRH_RX);
	if (pl011_split_lcrh(uap)) {
		int i;
		/*
		 * Wait 10 PCLKs before writing LCRH_TX register,
		 * to get this delay write read only register 10 times
		 */
		for (i = 0; i < 10; ++i)
			pl011_write(0xff, uap, REG_MIS);
		pl011_write(lcr_h, uap, REG_LCRH_TX);
	}
}

static int pl011_allocate_irq(struct uart_amba_port *uap)
{
	pl011_write(uap->im, uap, REG_IMSC);

	return request_irq(uap->port.irq, pl011_int, IRQF_SHARED, "uart-pl011", uap);
}

/*
 * Enable interrupts, only timeouts when using DMA
 * if initial RX DMA job failed, start in interrupt mode
 * as well.
 */
static void pl011_enable_interrupts(struct uart_amba_port *uap)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&uap->port.lock, flags);

	/* Clear out any spuriously appearing RX interrupts */
	pl011_write(UART011_RTIS | UART011_RXIS, uap, REG_ICR);

	/*
	 * RXIS is asserted only when the RX FIFO transitions from below
	 * to above the trigger threshold.  If the RX FIFO is already
	 * full to the threshold this can't happen and RXIS will now be
	 * stuck off.  Drain the RX FIFO explicitly to fix this:
	 */
	for (i = 0; i < uap->fifosize * 2; ++i) {
		if (pl011_read(uap, REG_FR) & UART01x_FR_RXFE)
			break;

		pl011_read(uap, REG_DR);
	}

	uap->im = UART011_RTIM | UART011_RXIM;
	pl011_write(uap->im, uap, REG_IMSC);
	spin_unlock_irqrestore(&uap->port.lock, flags);
}

static void pl011_unthrottle_rx(struct uart_port *port)
{
	struct uart_amba_port *uap = container_of(port, struct uart_amba_port, port);
	unsigned long flags;

	spin_lock_irqsave(&uap->port.lock, flags);

	uap->im = UART011_RTIM | UART011_RXIM;

	pl011_write(uap->im, uap, REG_IMSC);

	spin_unlock_irqrestore(&uap->port.lock, flags);
}

static int pl011_startup(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int cr;
	int retval;

	retval = pl011_hwinit(port);
	if (retval)
		goto clk_dis;

	retval = pl011_allocate_irq(uap);
	if (retval)
		goto clk_dis;

	pl011_write(uap->vendor->ifls, uap, REG_IFLS);

	spin_lock_irq(&uap->port.lock);

	cr = pl011_read(uap, REG_CR);
	cr &= UART011_CR_RTS | UART011_CR_DTR;
	cr |= UART01x_CR_UARTEN | UART011_CR_RXE;

	if (!(port->rs485.flags & SER_RS485_ENABLED))
		cr |= UART011_CR_TXE;

	pl011_write(cr, uap, REG_CR);

	spin_unlock_irq(&uap->port.lock);

	/*
	 * initialise the old status of the modem signals
	 */
	uap->old_status = pl011_read(uap, REG_FR) & UART01x_FR_MODEM_ANY;

	pl011_enable_interrupts(uap);

	return 0;

 clk_dis:
	clk_disable_unprepare(uap->clk);
	return retval;
}

static void pl011_shutdown_channel(struct uart_amba_port *uap,
					unsigned int lcrh)
{
      unsigned long val;

      val = pl011_read(uap, lcrh);
      val &= ~(UART01x_LCRH_BRK | UART01x_LCRH_FEN);
      pl011_write(val, uap, lcrh);
}

/*
 * disable the port. It should not disable RTS and DTR.
 * Also RTS and DTR state should be preserved to restore
 * it during startup().
 */
static void pl011_disable_uart(struct uart_amba_port *uap)
{
	unsigned int cr;

	uap->port.status &= ~(UPSTAT_AUTOCTS | UPSTAT_AUTORTS);
	spin_lock_irq(&uap->port.lock);
	cr = pl011_read(uap, REG_CR);
	cr &= UART011_CR_RTS | UART011_CR_DTR;
	cr |= UART01x_CR_UARTEN | UART011_CR_TXE;
	pl011_write(cr, uap, REG_CR);
	spin_unlock_irq(&uap->port.lock);

	/*
	 * disable break condition and fifos
	 */
	pl011_shutdown_channel(uap, REG_LCRH_RX);
	if (pl011_split_lcrh(uap))
		pl011_shutdown_channel(uap, REG_LCRH_TX);
}

static void pl011_disable_interrupts(struct uart_amba_port *uap)
{
	spin_lock_irq(&uap->port.lock);

	/* mask all interrupts and clear all pending ones */
	uap->im = 0;
	pl011_write(uap->im, uap, REG_IMSC);
	pl011_write(0xffff, uap, REG_ICR);

	spin_unlock_irq(&uap->port.lock);
}

static void pl011_shutdown(struct uart_port *port)
{
	struct uart_amba_port *uap =
		container_of(port, struct uart_amba_port, port);

	pl011_disable_interrupts(uap);


	if ((port->rs485.flags & SER_RS485_ENABLED) && uap->rs485_tx_started)
		pl011_rs485_tx_stop(uap);

	free_irq(uap->port.irq, uap);

	pl011_disable_uart(uap);

	/*
	 * Shut down the clock producer
	 */
	clk_disable_unprepare(uap->clk);
	/* Optionally let pins go into sleep states */
	pinctrl_pm_select_sleep_state(port->dev);

	if (dev_get_platdata(uap->port.dev)) {
		struct amba_pl011_data *plat;

		plat = dev_get_platdata(uap->port.dev);
		if (plat->exit)
			plat->exit();
	}

	if (uap->port.ops->flush_buffer)
		uap->port.ops->flush_buffer(port);
}

static void
pl011_setup_status_masks(struct uart_port *port, struct ktermios *termios)
{
	port->read_status_mask = UART011_DR_OE | 255;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= UART011_DR_FE | UART011_DR_PE;
	if (termios->c_iflag & (IGNBRK | BRKINT | PARMRK))
		port->read_status_mask |= UART011_DR_BE;

	/*
	 * Characters to ignore
	 */
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= UART011_DR_FE | UART011_DR_PE;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= UART011_DR_BE;
		/*
		 * If we're ignoring parity and break indicators,
		 * ignore overruns too (for real raw support).
		 */
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= UART011_DR_OE;
	}

	/*
	 * Ignore all characters if CREAD is not set.
	 */
	if ((termios->c_cflag & CREAD) == 0)
		port->ignore_status_mask |= UART_DUMMY_DR_RX;
}

static void
pl011_set_termios(struct uart_port *port, struct ktermios *termios,
		  const struct ktermios *old)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	unsigned int lcr_h, old_cr;
	unsigned long flags;
	unsigned int baud, quot, clkdiv;
	unsigned int bits;

	if (uap->vendor->oversampling)
		clkdiv = 8;
	else
		clkdiv = 16;

	/*
	 * Ask the core to calculate the divisor for us.
	 */
	baud = uart_get_baud_rate(port, termios, old, 0,
				  port->uartclk / clkdiv);

	if (baud > port->uartclk/16)
		quot = DIV_ROUND_CLOSEST(port->uartclk * 8, baud);
	else
		quot = DIV_ROUND_CLOSEST(port->uartclk * 4, baud);

	switch (termios->c_cflag & CSIZE) {
	case CS5:
		lcr_h = UART01x_LCRH_WLEN_5;
		break;
	case CS6:
		lcr_h = UART01x_LCRH_WLEN_6;
		break;
	case CS7:
		lcr_h = UART01x_LCRH_WLEN_7;
		break;
	default: // CS8
		lcr_h = UART01x_LCRH_WLEN_8;
		break;
	}
	if (termios->c_cflag & CSTOPB)
		lcr_h |= UART01x_LCRH_STP2;
	if (termios->c_cflag & PARENB) {
		lcr_h |= UART01x_LCRH_PEN;
		if (!(termios->c_cflag & PARODD))
			lcr_h |= UART01x_LCRH_EPS;
		if (termios->c_cflag & CMSPAR)
			lcr_h |= UART011_LCRH_SPS;
	}
	if (uap->fifosize > 1)
		lcr_h |= UART01x_LCRH_FEN;

	bits = tty_get_frame_size(termios->c_cflag);

	spin_lock_irqsave(&port->lock, flags);

	/*
	 * Update the per-port timeout.
	 */
	uart_update_timeout(port, termios->c_cflag, baud);

	/*
	 * Calculate the approximated time it takes to transmit one character
	 * with the given baud rate. We use this as the poll interval when we
	 * wait for the tx queue to empty.
	 */
	uap->rs485_tx_drain_interval = DIV_ROUND_UP(bits * 1000 * 1000, baud);

	pl011_setup_status_masks(port, termios);

	if (UART_ENABLE_MS(port, termios->c_cflag))
		pl011_enable_ms(port);

	if (port->rs485.flags & SER_RS485_ENABLED)
		termios->c_cflag &= ~CRTSCTS;

	old_cr = pl011_read(uap, REG_CR);

	if (termios->c_cflag & CRTSCTS) {
		if (old_cr & UART011_CR_RTS)
			old_cr |= UART011_CR_RTSEN;

		old_cr |= UART011_CR_CTSEN;
		port->status |= UPSTAT_AUTOCTS | UPSTAT_AUTORTS;
	} else {
		old_cr &= ~(UART011_CR_CTSEN | UART011_CR_RTSEN);
		port->status &= ~(UPSTAT_AUTOCTS | UPSTAT_AUTORTS);
	}

	if (uap->vendor->oversampling) {
		if (baud > port->uartclk / 16)
			old_cr |= ST_UART011_CR_OVSFACT;
		else
			old_cr &= ~ST_UART011_CR_OVSFACT;
	}

	/*
	 * Workaround for the ST Micro oversampling variants to
	 * increase the bitrate slightly, by lowering the divisor,
	 * to avoid delayed sampling of start bit at high speeds,
	 * else we see data corruption.
	 */
	if (uap->vendor->oversampling) {
		if ((baud >= 3000000) && (baud < 3250000) && (quot > 1))
			quot -= 1;
		else if ((baud > 3250000) && (quot > 2))
			quot -= 2;
	}
	/* Set baud rate */
	pl011_write(quot & 0x3f, uap, REG_FBRD);
	pl011_write(quot >> 6, uap, REG_IBRD);

	/*
	 * ----------v----------v----------v----------v-----
	 * NOTE: REG_LCRH_TX and REG_LCRH_RX MUST BE WRITTEN AFTER
	 * REG_FBRD & REG_IBRD.
	 * ----------^----------^----------^----------^-----
	 */
	pl011_write_lcr_h(uap, lcr_h);

	/*
	 * Receive was disabled by pl011_disable_uart during shutdown.
	 * Need to reenable receive if you need to use a tty_driver
	 * returns from tty_find_polling_driver() after a port shutdown.
	 */
	old_cr |= UART011_CR_RXE;
	pl011_write(old_cr, uap, REG_CR);

	spin_unlock_irqrestore(&port->lock, flags);
}

static const char *pl011_type(struct uart_port *port)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);
	return uap->port.type == PORT_AMBA ? uap->type : NULL;
}

/*
 * Configure/autoconfigure the port.
 */
static void pl011_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_AMBA;
}

/*
 * verify the new serial_struct (for TIOCSSERIAL).
 */
static int pl011_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	int ret = 0;
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_AMBA)
		ret = -EINVAL;
	if (ser->irq < 0 || ser->irq >= nr_irqs)
		ret = -EINVAL;
	if (ser->baud_base < 9600)
		ret = -EINVAL;
	if (port->mapbase != (unsigned long) ser->iomem_base)
		ret = -EINVAL;
	return ret;
}

static const struct uart_ops amba_pl011_pops = {
	.tx_empty	= pl011_tx_empty,
	.set_mctrl	= pl011_set_mctrl,
	.get_mctrl	= pl011_get_mctrl,
	.stop_tx	= pl011_stop_tx,
	.start_tx	= pl011_start_tx,
	.stop_rx	= pl011_stop_rx,
	.throttle	= pl011_throttle_rx,
	.unthrottle	= pl011_unthrottle_rx,
	.enable_ms	= pl011_enable_ms,
	.break_ctl	= pl011_break_ctl,
	.startup	= pl011_startup,
	.shutdown	= pl011_shutdown,
	.set_termios	= pl011_set_termios,
	.type		= pl011_type,
	.config_port	= pl011_config_port,
	.verify_port	= pl011_verify_port,
#ifdef CONFIG_CONSOLE_POLL
	.poll_init     = pl011_hwinit,
	.poll_get_char = pl011_get_poll_char,
	.poll_put_char = pl011_put_poll_char,
#endif
};

static struct uart_amba_port *amba_ports[UART_NR];

static void pl011_console_putchar(struct uart_port *port, unsigned char ch)
{
	struct uart_amba_port *uap =
	    container_of(port, struct uart_amba_port, port);

	while (pl011_read(uap, REG_FR) & UART01x_FR_TXFF)
		cpu_relax();
	pl011_write(ch, uap, REG_DR);
}

static void
pl011_console_write(struct console *co, const char *s, unsigned int count)
{
	struct uart_amba_port *uap = amba_ports[co->index];
	unsigned int old_cr = 0, new_cr;
	unsigned long flags;
	int locked = 1;

	clk_enable(uap->clk);

	local_irq_save(flags);
	if (uap->port.sysrq)
		locked = 0;
	else if (oops_in_progress)
		locked = spin_trylock(&uap->port.lock);
	else
		spin_lock(&uap->port.lock);

	/*
	 *	First save the CR then disable the interrupts
	 */
	if (!uap->vendor->always_enabled) {
		old_cr = pl011_read(uap, REG_CR);
		new_cr = old_cr & ~UART011_CR_CTSEN;
		new_cr |= UART01x_CR_UARTEN | UART011_CR_TXE;
		pl011_write(new_cr, uap, REG_CR);
	}

	uart_console_write(&uap->port, s, count, pl011_console_putchar);

	/*
	 *	Finally, wait for transmitter to become empty and restore the
	 *	TCR. Allow feature register bits to be inverted to work around
	 *	errata.
	 */
	while ((pl011_read(uap, REG_FR) ^ uap->vendor->inv_fr)
						& uap->vendor->fr_busy)
		cpu_relax();
	if (!uap->vendor->always_enabled)
		pl011_write(old_cr, uap, REG_CR);

	if (locked)
		spin_unlock(&uap->port.lock);
	local_irq_restore(flags);

	clk_disable(uap->clk);
}

static void pl011_console_get_options(struct uart_amba_port *uap, int *baud,
				      int *parity, int *bits)
{
	if (pl011_read(uap, REG_CR) & UART01x_CR_UARTEN) {
		unsigned int lcr_h, ibrd, fbrd;

		lcr_h = pl011_read(uap, REG_LCRH_TX);

		*parity = 'n';
		if (lcr_h & UART01x_LCRH_PEN) {
			if (lcr_h & UART01x_LCRH_EPS)
				*parity = 'e';
			else
				*parity = 'o';
		}

		if ((lcr_h & 0x60) == UART01x_LCRH_WLEN_7)
			*bits = 7;
		else
			*bits = 8;

		ibrd = pl011_read(uap, REG_IBRD);
		fbrd = pl011_read(uap, REG_FBRD);

		*baud = uap->port.uartclk * 4 / (64 * ibrd + fbrd);

		if (uap->vendor->oversampling) {
			if (pl011_read(uap, REG_CR)
				  & ST_UART011_CR_OVSFACT)
				*baud *= 2;
		}
	}
}

static int pl011_console_setup(struct console *co, char *options)
{
	struct uart_amba_port *uap;
	int baud = 38400;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';
	int ret;

	/*
	 * Check whether an invalid uart number has been specified, and
	 * if so, search for the first available port that does have
	 * console support.
	 */
	if (co->index >= UART_NR)
		co->index = 0;
	uap = amba_ports[co->index];
	if (!uap)
		return -ENODEV;

	/* Allow pins to be muxed in and configured */
	pinctrl_pm_select_default_state(uap->port.dev);

	ret = clk_prepare(uap->clk);
	if (ret)
		return ret;

	if (dev_get_platdata(uap->port.dev)) {
		struct amba_pl011_data *plat;

		plat = dev_get_platdata(uap->port.dev);
		if (plat->init)
			plat->init();
	}

	uap->port.uartclk = clk_get_rate(uap->clk);

	if (uap->vendor->fixed_options) {
		baud = uap->fixed_baud;
	} else {
		if (options)
			uart_parse_options(options,
					   &baud, &parity, &bits, &flow);
		else
			pl011_console_get_options(uap, &baud, &parity, &bits);
	}

	return uart_set_options(&uap->port, co, baud, parity, bits, flow);
}

/**
 *	pl011_console_match - non-standard console matching
 *	@co:	  registering console
 *	@name:	  name from console command line
 *	@idx:	  index from console command line
 *	@options: ptr to option string from console command line
 *
 *	Only attempts to match console command lines of the form:
 *	    console=pl011,mmio|mmio32,<addr>[,<options>]
 *	    console=pl011,0x<addr>[,<options>]
 *	This form is used to register an initial earlycon boot console and
 *	replace it with the amba_console at pl011 driver init.
 *
 *	Performs console setup for a match (as required by interface)
 *	If no <options> are specified, then assume the h/w is already setup.
 *
 *	Returns 0 if console matches; otherwise non-zero to use default matching
 */
static int pl011_console_match(struct console *co, char *name, int idx,
			       char *options)
{
	unsigned char iotype;
	resource_size_t addr;
	int i;

	/*
	 * Systems affected by the Qualcomm Technologies QDF2400 E44 erratum
	 * have a distinct console name, so make sure we check for that.
	 * The actual implementation of the erratum occurs in the probe
	 * function.
	 */
	if ((strcmp(name, "qdf2400_e44") != 0) && (strcmp(name, "pl011") != 0))
		return -ENODEV;

	if (uart_parse_earlycon(options, &iotype, &addr, &options))
		return -ENODEV;

	if (iotype != UPIO_MEM && iotype != UPIO_MEM32)
		return -ENODEV;

	/* try to match the port specified on the command line */
	for (i = 0; i < ARRAY_SIZE(amba_ports); i++) {
		struct uart_port *port;

		if (!amba_ports[i])
			continue;

		port = &amba_ports[i]->port;

		if (port->mapbase != addr)
			continue;

		co->index = i;
		port->cons = co;
		return pl011_console_setup(co, options);
	}

	return -ENODEV;
}

static struct uart_driver amba_reg;
static struct console amba_console = {
	.name		= "ttyAMA",
	.write		= pl011_console_write,
	.device		= uart_console_device,
	.setup		= pl011_console_setup,
	.match		= pl011_console_match,
	.flags		= CON_PRINTBUFFER | CON_ANYTIME,
	.index		= -1,
	.data		= &amba_reg,
};

#define AMBA_CONSOLE	(&amba_console)

static struct uart_driver amba_reg = {
	.owner			= THIS_MODULE,
	.driver_name	= "ttyAMA",
	.dev_name		= "ttyAMA",
	.major			= SERIAL_AMBA_MAJOR,
	.minor			= SERIAL_AMBA_MINOR,
	.nr			    = UART_NR,
	.cons			= AMBA_CONSOLE,
};

static int pl011_probe_dt_alias(int index, struct device *dev)
{
	
	struct device_node *np;
	static bool seen_dev_with_alias = false;
	static bool seen_dev_without_alias = false;
	int ret = index;

	if (!IS_ENABLED(CONFIG_OF))
		return ret;

	np = dev->of_node;
	if (!np)
		return ret;

	ret = of_alias_get_id(np, "serial");
	if (ret < 0) {
		seen_dev_without_alias = true;
		ret = index;
	} else {
		seen_dev_with_alias = true;
		if (ret >= ARRAY_SIZE(amba_ports) || amba_ports[ret] != NULL) {
			dev_warn(dev, "requested serial port %d  not available.\n", ret);
			ret = index;
		}
	}

	if (seen_dev_with_alias && seen_dev_without_alias)
		dev_warn(dev, "aliased and non-aliased serial devices found in device tree. Serial port enumeration may be unpredictable.\n");

	return ret;
}

/* unregisters the driver also if no more ports are left */
static void pl011_unregister_port(struct uart_amba_port *uap)
{
	int i;
	bool busy = false;

	for (i = 0; i < ARRAY_SIZE(amba_ports); i++) {
		if (amba_ports[i] == uap)
			amba_ports[i] = NULL;
		else if (amba_ports[i])
			busy = true;
	}
	if (!busy)
		uart_unregister_driver(&amba_reg);
}

static int pl011_find_free_port(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(amba_ports); i++)
		if (amba_ports[i] == NULL)
			return i;

	return -EBUSY;
}

static int pl011_setup_port(struct device *dev, struct uart_amba_port *uap,
			    struct resource *mmiobase, int index)
{
	void __iomem *base;

	base = devm_ioremap_resource(dev, mmiobase);
	if (IS_ERR(base))
		return PTR_ERR(base);

	index = pl011_probe_dt_alias(index, dev);

	uap->port.dev = dev;
	uap->port.mapbase = mmiobase->start;
	uap->port.membase = base;
	uap->port.fifosize = uap->fifosize;
	uap->port.has_sysrq = IS_ENABLED(CONFIG_SERIAL_AMBA_PL011_CONSOLE);
	uap->port.flags = UPF_BOOT_AUTOCONF;
	uap->port.line = index;

	amba_ports[index] = uap;

	return 0;
}

static int pl011_register_port(struct uart_amba_port *uap)
{
	int ret, i;

	/* Ensure interrupts from this UART are masked and cleared */
	pl011_write(0, uap, REG_IMSC);
	pl011_write(0xffff, uap, REG_ICR);

	if (!amba_reg.state) {
		ret = uart_register_driver(&amba_reg);
		if (ret < 0) {
			dev_err(uap->port.dev,
				"Failed to register AMBA-PL011 driver\n");
			for (i = 0; i < ARRAY_SIZE(amba_ports); i++)
				if (amba_ports[i] == uap)
					amba_ports[i] = NULL;
			return ret;
		}
	}

	ret = uart_add_one_port(&amba_reg, &uap->port);
	if (ret)
		pl011_unregister_port(uap);

	return ret;
}

static int pl011_probe(struct amba_device *dev, const struct amba_id *id)
{
	struct uart_amba_port *uap;
	struct vendor_data *vendor = id->data;
	int portnr, ret;

	portnr = pl011_find_free_port();
	if (portnr < 0)
		return portnr;

	uap = devm_kzalloc(&dev->dev, sizeof(struct uart_amba_port),
			   GFP_KERNEL);
	if (!uap)
		return -ENOMEM;

	uap->clk = devm_clk_get(&dev->dev, NULL);
	if (IS_ERR(uap->clk))
		return PTR_ERR(uap->clk);

	if (of_property_read_bool(dev->dev.of_node, "cts-event-workaround")) {
	    vendor->cts_event_workaround = true;
	    dev_info(&dev->dev, "cts_event_workaround enabled\n");
	}

	uap->reg_offset = vendor->reg_offset;
	uap->vendor = vendor;
	uap->fifosize = vendor->get_fifosize(dev);
	uap->port.iotype = vendor->access_32b ? UPIO_MEM32 : UPIO_MEM;
	uap->port.irq = dev->irq[0];
	uap->port.ops = &amba_pl011_pops;
	snprintf(uap->type, sizeof(uap->type), "PL011 rev%u", amba_rev(dev));

	ret = pl011_setup_port(&dev->dev, uap, &dev->res, portnr);
	if (ret)
		return ret;

	amba_set_drvdata(dev, uap);

	return pl011_register_port(uap);
}

static void pl011_remove(struct amba_device *dev)
{
	struct uart_amba_port *uap = amba_get_drvdata(dev);

	uart_remove_one_port(&amba_reg, &uap->port);
	pl011_unregister_port(uap);
}

static const struct amba_id pl011_ids[] = {
	{
		.id	= 0x00041011,
		.mask	= 0x000fffff,
		.data	= &vendor_arm,
	},
	{ 0, 0 },
};

MODULE_DEVICE_TABLE(amba, pl011_ids);

static struct amba_driver pl011_driver = {
	.drv = {
		.name	= "uart-pl011",
		.suppress_bind_attrs = IS_BUILTIN(CONFIG_SERIAL_AMBA_PL011),
	},
	.id_table	= pl011_ids,
	.probe		= pl011_probe,
	.remove		= pl011_remove,
};

static int __init pl011_init(void)
{
	printk(KERN_INFO "Serial: AMBA PL011 UART driver\n");
	return amba_driver_register(&pl011_driver);
}

static void __exit pl011_exit(void)
{
	amba_driver_unregister(&pl011_driver);
}

/*
 * While this can be a module, if builtin it's most likely the console
 * So let's leave module_exit but move module_init to an earlier place
 */
arch_initcall(pl011_init);
module_exit(pl011_exit);

MODULE_AUTHOR("ARM Ltd/Deep Blue Solutions Ltd");
MODULE_DESCRIPTION("ARM AMBA serial port driver");
MODULE_LICENSE("GPL");
