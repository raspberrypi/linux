/*
 * Driver for Broadcom BCM2708 SPI Controllers
 *
 * Copyright (C) 2012 Chris Boot, Martin Sperl
 *
 * This driver is inspired by:
 * spi-ath79.c, Copyright (C) 2009-2011 Gabor Juhos <juhosg@openwrt.org>
 * spi-atmel.c, Copyright (C) 2006 Atmel Corporation
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/log2.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/dma-mapping.h>
#include <mach/dma.h>

/* module arguments to select the type of processing we do */
#include <linux/moduleparam.h>
static short processmode=1;
module_param(processmode,short,0);
MODULE_PARM_DESC(processmode,"Processing mode: 0=polling, 1=interrupt driven, 2=dma");

/* SPI register offsets */
#define SPI_CS			0x00
#define SPI_FIFO		0x04
#define SPI_CLK			0x08
#define SPI_DLEN		0x0c
#define SPI_LTOH		0x10
#define SPI_DC			0x14

/* Bitfields in CS */
#define SPI_CS_LEN_LONG		0x02000000
#define SPI_CS_DMA_LEN		0x01000000
#define SPI_CS_CSPOL2		0x00800000
#define SPI_CS_CSPOL1		0x00400000
#define SPI_CS_CSPOL0		0x00200000
#define SPI_CS_RXF		0x00100000
#define SPI_CS_RXR		0x00080000
#define SPI_CS_TXD		0x00040000
#define SPI_CS_RXD		0x00020000
#define SPI_CS_DONE		0x00010000
#define SPI_CS_LEN		0x00002000
#define SPI_CS_REN		0x00001000
#define SPI_CS_ADCS		0x00000800
#define SPI_CS_INTR		0x00000400
#define SPI_CS_INTD		0x00000200
#define SPI_CS_DMAEN		0x00000100
#define SPI_CS_TA		0x00000080
#define SPI_CS_CSPOL		0x00000040
#define SPI_CS_CLEAR_RX		0x00000020
#define SPI_CS_CLEAR_TX		0x00000010
#define SPI_CS_CPOL		0x00000008
#define SPI_CS_CPHA		0x00000004
#define SPI_CS_CS_10		0x00000002
#define SPI_CS_CS_01		0x00000001

#define SPI_TIMEOUT_MS	150

#define DRV_NAME	"bcm2708_spi"
 
#define FLAGS_FIRST_TRANSFER 0x01
#define FLAGS_LAST_TRANSFER  0x02

/* the defines that are missing in arch/arm/mach-bcm2708/include/mach/dma.h */
/* the Base address for DMA on the (VideoCore) bus */
#define DMA_SPI_BASE 0x7E204000 

/* some offset addresses */
#ifndef BCM2708_DMA_SADDR
#define BCM2708_DMA_SADDR 0x0C
#endif
#ifndef BCM2708_DMA_DADDR
#define BCM2708_DMA_DADDR 0x10
#endif
#ifndef BCM2708_DMA_TLEN
#define BCM2708_DMA_TLEN 0x14
#endif
/* some flags */
#ifndef BCM2708_DMA_D_IGNORE
#define BCM2708_DMA_D_IGNORE (1<<7)
#endif
#ifndef BCM2708_DMA_S_IGNORE
#define BCM2708_DMA_S_IGNORE (1<<11)
#endif

struct bcm2708_spi_dma {
        int chan;
	int irq;
	void __iomem *base;
};

struct bcm2708_spi {
	spinlock_t lock;
	void __iomem *base;
	int irq;
	struct clk *clk;
	bool stopping;
	
	struct completion done;

	/* dma buffer structures */
	struct bcm2708_dma_cb *dma_buffer;
	dma_addr_t dma_buffer_handle;
	struct bcm2708_spi_dma dma_tx; 
	struct bcm2708_spi_dma dma_rx; 
	
	/* structures from the transfer buffer needed during the transfer */
	const char* tx_buf;
	int tx_len;
	char* rx_buf;
	int rx_len;
	int cs;
	/* statistics counter */
	u64 transfers_polling;
	u64 transfers_irqdriven;
	u64 transfers_dmadriven;
};

struct bcm2708_spi_state {
	u32 cs;
	u16 cdiv;
};

