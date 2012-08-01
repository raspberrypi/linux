/*
 * sdhci-bcm2708.c Support for SDHCI device on BCM2708
 * Copyright (c) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* Supports:
 * SDHCI platform device - Arasan SD controller in BCM2708
 *
 * Inspired by sdhci-pci.c, by Pierre Ossman
 */

#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sd.h>

#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <mach/dma.h>

#include "sdhci.h"

/*****************************************************************************\
 *									     *
 * Configuration							     *
 *									     *
\*****************************************************************************/

#define DRIVER_NAME "bcm2708_sdhci"

/* for the time being insist on DMA mode - PIO seems not to work */
#ifndef CONFIG_MMC_SDHCI_BCM2708_DMA
#warning Non-DMA (PIO) version of this driver currently unavailable
#endif
#undef CONFIG_MMC_SDHCI_BCM2708_DMA
#define CONFIG_MMC_SDHCI_BCM2708_DMA y

#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
/* #define CHECK_DMA_USE */
#endif
//#define LOG_REGISTERS

#define USE_SCHED_TIME
#define USE_SPACED_WRITES_2CLK 1  /* space consecutive register writes */
#define USE_SOFTWARE_TIMEOUTS 1   /* not hardware timeouts */
#define SOFTWARE_ERASE_TIMEOUT_SEC 30

#define SDHCI_BCM_DMA_CHAN 4   /* this default is normally overriden */
#define SDHCI_BCM_DMA_WAITS 0  /* delays slowing DMA transfers: 0-31 */
/* We are worried that SD card DMA use may be blocking the AXI bus for others */

/*! TODO: obtain these from the physical address */
#define DMA_SDHCI_BASE	 0x7e300000  /* EMMC register block on Videocore */
#define DMA_SDHCI_BUFFER (DMA_SDHCI_BASE + SDHCI_BUFFER)

#define BCM2708_SDHCI_SLEEP_TIMEOUT 1000   /* msecs */

/* Mhz clock that the EMMC core is running at. Should match the platform clockman settings */
#define BCM2708_EMMC_CLOCK_FREQ 50000000

#define REG_EXRDFIFO_EN     0x80
#define REG_EXRDFIFO_CFG    0x84

/*****************************************************************************\
 *									     *
 * Debug								     *
 *									     *
\*****************************************************************************/



