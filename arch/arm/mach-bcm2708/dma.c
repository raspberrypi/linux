/*
 *  linux/arch/arm/mach-bcm2708/dma.c
 *
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/scatterlist.h>

#include <mach/dma.h>
#include <mach/irqs.h>

/*****************************************************************************\
 *									     *
 * Configuration							     *
 *									     *
\*****************************************************************************/

#define CACHE_LINE_MASK 31
#define DRIVER_NAME BCM_DMAMAN_DRIVER_NAME
#define DEFAULT_DMACHAN_BITMAP 0x10  /* channel 4 only */

/* valid only for channels 0 - 14, 15 has its own base address */
#define BCM2708_DMA_CHAN(n)	((n)<<8) /* base address */
#define BCM2708_DMA_CHANIO(dma_base, n) \
   ((void __iomem *)((char *)(dma_base)+BCM2708_DMA_CHAN(n)))


/*****************************************************************************\
 *									     *
 * DMA Auxilliary Functions						     *
 *									     *
\*****************************************************************************/

/* A DMA buffer on an arbitrary boundary may separate a cache line into a
   section inside the DMA buffer and another section outside it.
   Even if we flush DMA buffers from the cache there is always the chance that
   during a DMA someone will access the part of a cache line that is outside
   the DMA buffer - which will then bring in unwelcome data.
   Without being able to dictate our own buffer pools we must insist that
   DMA buffers consist of a whole number of cache lines.
*/

extern int
bcm_sg_suitable_for_dma(struct scatterlist *sg_ptr, int sg_len)
{
	int i;

	for (i = 0; i < sg_len; i++) {
		if (sg_ptr[i].offset & CACHE_LINE_MASK ||
		    sg_ptr[i].length & CACHE_LINE_MASK)
			return 0;
	}

	return 1;
}
EXPORT_SYMBOL_GPL(bcm_sg_suitable_for_dma);

extern void
bcm_dma_start(void __iomem *dma_chan_base, dma_addr_t control_block)
{
	dsb();	/* ARM data synchronization (push) operation */

	writel(control_block,	     dma_chan_base + BCM2708_DMA_ADDR);
	writel(BCM2708_DMA_ACTIVE,   dma_chan_base + BCM2708_DMA_CS);
}

extern void bcm_dma_wait_idle(void __iomem *dma_chan_base)
{
  dsb();

  /* ugly busy wait only option for now */
  while (readl(dma_chan_base + BCM2708_DMA_CS) & BCM2708_DMA_ACTIVE)
    cpu_relax();
}

EXPORT_SYMBOL_GPL(bcm_dma_start);

extern bool bcm_dma_is_busy(void __iomem *dma_chan_base)
{
	dsb();

	return readl(dma_chan_base + BCM2708_DMA_CS) & BCM2708_DMA_ACTIVE;
}
EXPORT_SYMBOL_GPL(bcm_dma_is_busy);

/* Complete an ongoing DMA (assuming its results are to be ignored)
   Does nothing if there is no DMA in progress.
   This routine waits for the current AXI transfer to complete before
   terminating the current DMA.	 If the current transfer is hung on a DREQ used
   by an uncooperative peripheral the AXI transfer may never complete.	In this
   case the routine times out and return a non-zero error code.
   Use of this routine doesn't guarantee that the ongoing or aborted DMA
   does not produce an interrupt.
*/
extern int
bcm_dma_abort(void __iomem *dma_chan_base)
{
	unsigned long int cs;
	int rc = 0;

	cs = readl(dma_chan_base + BCM2708_DMA_CS);

	if (BCM2708_DMA_ACTIVE & cs) {
		long int timeout = 10000;

		/* write 0 to the active bit - pause the DMA */
		writel(0, dma_chan_base + BCM2708_DMA_CS);

		/* wait for any current AXI transfer to complete */
		while (0 != (cs & BCM2708_DMA_ISPAUSED) && --timeout >= 0)
			cs = readl(dma_chan_base + BCM2708_DMA_CS);

		if (0 != (cs & BCM2708_DMA_ISPAUSED)) {
			/* we'll un-pause when we set of our next DMA */
			rc = -ETIMEDOUT;

		} else if (BCM2708_DMA_ACTIVE & cs) {
			/* terminate the control block chain */
			writel(0, dma_chan_base + BCM2708_DMA_NEXTCB);

			/* abort the whole DMA */
			writel(BCM2708_DMA_ABORT | BCM2708_DMA_ACTIVE,
			       dma_chan_base + BCM2708_DMA_CS);
		}
	}

	return rc;
}
EXPORT_SYMBOL_GPL(bcm_dma_abort);


