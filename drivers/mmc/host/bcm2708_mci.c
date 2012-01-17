/*
 *  linux/drivers/mmc/host/bcm2708_mci.c - Broadcom BCM2708 MCI driver
 *
 *  Copyright (C) 2010 Broadcom, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/log2.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>

#include <asm/cacheflush.h>
#include <asm/div64.h>
#include <asm/io.h>
#include <asm/sizes.h>
//#include <asm/mach/mmc.h>

#include <mach/gpio.h>

#include "bcm2708_mci.h"

#define DRIVER_NAME "bcm2708_mci"

//#define PIO_DEBUG
#ifdef PIO_DEBUG
#define DBG(host,fmt,args...)	\
	printk(KERN_ERR"%s: %s: " fmt, mmc_hostname(host->mmc), __func__ , args)
#else
#define DBG(host,fmt,args...)	\
	pr_debug("%s: %s: " fmt, mmc_hostname(host->mmc), __func__ , args)
#endif

#define USE_DMA
#define USE_DMA_IRQ

#ifdef USE_DMA
#define SDHOST_DMA_CHANNEL 5
#endif

#define BCM2708_DMA_ACTIVE	(1 << 0)
#define BCM2708_DMA_INT		(1 << 2)

#define BCM2708_DMA_INT_EN	(1 << 0)
#define BCM2708_DMA_D_INC	(1 << 4)
#define BCM2708_DMA_D_WIDTH	(1 << 5)
#define BCM2708_DMA_D_DREQ	(1 << 6)
#define BCM2708_DMA_S_INC	(1 << 8)
#define BCM2708_DMA_S_WIDTH	(1 << 9)
#define BCM2708_DMA_S_DREQ	(1 << 10)

#define	BCM2708_DMA_PER_MAP(x)	((x) << 16)

#define BCM2708_DMA_DREQ_SDHOST	13

#define BCM2708_DMA_CS		0x00
#define BCM2708_DMA_ADDR	0x04

static void dump_sd_regs(void * mmc_base );
static int bcm2708_mci_reset(struct bcm2708_mci_host *host);

static void do_command(void __iomem *base, u32 c, u32 a)
{
	u32 cmdsts = 0;
	writel(a, base + BCM2708_MCI_ARGUMENT);
	writel(c | BCM2708_MCI_ENABLE, base + BCM2708_MCI_COMMAND);

	/* check for error and command done */
	cmdsts = readl(base + BCM2708_MCI_COMMAND);
	while ((cmdsts & BCM2708_MCI_ENABLE) && (!(cmdsts & BCM2708_MCI_FAIL_FLAG)))
		cmdsts = readl(base + BCM2708_MCI_COMMAND);
	if (cmdsts & BCM2708_MCI_FAIL_FLAG) {
		printk(KERN_DEBUG"%s: Command %d failed with arg %d\n", __func__, c, a);
		dump_sd_regs(base);
	}
}

//static void discard_words(void __iomem *base, int words)
//{
//	int i;
//	for (i = 0; i < words; i++) {
//		while (!(readl(base + BCM2708_MCI_STATUS) & BCM2708_MCI_DATAFLAG));
//		readl(base + BCM2708_MCI_DATA);
//	}
//}

#define CACHE_LINE_MASK 31

static int suitable_for_dma(struct scatterlist *sg_ptr, int sg_len)
{
	int i;

	for (i = 0; i < sg_len; i++) {
		if (sg_ptr[i].offset & CACHE_LINE_MASK || sg_ptr[i].length & CACHE_LINE_MASK)
			return 0;
	}

	return 1;
}

static void wait_for_complete(struct bcm2708_mci_host *host,
			      void __iomem *mmc_base)
{
#ifdef USE_SDHOST_IRQ
#error not implemented yet
#else
	while ((readl(mmc_base + BCM2708_MCI_STATUS) &
		(BCM2708_MCI_HSTS_BUSY | BCM2708_MCI_HSTS_BLOCK)) == 0)
		continue;

	writel(BCM2708_MCI_HSTS_BUSY | BCM2708_MCI_HSTS_BLOCK,
	       mmc_base + BCM2708_MCI_STATUS);
#endif
}