#define DBG(f, x...) \
	pr_debug(DRIVER_NAME " [%s()]: " f, __func__,## x)
//	printk(KERN_INFO DRIVER_NAME " [%s()]: " f, __func__,## x)//GRAYG


/*****************************************************************************\
 *									     *
 * High Precision Time							     *
 *									     *
\*****************************************************************************/

#ifdef USE_SCHED_TIME

#include <mach/frc.h>

typedef unsigned long hptime_t;

#define FMT_HPT "lu"

static inline hptime_t hptime(void)
{
	return frc_clock_ticks32();
}

#define HPTIME_CLK_NS 1000ul

#else

typedef unsigned long hptime_t;

#define FMT_HPT "lu"

static inline hptime_t hptime(void)
{
	return jiffies;
}

#define HPTIME_CLK_NS (1000000000ul/HZ)

#endif

static inline unsigned long int since_ns(hptime_t t)
{
	return (unsigned long)((hptime() - t) * HPTIME_CLK_NS);
}

static bool allow_highspeed = 1;
static int emmc_clock_freq = BCM2708_EMMC_CLOCK_FREQ;
static bool sync_after_dma = 1;
static bool missing_status = 1;

#if 0
static void hptime_test(void)
{
	hptime_t now;
	hptime_t later;

	now = hptime();
	msleep(10);
	later = hptime();

	printk(KERN_INFO DRIVER_NAME": 10ms = %"FMT_HPT" clks "
	       "(from %"FMT_HPT" to %"FMT_HPT") = %luns\n",
	       later-now, now, later,
	       (unsigned long)(HPTIME_CLK_NS * (later - now)));

	now = hptime();
	msleep(1000);
	later = hptime();

	printk(KERN_INFO DRIVER_NAME": 1s = %"FMT_HPT" clks "
	       "(from %"FMT_HPT" to %"FMT_HPT") = %luns\n",
	       later-now, now, later,
	       (unsigned long)(HPTIME_CLK_NS * (later - now)));
}
#endif

/*****************************************************************************\
 *									     *
 * SDHCI core callbacks							     *
 *									     *
\*****************************************************************************/


#ifdef CHECK_DMA_USE
/*#define CHECK_DMA_REG_USE*/
#endif

#ifdef CHECK_DMA_REG_USE
/* we don't expect anything to be using these registers during a
   DMA (except the IRQ status) - so check */
static void check_dma_reg_use(struct sdhci_host *host, int reg);
#else
#define check_dma_reg_use(host, reg)
#endif


static inline u32 sdhci_bcm2708_raw_readl(struct sdhci_host *host, int reg)
{
	return readl(host->ioaddr + reg);
}

u32 sdhci_bcm2708_readl(struct sdhci_host *host, int reg)
{
	u32 l = sdhci_bcm2708_raw_readl(host, reg);

#ifdef LOG_REGISTERS
	printk(KERN_ERR "%s: readl from 0x%02x, value 0x%08x\n",
	       mmc_hostname(host->mmc), reg, l);
#endif
	check_dma_reg_use(host, reg);

	return l;
}

u16 sdhci_bcm2708_readw(struct sdhci_host *host, int reg)
{
	u32 l = sdhci_bcm2708_raw_readl(host, reg & ~3);
	u32 w = l >> (reg << 3 & 0x18) & 0xffff;

#ifdef LOG_REGISTERS
	printk(KERN_ERR "%s: readw from 0x%02x, value 0x%04x\n",
	       mmc_hostname(host->mmc), reg, w);
#endif
	check_dma_reg_use(host, reg);

	return (u16)w;
}

u8 sdhci_bcm2708_readb(struct sdhci_host *host, int reg)
{
	u32 l = sdhci_bcm2708_raw_readl(host, reg & ~3);
	u32 b = l >> (reg << 3 & 0x18) & 0xff;

#ifdef LOG_REGISTERS
	printk(KERN_ERR "%s: readb from 0x%02x, value 0x%02x\n",
	       mmc_hostname(host->mmc), reg, b);
#endif
	check_dma_reg_use(host, reg);

	return (u8)b;
}


static void sdhci_bcm2708_raw_writel(struct sdhci_host *host, u32 val, int reg)
{
	u32 ier;

#if USE_SPACED_WRITES_2CLK
	static bool timeout_disabled = false;
	unsigned int ns_2clk = 0;
        
	/* The Arasan has a bugette whereby it may lose the content of
	 * successive writes to registers that are within two SD-card clock
	 * cycles of each other (a clock domain crossing problem).
	 * It seems, however, that the data register does not have this problem.
	 * (Which is just as well - otherwise we'd have to nobble the DMA engine
	 * too)
	 */
	if (reg != SDHCI_BUFFER && host->clock != 0) {
		/* host->clock is the clock freq in Hz */
		static hptime_t last_write_hpt;
		hptime_t now = hptime();
		ns_2clk = 2000000000/host->clock;

		if (now == last_write_hpt || now == last_write_hpt+1) {
			 /* we can't guarantee any significant time has
			  * passed - we'll have to wait anyway ! */
			ndelay(ns_2clk);
		} else
		{
			/* we must have waited at least this many ns: */
			unsigned int ns_wait = HPTIME_CLK_NS *
					       (last_write_hpt - now - 1);
			if (ns_wait < ns_2clk)
				ndelay(ns_2clk - ns_wait);
		}
		last_write_hpt = now;
	}
#if USE_SOFTWARE_TIMEOUTS
	/* The Arasan is clocked for timeouts using the SD clock which is too
	 * fast for ERASE commands and causes issues. So we disable timeouts
	 * for ERASE */
	if (host->cmd != NULL && host->cmd->opcode == MMC_ERASE &&
            reg == (SDHCI_COMMAND & ~3)) {
		mod_timer(&host->timer,
                          jiffies + SOFTWARE_ERASE_TIMEOUT_SEC * HZ);
		ier = readl(host->ioaddr + SDHCI_SIGNAL_ENABLE);
		ier &= ~SDHCI_INT_DATA_TIMEOUT;
		writel(ier, host->ioaddr + SDHCI_SIGNAL_ENABLE);
		timeout_disabled = true;
		ndelay(ns_2clk);
	} else if (timeout_disabled) {
		ier = readl(host->ioaddr + SDHCI_SIGNAL_ENABLE);
		ier |= SDHCI_INT_DATA_TIMEOUT;
		writel(ier, host->ioaddr + SDHCI_SIGNAL_ENABLE);
		timeout_disabled = false;
		ndelay(ns_2clk);
	}
#endif
	writel(val, host->ioaddr + reg);
#else
	void __iomem * regaddr = host->ioaddr + reg;

	writel(val, regaddr);

	if (reg != SDHCI_BUFFER && reg != SDHCI_INT_STATUS && host->clock != 0)
	{
		int timeout = 100000;
		while (val != readl(regaddr) && --timeout > 0)
		   continue;

		if (timeout <= 0)
			printk(KERN_ERR "%s: writing 0x%X to reg 0x%X "
			       "always gives 0x%X\n",
			       mmc_hostname(host->mmc),
			       val, reg, readl(regaddr));
		BUG_ON(timeout <= 0);
	}
#endif
}


void sdhci_bcm2708_writel(struct sdhci_host *host, u32 val, int reg)
{
#ifdef LOG_REGISTERS
	printk(KERN_ERR "%s: writel to 0x%02x, value 0x%08x\n",
	       mmc_hostname(host->mmc), reg, val);
#endif
	check_dma_reg_use(host, reg);

	sdhci_bcm2708_raw_writel(host, val, reg);
}

void sdhci_bcm2708_writew(struct sdhci_host *host, u16 val, int reg)
{
	static u32 shadow = 0;

	u32 p = reg == SDHCI_COMMAND ? shadow :
		       sdhci_bcm2708_raw_readl(host, reg & ~3);
	u32 s = reg << 3 & 0x18;
	u32 l = val << s;
	u32 m = 0xffff << s;

#ifdef LOG_REGISTERS
	printk(KERN_ERR "%s: writew to 0x%02x, value 0x%04x\n",
	       mmc_hostname(host->mmc), reg, val);
#endif

	if (reg == SDHCI_TRANSFER_MODE)
		shadow = (p & ~m) | l;
	else {
		check_dma_reg_use(host, reg);
		sdhci_bcm2708_raw_writel(host, (p & ~m) | l, reg & ~3);
	}
}

void sdhci_bcm2708_writeb(struct sdhci_host *host, u8 val, int reg)
{
	u32 p = sdhci_bcm2708_raw_readl(host, reg & ~3);
	u32 s = reg << 3 & 0x18;
	u32 l = val << s;
	u32 m = 0xff << s;

#ifdef LOG_REGISTERS
	printk(KERN_ERR "%s: writeb to 0x%02x, value 0x%02x\n",
	       mmc_hostname(host->mmc), reg, val);
#endif

       check_dma_reg_use(host, reg);
       sdhci_bcm2708_raw_writel(host, (p & ~m) | l, reg & ~3);
}

static unsigned int sdhci_bcm2708_get_max_clock(struct sdhci_host *host)
{
	return emmc_clock_freq;
}

/*****************************************************************************\
 *									     *
 * DMA Operation							     *
 *									     *
\*****************************************************************************/

struct sdhci_bcm2708_priv {
	int			dma_chan;
	int			dma_irq;
	void __iomem	       *dma_chan_base;
	struct bcm2708_dma_cb  *cb_base;   /* DMA control blocks */
	dma_addr_t		cb_handle;
	/* tracking scatter gather progress */
	unsigned		sg_ix;	   /* scatter gather list index */
	unsigned		sg_done;   /* bytes in current sg_ix done */
#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
	unsigned char		dma_wanted;  /* DMA transfer requested */
	unsigned char		dma_waits;   /* wait states in DMAs */
#ifdef CHECK_DMA_USE
	unsigned char		dmas_pending; /* no of unfinished DMAs */
	hptime_t		when_started;
	hptime_t		when_reset;
	hptime_t		when_stopped;
#endif
#endif
	/* signalling the end of a transfer */
	void		      (*complete)(struct sdhci_host *);
};

#define SDHCI_HOST_PRIV(host) \
	(struct sdhci_bcm2708_priv *)((struct sdhci_host *)(host)+1)



#ifdef CHECK_DMA_REG_USE
static void check_dma_reg_use(struct sdhci_host *host, int reg)
{
	struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);
	if (host_priv->dma_wanted && reg != SDHCI_INT_STATUS) {
		printk(KERN_INFO"%s: accessing register 0x%x during DMA\n",
		       mmc_hostname(host->mmc), reg);
	}
}
#endif