/*
 * This function sets the ALT mode on the SPI pins so that we can use them with
 * the SPI hardware.
 *
 * FIXME: This is a hack. Use pinmux / pinctrl.
 */
static void bcm2708_init_pinmode(void)
{
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))
	
	int pin;
	u32 *gpio = ioremap(0x20200000, SZ_16K);
	
	/* SPI is on GPIO 7..11 */
	for (pin = 7; pin <= 11; pin++) {
		INP_GPIO(pin);		/* set mode to GPIO input first */
		SET_GPIO_ALT(pin, 0);	/* set mode to ALT 0 */
	}
	
	iounmap(gpio);
	
#undef INP_GPIO
#undef SET_GPIO_ALT
}

static inline u32 bcm2708_rd(struct bcm2708_spi *bs, unsigned reg)
{
	return readl(bs->base + reg);
}

static inline void bcm2708_wr(struct bcm2708_spi *bs, unsigned reg, u32 val)
{
	writel(val, bs->base + reg);
}

static int bcm2708_setup_state(struct spi_master *master,
			struct device *dev, struct bcm2708_spi_state *state,
			u32 hz, u8 csel, u8 mode, u8 bpw)
{
	struct bcm2708_spi *bs = spi_master_get_devdata(master);
	int cdiv;
	unsigned long bus_hz;
	u32 cs = 0;
	
	bus_hz = clk_get_rate(bs->clk);
	
	if (hz >= bus_hz) {
		cdiv = 2; /* bus_hz / 2 is as fast as we can go */
	} else if (hz) {
		cdiv = DIV_ROUND_UP(bus_hz, hz);
		
		/* CDIV must be a power of 2, so round up */
		cdiv = roundup_pow_of_two(cdiv);

		if (cdiv > 65536) {
			dev_dbg(dev,
				"setup: %d Hz too slow, cdiv %u; min %ld Hz\n",
				hz, cdiv, bus_hz / 65536);
			return -EINVAL;
		} else if (cdiv == 65536) {
			cdiv = 0;
		} else if (cdiv == 1) {
			cdiv = 2; /* 1 gets rounded down to 0; == 65536 */
		}
	} else {
		cdiv = 0;
	}

	switch (bpw) {
	case 8:
		break;
	default:
		dev_dbg(dev, "setup: invalid bits_per_word %u (must be 8)\n",
			bpw);
		return -EINVAL;
	}

	if (mode & SPI_CPOL)
		cs |= SPI_CS_CPOL;
	if (mode & SPI_CPHA)
		cs |= SPI_CS_CPHA;

	if (!(mode & SPI_NO_CS)) {
		if (mode & SPI_CS_HIGH) {
			cs |= SPI_CS_CSPOL;
			cs |= SPI_CS_CSPOL0 << csel;
		}

		cs |= csel;
	} else {
		cs |= SPI_CS_CS_10 | SPI_CS_CS_01;
	}

	if (state) {
		state->cs = cs;
		state->cdiv = cdiv;
	}

	return 0;
}

static int bcm2708_register_dma(struct platform_device *pdev,
				struct bcm2708_spi_dma * d,
				struct bcm2708_dma_cb * dmabuffer,
				const char* name) {
	int ret;
	/* register DMA channel */
	ret = bcm_dma_chan_alloc(BCM_DMA_FEATURE_FAST,
				&d->base,
				&d->irq);
	if (ret<0) {
		dev_err(&pdev->dev, "couldn't allocate a DMA channel\n");
		return ret;
	}
	d->chan=ret;
	/* and write info */
	dev_info(&pdev->dev, "DMA channel %d at address 0x%08lx with irq %d\n",
		d->chan,(unsigned long)d->base,d->irq);
	return 0;
}

static int bcm2708_release_dma(struct platform_device *pdev,
			struct bcm2708_spi_dma * d) {
	if (!d->base) return 0;
	bcm_dma_chan_free(d->chan);
	d->base=NULL;
	d->chan=0;
	d->irq=0;
	return 0;
}

static int bcm2708_register_dmabuffer(struct platform_device *pdev,
				struct bcm2708_spi * bs) {
	/* for this to work you need to have set the following:
           in the bcm2708_spi_device definition:
	   .dev = {
	   .coherent_dma_mask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
	   },
           otherwise you get the message:
	   coherent DMA mask is unset
	   and the allocation fails...
	   learned the hard way, so as a hint for all 
	   who take this as a base...
	*/
	bs->dma_buffer= dma_alloc_writecombine(&pdev->dev, 
					SZ_4K,
					&bs->dma_buffer_handle,
					GFP_KERNEL);
        if (!bs->dma_buffer) {
                dev_err(&pdev->dev, "cannot allocate DMA CBs\n");
                return -ENOMEM;
        }
	return 0;
}

