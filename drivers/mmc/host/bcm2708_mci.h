/*
 *  linux/drivers/mmc/host/bcm2708_mci.c - Broadcom BCM2708 MCI driver
 *
 *  Copyright (C) 2010 Broadcom, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

struct clk;

#define BCM2708_MCI_COMMAND	0x00

#define BCM2708_MCI_READ     	(1 << 6)
#define BCM2708_MCI_WRITE    	(1 << 7)
#define BCM2708_MCI_LONGRESP 	(1 << 9)
#define BCM2708_MCI_NORESP   	(1 << 10)
#define BCM2708_MCI_BUSY     	(1 << 11)
#define BCM2708_MCI_FAIL_FLAG	(1 << 14)
#define BCM2708_MCI_ENABLE   	(1 << 15)

#define BCM2708_MCI_ARGUMENT	0x04

#define BCM2708_MCI_TIMEOUT	0x08
#define BCM2708_MCI_CLKDIV	0x0c


#define BCM2708_MCI_RESPONSE0	0x10
#define BCM2708_MCI_RESPONSE1	0x14
#define BCM2708_MCI_RESPONSE2	0x18
#define BCM2708_MCI_RESPONSE3	0x1c

#define BCM2708_MCI_STATUS	0x20

#define BCM2708_MCI_VDD	0x30
#define BCM2708_MCI_VDD_ENABLE	(1 << 0)

#define BCM2708_MCI_EDM	0x34

#define BCM2708_MCI_HOSTCONFIG	0x38

#define BCM2708_MCI_HOSTCONFIG_WIDE_INT_BUS 0x2
#define BCM2708_MCI_HOSTCONFIG_WIDEEXT_4BIT 0x4
#define BCM2708_MCI_HOSTCONFIG_SLOW_CARD 0x8
#define BCM2708_MCI_HOSTCONFIG_BLOCK_IRPT_EN (1<<8)
#define BCM2708_MCI_HOSTCONFIG_BUSY_IRPT_EN (1<<10)
#define BCM2708_MCI_HOSTCONFIG_WIDEEXT_CLR 0xFFFFFFFB


#define BCM2708_MCI_DATAFLAG	(1 << 0)
#define BCM2708_MCI_CMDTIMEOUT	(1 << 6)
#define BCM2708_MCI_HSTS_BLOCK	(1 << 9)	/**< block flag in status reg */
#define BCM2708_MCI_HSTS_BUSY	(1 << 10)	/**< Busy flag in status reg */

#define BCM2708_MCI_HBCT	0x3c
#define BCM2708_MCI_DATA	0x40
#define BCM2708_MCI_HBLC	0x50

#define NR_SG		16

typedef struct bulk_data_struct
{
   unsigned long info;
   unsigned long src;
   unsigned long dst;
   unsigned long length;
   unsigned long stride;
   unsigned long next;
   unsigned long pad[2];
} BCM2708_DMA_CB_T;

struct bcm2708_mci_host {
	struct platform_device	*dev;

	void __iomem		*mmc_base;
	void __iomem		*dma_base;
	void __iomem		*gpio_base;

	BCM2708_DMA_CB_T	*cb_base;
	dma_addr_t		cb_handle;

	struct mmc_host		*mmc;

	struct semaphore 	sem;

	int is_acmd;
	int present;
};

static inline char *bcm2708_mci_kmap_atomic(struct scatterlist *sg, unsigned long *flags)
{
//	local_irq_save(*flags);
	return kmap_atomic(sg_page(sg), KM_BIO_SRC_IRQ) + sg->offset;
}

static inline void bcm2708_mci_kunmap_atomic(void *buffer, unsigned long *flags)
{
	kunmap_atomic(buffer, KM_BIO_SRC_IRQ);
//	local_irq_restore(*flags);
}