#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA

static void sdhci_clear_set_irqgen(struct sdhci_host *host, u32 clear, u32 set)
{
	u32 ier;

	ier = sdhci_bcm2708_raw_readl(host, SDHCI_SIGNAL_ENABLE);
	ier &= ~clear;
	ier |= set;
	/* change which requests generate IRQs - makes no difference to
	   the content of SDHCI_INT_STATUS, or the need to acknowledge IRQs */
	sdhci_bcm2708_raw_writel(host, ier, SDHCI_SIGNAL_ENABLE);
}

static void sdhci_signal_irqs(struct sdhci_host *host, u32 irqs)
{
	sdhci_clear_set_irqgen(host, 0, irqs);
}

static void sdhci_unsignal_irqs(struct sdhci_host *host, u32 irqs)
{
	sdhci_clear_set_irqgen(host, irqs, 0);
}



static void schci_bcm2708_cb_read(struct sdhci_bcm2708_priv *host,
				  int ix,
				  dma_addr_t dma_addr, unsigned len,
				  int /*bool*/ is_last)
{
	struct bcm2708_dma_cb *cb = &host->cb_base[ix];
        unsigned char dmawaits = host->dma_waits;

	cb->info   = BCM2708_DMA_PER_MAP(BCM2708_DMA_DREQ_EMMC) |
		     BCM2708_DMA_WAITS(dmawaits) |
		     BCM2708_DMA_S_DREQ	 |
		     BCM2708_DMA_D_WIDTH |
		     BCM2708_DMA_D_INC;
	cb->src	   = DMA_SDHCI_BUFFER;	/* DATA register DMA address */
	cb->dst	   = dma_addr;
	cb->length = len;
	cb->stride = 0;

	if (is_last) {
		cb->info |= BCM2708_DMA_INT_EN |
		     BCM2708_DMA_WAIT_RESP;
		cb->next = 0;
	} else
		cb->next = host->cb_handle +
			   (ix+1)*sizeof(struct bcm2708_dma_cb);

	cb->pad[0] = 0;
	cb->pad[1] = 0;
}

static void schci_bcm2708_cb_write(struct sdhci_bcm2708_priv *host,
				   int ix,
				   dma_addr_t dma_addr, unsigned len,
				   int /*bool*/ is_last)
{
	struct bcm2708_dma_cb *cb = &host->cb_base[ix];
        unsigned char dmawaits = host->dma_waits;

	/* We can make arbitrarily large writes as long as we specify DREQ to
	   pace the delivery of bytes to the Arasan hardware */
	cb->info   = BCM2708_DMA_PER_MAP(BCM2708_DMA_DREQ_EMMC) |
		     BCM2708_DMA_WAITS(dmawaits) |
		     BCM2708_DMA_D_DREQ	 |
		     BCM2708_DMA_S_WIDTH |
		     BCM2708_DMA_S_INC;
	cb->src	   = dma_addr;
	cb->dst	   = DMA_SDHCI_BUFFER;	/* DATA register DMA address */
	cb->length = len;
	cb->stride = 0;

	if (is_last) {
		cb->info |= BCM2708_DMA_INT_EN |
		     BCM2708_DMA_WAIT_RESP;
		cb->next = 0;
	} else
		cb->next = host->cb_handle +
			   (ix+1)*sizeof(struct bcm2708_dma_cb);

	cb->pad[0] = 0;
	cb->pad[1] = 0;
}


static void schci_bcm2708_dma_go(struct sdhci_host *host)
{
	struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);
	void __iomem *dma_chan_base = host_priv->dma_chan_base;

	BUG_ON(host_priv->dma_wanted);
#ifdef CHECK_DMA_USE
	if (host_priv->dma_wanted)
		printk(KERN_ERR "%s: DMA already in progress - "
		       "now %"FMT_HPT", last started %lu "
		       "reset %lu stopped %lu\n",
		       mmc_hostname(host->mmc),
		       hptime(), since_ns(host_priv->when_started),
		       since_ns(host_priv->when_reset),
		       since_ns(host_priv->when_stopped));
	else if (host_priv->dmas_pending > 0)
		printk(KERN_INFO "%s: note - new DMA when %d reset DMAs "
		       "already in progress - "
		       "now %"FMT_HPT", started %lu reset %lu stopped %lu\n",
		       mmc_hostname(host->mmc),
		       host_priv->dmas_pending,
		       hptime(), since_ns(host_priv->when_started),
		       since_ns(host_priv->when_reset),
		       since_ns(host_priv->when_stopped));
	host_priv->dmas_pending += 1;
	host_priv->when_started = hptime();
#endif
	host_priv->dma_wanted = 1;
	DBG("PDMA go - base %p handle %08X\n", dma_chan_base,
	    host_priv->cb_handle);
	bcm_dma_start(dma_chan_base, host_priv->cb_handle);
}