/***************************************************************************** \
 *									     *
 * DMA Manager Device Methods						     *
 *									     *
\*****************************************************************************/

struct vc_dmaman {
	void __iomem *dma_base;
	u32 chan_available; /* bitmap of available channels */
	u32 has_feature[BCM_DMA_FEATURE_COUNT]; /* bitmap of feature presence */
};

static void vc_dmaman_init(struct vc_dmaman *dmaman, void __iomem *dma_base,
			   u32 chans_available)
{
	dmaman->dma_base = dma_base;
	dmaman->chan_available = chans_available;
	dmaman->has_feature[BCM_DMA_FEATURE_FAST_ORD] = 0x0c;  /* chans 2 & 3 */
	dmaman->has_feature[BCM_DMA_FEATURE_BULK_ORD] = 0x01;  /* chan 0 */
	dmaman->has_feature[BCM_DMA_FEATURE_NORMAL_ORD] = 0xfe;  /* chans 1 to 7 */
	dmaman->has_feature[BCM_DMA_FEATURE_LITE_ORD] = 0x7f00;  /* chans 8 to 14 */
}

static int vc_dmaman_chan_alloc(struct vc_dmaman *dmaman,
				unsigned preferred_feature_set)
{
	u32 chans;
	int feature;

	chans = dmaman->chan_available;
	for (feature = 0; feature < BCM_DMA_FEATURE_COUNT; feature++)
		/* select the subset of available channels with the desired
		   feature so long as some of the candidate channels have that
		   feature */
		if ((preferred_feature_set & (1 << feature)) &&
		    (chans & dmaman->has_feature[feature]))
			chans &= dmaman->has_feature[feature];

	if (chans) {
		int chan = 0;
		/* return the ordinal of the first channel in the bitmap */
		while (chans != 0 && (chans & 1) == 0) {
			chans >>= 1;
			chan++;
		}
		/* claim the channel */
		dmaman->chan_available &= ~(1 << chan);
		return chan;
	} else
		return -ENOMEM;
}

static int vc_dmaman_chan_free(struct vc_dmaman *dmaman, int chan)
{
	if (chan < 0)
		return -EINVAL;
	else if ((1 << chan) & dmaman->chan_available)
		return -EIDRM;
	else {
		dmaman->chan_available |= (1 << chan);
		return 0;
	}
}

/*****************************************************************************\
 *									     *
 * DMA IRQs								     *
 *									     *
\*****************************************************************************/

static unsigned char bcm_dma_irqs[] = {
	IRQ_DMA0,
	IRQ_DMA1,
	IRQ_DMA2,
	IRQ_DMA3,
	IRQ_DMA4,
	IRQ_DMA5,
	IRQ_DMA6,
	IRQ_DMA7,
	IRQ_DMA8,
	IRQ_DMA9,
	IRQ_DMA10,
	IRQ_DMA11,
	IRQ_DMA12
};


/***************************************************************************** \
 *									     *
 * DMA Manager Monitor							     *
 *									     *
\*****************************************************************************/

static struct device *dmaman_dev;	/* we assume there's only one! */

extern int bcm_dma_chan_alloc(unsigned preferred_feature_set,
			      void __iomem **out_dma_base, int *out_dma_irq)
{
	if (!dmaman_dev)
		return -ENODEV;
	else {
		struct vc_dmaman *dmaman  = dev_get_drvdata(dmaman_dev);
		int rc;

		device_lock(dmaman_dev);
		rc = vc_dmaman_chan_alloc(dmaman, preferred_feature_set);
		if (rc >= 0) {
			*out_dma_base = BCM2708_DMA_CHANIO(dmaman->dma_base,
							   rc);
			*out_dma_irq = bcm_dma_irqs[rc];
		}
		device_unlock(dmaman_dev);

		return rc;
	}
}
EXPORT_SYMBOL_GPL(bcm_dma_chan_alloc);