static void dump_sd_regs(void * mmc_base )
{
	printk(KERN_DEBUG"Registers:\n");
	printk(KERN_DEBUG"SDCMD:0x%x\n", readl(mmc_base + BCM2708_MCI_COMMAND));
	printk(KERN_DEBUG"SDARG:0x%x\n", readl(mmc_base + BCM2708_MCI_ARGUMENT));
	printk(KERN_DEBUG"SDTOUT:0x%x\n", readl(mmc_base + BCM2708_MCI_TIMEOUT));
	printk(KERN_DEBUG"SDCDIV:0x%x\n", readl(mmc_base + BCM2708_MCI_CLKDIV));
	printk(KERN_DEBUG"SDRSP0:0x%x\n", readl(mmc_base + BCM2708_MCI_RESPONSE0));
	printk(KERN_DEBUG"SDRSP1:0x%x\n", readl(mmc_base + BCM2708_MCI_RESPONSE1));
	printk(KERN_DEBUG"SDRSP2:0x%x\n", readl(mmc_base + BCM2708_MCI_RESPONSE2));
	printk(KERN_DEBUG"SDRSP3:0x%x\n", readl(mmc_base + BCM2708_MCI_RESPONSE3));
	printk(KERN_DEBUG"SDHSTS:0x%x\n", readl(mmc_base + BCM2708_MCI_STATUS));
	printk(KERN_DEBUG"SDPO:0x%x\n", readl(mmc_base + BCM2708_MCI_VDD));
	printk(KERN_DEBUG"SDEDM:0x%x\n", readl(mmc_base + BCM2708_MCI_EDM));
	printk(KERN_DEBUG"SDHCFG:0x%x\n", readl(mmc_base + BCM2708_MCI_HOSTCONFIG));
	printk(KERN_DEBUG"SDHBCT:0x%x\n", readl(mmc_base + BCM2708_MCI_HBCT));
	//printk(KERN_ERR"SDDATA:0x%x\n", readl(mmc_base + BCM2708_MCI_DATA));
	printk(KERN_DEBUG"SDHBLC:0x%x\n", readl(mmc_base + BCM2708_MCI_HBLC));
}