static int bcm2708_release_dmabuffer(struct platform_device *pdev,
				struct bcm2708_spi * bs) {
	if (!bs->dma_buffer) return 0;
	dma_free_writecombine(&pdev->dev, SZ_4K, 
			bs->dma_buffer,
			bs->dma_buffer_handle);
	bs->dma_buffer=NULL;
	bs->dma_buffer_handle=0;
	return 0;
}

irqreturn_t bcm2708_transfer_one_message_dma_irqhandler(int irq, void* dev) {
	struct spi_master *master = dev;
	struct bcm2708_spi *bs = spi_master_get_devdata(master);
	/* mark the rx DMA-interrupt as handled 
	   - it will (level) trigger otherwise again */
	writel(BCM2708_DMA_INT, bs->dma_rx.base+BCM2708_DMA_CS);
	
	/* and wake up the thread to continue its work - returning ...*/
	complete(&bs->done);
 	/* return IRQ handled */
 	return IRQ_HANDLED;
}

/* We could improve on DMA options, by chaining individual xfer messages
   into a more complex CB chain that takes care of all the transfers in one "go"
   resulting in only one interrupt getting delivered at the end of the sequence.
   This would reduce the "gap" between transfers to virtually 0 (maybe one SPI clock lost)
   at the cost of possibly saturating the AXI bus.
   Theoretically it would be possible to chain 63 requests together using a single page
   that way we could run say 62 4KB DMA requests and by this transfer 248 KB 
   without a single CPU cycle needed - except for the final notification IRQ. 
   (assuming that the driver requesting this is doing async transfers)
   Assuming this, we could at a SPI bus speed of 15.625MHz 
   (core frequency of 250MHz with divider of 16)
   we could transfer about 1.953MB/s with just 7.8 interrupts (and Engine wakeups)
   (250MHz/16(divider)/8(bit/byte)/(62(CBs we can chain)*4(kb/CB transfer))
   But before adding this extra complexity to make this possible 
   the driver needing this needs to get written first...
   Note: that this would also mean that the SPI bus is really dedicated to this one device!!!

   The other thing that could also help was (assuming that DMA in VideoCORE
   does not have any errata - like on other arm platforms) if there was an 
   API that could map kernel addresses directly to BUS addresses independently
   from if the xfer block has been allocated in the DMA region (the allocation
   call of which returning also returns the bus address), then we could also 
   enable DMA by default on all transfers  and not only on selected ones.
   This could help doing DMA transfers directly to user space without copying
   - if there is an API allowing that...
*/