static void
sdhci_platdma_read(struct sdhci_host *host, dma_addr_t dma_addr, size_t len)
{
	struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);

	DBG("PDMA to read %d bytes\n", len);
	host_priv->sg_done += len;
	schci_bcm2708_cb_read(host_priv, 0, dma_addr, len, 1/*TRUE*/);
	schci_bcm2708_dma_go(host);
}


static void
sdhci_platdma_write(struct sdhci_host *host, dma_addr_t dma_addr, size_t len)
{
	struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);

	DBG("PDMA to write %d bytes\n", len);
	//BUG_ON(0 != (len & 0x1ff));

	host_priv->sg_done += len;
	schci_bcm2708_cb_write(host_priv, 0, dma_addr, len, 1/*TRUE*/);
	schci_bcm2708_dma_go(host);
}

/*! space is avaiable to receive into or data is available to write
  Platform DMA exported function
*/
void
sdhci_bcm2708_platdma_avail(struct sdhci_host *host, unsigned int *ref_intmask,
			    void(*completion_callback)(struct sdhci_host *host))
{
	struct mmc_data *data = host->data;
	struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);
	int sg_ix;
	size_t bytes;
	dma_addr_t addr;

	BUG_ON(NULL == data);
	BUG_ON(0 == data->blksz);

	host_priv->complete = completion_callback;

	sg_ix = host_priv->sg_ix;
	BUG_ON(sg_ix >= data->sg_len);

	/* we can DMA blocks larger than blksz - it may hang the DMA
	   channel but we are its only user */
	bytes = sg_dma_len(&data->sg[sg_ix]) - host_priv->sg_done;
	addr = sg_dma_address(&data->sg[sg_ix]) + host_priv->sg_done;

	if (bytes > 0) {
		/* We're going to poll for read/write available state until
		   we finish this DMA
		*/

		if (data->flags & MMC_DATA_READ) {
			if (*ref_intmask & SDHCI_INT_DATA_AVAIL)  {
				sdhci_unsignal_irqs(host, SDHCI_INT_DATA_AVAIL |
						    SDHCI_INT_SPACE_AVAIL);
				sdhci_platdma_read(host, addr, bytes);
			}
		} else {
			if (*ref_intmask & SDHCI_INT_SPACE_AVAIL) {
				sdhci_unsignal_irqs(host, SDHCI_INT_DATA_AVAIL |
						    SDHCI_INT_SPACE_AVAIL);
				sdhci_platdma_write(host, addr, bytes);
			}
		}
	}
	/* else:
	   we have run out of bytes that need transferring (e.g. we may be in
	   the middle of the last DMA transfer), or
	   it is also possible that we've been called when another IRQ is
	   signalled, even though we've turned off signalling of our own IRQ */

	*ref_intmask &= ~SDHCI_INT_DATA_END;
	/* don't let the main sdhci driver act on this .. we'll deal with it
	   when we respond to the DMA - if one is currently in progress */
}

/* is it possible to DMA the given mmc_data structure?
   Platform DMA exported function
*/
int /*bool*/
sdhci_bcm2708_platdma_dmaable(struct sdhci_host *host, struct mmc_data *data)
{
	struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);
	int ok = bcm_sg_suitable_for_dma(data->sg, data->sg_len);

	if (!ok)
		DBG("Reverting to PIO - bad cache alignment\n");

	else {
		host_priv->sg_ix = 0;	 /* first SG index */
		host_priv->sg_done = 0;	 /* no bytes done */
	}

	return ok;
}

#include <mach/arm_control.h> //GRAYG
/*! the current SD transacton has been abandonned
  We need to tidy up if we were in the middle of a DMA
  Platform DMA exported function
*/
void
sdhci_bcm2708_platdma_reset(struct sdhci_host *host, struct mmc_data *data)
{
	struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);
	unsigned long flags;

	BUG_ON(NULL == host);

	spin_lock_irqsave(&host->lock, flags);

	if (host_priv->dma_wanted) {
		if (NULL == data) {
			printk(KERN_ERR "%s: ongoing DMA reset - no data!\n",
			       mmc_hostname(host->mmc));
			BUG_ON(NULL == data);
		} else {
			struct scatterlist *sg;
			int sg_len;
			int sg_todo;
			int rc;
			unsigned long cs;

			sg = data->sg;
			sg_len = data->sg_len;
			sg_todo = sg_dma_len(&sg[host_priv->sg_ix]);

			cs = readl(host_priv->dma_chan_base + BCM2708_DMA_CS);

			if (!(BCM2708_DMA_ACTIVE & cs))
				printk(KERN_INFO "%s: missed completion of "
				       "cmd %d DMA (%d/%d [%d]/[%d]) - "
				       "ignoring it\n",
				       mmc_hostname(host->mmc),
				       host->last_cmdop,
				       host_priv->sg_done, sg_todo,
				       host_priv->sg_ix+1, sg_len);
			else
				printk(KERN_INFO "%s: resetting ongoing cmd %d"
				       "DMA before %d/%d [%d]/[%d] complete\n",
				       mmc_hostname(host->mmc),
				       host->last_cmdop,
				       host_priv->sg_done, sg_todo,
				       host_priv->sg_ix+1, sg_len);
#ifdef CHECK_DMA_USE
			printk(KERN_INFO "%s: now %"FMT_HPT" started %lu "
			       "last reset %lu last stopped %lu\n",
			       mmc_hostname(host->mmc),
			       hptime(), since_ns(host_priv->when_started),
			       since_ns(host_priv->when_reset),
			       since_ns(host_priv->when_stopped));
			{	unsigned long info, debug;
				void __iomem *base;
				unsigned long pend0, pend1, pend2;
				   
				base = host_priv->dma_chan_base;
				cs = readl(base + BCM2708_DMA_CS);
				info = readl(base + BCM2708_DMA_INFO);
				debug = readl(base + BCM2708_DMA_DEBUG);
				printk(KERN_INFO "%s: DMA%d CS=%08lX TI=%08lX "
				       "DEBUG=%08lX\n",
				       mmc_hostname(host->mmc),
                                       host_priv->dma_chan,
				       cs, info, debug);
				pend0 = readl(__io_address(ARM_IRQ_PEND0));
				pend1 = readl(__io_address(ARM_IRQ_PEND1));
				pend2 = readl(__io_address(ARM_IRQ_PEND2));
				
				printk(KERN_INFO "%s: PEND0=%08lX "
				       "PEND1=%08lX PEND2=%08lX\n",
				       mmc_hostname(host->mmc),
				       pend0, pend1, pend2);
				
				//gintsts = readl(__io_address(GINTSTS));
				//gintmsk = readl(__io_address(GINTMSK));
				//printk(KERN_INFO "%s: USB GINTSTS=%08lX"
				//	 "GINTMSK=%08lX\n",
				//	 mmc_hostname(host->mmc), gintsts, gintmsk);
			}
#endif
			rc = bcm_dma_abort(host_priv->dma_chan_base);
			BUG_ON(rc != 0);
		}
		host_priv->dma_wanted = 0;
#ifdef CHECK_DMA_USE
		host_priv->when_reset = hptime();
#endif
	}

	spin_unlock_irqrestore(&host->lock, flags);
}


