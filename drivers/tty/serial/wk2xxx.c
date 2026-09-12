// SPDX-License-Identifier: GPL-2.0+
/*
 * WK2xxx SPI to UART bridge tty serial driver
 *
 * SPI-to-UART bridge ICs from WKmic (Chengdu Weikai Microelectronics):
 * WK2124, WK2132, WK2168, WK2202 and WK2204. Each IC exposes two or four
 * full-duplex UART channels with 256-byte RX/TX FIFOs through a single SPI
 * slave interface and one interrupt line. The slave register set is split
 * into two banks (page 0 / page 1) selected by the SPAGE register.
 *
 * This driver is a rework of the vendor "wk2xxx" driver (originally at
 * https://github.com/britus/wk2xxx) and is modeled after the NXP sc16is7xx
 * driver.
 *
 * (C) Copyright 2022 WKIC Ltd. by Xu XunWei Tech, Xuxunwei
 * (C) Copyright 2024 EoF Software Labs, B. Eschrich
 * Copyright (C) 2026 EDATEC Technology Co., Ltd. <zjzhao@edatec.cn>
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/sched.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/units.h>
#include <linux/workqueue.h>

#define WK2XXX_NAME		"wk2xxx"
#define WK2XXX_MAX_DEVS		8	/* Total number of lines. */
#define WK2XXX_MAX_PORTS	4	/* Max number of ports per IC. */
#define WK2XXX_FIFO_SIZE	256
#define WK2XXX_MAX_SPI_LEN	30	/* Max bytes per SPI FIFO burst. */
#define WK2XXX_MAX_TX_CHARS	200	/* Leave headroom in the TX FIFO. */
#define WK2XXX_RXFIFO_LEVEL	0x40	/* RX FIFO trigger level. */
#define WK2XXX_TXFIFO_LEVEL	0x01	/* TX FIFO trigger level. */
#define WK2XXX_POLL_PERIOD_MS	10

/* SPI command byte: bit 6 = read, bit 7 = FIFO access. */
#define WK2XXX_SPI_READ		BIT(6)
#define WK2XXX_SPI_FIFO_WRITE	BIT(7)
#define WK2XXX_SPI_FIFO_READ	(BIT(7) | BIT(6))

/* Marker used to address registers located in page 1. */
#define WK2XXX_PAGE1		BIT(7)

/* Global registers. */
#define WK2XXX_GENA_REG		0x00	/* Global UART enable */
#define WK2XXX_GRST_REG		0x01	/* Global reset */
#define WK2XXX_GMUT_REG		0x02	/* Master UART control */
#define WK2XXX_GIER_REG		0x10	/* Global interrupt enable */
#define WK2XXX_GIFR_REG		0x11	/* Global interrupt flag */

/* Port (sub-UART) registers, page 0. */
#define WK2XXX_SPAGE_REG	0x03	/* Register page select */
#define WK2XXX_SCR_REG		0x04	/* Slave control */
#define WK2XXX_LCR_REG		0x05	/* Line control */
#define WK2XXX_FCR_REG		0x06	/* FIFO control */
#define WK2XXX_SIER_REG		0x07	/* Slave interrupt enable */
#define WK2XXX_SIFR_REG		0x08	/* Slave interrupt flag */
#define WK2XXX_TFCNT_REG	0x09	/* TX FIFO count */
#define WK2XXX_RFCNT_REG	0x0a	/* RX FIFO count */
#define WK2XXX_FSR_REG		0x0b	/* FIFO status */
#define WK2XXX_LSR_REG		0x0c	/* Line status */
#define WK2XXX_FDAT_REG		0x0d	/* FIFO data */
#define WK2XXX_FWCR_REG		0x0e	/* Flow control */
#define WK2XXX_RS485_REG	0x0f	/* RS485 control */

/* Port (sub-UART) registers, page 1. */
#define WK2XXX_BAUD1_REG	(0x04 | WK2XXX_PAGE1)	/* Divisor Latch High */
#define WK2XXX_BAUD0_REG	(0x05 | WK2XXX_PAGE1)	/* Divisor Latch Low */
#define WK2XXX_PRES_REG		(0x06 | WK2XXX_PAGE1)	/* Fractional divisor */
#define WK2XXX_RFTL_REG		(0x07 | WK2XXX_PAGE1)	/* RX FIFO trigger level */
#define WK2XXX_TFTL_REG		(0x08 | WK2XXX_PAGE1)	/* TX FIFO trigger level */
#define WK2XXX_FWTH_REG		(0x09 | WK2XXX_PAGE1)	/* Flow control high level */
#define WK2XXX_FWTL_REG		(0x0a | WK2XXX_PAGE1)	/* Flow control low level */
#define WK2XXX_XON1_REG		(0x0b | WK2XXX_PAGE1)	/* Xon word */
#define WK2XXX_XOFF1_REG	(0x0c | WK2XXX_PAGE1)	/* Xoff word */
#define WK2XXX_SADR_REG		(0x0d | WK2XXX_PAGE1)	/* RS485 auto address */
#define WK2XXX_SAEN_REG		(0x0e | WK2XXX_PAGE1)	/* RS485 address mask */
#define WK2XXX_RRSDLY_REG	(0x0f | WK2XXX_PAGE1)	/* RS485 RTS delay */

/* SCR register bits. */
#define WK2XXX_SCR_RXEN_BIT	BIT(0)
#define WK2XXX_SCR_TXEN_BIT	BIT(1)

/* LCR register bits. */
#define WK2XXX_LCR_STPL_BIT	BIT(0)	/* Two stop bits */
#define WK2XXX_LCR_PAM0_BIT	BIT(1)	/* Parity mode bit 0 */
#define WK2XXX_LCR_PAM1_BIT	BIT(2)	/* Parity mode bit 1 */
#define WK2XXX_LCR_PAEN_BIT	BIT(3)	/* Parity enable */
#define WK2XXX_LCR_BREAK_BIT	BIT(5)	/* TX break */

/* SIER register bits. */
#define WK2XXX_SIER_RFTRIG_IEN_BIT	BIT(0)	/* RX FIFO trigger */
#define WK2XXX_SIER_RXOUT_IEN_BIT	BIT(1)	/* RX time-out */
#define WK2XXX_SIER_TFTRIG_IEN_BIT	BIT(2)	/* TX FIFO trigger */