static int bcm2708_transfer_one_message_dma(struct spi_master *master,
					struct bcm2708_spi_state* stp,
					struct spi_transfer* xfer,
					int flags
	) {
	struct bcm2708_spi *bs = spi_master_get_devdata(master);
	struct bcm2708_dma_cb *cbs=bs->dma_buffer;
	u32 cs=0;
	/* calculate dma transfer sizes - words */
	int dmaleninitial=4;
	int dmalen=xfer->len;
	/* if size <=0 then return immediately and OK - nothing to do*/
	if (xfer->len<=0) {return 0; }

	/* increment type counter */
	bs->transfers_dmadriven++;

	/* check for length - one page size max !!! */
	if (xfer->len>4096) {
		dev_err(&master->dev,"Max allowed package size exceeded");
		return -EINVAL;
	}
	/* on first transfer reset the RX/TX */
	cs=stp->cs|SPI_CS_DMAEN;
	if (flags&FLAGS_FIRST_TRANSFER) {
		bcm2708_wr(bs, SPI_CS, cs | SPI_CS_CLEAR_RX | SPI_CS_CLEAR_TX);
	}
	/* auto deselect CS if it is the last */
	if (flags&FLAGS_LAST_TRANSFER) { cs|=SPI_CS_ADCS; }

	/* store data for interrupts and more */
	bs->rx_buf=xfer->rx_buf;
	bs->tx_buf=xfer->tx_buf;
	bs->rx_len=xfer->len;
	bs->tx_len=xfer->len;
	bs->cs=cs;

	/* now set up the Registers */
	bcm2708_wr(bs, SPI_CLK, stp->cdiv);
	bcm2708_wr(bs, SPI_CS, cs);

	/* start filling in the CBs */
	/* first set up the flags for the fifo 
	   - needs to be set 256 bit alligned, so abusing the first cb */
	cbs[0].info=(xfer->len<<16) /* the length in bytes to transfer */
		| ( cs&0xff ) /* the bottom 8 bit flags for the SPI interface */
		| SPI_CS_TA; /* and enable transfer */

	/* tx info - set len/flags in the first CB */
	cbs[1].info=BCM2708_DMA_PER_MAP(6) /* DREQ 6 = SPI TX in PERMAP */
		| BCM2708_DMA_D_DREQ; /* destination DREQ trigger */
	cbs[1].src=bs->dma_buffer_handle+0*sizeof(struct bcm2708_dma_cb);
	cbs[1].dst=(unsigned long)(DMA_SPI_BASE+SPI_FIFO);
	cbs[1].length=dmaleninitial;
	cbs[1].stride=0;
	cbs[1].next=bs->dma_buffer_handle+2*sizeof(struct bcm2708_dma_cb);
	/* and the tx-data in the second CB */
	cbs[2].info=cbs[1].info;
	if (xfer->tx_buf) {
		cbs[2].info|=BCM2708_DMA_S_INC; /* source increment by 4 */
		cbs[2].src=(unsigned long)xfer->tx_dma;
	} else {
		cbs[3].info|=BCM2708_DMA_S_IGNORE; /* ignore source */
		cbs[2].src=bs->dma_buffer_handle+127*sizeof(struct bcm2708_dma_cb);
	}
	cbs[2].dst=cbs[1].dst;
        cbs[2].length=dmalen;
	cbs[2].stride=0;
	cbs[2].next=(unsigned long)0;
	/* and here the RX Data */
	/* rx info - set bytes/clock */
	cbs[3].info=BCM2708_DMA_PER_MAP(7) /* DREQ 7 = SPI RX in PERMAP */
		| BCM2708_DMA_S_DREQ /* source DREQ trigger */
		| BCM2708_DMA_INT_EN; /* enable interrupt */
	if (xfer->rx_buf) {
		cbs[3].info|=BCM2708_DMA_D_INC; /* destination increment by 4 */
		cbs[3].dst=(unsigned long)xfer->rx_dma;
	} else {
		cbs[3].info|=BCM2708_DMA_D_IGNORE; /* ignore destination */
	}
	cbs[3].src=cbs[1].dst;
        cbs[3].length=xfer->len;
	cbs[3].stride=0;
	cbs[3].next=(unsigned long)0;
	/* initialize done */
	INIT_COMPLETION(bs->done);
	/* write CB to process */
	writel(
		bs->dma_buffer_handle+3*sizeof(struct bcm2708_dma_cb),
		bs->dma_rx.base+BCM2708_DMA_ADDR
		);
	writel(
		bs->dma_buffer_handle+1*sizeof(struct bcm2708_dma_cb),
		bs->dma_tx.base+BCM2708_DMA_ADDR
		);
	dsb();
	/* start DMA - this should also enable the DMA */
	writel(BCM2708_DMA_ACTIVE, bs->dma_tx.base+BCM2708_DMA_CS);
	writel(BCM2708_DMA_ACTIVE, bs->dma_rx.base+BCM2708_DMA_CS);
	
	/* now we are running - waiting to get woken by interrupt */
	/* the timeout may be too short - depend on amount of data and frequency... */
	if (wait_for_completion_timeout(
			&bs->done,
			msecs_to_jiffies(SPI_TIMEOUT_MS*10)) == 0) {
		/* clear cs */
		
		/* inform of event and return with error */
		dev_err(&master->dev, "DMA transfer timed out");
		/* need to abort Interrupts */
		bcm_dma_abort(bs->dma_tx.base);
		bcm_dma_abort(bs->dma_rx.base);
		return -ETIMEDOUT;
	}
	/* and return */
	return 0;
}