static void sdhci_bcm2708_dma_complete_irq(struct sdhci_host *host,
					   u32 dma_cs)
{
	struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);
	struct mmc_data *data;
	struct scatterlist *sg;
	int sg_len;
	int sg_ix;
	int sg_todo;
	unsigned long flags;

	BUG_ON(NULL == host);

	spin_lock_irqsave(&host->lock, flags);
	data = host->data;

#ifdef CHECK_DMA_USE
	if (host_priv->dmas_pending <= 0)
		DBG("on completion no DMA in progress - "
		    "now %"FMT_HPT" started %lu reset %lu stopped %lu\n",
		    hptime(), since_ns(host_priv->when_started),
		    since_ns(host_priv->when_reset),
		    since_ns(host_priv->when_stopped));
	else if (host_priv->dmas_pending > 1)
		DBG("still %d DMA in progress after completion - "
		    "now %"FMT_HPT" started %lu reset %lu stopped %lu\n",
		    host_priv->dmas_pending - 1,
		    hptime(), since_ns(host_priv->when_started),
		    since_ns(host_priv->when_reset),
		    since_ns(host_priv->when_stopped));
	BUG_ON(host_priv->dmas_pending <= 0);
	host_priv->dmas_pending -= 1;
	host_priv->when_stopped = hptime();
#endif
	host_priv->dma_wanted = 0;

	if (NULL == data) {
		DBG("PDMA unused completion - status 0x%X\n", dma_cs);
		spin_unlock_irqrestore(&host->lock, flags);
		return;
	}
	sg = data->sg;
	sg_len = data->sg_len;
	sg_todo = sg_dma_len(&sg[host_priv->sg_ix]);

	DBG("PDMA complete %d/%d [%d]/[%d]..\n",
	    host_priv->sg_done, sg_todo,
	    host_priv->sg_ix+1, sg_len);

	BUG_ON(host_priv->sg_done > sg_todo);

	if (host_priv->sg_done >= sg_todo) {
		host_priv->sg_ix++;
		host_priv->sg_done = 0;
	}

	sg_ix = host_priv->sg_ix;
	if (sg_ix < sg_len) {
		u32 irq_mask;
		/* Set off next DMA if we've got the capacity */

		if (data->flags & MMC_DATA_READ)
			irq_mask = SDHCI_INT_DATA_AVAIL;
		else
			irq_mask = SDHCI_INT_SPACE_AVAIL;

		/* We have to use the interrupt status register on the BCM2708
		   rather than the SDHCI_PRESENT_STATE register because latency
		   in the glue logic means that the information retrieved from
		   the latter is not always up-to-date w.r.t the DMA engine -
		   it may not indicate that a read or a write is ready yet */
		if (sdhci_bcm2708_raw_readl(host, SDHCI_INT_STATUS) &
		    irq_mask) {
			size_t bytes = sg_dma_len(&sg[sg_ix]) -
				       host_priv->sg_done;
			dma_addr_t addr = sg_dma_address(&data->sg[sg_ix]) +
					  host_priv->sg_done;

			/* acknowledge interrupt */
			sdhci_bcm2708_raw_writel(host, irq_mask,
						 SDHCI_INT_STATUS);

			BUG_ON(0 == bytes);

			if (data->flags & MMC_DATA_READ)
				sdhci_platdma_read(host, addr, bytes);
			else
				sdhci_platdma_write(host, addr, bytes);
		} else {
			DBG("PDMA - wait avail\n");
			/* may generate an IRQ if already present */
			sdhci_signal_irqs(host, SDHCI_INT_DATA_AVAIL |
						SDHCI_INT_SPACE_AVAIL);
		}
	} else {
		if (sync_after_dma) {
			/* On the Arasan controller the stop command (which will be
			   scheduled after this completes) does not seem to work
			   properly if we allow it to be issued when we are
			   transferring data to/from the SD card.
			   We get CRC and DEND errors unless we wait for
			   the SD controller to finish reading/writing to the card. */
			u32 state_mask;
			int timeout=30*5000;

			DBG("PDMA over - sync card\n");
			if (data->flags & MMC_DATA_READ)
				state_mask = SDHCI_DOING_READ;
			else
				state_mask = SDHCI_DOING_WRITE;

			while (0 != (sdhci_bcm2708_raw_readl(host, SDHCI_PRESENT_STATE) 
				& state_mask) && --timeout > 0)
			{
				udelay(1);
				continue;
			}
			if (timeout <= 0)
				printk(KERN_ERR"%s: final %s to SD card still "
				       "running\n",
				       mmc_hostname(host->mmc),
				       data->flags & MMC_DATA_READ? "read": "write");
		}
		if (host_priv->complete) {
			(*host_priv->complete)(host);
			DBG("PDMA %s complete\n",
			    data->flags & MMC_DATA_READ?"read":"write");
			sdhci_signal_irqs(host, SDHCI_INT_DATA_AVAIL |
						SDHCI_INT_SPACE_AVAIL);
		}
	}
	spin_unlock_irqrestore(&host->lock, flags);
}