static void
bcm2708_mci_start_command(struct bcm2708_mci_host *host, struct mmc_command *cmd, struct mmc_data *data)
{
	void __iomem *mmc_base = host->mmc_base;
	void __iomem *dma_base = host->dma_base;
	u32 status;
	u32 c;
	int redo = 0;

	DBG(host, "op %02x arg %08x flags %08x\n",
	    cmd->opcode, cmd->arg, cmd->flags);

back:

	/*
	 * clear the controller status register
	 */

	writel(-1, mmc_base + BCM2708_MCI_STATUS);

	/*
	 * build the command register write, incorporating no
         * response, long response, busy, read and write flags
	 */

	c = cmd->opcode;
	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136)
			c |= BCM2708_MCI_LONGRESP;
	} else
		c |= BCM2708_MCI_NORESP;
	if (cmd->flags & MMC_RSP_BUSY)
		c |= BCM2708_MCI_BUSY;

	if (data) {
		if (data->flags & MMC_DATA_READ)
			c |= BCM2708_MCI_READ;
		else
			c |= BCM2708_MCI_WRITE;

		DBG(host, "BYTECOUT %d BLOCKCOUNT %d .. ",readl(mmc_base + BCM2708_MCI_HBCT), readl(mmc_base + BCM2708_MCI_HBLC));
		DBG(host, "set blocksize to %d\n", data->blksz);
		DBG(host, "set blockcnt to %d\n", data->blocks);
		writel( data->blksz, mmc_base + BCM2708_MCI_HBCT);
		writel(data->blocks, mmc_base + BCM2708_MCI_HBLC);
	}

	/*
         * run the command and wait for it to complete
	 */

	DBG(host, "executing command=%d\n", cmd->opcode);

	do_command(mmc_base, c, cmd->arg);

	DBG(host, "done cmd=%d\n", cmd->opcode);

	if (c & BCM2708_MCI_BUSY) {

		DBG(host, "waiting for command(%d) to complete\n", cmd->opcode);
		wait_for_complete(host, mmc_base);
		DBG(host, "done waiting for command(%d)\n", cmd->opcode);
	}

	/*
	 * retrieve the response and error (if any)
	 */

	status = readl(mmc_base + BCM2708_MCI_STATUS);

	if (cmd->flags & MMC_RSP_136) {
		cmd->resp[3] = readl(mmc_base + BCM2708_MCI_RESPONSE0);
		cmd->resp[2] = readl(mmc_base + BCM2708_MCI_RESPONSE1);
		cmd->resp[1] = readl(mmc_base + BCM2708_MCI_RESPONSE2);
		cmd->resp[0] = readl(mmc_base + BCM2708_MCI_RESPONSE3);
	} else {
		cmd->resp[0] = readl(mmc_base + BCM2708_MCI_RESPONSE0);
	}

	if (status & BCM2708_MCI_CMDTIMEOUT) {
		printk(KERN_DEBUG "mmc driver saw timeout with opcode = %d, data = 0x%08x, timeout = %d", cmd->opcode, (unsigned int)data, readl(mmc_base + BCM2708_MCI_TIMEOUT));
		if (data)
			printk(KERN_DEBUG " data->sg_len = %d\n", data->sg_len);
		else
			printk(KERN_DEBUG "\n");
		if (!redo) {
			printk(KERN_DEBUG "redo\n");
			redo = 1;
			goto back;
		} else
			cmd->error = -ETIMEDOUT;
	}

	/*
	 * pump data if necessary
	 */

	if (data) {
		unsigned int sg_len = data->sg_len;
		struct scatterlist *sg_ptr = data->sg;

		data->bytes_xfered = 0;

#ifdef USE_DMA
		if (suitable_for_dma(sg_ptr, sg_len)) {
			int i, count = dma_map_sg(&host->dev->dev, sg_ptr, sg_len, data->flags & MMC_DATA_READ ? DMA_FROM_DEVICE : DMA_TO_DEVICE);

			for (i = 0; i < count; i++) {
				BCM2708_DMA_CB_T *cb = &host->cb_base[i];

				if (data->flags & MMC_DATA_READ) {
					cb->info = BCM2708_DMA_PER_MAP(BCM2708_DMA_DREQ_SDHOST)|BCM2708_DMA_S_DREQ|BCM2708_DMA_D_WIDTH|BCM2708_DMA_D_INC;
					cb->src = 0x7e202040;
					cb->dst = sg_dma_address(&sg_ptr[i]);
				} else {
					cb->info = BCM2708_DMA_PER_MAP(BCM2708_DMA_DREQ_SDHOST)|BCM2708_DMA_S_WIDTH|BCM2708_DMA_S_INC|BCM2708_DMA_D_DREQ;
					cb->src = sg_dma_address(&sg_ptr[i]);
					cb->dst = 0x7e202040;
				}
 
				cb->length = sg_dma_len(&sg_ptr[i]);
				cb->stride = 0;

				if (i == count - 1) {
#ifdef USE_DMA_IRQ
					cb->info |= BCM2708_DMA_INT_EN;
#endif
					cb->next = 0;
				} else 
					cb->next = host->cb_handle + (i + 1) * sizeof(BCM2708_DMA_CB_T);

				cb->pad[0] = 0;
				cb->pad[1] = 0;

				data->bytes_xfered += sg_ptr[i].length;
			}

			dsb();	// data barrier operation

			writel(host->cb_handle, dma_base + BCM2708_DMA_ADDR);
			writel(BCM2708_DMA_ACTIVE, dma_base + BCM2708_DMA_CS);

#ifdef USE_DMA_IRQ
			down(&host->sem);
#else
			while ((readl(dma_base + BCM2708_DMA_CS) & BCM2708_DMA_ACTIVE));
#endif
			dma_unmap_sg(&host->dev->dev, sg_ptr, sg_len, data->flags & MMC_DATA_READ ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
		} else
#endif
		while (sg_len) {
			unsigned long flags;
			char *buffer;
			u32 *ptr, *lim;

			DBG(host, "sg_len=%d sg_ptr=%p len=%d\n", sg_len, sg_ptr, sg_ptr->length);

			/*
			 * map the current scatter buffer
			 */

			buffer = bcm2708_mci_kmap_atomic(sg_ptr, &flags);

			/*
			 * pump the data
			 */

			ptr = (u32 *)(buffer);
			lim = (u32 *)(buffer + sg_ptr->length);

			while (ptr < lim)
			{
#ifdef PIO_DEBUG
				unsigned int wait_count = 1;
#endif
				while (!(readl(mmc_base + BCM2708_MCI_STATUS) & BCM2708_MCI_DATAFLAG)) 
				{
#ifdef PIO_DEBUG
					wait_count++;
					if ( 0 == (wait_count % 20000) ) {

						printk(KERN_ERR"Timeout waiting for data flag\n");
						dump_sd_regs(mmc_base);
					}
#endif
				}

				if (data->flags & MMC_DATA_READ)
					*ptr++ = readl(mmc_base + BCM2708_MCI_DATA);
				else
				{
#ifdef PIO_DEBUG
					uint32_t fifo_bytes, fifo_wait_count = 1;

					fifo_bytes = readl(mmc_base + BCM2708_MCI_EDM);
					fifo_bytes = (fifo_bytes >> 4)  & 0xf;

					while(fifo_bytes > 3)
					{
						fifo_wait_count++;
						if ( 0 == (fifo_wait_count % 20000) ) {
							printk(KERN_ERR"waiting for fifo_bytes < 3\n");
							dump_sd_regs(mmc_base);
						}

						fifo_bytes = readl(mmc_base + BCM2708_MCI_EDM);
						fifo_bytes = (fifo_bytes >> 4)  & 0xf;
					}

					BUG_ON(fifo_bytes > 3);
#endif
					writel(*ptr++, mmc_base + BCM2708_MCI_DATA);
				}
			}

			DBG(host, "done reading/writing %d bytes from mmc\n", sg_ptr->length);


			/*
			 * unmap the buffer
			 */

			bcm2708_mci_kunmap_atomic(buffer, &flags);

			/*
			 * if we were reading, and we have completed this
			 * page, ensure that the data cache is coherent
			 */

			if (data->flags & MMC_DATA_READ)
				flush_dcache_page(sg_page(sg_ptr));

			data->bytes_xfered += sg_ptr->length;

			sg_ptr++;
			sg_len--; 
		}

//		if (host->is_acmd && cmd->opcode == SD_APP_SEND_SCR)
//			discard_words(mmc_base, 126);
//		if (host->is_acmd && cmd->opcode == SD_APP_SEND_NUM_WR_BLKS) 
//			discard_words(mmc_base, 127);
//		if (!host->is_acmd && cmd->opcode == SD_SWITCH)
//			discard_words(mmc_base, 112);

		if (data->stop) {

			DBG(host, "sending stop command %p\n", data->stop);
			bcm2708_mci_start_command(host, data->stop, 0);

			while ((readl(mmc_base + BCM2708_MCI_STATUS) &
					 BCM2708_MCI_DATAFLAG))
			{
				DBG(host, "error data flag still set read=%d bytes\n", sg_ptr->length);
				printk(KERN_ERR"SDDATA:0x%x\n", readl(mmc_base + BCM2708_MCI_DATA));
				dump_sd_regs(mmc_base);
			}
		}
	}
	/*
	 * remember if we're an application command
	 */
	host->is_acmd = cmd->opcode == MMC_APP_CMD;
}