static irqreturn_t bcm2708_transfer_one_message_irqdriven_irqhandler(int irq, void *dev_id) {

	struct spi_master *master=dev_id;
	char b;
	struct bcm2708_spi *bs = spi_master_get_devdata(master);
	spin_lock(&bs->lock);
	/* if we got more data then write */
	while ((bs->tx_len>0)&&(bcm2708_rd(bs, SPI_CS)&SPI_CS_TXD)) {
		/* decide on data to send */
		if (bs->tx_buf) { b=*(bs->tx_buf);(bs->tx_buf)++; } else {b=0;}
		bcm2708_wr(bs,SPI_FIFO,b);
		/* and decrement rx_len */
		(bs->tx_len)--;
	}
	/* check for reads */
	while (bcm2708_rd(bs, SPI_CS)&SPI_CS_RXD) {
		/* getting byte from fifo */
		b=bcm2708_rd(bs,SPI_FIFO);
		/* store it if requested */
		if (bs->rx_buf) { *(bs->rx_buf)=b;(bs->rx_buf)++; }
		/* and decrement rx_len */
		(bs->rx_len)--;
	}
	spin_unlock(&bs->lock);
	
	/* and if we have rx_len as 0 then wakeup the process */
	if (bs->rx_len==0) {
		/* clean the transfers including all interrupts */
		bcm2708_wr(bs, SPI_CS,bs->cs);
		/* and wake up the thread to continue its work */
		complete(&bs->done);
	}
	
	/* return IRQ handled */
	return IRQ_HANDLED;
}

static int bcm2708_transfer_one_message_irqdriven(struct spi_master *master,
						struct bcm2708_spi_state* stp,
						struct spi_transfer* xfer,
						int flags
	) {
	volatile u32 cs;
	char b;
	struct bcm2708_spi *bs = spi_master_get_devdata(master);
	unsigned long iflags;
	/* increment type counter */
	bs->transfers_irqdriven++;
	
	/* store the data somewhere where the interrupt handler can see it */
	bs->tx_buf=xfer->tx_buf;
	bs->tx_len=xfer->len;
	bs->rx_buf=xfer->rx_buf;
	bs->rx_len=xfer->len;
	bs->cs=stp->cs;

	/* if we are not the last xfer - keep flags when done */
	if (!(flags | FLAGS_LAST_TRANSFER)) {
		bs->cs|=SPI_CS_TA|SPI_CS_INTR|SPI_CS_INTD;
	}
	
	/* set up the spinlock - do we really need to disable interrupts here?*/
	spin_lock_irqsave(&bs->lock,iflags);
	
	/* start by setting up the SPI controller */
	cs=stp->cs|SPI_CS_TA|SPI_CS_INTR|SPI_CS_INTD;
	bcm2708_wr(bs, SPI_CLK, stp->cdiv);
	bcm2708_wr(bs, SPI_CS, cs);
	
	/* fill as much of a buffer as possible */
	while ((bcm2708_rd(bs, SPI_CS)&SPI_CS_TXD)&&(bs->tx_len>0)) {
		/* store it if requested */
		if (bs->tx_buf) { b=*(bs->tx_buf);bs->tx_buf++; } else {b=0;}
		bcm2708_wr(bs,SPI_FIFO,b);
		/* and decrement rx_len */
		bs->tx_len--;
	}
	
	/* now enable the interrupts after we have initialized completion */
	INIT_COMPLETION(bs->done);
	spin_unlock_irqrestore(&bs->lock,iflags);
	
	/* and wait for last interrupt to wake us up */
	if (wait_for_completion_timeout(&bs->done,
						msecs_to_jiffies(SPI_TIMEOUT_MS)) == 0) {
		dev_err(&master->dev, "transfer timed out\n");
		return -ETIMEDOUT;
	} 
	
	/* and return */
	return 0;
}