static irqreturn_t sdhci_bcm2708_dma_irq(int irq, void *dev_id)
{
	irqreturn_t result = IRQ_NONE;
	struct sdhci_host *host = dev_id;
	struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);
	u32 dma_cs; /* control and status register */
	unsigned long flags;

	BUG_ON(NULL == dev_id);
	BUG_ON(NULL == host_priv->dma_chan_base);

	spin_lock_irqsave(&host->lock, flags);

	dma_cs = readl(host_priv->dma_chan_base + BCM2708_DMA_CS);

	if (dma_cs & BCM2708_DMA_ERR) {
		unsigned long debug;
		debug = readl(host_priv->dma_chan_base +
			      BCM2708_DMA_DEBUG);
		printk(KERN_ERR "%s: DMA error - CS %lX DEBUG %lX\n",
		       mmc_hostname(host->mmc), (unsigned long)dma_cs,
		       (unsigned long)debug);
		/* reset error */
		writel(debug, host_priv->dma_chan_base +
		       BCM2708_DMA_DEBUG);
	}
	if (dma_cs & BCM2708_DMA_INT) {
		/* acknowledge interrupt */
		writel(BCM2708_DMA_INT,
		       host_priv->dma_chan_base + BCM2708_DMA_CS);

		dsb(); /* ARM data synchronization (push) operation */

		if (!host_priv->dma_wanted) {
			/* ignore this interrupt - it was reset */
			printk(KERN_INFO "%s: DMA IRQ %X ignored - "
			       "results were reset\n",
			       mmc_hostname(host->mmc), dma_cs);
#ifdef CHECK_DMA_USE
			printk(KERN_INFO "%s: now %"FMT_HPT
			       " started %lu reset %lu stopped %lu\n",
			       mmc_hostname(host->mmc), hptime(),
			       since_ns(host_priv->when_started),
			       since_ns(host_priv->when_reset),
			       since_ns(host_priv->when_stopped));
			host_priv->dmas_pending--;
#endif
		} else
			sdhci_bcm2708_dma_complete_irq(host, dma_cs);

		result = IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&host->lock, flags);

	return result;
}
#endif /* CONFIG_MMC_SDHCI_BCM2708_DMA */


/***************************************************************************** \
 *									     *
 * Device Attributes							     *
 *									     *
\*****************************************************************************/


/**
 * Show the DMA-using status
 */
static ssize_t attr_dma_show(struct device *_dev,
			     struct device_attribute *attr, char *buf)
{
	struct sdhci_host *host = (struct sdhci_host *)dev_get_drvdata(_dev);

	if (host) {
		int use_dma = (host->flags & SDHCI_USE_PLATDMA? 1:0);
		return sprintf(buf, "%d\n", use_dma);
	} else
		return -EINVAL;
}

/**
 * Set the DMA-using status
 */
static ssize_t attr_dma_store(struct device *_dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct sdhci_host *host = (struct sdhci_host *)dev_get_drvdata(_dev);

	if (host) {
#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
		int on = simple_strtol(buf, NULL, 0);
		if (on) {
			host->flags |= SDHCI_USE_PLATDMA;
			sdhci_bcm2708_writel(host, 1, REG_EXRDFIFO_EN);
			printk(KERN_INFO "%s: DMA enabled\n",
			       mmc_hostname(host->mmc));
		} else {
			host->flags &= ~(SDHCI_USE_PLATDMA | SDHCI_REQ_USE_DMA);
			sdhci_bcm2708_writel(host, 0, REG_EXRDFIFO_EN);
			printk(KERN_INFO "%s: DMA disabled\n",
			       mmc_hostname(host->mmc));
		}
#endif
		return count;
	} else
		return -EINVAL;
}

static DEVICE_ATTR(use_dma, S_IRUGO | S_IWUGO, attr_dma_show, attr_dma_store);


/**
 * Show the DMA wait states used
 */
static ssize_t attr_dmawait_show(struct device *_dev,
			         struct device_attribute *attr, char *buf)
{
	struct sdhci_host *host = (struct sdhci_host *)dev_get_drvdata(_dev);

	if (host) {
		struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);
		int dmawait = host_priv->dma_waits;
		return sprintf(buf, "%d\n", dmawait);
	} else
		return -EINVAL;
}

/**
 * Set the DMA wait state used
 */
static ssize_t attr_dmawait_store(struct device *_dev,
			          struct device_attribute *attr,
    			          const char *buf, size_t count)
{
	struct sdhci_host *host = (struct sdhci_host *)dev_get_drvdata(_dev);

	if (host) {
#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
		struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);
		int dma_waits = simple_strtol(buf, NULL, 0);
		if (dma_waits >= 0 && dma_waits < 32) 
                        host_priv->dma_waits = dma_waits;
		else
			printk(KERN_ERR "%s: illegal dma_waits value - %d",
			       mmc_hostname(host->mmc), dma_waits);
#endif
		return count;
	} else
		return -EINVAL;
}

static DEVICE_ATTR(dma_wait, S_IRUGO | S_IWUGO,
                   attr_dmawait_show, attr_dmawait_store);


/**
 * Show the DMA-using status
 */
static ssize_t attr_status_show(struct device *_dev,
				struct device_attribute *attr, char *buf)
{
	struct sdhci_host *host = (struct sdhci_host *)dev_get_drvdata(_dev);