static void bcm2708_mci_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct bcm2708_mci_host *host = mmc_priv(mmc);

	if (mrq->data && !is_power_of_2(mrq->data->blksz)) {
		printk(KERN_ERR "%s: Unsupported block size (%d bytes)\n",
			mmc_hostname(mmc), mrq->data->blksz);
		mrq->cmd->error = -EINVAL;
		mmc_request_done(mmc, mrq);
		return;
	}

	bcm2708_mci_start_command(host, mrq->cmd, mrq->data);

	mmc_request_done(host->mmc, mrq);
}

static void bcm2708_mci_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{

	struct bcm2708_mci_host *host = mmc_priv(mmc);
	void *mmc_base = host->mmc_base;


	printk(KERN_DEBUG"%s: Want to set clock: %d width: %d\n", mmc_hostname(mmc),
			ios->clock, ios->bus_width);

	if (ios->clock == 25000000 || ios->clock == 26000000) {
		printk(KERN_DEBUG"%s setting clock div to 10 (8+2)\n", mmc_hostname(mmc));
		writel(0x8, mmc_base + BCM2708_MCI_CLKDIV);
	} else if (ios->clock == 50000000 || ios->clock == 52000000) {
		printk(KERN_DEBUG"%s setting clock div to 5 (3+2)\n", mmc_hostname(mmc));
		writel(0x3, mmc_base + BCM2708_MCI_CLKDIV);
	} else {
		// On init or unknown clock, we set the clock really low
		printk(KERN_DEBUG"%s Setting clock div to 0x4e0\n", mmc_hostname(mmc));
		writel(0x4e0, mmc_base + BCM2708_MCI_CLKDIV);
	}

	if (ios->bus_width) {
		uint32_t hcfg;
		hcfg = readl(mmc_base + BCM2708_MCI_HOSTCONFIG);
		printk(KERN_DEBUG"%s setting bus width to %d\n", mmc_hostname(mmc), ios->bus_width);

		hcfg &= BCM2708_MCI_HOSTCONFIG_WIDEEXT_CLR;
		hcfg |= (ios->bus_width == MMC_BUS_WIDTH_4) ? BCM2708_MCI_HOSTCONFIG_WIDEEXT_4BIT : 0;

		writel(hcfg, mmc_base + BCM2708_MCI_HOSTCONFIG);
	}
}