static int bcm2708_transfer_one_message_poll(struct spi_master *master,
					struct bcm2708_spi_state* stp,
					struct spi_transfer* xfer,
					int flags
	) {
	volatile u32 cs;
	char b;
	const char* tx_buf=xfer->tx_buf;
	int tx_len=xfer->len;
	char* rx_buf=xfer->rx_buf;
	int rx_len=xfer->len;
	
	struct bcm2708_spi *bs = spi_master_get_devdata(master);

	/* increment type counter */
	bs->transfers_polling++;
	
	/* start by setting up the SPI controller */
	cs=stp->cs|SPI_CS_TA;
	bcm2708_wr(bs, SPI_CLK, stp->cdiv);
	bcm2708_wr(bs, SPI_CS, cs);
	/* loop until rxlen is 0 */
	while ((rx_len>0)) {
		cs=bcm2708_rd(bs, SPI_CS);
		if (cs&SPI_CS_TXD) {
			if (tx_len>0) {
				/* decide on data to send */
				if (tx_buf) { b=*tx_buf;tx_buf++; } else {b=0;}
				bcm2708_wr(bs,SPI_FIFO,b);
				/* and decrement rx_len */
				tx_len--;
			}
		}
		if (cs&SPI_CS_RXD) {
			/* getting byte from fifo */
			b=bcm2708_rd(bs,SPI_FIFO);
			/* store it if requested */
			if (rx_buf) { *rx_buf=b;rx_buf++; }
			/* and decrement rx_len */
			rx_len--;
		}
	}
	/* and release cs */
	bcm2708_wr(bs, SPI_CS, stp->cs);
	/* and return OK */
	return 0;
}

/* this one sends a message */
static int bcm2708_transfer_one_message(struct spi_master *master,
					struct spi_message* msg) {
	struct spi_transfer *xfer;
	struct bcm2708_spi *bs = spi_master_get_devdata(master);
	struct spi_device* spi=msg->spi;
	struct bcm2708_spi_state state;
	int status=0;
	int count=0;
	int transfers=0;
	list_for_each_entry(xfer, &msg->transfers, transfer_list) { transfers++; }
	
	/* loop all the transfer entries to check for transfer issues first */
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		int can_dma=1;
		int flags=0;
		/* increment count */
		count++;
		/* calculate flags */
		if (count==1) { 
			/* clear the queues */
			bcm2708_wr(bs, SPI_CS, bcm2708_rd(bs, SPI_CS) | SPI_CS_CLEAR_RX | SPI_CS_CLEAR_TX);
			flags|=FLAGS_FIRST_TRANSFER; 
		}
		if (count==transfers) { flags|=FLAGS_LAST_TRANSFER; }
		/* check if elegable for DMA */
		if ((xfer->tx_buf)&&(!xfer->tx_dma)) { can_dma=0; }
		if ((xfer->rx_buf)&&(!xfer->rx_dma)) { can_dma=0; }

		/* configure SPI - use global settings if not explicitly set */		
		if (xfer->bits_per_word || xfer->speed_hz) {
			status = bcm2708_setup_state(spi->master, &spi->dev, &state,
						xfer->speed_hz ? xfer->speed_hz : spi->max_speed_hz,
						spi->chip_select, spi->mode,
						xfer->bits_per_word ? xfer->bits_per_word :
						spi->bits_per_word);
		} else {
			state.cs=((struct bcm2708_spi_state*)spi->controller_state)->cs;
			state.cdiv=((struct bcm2708_spi_state*)spi->controller_state)->cdiv;
		}
		if (status)
			goto exit;
		/* keep Transfer active until we are triggering the last one */
		if (!(flags&FLAGS_LAST_TRANSFER)) { state.cs|= SPI_CS_TA; }
		/* now send the message over SPI */
		switch (processmode) {
		case 0: /* polling */
			status=bcm2708_transfer_one_message_poll(
				master,&state,xfer,flags);
			break;
		case 1: /* interrupt driven */
			status=bcm2708_transfer_one_message_irqdriven(
				master,&state,xfer,flags);
			break;
		case 2: /* dma driven */
			if (can_dma) {
				status=bcm2708_transfer_one_message_dma(
					master,&state,xfer,flags
					);
				break;
			} else {
				status=bcm2708_transfer_one_message_irqdriven(
					master,&state,xfer,flags
					);
				break;
			}
		default:
			/* by default use the interrupt version */
			status=bcm2708_transfer_one_message_irqdriven(
				master,&state,xfer,flags);
			break;
		}
		if (status)
			goto exit;
		/* delay if given */
	        if (xfer->delay_usecs)
        	        udelay(xfer->delay_usecs);
		/* and add up the result */
		msg->actual_length += xfer->len;
	}
exit:
	msg->status = status;
	spi_finalize_current_message(master);
	return status;
}

static int bcm2708_prepare_transfer(struct spi_master *master) {
	return 0;
}

static int bcm2708_unprepare_transfer(struct spi_master *master) {
	return 0;
}