/* SIFR register bits. */
#define WK2XXX_SIFR_RFTRIG_INT_BIT	BIT(0)
#define WK2XXX_SIFR_RXOVT_INT_BIT	BIT(1)
#define WK2XXX_SIFR_TFTRIG_INT_BIT	BIT(2)

/* FSR register bits. */
#define WK2XXX_FSR_TBUSY_BIT	BIT(0)
#define WK2XXX_FSR_TFULL_BIT	BIT(1)
#define WK2XXX_FSR_TDAT_BIT	BIT(2)
#define WK2XXX_FSR_RDAT_BIT	BIT(3)
#define WK2XXX_FSR_RFPE_BIT	BIT(4)	/* RX FIFO parity error */
#define WK2XXX_FSR_RFFE_BIT	BIT(5)	/* RX FIFO frame error */
#define WK2XXX_FSR_RFBI_BIT	BIT(6)	/* RX FIFO break */
#define WK2XXX_FSR_RFOE_BIT	BIT(7)	/* RX FIFO overrun */
#define WK2XXX_FSR_ERR_MASK	GENMASK(7, 4)

/* LSR error bits, for use with uart_insert_char(). */
#define WK2XXX_LSR_PE_BIT	BIT(0)
#define WK2XXX_LSR_FE_BIT	BIT(1)
#define WK2XXX_LSR_BI_BIT	BIT(2)
#define WK2XXX_LSR_OE_BIT	BIT(3)
#define WK2XXX_LSR_BRK_ERROR_MASK	(WK2XXX_LSR_OE_BIT | WK2XXX_LSR_PE_BIT | \
					 WK2XXX_LSR_FE_BIT | WK2XXX_LSR_BI_BIT)

/*
 * FWCR register bits. The flow-control mode is selected by the FWM2-0
 * field in bits 6-4 (WK2132 has no FWCR register; writing it is ignored).
 */
#define WK2XXX_FWCR_FWM_MASK	GENMASK(6, 4)
#define WK2XXX_FWCR_FWM_RTS_CTS	FIELD_PREP(WK2XXX_FWCR_FWM_MASK, 0x3)

/* RS485 register bits. */
#define WK2XXX_RS485_RTSINV_BIT		BIT(0)
#define WK2XXX_RS485_RTSEN_BIT		BIT(1)
#define WK2XXX_RS485_RSRS485_BIT	BIT(6)

struct wk2xxx_devtype {
	const char	*name;
	int		nr_uart;
	unsigned long	crystal_freq;
};

#define WK2XXX_RECONF_IER	BIT(0)
#define WK2XXX_RECONF_RS485	BIT(1)

struct wk2xxx_one_config {
	unsigned int	flags;
	u8		ier_mask;
	u8		ier_val;
};

struct wk2xxx_one {
	struct uart_port	port;
	struct mutex		tx_lock;	/* Serializes the TX path. */
	struct kthread_work	tx_work;
	struct kthread_work	reg_work;
	struct wk2xxx_one_config config;
	unsigned char		buf[WK2XXX_FIFO_SIZE];	/* RX buffer. */
};

struct wk2xxx_port {
	const struct wk2xxx_devtype	*devtype;
	struct spi_device		*spi;
	struct mutex			reg_lock;	/* SPI register access. */
	struct kthread_worker		kworker;
	struct task_struct		*kworker_task;
	struct kthread_delayed_work	poll_work;
	bool				polling;
	struct wk2xxx_one		p[];
};

static DEFINE_IDA(wk2xxx_lines);

static struct uart_driver wk2xxx_uart = {
	.owner		= THIS_MODULE,
	.driver_name	= WK2XXX_NAME,
	.dev_name	= "ttyWK",
	.nr		= WK2XXX_MAX_DEVS,
};

#define to_wk2xxx_one(p, e)	((container_of((p), struct wk2xxx_one, e)))

static const struct wk2xxx_devtype wk2124_devtype = {
	.name		= "WK2124",
	.nr_uart	= 4,
	.crystal_freq	= 11059200,
};

static const struct wk2xxx_devtype wk2132_devtype = {
	.name		= "WK2132",
	.nr_uart	= 2,
	.crystal_freq	= 11059200,
};

static const struct wk2xxx_devtype wk2168_devtype = {
	.name		= "WK2168",
	.nr_uart	= 4,
	.crystal_freq	= 11059200,
};

static const struct wk2xxx_devtype wk2202_devtype = {
	.name		= "WK2202",
	.nr_uart	= 2,
	.crystal_freq	= 11059200,
};

static const struct wk2xxx_devtype wk2204_devtype = {
	.name		= "WK2204",
	.nr_uart	= 4,
	.crystal_freq	= 11059200,
};

/*
 * The following functions are the low-level SPI accessors. The caller must
 * hold s->reg_lock, so that multi-byte accesses and page switches are
 * performed atomically on the SPI bus.
 */
static int wk2xxx_spi_transfer(struct wk2xxx_port *s, const u8 *tx, u8 *rx,
			       unsigned int len)
{
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = len,
	};
	struct spi_message msg;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	return spi_sync(s->spi, &msg);
}

static int wk2xxx_raw_read(struct wk2xxx_port *s, u8 addr, u8 *val)
{
	u8 tx[2] = { WK2XXX_SPI_READ | addr, 0 };
	u8 rx[2] = { 0, 0 };
	int ret;

	ret = wk2xxx_spi_transfer(s, tx, rx, sizeof(tx));
	if (ret)
		return ret;

	*val = rx[1];
	return 0;
}

static int wk2xxx_raw_write(struct wk2xxx_port *s, u8 addr, u8 val)
{
	u8 tx[2] = { addr, val };
	u8 rx[2] = { 0, 0 };

	return wk2xxx_spi_transfer(s, tx, rx, sizeof(tx));
}

static unsigned int wk2xxx_port_addr(unsigned int portno, u8 reg)
{
	/* The sub-UART number is encoded in the upper nibble of the cmd byte. */
	return (portno << 4) | reg;
}