	if (host) {
		struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);
		return sprintf(buf,
			       "present: yes\n"
			       "power: %s\n"
			       "clock: %u Hz\n"
#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
			       "dma: %s (%d waits)\n",
#else
			       "dma: unconfigured\n",
#endif
			       "always on",
			       host->clock
#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
			       , (host->flags & SDHCI_USE_PLATDMA)? "on": "off"
                               , host_priv->dma_waits
#endif
			       );
	} else
		return -EINVAL;
}

static DEVICE_ATTR(status, S_IRUGO, attr_status_show, NULL);

/***************************************************************************** \
 *									     *
 * Power Management							     *
 *									     *
\*****************************************************************************/


#ifdef CONFIG_PM
static int sdhci_bcm2708_suspend(struct platform_device *dev, pm_message_t state)
{
	struct sdhci_host *host = (struct sdhci_host *)
				  platform_get_drvdata(dev);
	int ret = 0;

	if (host->mmc) {
		ret = mmc_suspend_host(host->mmc);
	}

	return ret;
}

static int sdhci_bcm2708_resume(struct platform_device *dev)
{
	struct sdhci_host *host = (struct sdhci_host *)
				  platform_get_drvdata(dev);
	int ret = 0;

	if (host->mmc) {
		ret = mmc_resume_host(host->mmc);
	}

	return ret;
}
#endif


/*****************************************************************************\
 *                                                                           *
 * Device quirk functions. Implemented as local ops because the flags        *
 * field is out of space with newer kernels. This implementation can be      *
 * back ported to older kernels as well.                                     *
\****************************************************************************/
static unsigned int sdhci_bcm2708_quirk_extra_ints(struct sdhci_host *host)
{
        return 1;
}

static unsigned int sdhci_bcm2708_quirk_spurious_crc(struct sdhci_host *host)
{
        return 1;
}

static unsigned int sdhci_bcm2708_quirk_voltage_broken(struct sdhci_host *host)
{
        return 1;
}

static unsigned int sdhci_bcm2708_uhs_broken(struct sdhci_host *host)
{
        return 1;
}

static unsigned int sdhci_bcm2708_missing_status(struct sdhci_host *host)
{
	return 1;
}

/***************************************************************************** \
 *									     *
 * Device ops								     *
 *									     *
\*****************************************************************************/

static struct sdhci_ops sdhci_bcm2708_ops = {
#ifdef CONFIG_MMC_SDHCI_IO_ACCESSORS
	.read_l = sdhci_bcm2708_readl,
	.read_w = sdhci_bcm2708_readw,
	.read_b = sdhci_bcm2708_readb,
	.write_l = sdhci_bcm2708_writel,
	.write_w = sdhci_bcm2708_writew,
	.write_b = sdhci_bcm2708_writeb,
#else
#error The BCM2708 SDHCI driver needs CONFIG_MMC_SDHCI_IO_ACCESSORS to be set
#endif
	.get_max_clock = sdhci_bcm2708_get_max_clock,

#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
	// Platform DMA operations
	.pdma_able  = sdhci_bcm2708_platdma_dmaable,
	.pdma_avail = sdhci_bcm2708_platdma_avail,
	.pdma_reset = sdhci_bcm2708_platdma_reset,
#endif
	.extra_ints = sdhci_bcm2708_quirk_extra_ints,
	.spurious_crc_acmd51 = sdhci_bcm2708_quirk_spurious_crc,
	.voltage_broken = sdhci_bcm2708_quirk_voltage_broken,
	.uhs_broken = sdhci_bcm2708_uhs_broken,
};

/*****************************************************************************\
 *									     *
 * Device probing/removal						     *
 *									     *
\*****************************************************************************/

static int sdhci_bcm2708_probe(struct platform_device *pdev)
{
	struct sdhci_host *host;
	struct resource *iomem;
	struct sdhci_bcm2708_priv *host_priv;
	int ret;

	BUG_ON(pdev == NULL);

	iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!iomem) {
		ret = -ENOMEM;
		goto err;
	}

	if (resource_size(iomem) != 0x100)
		dev_err(&pdev->dev, "Invalid iomem size. You may "
			"experience problems.\n");

	if (pdev->dev.parent)
		host = sdhci_alloc_host(pdev->dev.parent,
					sizeof(struct sdhci_bcm2708_priv));
	else
		host = sdhci_alloc_host(&pdev->dev,
					sizeof(struct sdhci_bcm2708_priv));

	if (IS_ERR(host)) {
		ret = PTR_ERR(host);
		goto err;
	}
	if (missing_status) {
		sdhci_bcm2708_ops.missing_status = sdhci_bcm2708_missing_status;
	}

	host->hw_name = "BCM2708_Arasan";
	host->ops = &sdhci_bcm2708_ops;
	host->irq = platform_get_irq(pdev, 0);

	host->quirks = SDHCI_QUIRK_BROKEN_CARD_DETECTION |
		       SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		       SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
               SDHCI_QUIRK_MISSING_CAPS |
               SDHCI_QUIRK_NO_HISPD_BIT |
               (sync_after_dma ? 0:SDHCI_QUIRK_MULTIBLOCK_READ_ACMD12);


#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
	host->flags = SDHCI_USE_PLATDMA;
#endif

	if (!request_mem_region(iomem->start, resource_size(iomem),
				mmc_hostname(host->mmc))) {
		dev_err(&pdev->dev, "cannot request region\n");
		ret = -EBUSY;
		goto err_request;
	}

	host->ioaddr = ioremap(iomem->start, resource_size(iomem));
	if (!host->ioaddr) {
		dev_err(&pdev->dev, "failed to remap registers\n");
		ret = -ENOMEM;
		goto err_remap;
	}

	host_priv = SDHCI_HOST_PRIV(host);

#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
	host_priv->dma_wanted = 0;
#ifdef CHECK_DMA_USE
	host_priv->dmas_pending = 0;
	host_priv->when_started = 0;
	host_priv->when_reset = 0;
	host_priv->when_stopped = 0;