static int bcm2708_spi_setup(struct spi_device *spi)
{
	struct bcm2708_spi *bs = spi_master_get_devdata(spi->master);
	struct bcm2708_spi_state *state;
	int ret;
	
	// configure master 
	
	if (bs->stopping)
		return -ESHUTDOWN;
	
	if (!(spi->mode & SPI_NO_CS) &&
		(spi->chip_select > spi->master->num_chipselect)) {
		dev_dbg(&spi->dev,
			"setup: invalid chipselect %u (%u defined)\n",
			spi->chip_select, spi->master->num_chipselect);
		return -EINVAL;
	}
	
	state = spi->controller_state;
	if (!state) {
		state = kzalloc(sizeof(*state), GFP_KERNEL);
		if (!state)
			return -ENOMEM;
		
		spi->controller_state = state;
	}
	
	ret = bcm2708_setup_state(spi->master, &spi->dev, state,
				spi->max_speed_hz, spi->chip_select, spi->mode,
				spi->bits_per_word);
	if (ret < 0) {
		kfree(state);
		spi->controller_state = NULL;
	}
	
	dev_dbg(&spi->dev,
		"setup: cd %d: %d Hz, bpw %u, mode 0x%x -> CS=%08x CDIV=%04x\n",
		spi->chip_select, spi->max_speed_hz, spi->bits_per_word,
		spi->mode, state->cs, state->cdiv);
	
	return 0;
}

static void bcm2708_spi_cleanup(struct spi_device *spi)
{
	if (spi->controller_state) {
		kfree(spi->controller_state);
		spi->controller_state = NULL;
	}
}

static int __devinit bcm2708_spi_probe(struct platform_device *pdev)
{
	struct resource *regs;
	int irq, err = -ENOMEM;
	struct clk *clk;
	struct spi_master *master;
	struct bcm2708_spi *bs;
	const char* mode;
	
	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!regs) {
		dev_err(&pdev->dev, "could not get IO memory\n");
		return -ENXIO;
	}
	
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "could not get IRQ\n");
		return irq;
	}
	
	clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "could not find clk: %ld\n", PTR_ERR(clk));
		return PTR_ERR(clk);
	}
	
	bcm2708_init_pinmode();
	
	master = spi_alloc_master(&pdev->dev, sizeof(*bs));
	if (!master) {
		dev_err(&pdev->dev, "spi_alloc_master() failed\n");
		goto out_clk_put;
	}
	
	/* the spi->mode bits understood by this driver: */
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH | SPI_NO_CS;
	
	master->bus_num = pdev->id;
	master->num_chipselect = 3;
	master->setup = bcm2708_spi_setup;
	master->cleanup = bcm2708_spi_cleanup;
	master->rt =1;

	master->prepare_transfer_hardware       = bcm2708_prepare_transfer;
	master->transfer_one_message            = bcm2708_transfer_one_message;
	master->unprepare_transfer_hardware     = bcm2708_unprepare_transfer;

	platform_set_drvdata(pdev, master);


	bs = spi_master_get_devdata(master);
	spin_lock_init(&bs->lock);
	init_completion(&bs->done);

	/* set counters */
	bs->transfers_polling=0;
	bs->transfers_irqdriven=0;
	bs->transfers_dmadriven=0;
	
	/* get Register Map */
	bs->base = ioremap(regs->start, resource_size(regs));
	if (!bs->base) {
		dev_err(&pdev->dev, "could not remap memory\n");
		goto out_master_put;
	}

	bs->irq = irq;
	bs->clk = clk;
	bs->stopping = false;

	err = request_irq(irq, 
			bcm2708_transfer_one_message_irqdriven_irqhandler,
			0,
			dev_name(&pdev->dev),
			master);
	if (err) {
		dev_err(&pdev->dev, "could not request IRQ: %d\n", err);
		goto out_iounmap;
	}

	/* enable DMA */
	/* register memory buffer for DMA */
	if ((err=bcm2708_register_dmabuffer(pdev,bs))) 
		goto out_free_irq;
	/* register channels and irq */
	if ((err=bcm2708_register_dma(pdev,
						&bs->dma_rx,
						bs->dma_buffer,
						DRV_NAME "(rxDMA)"
				)))
		goto out_free_dma_buffer;
	if ((err=bcm2708_register_dma(pdev,
						&bs->dma_tx,
						bs->dma_buffer,
						DRV_NAME "(txDMA)"
				)))
		goto out_free_dma_rx;
	/* register IRQ for RX dma channel  */
	err = request_irq(bs->dma_rx.irq,
			bcm2708_transfer_one_message_dma_irqhandler, 
			0, 
			dev_name(&pdev->dev),
			master);
	if (err) {
		dev_err(&pdev->dev, "could not request IRQ: %d\n", err);
		goto out_free_dma_tx;
	}
	
	
	/* initialise the hardware */
	clk_enable(clk);
	bcm2708_wr(bs, SPI_CS, SPI_CS_REN | SPI_CS_CLEAR_RX | SPI_CS_CLEAR_TX);
	
	err = spi_register_master(master);
	if (err) {
		dev_err(&pdev->dev, "could not register SPI master: %d\n", err);
		goto out_free_dma_irq;
	}
	
	dev_info(&pdev->dev, "SPI Controller at 0x%08lx (irq %d)\n",
		(unsigned long)regs->start, irq);
	
	/* now send the message over SPI */
	switch (processmode) {
	case 0:  mode="polling"; break;
	case 1:  mode="interrupt-driven"; break;
	case 2:  mode="dma"; break;
	default: /* for unsupported modes return with errors */
		dev_err(&pdev->dev, "Unsupported processmode %i\n",
			processmode);
		goto out_free_dma_irq;
		break;
	}
	dev_info(&pdev->dev, "SPI Controller running in %s mode\n",mode);
	return 0;