static int wk2xxx_raw_port_read(struct wk2xxx_port *s, unsigned int portno,
				u8 reg, u8 *val)
{
	int ret;

	if (reg & WK2XXX_PAGE1) {
		ret = wk2xxx_raw_write(s, wk2xxx_port_addr(portno, WK2XXX_SPAGE_REG), 1);
		if (ret)
			return ret;
		ret = wk2xxx_raw_read(s, wk2xxx_port_addr(portno, reg & 0x0f), val);
		wk2xxx_raw_write(s, wk2xxx_port_addr(portno, WK2XXX_SPAGE_REG), 0);
		return ret;
	}

	return wk2xxx_raw_read(s, wk2xxx_port_addr(portno, reg & 0x0f), val);
}

static int wk2xxx_raw_port_write(struct wk2xxx_port *s, unsigned int portno,
				 u8 reg, u8 val)
{
	int ret;

	if (reg & WK2XXX_PAGE1) {
		ret = wk2xxx_raw_write(s, wk2xxx_port_addr(portno, WK2XXX_SPAGE_REG), 1);
		if (ret)
			return ret;
		ret = wk2xxx_raw_write(s, wk2xxx_port_addr(portno, reg & 0x0f), val);
		wk2xxx_raw_write(s, wk2xxx_port_addr(portno, WK2XXX_SPAGE_REG), 0);
		return ret;
	}

	return wk2xxx_raw_write(s, wk2xxx_port_addr(portno, reg & 0x0f), val);
}

/*
 * Locked wrappers used outside the register sequences that already hold
 * s->reg_lock.
 */
static int wk2xxx_reg_read(struct wk2xxx_port *s, u8 reg, u8 *val)
{
	guard(mutex)(&s->reg_lock);
	return wk2xxx_raw_read(s, reg, val);
}

static int wk2xxx_port_reg_read(struct wk2xxx_port *s, unsigned int portno,
				u8 reg, u8 *val)
{
	guard(mutex)(&s->reg_lock);
	return wk2xxx_raw_port_read(s, portno, reg, val);
}

static int wk2xxx_port_reg_write(struct wk2xxx_port *s, unsigned int portno,
				 u8 reg, u8 val)
{
	guard(mutex)(&s->reg_lock);
	return wk2xxx_raw_port_write(s, portno, reg, val);
}

static void wk2xxx_port_reg_update(struct wk2xxx_port *s, unsigned int portno,
				   u8 reg, u8 mask, u8 val)
{
	u8 r = 0;

	scoped_guard(mutex, &s->reg_lock) {
		if (wk2xxx_raw_port_read(s, portno, reg, &r))
			return;
		wk2xxx_raw_port_write(s, portno, reg, (r & ~mask) | val);
	}
}

static int wk2xxx_fifo_read(struct wk2xxx_port *s, unsigned int portno,
			    u8 *buf, unsigned int len)
{
	u8 tx[WK2XXX_MAX_SPI_LEN + 1];
	u8 rx[WK2XXX_MAX_SPI_LEN + 1];
	int ret;

	if (len == 0 || len > WK2XXX_MAX_SPI_LEN)
		return -EINVAL;

	memset(tx, 0, sizeof(tx));
	tx[0] = wk2xxx_port_addr(portno, WK2XXX_SPI_FIFO_READ);

	guard(mutex)(&s->reg_lock);
	ret = wk2xxx_spi_transfer(s, tx, rx, len + 1);
	if (ret)
		return ret;

	memcpy(buf, rx + 1, len);
	return 0;
}

static int wk2xxx_fifo_write(struct wk2xxx_port *s, unsigned int portno,
			     const u8 *buf, unsigned int len)
{
	u8 tx[WK2XXX_MAX_SPI_LEN + 1];
	u8 rx[WK2XXX_MAX_SPI_LEN + 1];

	if (len == 0 || len > WK2XXX_MAX_SPI_LEN)
		return -EINVAL;

	tx[0] = wk2xxx_port_addr(portno, WK2XXX_SPI_FIFO_WRITE);
	memcpy(tx + 1, buf, len);

	guard(mutex)(&s->reg_lock);
	return wk2xxx_spi_transfer(s, tx, rx, len + 1);
}

static void wk2xxx_ier_set(struct uart_port *port, u8 bit)
{
	struct wk2xxx_one *one = to_wk2xxx_one(port, port);
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);

	lockdep_assert_held_once(&port->lock);

	one->config.flags |= WK2XXX_RECONF_IER;
	one->config.ier_mask |= bit;
	one->config.ier_val |= bit;
	kthread_queue_work(&s->kworker, &one->reg_work);
}

static void wk2xxx_ier_clear(struct uart_port *port, u8 bit)
{
	struct wk2xxx_one *one = to_wk2xxx_one(port, port);
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);

	lockdep_assert_held_once(&port->lock);

	one->config.flags |= WK2XXX_RECONF_IER;
	one->config.ier_mask |= bit;
	one->config.ier_val &= ~bit;
	kthread_queue_work(&s->kworker, &one->reg_work);
}

static void wk2xxx_stop_tx(struct uart_port *port)
{
	wk2xxx_ier_clear(port, WK2XXX_SIER_TFTRIG_IEN_BIT);
}

static void wk2xxx_stop_rx(struct uart_port *port)
{
	wk2xxx_ier_clear(port, WK2XXX_SIER_RFTRIG_IEN_BIT |
			 WK2XXX_SIER_RXOUT_IEN_BIT);
}

static void wk2xxx_throttle(struct uart_port *port)
{
	unsigned long flags;

	/* Stop draining the RX FIFO to apply back-pressure. */
	uart_port_lock_irqsave(port, &flags);
	wk2xxx_ier_clear(port, WK2XXX_SIER_RFTRIG_IEN_BIT);
	uart_port_unlock_irqrestore(port, flags);
}

static void wk2xxx_unthrottle(struct uart_port *port)
{
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	wk2xxx_ier_set(port, WK2XXX_SIER_RFTRIG_IEN_BIT);
	uart_port_unlock_irqrestore(port, flags);
}