static int bcm2708_mci_get_cd(struct mmc_host *mmc)
{
	int present = -ENOSYS;

	struct bcm2708_mci_host *host = mmc_priv(mmc);
	void *gpio_base = host->gpio_base;

	present = readl( (gpio_base + GP_LEV0) );
	
	if ((present & (1<<29))==(1<<29))
		present = 0;
	else
		present = 1;

	printk(KERN_DEBUG"***sdcard present***=%d\n", present);

	// FIXME - For now force SD card present for 2835DK 
	present = 1;
	return present;
}

/*
 * Handle completion of command and data transfers.
 */

//static irqreturn_t bcm2708_mci_command_irq(int irq, void *dev_id)
//{
//	struct bcm2708_mci_host *host = dev_id;
//
//	writel(BCM2708_DMA_INT, host->dma_base + BCM2708_DMA_CS);
//
//	printk(KERN_ERR "irq\n");
//
//	return IRQ_RETVAL(0);
//}

static irqreturn_t bcm2708_mci_sddet_irq(int irq, void *dev_id)
{
	struct bcm2708_mci_host *host = dev_id;
	irqreturn_t handled = IRQ_NONE;
	int present;

	present = bcm2708_mci_get_cd(host->mmc);

	if (present!=host->present)
	{
		host->present = present;
		printk(KERN_DEBUG "SDDET IRQ: sdcard present: %d\n",present);
		bcm2708_mci_reset(host);
		mmc_detect_change(host->mmc, msecs_to_jiffies(500));
	}

	return IRQ_RETVAL(handled);
}