extern int bcm_dma_chan_free(int channel)
{
	if (dmaman_dev) {
		struct vc_dmaman *dmaman = dev_get_drvdata(dmaman_dev);
		int rc;

		device_lock(dmaman_dev);
		rc = vc_dmaman_chan_free(dmaman, channel);
		device_unlock(dmaman_dev);

		return rc;
	} else
		return -ENODEV;
}
EXPORT_SYMBOL_GPL(bcm_dma_chan_free);

static int dev_dmaman_register(const char *dev_name, struct device *dev)
{
	int rc = dmaman_dev ? -EINVAL : 0;
	dmaman_dev = dev;
	return rc;
}

static void dev_dmaman_deregister(const char *dev_name, struct device *dev)
{
	dmaman_dev = NULL;
}

/*****************************************************************************\
 *									     *
 * DMA Device								     *
 *									     *
\*****************************************************************************/

static int dmachans = -1; /* module parameter */

static int bcm_dmaman_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct vc_dmaman *dmaman;
	struct resource *dma_res = NULL;
	void __iomem *dma_base = NULL;
	int have_dma_region = 0;

	dmaman = kzalloc(sizeof(*dmaman), GFP_KERNEL);
	if (NULL == dmaman) {
		printk(KERN_ERR DRIVER_NAME ": failed to allocate "
		       "DMA management memory\n");
		ret = -ENOMEM;
	} else {

		dma_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (dma_res == NULL) {
			printk(KERN_ERR DRIVER_NAME ": failed to obtain memory "
			       "resource\n");
			ret = -ENODEV;
		} else if (!request_mem_region(dma_res->start,
					       resource_size(dma_res),
					       DRIVER_NAME)) {
			dev_err(&pdev->dev, "cannot obtain DMA region\n");
			ret = -EBUSY;
		} else {
			have_dma_region = 1;
			dma_base = ioremap(dma_res->start,
					   resource_size(dma_res));
			if (!dma_base) {
				dev_err(&pdev->dev, "cannot map DMA region\n");
				ret = -ENOMEM;
			} else {
				/* use module parameter if one was provided */
				if (dmachans > 0)
					vc_dmaman_init(dmaman, dma_base,
						       dmachans);
				else
					vc_dmaman_init(dmaman, dma_base,
						       DEFAULT_DMACHAN_BITMAP);

				platform_set_drvdata(pdev, dmaman);
				dev_dmaman_register(DRIVER_NAME, &pdev->dev);

				printk(KERN_INFO DRIVER_NAME ": DMA manager "
				       "at %p\n", dma_base);
			}
		}
	}
	if (ret != 0) {
		if (dma_base)
			iounmap(dma_base);
		if (dma_res && have_dma_region)
			release_mem_region(dma_res->start,
					   resource_size(dma_res));
		if (dmaman)
			kfree(dmaman);
	}
	return ret;
}

static int bcm_dmaman_remove(struct platform_device *pdev)
{
	struct vc_dmaman *dmaman = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	dev_dmaman_deregister(DRIVER_NAME, &pdev->dev);
	kfree(dmaman);

	return 0;
}

static struct platform_driver bcm_dmaman_driver = {
	.probe = bcm_dmaman_probe,
	.remove = bcm_dmaman_remove,

	.driver = {
		   .name = DRIVER_NAME,
		   .owner = THIS_MODULE,
		   },
};

/*****************************************************************************\
 *									     *
 * Driver init/exit							     *
 *									     *
\*****************************************************************************/

static int __init bcm_dmaman_drv_init(void)
{
	int ret;

	ret = platform_driver_register(&bcm_dmaman_driver);
	if (ret != 0) {
		printk(KERN_ERR DRIVER_NAME ": failed to register "
		       "on platform\n");
	}

	return ret;
}

static void __exit bcm_dmaman_drv_exit(void)
{
	platform_driver_unregister(&bcm_dmaman_driver);
}

module_init(bcm_dmaman_drv_init);
module_exit(bcm_dmaman_drv_exit);

module_param(dmachans, int, 0644);

MODULE_AUTHOR("Gray Girling <grayg@broadcom.com>");
MODULE_DESCRIPTION("DMA channel manager driver");
MODULE_LICENSE("GPL");

MODULE_PARM_DESC(dmachans, "Bitmap of DMA channels available to the ARM");