static void wk2xxx_handle_tx(struct uart_port *port)
{
	struct wk2xxx_one *one = to_wk2xxx_one(port, port);
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);
	struct tty_port *tport = &port->state->port;
	unsigned long flags;
	unsigned int portno = port->iobase;
	unsigned int txlen, to_send, sent;
	const unsigned char *tail;
	u8 fsr, tfcnt;

	mutex_lock(&one->tx_lock);

	if (unlikely(port->x_char)) {
		wk2xxx_port_reg_write(s, portno, WK2XXX_FDAT_REG, port->x_char);
		port->icount.tx++;
		port->x_char = 0;
		goto out;
	}

	if (kfifo_is_empty(&tport->xmit_fifo) || uart_tx_stopped(port)) {
		uart_port_lock_irqsave(port, &flags);
		wk2xxx_stop_tx(port);
		uart_port_unlock_irqrestore(port, flags);
		goto out;
	}

	/* Limit to the free space available in the TX FIFO. */
	wk2xxx_port_reg_read(s, portno, WK2XXX_TFCNT_REG, &tfcnt);
	if (tfcnt == 0) {
		wk2xxx_port_reg_read(s, portno, WK2XXX_FSR_REG, &fsr);
		txlen = (fsr & WK2XXX_FSR_TFULL_BIT) ? 0 : WK2XXX_FIFO_SIZE;
	} else {
		txlen = WK2XXX_FIFO_SIZE - tfcnt;
	}
	if (txlen > WK2XXX_MAX_TX_CHARS)
		txlen = WK2XXX_MAX_TX_CHARS;

	to_send = kfifo_out_linear_ptr(&tport->xmit_fifo, &tail, txlen);
	sent = to_send;
	while (to_send) {
		unsigned int chunk = min_t(unsigned int, to_send,
					   WK2XXX_MAX_SPI_LEN);

		wk2xxx_fifo_write(s, portno, tail, chunk);
		tail += chunk;
		to_send -= chunk;
	}
	uart_xmit_advance(port, sent);

	uart_port_lock_irqsave(port, &flags);
	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	if (kfifo_is_empty(&tport->xmit_fifo))
		wk2xxx_stop_tx(port);
	else
		wk2xxx_ier_set(port, WK2XXX_SIER_TFTRIG_IEN_BIT);
	uart_port_unlock_irqrestore(port, flags);

out:
	mutex_unlock(&one->tx_lock);
}

static void wk2xxx_handle_rx(struct uart_port *port)
{
	struct wk2xxx_one *one = to_wk2xxx_one(port, port);
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);
	unsigned int portno = port->iobase;
	unsigned int i, rxlen, len_p, chunk;
	u8 fsr, rfcnt, lsr = 0, flag = TTY_NORMAL;

	wk2xxx_port_reg_read(s, portno, WK2XXX_FSR_REG, &fsr);

	if (!(fsr & WK2XXX_FSR_RDAT_BIT))
		return;

	/* Get the number of bytes available in the RX FIFO. */
	wk2xxx_port_reg_read(s, portno, WK2XXX_RFCNT_REG, &rfcnt);
	if (rfcnt == 0) {
		/* The count may race with the FIFO status bit; retry once. */
		wk2xxx_port_reg_read(s, portno, WK2XXX_RFCNT_REG, &rfcnt);
		rxlen = rfcnt ? rfcnt : WK2XXX_FIFO_SIZE;
	} else {
		rxlen = rfcnt;
	}

	/* Read the FIFO contents in chunks. */
	len_p = 0;
	while (rxlen) {
		chunk = min_t(unsigned int, rxlen, WK2XXX_MAX_SPI_LEN);
		wk2xxx_fifo_read(s, portno, one->buf + len_p, chunk);
		len_p += chunk;
		rxlen -= chunk;
	}
	rxlen = len_p;

	/* Map the FIFO status register error flags to line status. */
	if (fsr & WK2XXX_FSR_ERR_MASK) {
		if (fsr & WK2XXX_FSR_RFPE_BIT) {
			port->icount.parity++;
			lsr |= WK2XXX_LSR_PE_BIT;
			flag = TTY_PARITY;
		}
		if (fsr & WK2XXX_FSR_RFFE_BIT) {
			port->icount.frame++;
			lsr |= WK2XXX_LSR_FE_BIT;
			flag = TTY_FRAME;
		}
		if (fsr & WK2XXX_FSR_RFOE_BIT) {
			port->icount.overrun++;
			lsr |= WK2XXX_LSR_OE_BIT;
			flag = TTY_OVERRUN;
		}
		if (fsr & WK2XXX_FSR_RFBI_BIT) {
			port->icount.brk++;
			lsr |= WK2XXX_LSR_BI_BIT;
			flag = TTY_BREAK;
		}
	}

	port->icount.rx += rxlen;

	for (i = 0; i < rxlen; ++i) {
		u8 ch = one->buf[i];

		if (uart_handle_sysrq_char(port, ch))
			continue;

		if (lsr & port->ignore_status_mask)
			continue;

		uart_insert_char(port, lsr, WK2XXX_LSR_OE_BIT, ch, flag);
	}

	tty_flip_buffer_push(&port->state->port);
}

static bool wk2xxx_port_irq(struct wk2xxx_port *s, unsigned int portno)
{
	struct uart_port *port = &s->p[portno].port;
	u8 sifr, sier;
	bool rc = false;

	wk2xxx_port_reg_read(s, portno, WK2XXX_SIFR_REG, &sifr);
	wk2xxx_port_reg_read(s, portno, WK2XXX_SIER_REG, &sier);

	if (sifr & (WK2XXX_SIFR_RFTRIG_INT_BIT | WK2XXX_SIFR_RXOVT_INT_BIT)) {
		wk2xxx_handle_rx(port);
		rc = true;
	}

	if ((sifr & WK2XXX_SIFR_TFTRIG_INT_BIT) &&
	    (sier & WK2XXX_SIER_TFTRIG_IEN_BIT)) {
		wk2xxx_handle_tx(port);
		rc = true;
	}

	return rc;
}