out_free_dma_irq:
	free_irq(bs->dma_rx.irq, master);
out_free_dma_tx:
	bcm2708_release_dma(pdev,&bs->dma_tx);
out_free_dma_rx:
	bcm2708_release_dma(pdev,&bs->dma_rx);
out_free_dma_buffer:
	bcm2708_release_dmabuffer(pdev,bs);
out_free_irq:
	free_irq(bs->irq, master);
out_iounmap:
	iounmap(bs->base);
out_master_put:
	spi_master_put(master);
out_clk_put:
	clk_put(clk);
	return err;
}

static int __devexit bcm2708_spi_remove(struct platform_device *pdev)
{
	struct spi_master *master = platform_get_drvdata(pdev);
	struct bcm2708_spi *bs = spi_master_get_devdata(master);

	/* first report on usage */
	dev_info(&pdev->dev,"SPI Bus statistics: %llu poll %llu interrupt and %llu dma driven messages\n",
		bs->transfers_polling,
		bs->transfers_irqdriven,
		bs->transfers_dmadriven
		); 

	/* reset the hardware and block queue progress */
	bs->stopping = true;
	bcm2708_wr(bs, SPI_CS, SPI_CS_CLEAR_RX | SPI_CS_CLEAR_TX);

	clk_disable(bs->clk);
	clk_put(bs->clk);
	free_irq(bs->irq, master);
	iounmap(bs->base);

	/* release DMA */
	free_irq(bs->dma_rx.irq, master);
	bcm2708_release_dma(pdev,&bs->dma_tx);
	bcm2708_release_dma(pdev,&bs->dma_rx);
	bcm2708_release_dmabuffer(pdev,bs);

	/* and unregister device */
	spi_unregister_master(master);

	return 0;
}

static struct platform_driver bcm2708_spi_driver = {
	.driver		= {
		.name	= DRV_NAME,
		.owner	= THIS_MODULE,
	},
	.probe		= bcm2708_spi_probe,
	.remove		= __devexit_p(bcm2708_spi_remove),
};


static int __init bcm2708_spi_init(void)
{
        /* range check for processmode */
        if ((processmode<0) || (processmode>3)) { 
	        processmode=1; 
	}
	return platform_driver_probe(&bcm2708_spi_driver, bcm2708_spi_probe);
}
module_init(bcm2708_spi_init);

static void __exit bcm2708_spi_exit(void)
{
	platform_driver_unregister(&bcm2708_spi_driver);
}
module_exit(bcm2708_spi_exit);

//module_platform_driver(bcm2708_spi_driver);

MODULE_DESCRIPTION("SPI controller driver for Broadcom BCM2708");
MODULE_AUTHOR("Chris Boot <bootc@bootc.net>, Martin Sperl");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRV_NAME);