#ifdef USE_DMA_IRQ
static irqreturn_t bcm2708_mci_data_irq(int irq, void *dev_id)
{
	struct bcm2708_mci_host *host = dev_id;
        irqreturn_t handled = IRQ_NONE;

	if (0 != (BCM2708_DMA_INT & readl(host->dma_base + BCM2708_DMA_CS))) {
		writel(BCM2708_DMA_INT, host->dma_base + BCM2708_DMA_CS);
		dsb();
		handled = IRQ_HANDLED;
		up(&host->sem);
	} else {
		printk(KERN_ERR"bcm2708_mci irq check failed !!\n");
	}

	return IRQ_RETVAL(handled);
}
#endif

static const struct mmc_host_ops bcm2708_mci_ops = {
	.request	= bcm2708_mci_request,
	.set_ios	= bcm2708_mci_set_ios,
	.get_cd		= bcm2708_mci_get_cd,
};

static int bcm2708_mci_reset(struct bcm2708_mci_host *host)
{

	void *mmc_base = host->mmc_base;

	// pin muxing/gpios is done by vcloader

	printk(KERN_DEBUG"%s:Resetting BCM2708 MCI Controller.\n", __func__ );

	writel(0, mmc_base + BCM2708_MCI_COMMAND);
	writel(0, mmc_base + BCM2708_MCI_ARGUMENT);
	writel(0x00F00000, mmc_base + BCM2708_MCI_TIMEOUT);
	writel(0, mmc_base + BCM2708_MCI_CLKDIV);
	writel(0, mmc_base + BCM2708_MCI_STATUS);
	writel(0, mmc_base + BCM2708_MCI_VDD);
	writel(0, mmc_base + BCM2708_MCI_HOSTCONFIG);
	writel(0, mmc_base + BCM2708_MCI_HBCT);
	writel(0, mmc_base + BCM2708_MCI_HBLC);

	writel( BCM2708_MCI_HOSTCONFIG_SLOW_CARD | BCM2708_MCI_HOSTCONFIG_BUSY_IRPT_EN |
			BCM2708_MCI_HOSTCONFIG_BLOCK_IRPT_EN | BCM2708_MCI_HOSTCONFIG_WIDE_INT_BUS,
			mmc_base + BCM2708_MCI_HOSTCONFIG);

	// On A0 silicon it has been observed that the following must hold
	// WRITE_THRESHOLD<=5 and READ_THRESHOLD<=WRITE_THRESHOLD+1
	// with the chip running at 150MHz (with the interface running @ 150/22 = 6.8 MHz)
	// the second requirement suggests that the verilog does not properly separate the read / write FIFOs
	// On V3XDS Read=2 & Write=6

#define READ_THRESHOLD  3
#define WRITE_THRESHOLD 3
#if 1 // !!! This is still required, without it we get CRC16 errors in data.
	{
		uint32_t temp;
		temp = readl(mmc_base + BCM2708_MCI_EDM);
		temp &= ~((0x1F<<14) | (0x1F<<9));
		temp  |= (WRITE_THRESHOLD << 9) | (READ_THRESHOLD << 14);
		writel(temp, mmc_base + BCM2708_MCI_EDM);
	}
#endif

	// Power on delay
	mdelay(10);
	writel(BCM2708_MCI_VDD_ENABLE, mmc_base + BCM2708_MCI_VDD);
	mdelay(10);

	return 0;
}