static irqreturn_t wk2xxx_irq(int irq, void *dev_id)
{
	struct wk2xxx_port *s = dev_id;
	bool keep_polling;

	do {
		u8 gifr;
		int i;

		keep_polling = false;

		if (wk2xxx_reg_read(s, WK2XXX_GIFR_REG, &gifr))
			return IRQ_HANDLED; /* Bus error; give up this pass. */

		for (i = 0; i < s->devtype->nr_uart; ++i)
			if (gifr & BIT(i))
				keep_polling |= wk2xxx_port_irq(s, i);
	} while (keep_polling);

	return IRQ_HANDLED;
}

static void wk2xxx_poll_proc(struct kthread_work *ws)
{
	struct wk2xxx_port *s = container_of(ws, struct wk2xxx_port,
					     poll_work.work);

	/* Reuse the IRQ handler; the interrupt ID is unused here. */
	wk2xxx_irq(0, s);

	kthread_queue_delayed_work(&s->kworker, &s->poll_work,
				   msecs_to_jiffies(WK2XXX_POLL_PERIOD_MS));
}

static void wk2xxx_tx_proc(struct kthread_work *ws)
{
	struct uart_port *port = &(to_wk2xxx_one(ws, tx_work)->port);

	wk2xxx_handle_tx(port);
}

static void wk2xxx_start_tx(struct uart_port *port)
{
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);
	struct wk2xxx_one *one = to_wk2xxx_one(port, port);

	kthread_queue_work(&s->kworker, &one->tx_work);
}

static void wk2xxx_reconf_rs485(struct uart_port *port)
{
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);
	u8 rs485 = 0;

	if (port->rs485.flags & SER_RS485_ENABLED) {
		rs485 = WK2XXX_RS485_RSRS485_BIT | WK2XXX_RS485_RTSEN_BIT;
		if (port->rs485.flags & SER_RS485_RTS_AFTER_SEND)
			rs485 |= WK2XXX_RS485_RTSINV_BIT;
	}

	wk2xxx_port_reg_write(s, port->iobase, WK2XXX_RS485_REG, rs485);
}

static int wk2xxx_config_rs485(struct uart_port *port, struct ktermios *termios,
			       struct serial_rs485 *rs485)
{
	struct wk2xxx_one *one = to_wk2xxx_one(port, port);
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);

	if (rs485->flags & SER_RS485_ENABLED) {
		/*
		 * RTS is driven by hardware and its timing cannot be
		 * influenced from the driver.
		 */
		if (rs485->delay_rts_after_send)
			return -EINVAL;
	}

	one->config.flags |= WK2XXX_RECONF_RS485;
	kthread_queue_work(&s->kworker, &one->reg_work);

	return 0;
}

static void wk2xxx_reg_proc(struct kthread_work *ws)
{
	struct wk2xxx_one *one = to_wk2xxx_one(ws, reg_work);
	struct wk2xxx_port *s = dev_get_drvdata(one->port.dev);
	struct wk2xxx_one_config config;
	unsigned long irqflags;

	uart_port_lock_irqsave(&one->port, &irqflags);
	config = one->config;
	memset(&one->config, 0, sizeof(one->config));
	uart_port_unlock_irqrestore(&one->port, irqflags);

	if (config.flags & WK2XXX_RECONF_IER)
		wk2xxx_port_reg_update(s, one->port.iobase, WK2XXX_SIER_REG,
				       config.ier_mask, config.ier_val);

	if (config.flags & WK2XXX_RECONF_RS485)
		wk2xxx_reconf_rs485(&one->port);
}

static unsigned int wk2xxx_tx_empty(struct uart_port *port)
{
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);
	u8 fsr;

	wk2xxx_port_reg_read(s, port->iobase, WK2XXX_FSR_REG, &fsr);

	return (fsr & (WK2XXX_FSR_TDAT_BIT | WK2XXX_FSR_TBUSY_BIT)) ? 0 :
		TIOCSER_TEMT;
}

static unsigned int wk2xxx_get_mctrl(struct uart_port *port)
{
	/* The WK2xxx does not expose modem control lines. */
	return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void wk2xxx_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	/* The WK2xxx does not support modem control lines. */
}

static void wk2xxx_enable_ms(struct uart_port *port)
{
	/* The WK2xxx does not have modem status registers. */
}

static void wk2xxx_break_ctl(struct uart_port *port, int break_state)
{
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);

	wk2xxx_port_reg_update(s, port->iobase, WK2XXX_LCR_REG,
			       WK2XXX_LCR_BREAK_BIT,
			       break_state ? WK2XXX_LCR_BREAK_BIT : 0);
}

/*
 * Configure a sub-UART: disable interrupts and TX/RX, program the line
 * control and baud rate registers and restore the previous state.
 */
static void wk2xxx_conf_port(struct uart_port *port, u8 lcr, u8 fwcr,
			     u8 baud0, u8 baud1, u8 pres)
{
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);
	unsigned int portno = port->iobase;
	u8 sier, scr, fsr;
	int count = 200;

	scoped_guard(mutex, &s->reg_lock) {
		/* Disable all sub-UART interrupts. */
		wk2xxx_raw_port_read(s, portno, WK2XXX_SIER_REG, &sier);
		wk2xxx_raw_port_write(s, portno, WK2XXX_SIER_REG, 0);

		/* Wait for the transmitter to become idle. */
		do {
			wk2xxx_raw_port_read(s, portno, WK2XXX_FSR_REG, &fsr);
		} while ((fsr & WK2XXX_FSR_TBUSY_BIT) && count--);

		/* Disable the transmitter and receiver. */
		wk2xxx_raw_port_read(s, portno, WK2XXX_SCR_REG, &scr);
		wk2xxx_raw_port_write(s, portno, WK2XXX_SCR_REG,
				      scr & ~(WK2XXX_SCR_TXEN_BIT |
					      WK2XXX_SCR_RXEN_BIT));

		/* Program the line control register. */
		wk2xxx_raw_port_write(s, portno, WK2XXX_LCR_REG, lcr);

		/* Configure hardware flow control levels. */
		if (fwcr) {
			wk2xxx_raw_port_write(s, portno, WK2XXX_FWCR_REG, fwcr);
			wk2xxx_raw_port_write(s, portno, WK2XXX_SPAGE_REG, 1);
			wk2xxx_raw_port_write(s, portno, WK2XXX_FWTH_REG, 0xf0);
			wk2xxx_raw_port_write(s, portno, WK2XXX_FWTL_REG, 0x80);
			wk2xxx_raw_port_write(s, portno, WK2XXX_SPAGE_REG, 0);
		}

		/* Program the baud rate generator (page 1 registers). */
		wk2xxx_raw_port_write(s, portno, WK2XXX_SPAGE_REG, 1);
		wk2xxx_raw_port_write(s, portno, WK2XXX_BAUD0_REG, baud0);
		wk2xxx_raw_port_write(s, portno, WK2XXX_BAUD1_REG, baud1);
		wk2xxx_raw_port_write(s, portno, WK2XXX_PRES_REG, pres);
		wk2xxx_raw_port_write(s, portno, WK2XXX_SPAGE_REG, 0);

		/* Re-enable the transmitter and receiver. */
		wk2xxx_raw_port_write(s, portno, WK2XXX_SCR_REG,
				      scr | (WK2XXX_SCR_TXEN_BIT |
					     WK2XXX_SCR_RXEN_BIT));

		/* Restore the interrupt enable register. */
		wk2xxx_raw_port_write(s, portno, WK2XXX_SIER_REG, sier);
	}
}