#endif
	host_priv->sg_ix = 0;
	host_priv->sg_done = 0;
	host_priv->complete = NULL;
	host_priv->dma_waits = SDHCI_BCM_DMA_WAITS;

	host_priv->cb_base = dma_alloc_writecombine(&pdev->dev, SZ_4K,
						    &host_priv->cb_handle,
						    GFP_KERNEL);
	if (!host_priv->cb_base) {
		dev_err(&pdev->dev, "cannot allocate DMA CBs\n");
		ret = -ENOMEM;
		goto err_alloc_cb;
	}

	ret = bcm_dma_chan_alloc(BCM_DMA_FEATURE_FAST,
				 &host_priv->dma_chan_base,
				 &host_priv->dma_irq);
	if (ret < 0) {
		dev_err(&pdev->dev, "couldn't allocate a DMA channel\n");
		goto err_add_dma;
	}
	host_priv->dma_chan = ret;

	ret = request_irq(host_priv->dma_irq, sdhci_bcm2708_dma_irq,
			  IRQF_SHARED, DRIVER_NAME " (dma)", host);
	if (ret) {
		dev_err(&pdev->dev, "cannot set DMA IRQ\n");
		goto err_add_dma_irq;
	}
	DBG("DMA CBs %p handle %08X DMA%d %p DMA IRQ %d\n",
	    host_priv->cb_base, (unsigned)host_priv->cb_handle,
	    host_priv->dma_chan, host_priv->dma_chan_base,
	    host_priv->dma_irq);

    if (allow_highspeed)
        host->mmc->caps |= MMC_CAP_SD_HIGHSPEED | MMC_CAP_MMC_HIGHSPEED;

    /* single block writes cause data loss with some SD cards! */
    host->mmc->caps2 |= MMC_CAP2_FORCE_MULTIBLOCK;
#endif

	ret = sdhci_add_host(host);
	if (ret)
		goto err_add_host;

	platform_set_drvdata(pdev, host);
	ret = device_create_file(&pdev->dev, &dev_attr_use_dma);
	ret = device_create_file(&pdev->dev, &dev_attr_dma_wait);
	ret = device_create_file(&pdev->dev, &dev_attr_status);

#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
	/* enable extension fifo for paced DMA transfers */
	sdhci_bcm2708_writel(host, 1, REG_EXRDFIFO_EN);
	sdhci_bcm2708_writel(host, 4, REG_EXRDFIFO_CFG);
#endif

	printk(KERN_INFO "%s: BCM2708 SDHC host at 0x%08llx DMA %d IRQ %d\n",
	       mmc_hostname(host->mmc), (unsigned long long)iomem->start,
	       host_priv->dma_chan, host_priv->dma_irq);

	return 0;

err_add_host:
#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
	free_irq(host_priv->dma_irq, host);
err_add_dma_irq:
	bcm_dma_chan_free(host_priv->dma_chan);
err_add_dma:
	dma_free_writecombine(&pdev->dev, SZ_4K, host_priv->cb_base,
			      host_priv->cb_handle);
err_alloc_cb:
#endif
	iounmap(host->ioaddr);
err_remap:
	release_mem_region(iomem->start, resource_size(iomem));
err_request:
	sdhci_free_host(host);
err:
	dev_err(&pdev->dev, "probe failed, err %d\n", ret);
	return ret;
}

static int sdhci_bcm2708_remove(struct platform_device *pdev)
{
	struct sdhci_host *host = platform_get_drvdata(pdev);
	struct resource *iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct sdhci_bcm2708_priv *host_priv = SDHCI_HOST_PRIV(host);
	int dead;
	u32 scratch;

	dead = 0;
	scratch = sdhci_bcm2708_readl(host, SDHCI_INT_STATUS);
	if (scratch == (u32)-1)
		dead = 1;

	device_remove_file(&pdev->dev, &dev_attr_status);
	device_remove_file(&pdev->dev, &dev_attr_dma_wait);
	device_remove_file(&pdev->dev, &dev_attr_use_dma);

#ifdef CONFIG_MMC_SDHCI_BCM2708_DMA
	free_irq(host_priv->dma_irq, host);
	dma_free_writecombine(&pdev->dev, SZ_4K, host_priv->cb_base,
			      host_priv->cb_handle);
#endif
	sdhci_remove_host(host, dead);
	iounmap(host->ioaddr);
	release_mem_region(iomem->start, resource_size(iomem));
	sdhci_free_host(host);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver sdhci_bcm2708_driver = {
	.driver = {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
	.probe		= sdhci_bcm2708_probe,
	.remove		= sdhci_bcm2708_remove,

#ifdef CONFIG_PM
	.suspend = sdhci_bcm2708_suspend,
	.resume = sdhci_bcm2708_resume,
#endif

};

/*****************************************************************************\
 *									     *
 * Driver init/exit							     *
 *									     *
\*****************************************************************************/

static int __init sdhci_drv_init(void)
{
	return platform_driver_register(&sdhci_bcm2708_driver);
}

static void __exit sdhci_drv_exit(void)
{
	platform_driver_unregister(&sdhci_bcm2708_driver);
}

module_init(sdhci_drv_init);
module_exit(sdhci_drv_exit);

module_param(allow_highspeed, bool, 0444);
module_param(emmc_clock_freq, int, 0444);
module_param(sync_after_dma, bool, 0444);
module_param(missing_status, bool, 0444);

MODULE_DESCRIPTION("Secure Digital Host Controller Interface platform driver");
MODULE_AUTHOR("Broadcom <info@broadcom.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:"DRIVER_NAME);

MODULE_PARM_DESC(allow_highspeed, "Allow high speed transfers modes");
MODULE_PARM_DESC(emmc_clock_freq, "Specify the speed of emmc clock");
MODULE_PARM_DESC(sync_after_dma, "Block in driver until dma complete");
MODULE_PARM_DESC(missing_status, "Use the missing status quirk");