static int __devinit bcm2708_mci_probe(struct platform_device *pdev)
{
	struct mmc_host *mmc;
	struct bcm2708_mci_host *host;
	struct resource *mmc_res;
	struct resource *dma_res;
	struct resource *gpio_res;
	struct resource *dat_res;
	struct resource *sddet_res;
	int ret;

	mmc = mmc_alloc_host(sizeof(struct bcm2708_mci_host), &pdev->dev);
	if (!mmc) {
		ret = -ENOMEM;
		dev_dbg(&pdev->dev, "couldn't allocate mmc host\n");
		goto fail0;
	}

	host = mmc_priv(mmc);
	host->mmc = mmc;

	host->dev = pdev;

	sema_init(&host->sem, 0);

#ifdef USE_DMA
	host->cb_base = dma_alloc_writecombine(&pdev->dev, SZ_4K, &host->cb_handle, GFP_KERNEL);
	if (!host->cb_base) {
		ret = -ENOMEM;
		dev_dbg(&pdev->dev, "couldn't allocate dma base\n");
		goto fail1;
	}
#endif

	mmc_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mmc_res) {
		ret = -ENXIO;
		dev_dbg(&pdev->dev, "couldn't allocate mmc memory resource 0\n");
		goto fail2;
	}

	if (!request_mem_region(mmc_res->start, mmc_res->end - mmc_res->start + 1, DRIVER_NAME)) {
		ret = -EBUSY;
		goto fail2;
	}

	dma_res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!dma_res) {
		ret = -ENXIO;
		dev_dbg(&pdev->dev, "couldn't allocate dma memory resource 1\n");
		goto fail3;
	}

	/*
	 * Map I/O regions
	 */

	host->mmc_base = ioremap(mmc_res->start, resource_size(mmc_res));
	if (!host->mmc_base) {
		ret = -ENOMEM;
		goto fail3;
	}

	gpio_res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (!gpio_res) {
		ret = -ENXIO;
		dev_dbg(&pdev->dev, "couldn't allocate gpio resource\n");
		goto fail4;
	}

	/*
	 * Map I/O regions
	 */

	host->gpio_base = ioremap(gpio_res->start, resource_size(gpio_res));
	if (!host->gpio_base) {
		ret = -ENOMEM;
		goto fail4;
	}

#ifdef USE_DMA
	host->dma_base = __io_address(dma_res->start);

	if (!host->dma_base) {
		ret = -ENOMEM;
		goto fail5;
	}

	// USE DMA5 channel
	host->dma_base = (void __iomem *)((char *) host->dma_base + (SDHOST_DMA_CHANNEL * 0x100));

	dev_dbg(&pdev->dev, "%s: using dma channel %d for sdhost\n", __func__, SDHOST_DMA_CHANNEL);

	/*
	 * Grab interrupts.
	 */
#ifdef USE_DMA_IRQ
	dat_res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!dat_res) {
		ret = -ENXIO;
		dev_dbg(&pdev->dev, "couldn't allocate irq for dma\n");
		goto fail5;
	}

	ret = request_irq(dat_res->start, bcm2708_mci_data_irq, 0, DRIVER_NAME " (dat)", host);
	if (ret) {
		goto fail5;
	}
	dev_dbg(&pdev->dev, "%s: using dma interrupt number %d for sdhost\n", __func__, dat_res->start);

#endif
#endif

	host->present = bcm2708_mci_get_cd(host->mmc);

	sddet_res = platform_get_resource(pdev, IORESOURCE_IRQ, 1);
	if (!sddet_res) {
		ret = -ENXIO;
		dev_dbg(&pdev->dev, "couldn't allocate irq for sd detect\n");
		goto fail6;
	}

	ret = request_irq(sddet_res->start, bcm2708_mci_sddet_irq, 0, DRIVER_NAME " (cmd)", host);
	if (ret) {
		goto fail6;
	}

	host->is_acmd = 0;

	mmc->ops = &bcm2708_mci_ops;
	mmc->f_min = 200000;
	mmc->f_max = 52000000;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;

	/*
	 * We can do SGIO
	 */
	mmc->max_segs = NR_SG;

	/*
	 * Since we only have a 16-bit data length register, we must
	 * ensure that we don't exceed 2^16-1 bytes in a single request.
	 */
	mmc->max_req_size = 65535;

	/*
	 * Set the maximum segment size.  Since we aren't doing DMA
	 * (yet) we are only limited by the data length register.
	 */
	mmc->max_seg_size = mmc->max_req_size;

	/*
	 * Block size can be up to 2048 bytes, but must be a power of two.
	 */
	mmc->max_blk_size = 2048;

	/*
	 * No limit on the number of blocks transferred.
	 */
	mmc->max_blk_count = mmc->max_req_size;

	/*
	 * We support 4-bit data (at least on the DB)
	 */

	mmc->caps |= (MMC_CAP_4_BIT_DATA | MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED) ;

	bcm2708_mci_reset(host);

	mmc_add_host(mmc);

	printk(KERN_INFO "%s: BCM2708 SD host at 0x%08llx 0x%08llx\n",
		mmc_hostname(mmc),
		(unsigned long long)mmc_res->start, (unsigned long long)dma_res->start);

	return 0;