static void wk2xxx_calc_divisor(unsigned long clk, unsigned int baud,
				u8 *baud0, u8 *baud1, u8 *pres)
{
	unsigned int div;

	div = clk / (baud * 16);
	if (div == 0)
		div = 1;
	div--;
	*baud0 = div & 0xff;
	*baud1 = (div >> 8) & 0xff;
	*pres = ((unsigned long long)(clk % (baud * 16)) * 100 / baud + 50) / 100;
}

static void wk2xxx_set_termios(struct uart_port *port, struct ktermios *termios,
			       const struct ktermios *old)
{
	unsigned int baud;
	unsigned long flags;
	u8 lcr = 0, fwcr = 0;
	u8 baud0, baud1, pres;

	/* The WK2xxx supports 8 data bits only. */
	termios->c_cflag &= ~CSIZE;
	termios->c_cflag |= CS8;

	/* Parity. */
	if (termios->c_cflag & PARENB) {
		lcr |= WK2XXX_LCR_PAEN_BIT;
		switch (termios->c_cflag & (PARODD | CMSPAR)) {
		case 0:
			lcr |= WK2XXX_LCR_PAM1_BIT;	/* even */
			break;
		case PARODD:
			lcr |= WK2XXX_LCR_PAM0_BIT;	/* odd */
			break;
		case CMSPAR:
			break;				/* space */
		case PARODD | CMSPAR:
			lcr |= WK2XXX_LCR_PAM1_BIT |
			       WK2XXX_LCR_PAM0_BIT;	/* mark */
			break;
		}
	}

	/* Stop bits. */
	if (termios->c_cflag & CSTOPB)
		lcr |= WK2XXX_LCR_STPL_BIT;

	/* Set read status mask. */
	port->read_status_mask = WK2XXX_LSR_OE_BIT;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= WK2XXX_LSR_PE_BIT |
					  WK2XXX_LSR_FE_BIT;
	if (termios->c_iflag & (BRKINT | PARMRK))
		port->read_status_mask |= WK2XXX_LSR_BI_BIT;

	/* Set status ignore mask. */
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNBRK)
		port->ignore_status_mask |= WK2XXX_LSR_BI_BIT;
	if (!(termios->c_cflag & CREAD))
		port->ignore_status_mask |= WK2XXX_LSR_BRK_ERROR_MASK;

	/* Configure flow control. */
	port->status &= ~(UPSTAT_AUTOCTS | UPSTAT_AUTORTS);
	if (termios->c_cflag & CRTSCTS) {
		fwcr = WK2XXX_FWCR_FWM_RTS_CTS;
		port->status |= UPSTAT_AUTOCTS | UPSTAT_AUTORTS;
	}

	/* Get the baud rate generator configuration. */
	baud = uart_get_baud_rate(port, termios, old,
				  port->uartclk / 16 / 0xffff,
				  port->uartclk / 16);

	wk2xxx_calc_divisor(port->uartclk, baud, &baud0, &baud1, &pres);
	wk2xxx_conf_port(port, lcr, fwcr, baud0, baud1, pres);

	uart_port_lock_irqsave(port, &flags);
	uart_update_timeout(port, termios->c_cflag, baud);
	uart_port_unlock_irqrestore(port, flags);
}

static int wk2xxx_startup(struct uart_port *port)
{
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);
	unsigned int portno = port->iobase;
	u8 reg;

	scoped_guard(mutex, &s->reg_lock) {
		/* Enable the sub-UART. */
		wk2xxx_raw_read(s, WK2XXX_GENA_REG, &reg);
		reg |= BIT(portno);
		wk2xxx_raw_write(s, WK2XXX_GENA_REG, reg);

		/* Reset the sub-UART. */
		wk2xxx_raw_write(s, WK2XXX_GRST_REG, BIT(portno));

		/* Enable the sub-UART interrupt in the global mask. */
		wk2xxx_raw_read(s, WK2XXX_GIER_REG, &reg);
		reg |= BIT(portno);
		wk2xxx_raw_write(s, WK2XXX_GIER_REG, reg);

		/* Enable RX FIFO trigger and RX time-out interrupts. */
		wk2xxx_raw_port_write(s, portno, WK2XXX_SIER_REG,
				      WK2XXX_SIER_RFTRIG_IEN_BIT |
				      WK2XXX_SIER_RXOUT_IEN_BIT);

		/* Enable the transmitter and receiver. */
		wk2xxx_raw_port_read(s, portno, WK2XXX_SCR_REG, &reg);
		reg |= WK2XXX_SCR_TXEN_BIT | WK2XXX_SCR_RXEN_BIT;
		wk2xxx_raw_port_write(s, portno, WK2XXX_SCR_REG, reg);

		/* Reset and configure the FIFOs. */
		wk2xxx_raw_port_write(s, portno, WK2XXX_FCR_REG, 0xff);
		wk2xxx_raw_port_write(s, portno, WK2XXX_FCR_REG, 0xfc);

		/* Set the RX/TX FIFO trigger levels. */
		wk2xxx_raw_port_write(s, portno, WK2XXX_SPAGE_REG, 1);
		wk2xxx_raw_port_write(s, portno, WK2XXX_RFTL_REG,
				      WK2XXX_RXFIFO_LEVEL);
		wk2xxx_raw_port_write(s, portno, WK2XXX_TFTL_REG,
				      WK2XXX_TXFIFO_LEVEL);
		wk2xxx_raw_port_write(s, portno, WK2XXX_SPAGE_REG, 0);
	}

	kfifo_reset(&port->state->port.xmit_fifo);

	if (s->polling)
		kthread_queue_delayed_work(&s->kworker, &s->poll_work,
					   msecs_to_jiffies(WK2XXX_POLL_PERIOD_MS));

	return 0;
}

static void wk2xxx_shutdown(struct uart_port *port)
{
	struct wk2xxx_port *s = dev_get_drvdata(port->dev);
	unsigned int portno = port->iobase;
	u8 reg;

	scoped_guard(mutex, &s->reg_lock) {
		/* Disable the sub-UART interrupt in the global mask. */
		wk2xxx_raw_read(s, WK2XXX_GIER_REG, &reg);
		reg &= ~BIT(portno);
		wk2xxx_raw_write(s, WK2XXX_GIER_REG, reg);

		/* Disable all sub-UART interrupts. */
		wk2xxx_raw_port_write(s, portno, WK2XXX_SIER_REG, 0);

		/* Reset the sub-UART. */
		wk2xxx_raw_read(s, WK2XXX_GRST_REG, &reg);
		reg |= BIT(portno);
		wk2xxx_raw_write(s, WK2XXX_GRST_REG, reg);

		/* Disable the sub-UART. */
		wk2xxx_raw_read(s, WK2XXX_GENA_REG, &reg);
		reg &= ~BIT(portno);
		wk2xxx_raw_write(s, WK2XXX_GENA_REG, reg);
	}

	if (s->polling)
		kthread_cancel_delayed_work_sync(&s->poll_work);

	kthread_flush_worker(&s->kworker);
}

static const char *wk2xxx_type(struct uart_port *port)
{
	return (port->type == PORT_WK2XXX) ? WK2XXX_NAME : NULL;
}

static int wk2xxx_request_port(struct uart_port *port)
{
	/* Do nothing. */
	return 0;
}

static void wk2xxx_null_void(struct uart_port *port)
{
	/* Do nothing. */
}

static void wk2xxx_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_WK2XXX;
}

static int wk2xxx_verify_port(struct uart_port *port, struct serial_struct *s)
{
	if ((s->type != PORT_UNKNOWN) && (s->type != PORT_WK2XXX))
		return -EINVAL;
	if (s->irq != port->irq)
		return -EINVAL;

	return 0;
}

static const struct uart_ops wk2xxx_ops = {
	.tx_empty	= wk2xxx_tx_empty,
	.set_mctrl	= wk2xxx_set_mctrl,
	.get_mctrl	= wk2xxx_get_mctrl,
	.stop_tx	= wk2xxx_stop_tx,
	.start_tx	= wk2xxx_start_tx,
	.throttle	= wk2xxx_throttle,
	.unthrottle	= wk2xxx_unthrottle,
	.stop_rx	= wk2xxx_stop_rx,
	.enable_ms	= wk2xxx_enable_ms,
	.break_ctl	= wk2xxx_break_ctl,
	.startup	= wk2xxx_startup,
	.shutdown	= wk2xxx_shutdown,
	.set_termios	= wk2xxx_set_termios,
	.type		= wk2xxx_type,
	.request_port	= wk2xxx_request_port,
	.release_port	= wk2xxx_null_void,
	.config_port	= wk2xxx_config_port,
	.verify_port	= wk2xxx_verify_port,
};

static const struct serial_rs485 wk2xxx_rs485_supported = {
	.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND |
		 SER_RS485_RTS_AFTER_SEND,
	.delay_rts_before_send = 1,
	.delay_rts_after_send = 1,	/* Not supported. */
};