fail6:
#ifdef USE_DMA_IRQ
	free_irq(dat_res->start, host);
#endif
fail5:
	iounmap(host->gpio_base);
fail4:
	iounmap(host->mmc_base);
fail3:
	release_mem_region(mmc_res->start, mmc_res->end - mmc_res->start + 1);
fail2:
	dma_free_writecombine(&pdev->dev, SZ_4K, host->cb_base, host->cb_handle);
fail1:
	mmc_free_host(mmc);
fail0:
	dev_err(&pdev->dev, "probe failed, err %d\n", ret);
	return ret;
}

static int __devexit bcm2708_mci_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);

	if (mmc) {
		struct bcm2708_mci_host *host = mmc_priv(mmc);
		struct resource *res;
		struct resource *res2;

		mmc_remove_host(mmc);
#ifdef USE_DMA
#ifdef USE_DMA_IRQ
		res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
		free_irq(res->start, host);
#endif
#endif

		res2 = platform_get_resource(pdev, IORESOURCE_IRQ, 1);
		free_irq(res2->start, host);

		iounmap(host->mmc_base);
		iounmap(host->gpio_base);
		iounmap(host->dma_base);

		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		release_mem_region(res->start, resource_size(res));
#ifdef USE_DMA
		dma_free_writecombine(&pdev->dev, SZ_4K, host->cb_base, host->cb_handle);
#endif

		mmc_free_host(mmc);
		platform_set_drvdata(pdev, NULL);

		return 0;
	} else
		return -1;
}

#ifdef CONFIG_PM
static int bcm2708_mci_suspend(struct platform_device *dev, pm_message_t state)
{
	struct mmc_host *mmc = platform_get_drvdata(dev);
	int ret = 0;

	if (mmc) {
		ret = mmc_suspend_host(mmc);
	}

	return ret;
}

static int bcm2708_mci_resume(struct platform_device *dev)
{
	struct mmc_host *mmc = platform_get_drvdata(dev);
	int ret = 0;

	if (mmc) {
		ret = mmc_resume_host(mmc);
	}

	return ret;
}
#else
#define bcm2708_mci_suspend	NULL
#define bcm2708_mci_resume	NULL
#endif

static struct platform_driver bcm2708_mci_driver = {
	.probe		= bcm2708_mci_probe,
	.remove		= bcm2708_mci_remove,
	.suspend	= bcm2708_mci_suspend,
	.resume		= bcm2708_mci_resume,
	.driver		= {
		.name	= DRIVER_NAME,
		.owner  = THIS_MODULE,
	},
};

static int __init bcm2708_mci_init(void)
{
	return platform_driver_register(&bcm2708_mci_driver);
}

static void __exit bcm2708_mci_exit(void)
{
	platform_driver_unregister(&bcm2708_mci_driver);
}

module_init(bcm2708_mci_init);
module_exit(bcm2708_mci_exit);

MODULE_DESCRIPTION("BCM2708 Multimedia Card Interface driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:bcm2708_mci");