static int wk2xxx_probe(struct spi_device *spi)
{
	const struct wk2xxx_devtype *devtype;
	struct device *dev = &spi->dev;
	struct wk2xxx_port *s;
	unsigned long uartclk;
	u32 clock_freq = 0;
	bool port_registered[WK2XXX_MAX_PORTS];
	u8 val;
	int i, ret;

	/* Setup SPI bus. The SPI mode follows the device tree (spi-cpha,
	 * spi-cpol); it defaults to SPI mode 0 when unspecified.
	 */
	spi->bits_per_word = 8;
	spi->max_speed_hz = spi->max_speed_hz ? : 10 * HZ_PER_MHZ;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	devtype = spi_get_device_match_data(spi);
	if (!devtype)
		return dev_err_probe(dev, -ENODEV, "Failed to match device\n");

	/* Allocate port structure. */
	s = devm_kzalloc(dev, struct_size(s, p, devtype->nr_uart), GFP_KERNEL);
	if (!s)
		return dev_err_probe(dev, -ENOMEM,
				     "Error allocating port structure\n");

	s->devtype = devtype;
	s->spi = spi;
	mutex_init(&s->reg_lock);
	dev_set_drvdata(dev, s);

	/*
	 * The WK2xxx has no identification register, so the best we can do
	 * is to check that communication is at all possible.
	 */
	ret = wk2xxx_reg_read(s, WK2XXX_GENA_REG, &val);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read GENA register\n");

	/* Crystal clock; allow an optional DT override. */
	uartclk = devtype->crystal_freq;
	if (device_property_read_u32(dev, "clock-frequency", &clock_freq) == 0)
		uartclk = clock_freq;

	/* Mark each port line and status as uninitialized. */
	for (i = 0; i < devtype->nr_uart; ++i) {
		s->p[i].port.line = WK2XXX_MAX_DEVS;
		port_registered[i] = false;
	}

	kthread_init_worker(&s->kworker);
	s->kworker_task = kthread_run(kthread_worker_fn, &s->kworker,
				      "wk2xxx");
	if (IS_ERR(s->kworker_task)) {
		ret = PTR_ERR(s->kworker_task);
		goto out_ports;
	}
	sched_set_fifo(s->kworker_task);

	for (i = 0; i < devtype->nr_uart; ++i) {
		ret = ida_alloc_max(&wk2xxx_lines, WK2XXX_MAX_DEVS - 1,
				    GFP_KERNEL);
		if (ret < 0)
			goto out_ports;

		s->p[i].port.line = ret;

		/* Initialize port data. */
		s->p[i].port.dev	= dev;
		s->p[i].port.irq	= spi->irq;
		s->p[i].port.type	= PORT_WK2XXX;
		s->p[i].port.fifosize	= WK2XXX_FIFO_SIZE;
		s->p[i].port.flags	= UPF_FIXED_TYPE | UPF_LOW_LATENCY;
		s->p[i].port.iobase	= i;
		/*
		 * Use all ones as membase so that uart_configure_port() in
		 * serial_core.c does not abort for SPI devices.
		 */
		s->p[i].port.membase	= (void __iomem *)~0;
		s->p[i].port.iotype	= UPIO_PORT;
		s->p[i].port.uartclk	= uartclk;
		s->p[i].port.rs485_config = wk2xxx_config_rs485;
		s->p[i].port.rs485_supported = wk2xxx_rs485_supported;
		s->p[i].port.ops	= &wk2xxx_ops;

		mutex_init(&s->p[i].tx_lock);

		kthread_init_work(&s->p[i].tx_work, wk2xxx_tx_proc);
		kthread_init_work(&s->p[i].reg_work, wk2xxx_reg_proc);

		ret = uart_get_rs485_mode(&s->p[i].port);
		if (ret)
			goto out_ports;

		/* Register port. */
		ret = uart_add_one_port(&wk2xxx_uart, &s->p[i].port);
		if (ret)
			goto out_ports;

		port_registered[i] = true;
	}

	if (spi->irq <= 0) {
		/* Poll the device instead of using interrupts. */
		s->polling = true;
		kthread_init_delayed_work(&s->poll_work, wk2xxx_poll_proc);
		return 0;
	}

	/*
	 * Setup interrupt. We first try to acquire the IRQ line as level IRQ.
	 * If that succeeds, we can allow sharing the interrupt as well.
	 * In case the interrupt controller doesn't support that, we fall
	 * back to a non-shared falling-edge trigger.
	 */
	ret = devm_request_threaded_irq(dev, spi->irq, NULL, wk2xxx_irq,
					IRQF_TRIGGER_LOW | IRQF_SHARED |
					IRQF_ONESHOT,
					dev_name(dev), s);
	if (!ret)
		return 0;

	ret = devm_request_threaded_irq(dev, spi->irq, NULL, wk2xxx_irq,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					dev_name(dev), s);
	if (!ret)
		return 0;

out_ports:
	for (i = 0; i < devtype->nr_uart; i++) {
		if (s->p[i].port.line < WK2XXX_MAX_DEVS)
			ida_free(&wk2xxx_lines, s->p[i].port.line);
		if (port_registered[i])
			uart_remove_one_port(&wk2xxx_uart, &s->p[i].port);
	}

	if (!IS_ERR(s->kworker_task))
		kthread_stop(s->kworker_task);

	return ret;
}

static void wk2xxx_remove(struct spi_device *spi)
{
	struct wk2xxx_port *s = dev_get_drvdata(&spi->dev);
	int i;

	for (i = 0; i < s->devtype->nr_uart; i++) {
		ida_free(&wk2xxx_lines, s->p[i].port.line);
		uart_remove_one_port(&wk2xxx_uart, &s->p[i].port);
	}

	if (s->polling)
		kthread_cancel_delayed_work_sync(&s->poll_work);

	kthread_flush_worker(&s->kworker);
	kthread_stop(s->kworker_task);
}

static const struct of_device_id wk2xxx_dt_ids[] = {
	{ .compatible = "wkmic,wk2124", .data = &wk2124_devtype },
	{ .compatible = "wkmic,wk2132", .data = &wk2132_devtype },
	{ .compatible = "wkmic,wk2168", .data = &wk2168_devtype },
	{ .compatible = "wkmic,wk2202", .data = &wk2202_devtype },
	{ .compatible = "wkmic,wk2204", .data = &wk2204_devtype },
	{ }
};
MODULE_DEVICE_TABLE(of, wk2xxx_dt_ids);

static const struct spi_device_id wk2xxx_id_table[] = {
	{ "wk2124", (kernel_ulong_t)&wk2124_devtype },
	{ "wk2132", (kernel_ulong_t)&wk2132_devtype },
	{ "wk2168", (kernel_ulong_t)&wk2168_devtype },
	{ "wk2202", (kernel_ulong_t)&wk2202_devtype },
	{ "wk2204", (kernel_ulong_t)&wk2204_devtype },
	{ }
};
MODULE_DEVICE_TABLE(spi, wk2xxx_id_table);

static struct spi_driver wk2xxx_spi_driver = {
	.driver = {
		.name		= WK2XXX_NAME,
		.of_match_table	= wk2xxx_dt_ids,
	},
	.probe		= wk2xxx_probe,
	.remove		= wk2xxx_remove,
	.id_table	= wk2xxx_id_table,
};

static int __init wk2xxx_init(void)
{
	int ret;

	ret = uart_register_driver(&wk2xxx_uart);
	if (ret)
		return ret;

	ret = spi_register_driver(&wk2xxx_spi_driver);
	if (ret)
		uart_unregister_driver(&wk2xxx_uart);

	return ret;
}
module_init(wk2xxx_init);

static void __exit wk2xxx_exit(void)
{
	spi_unregister_driver(&wk2xxx_spi_driver);
	uart_unregister_driver(&wk2xxx_uart);
}
module_exit(wk2xxx_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Xuxunwei");
MODULE_AUTHOR("B. Eschrich");
MODULE_AUTHOR("EDATEC Technology Co., Ltd. <zjzhao@edatec.cn>");
MODULE_DESCRIPTION("WK2xxx SPI UART driver");
