/*
 * BCM2708 legacy DMA API
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
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_data/dma-bcm2708.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>

#include "virt-dma.h"

#define CACHE_LINE_MASK 31
#define DEFAULT_DMACHAN_BITMAP 0x10  /* channel 4 only */

/* valid only for channels 0 - 14, 15 has its own base address */
#define BCM2708_DMA_CHAN(n)	((n) << 8) /* base address */
#define BCM2708_DMA_CHANIO(dma_base, n) \
	((void __iomem *)((char *)(dma_base) + BCM2708_DMA_CHAN(n)))

struct vc_dmaman {
	void __iomem *dma_base;
	u32 chan_available; /* bitmap of available channels */
	u32 has_feature[BCM_DMA_FEATURE_COUNT]; /* bitmap of feature presence */
	struct mutex lock;
};

static struct device *dmaman_dev;	/* we assume there's only one! */
static struct vc_dmaman *g_dmaman;	/* DMA manager */

/* DMA Auxiliary Functions */

/* A DMA buffer on an arbitrary boundary may separate a cache line into a
   section inside the DMA buffer and another section outside it.
   Even if we flush DMA buffers from the cache there is always the chance that
   during a DMA someone will access the part of a cache line that is outside
   the DMA buffer - which will then bring in unwelcome data.
   Without being able to dictate our own buffer pools we must insist that
   DMA buffers consist of a whole number of cache lines.
*/
extern int bcm_sg_suitable_for_dma(struct scatterlist *sg_ptr, int sg_len)
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

extern void bcm_dma_start(void __iomem *dma_chan_base,
			  dma_addr_t control_block)
{
	dsb(sy);	/* ARM data synchronization (push) operation */

	writel(control_block, dma_chan_base + BCM2708_DMA_ADDR);
	writel(BCM2708_DMA_ACTIVE, dma_chan_base + BCM2708_DMA_CS);
}
EXPORT_SYMBOL_GPL(bcm_dma_start);

extern void bcm_dma_wait_idle(void __iomem *dma_chan_base)
{
	dsb(sy);

	/* ugly busy wait only option for now */
	while (readl(dma_chan_base + BCM2708_DMA_CS) & BCM2708_DMA_ACTIVE)
		cpu_relax();
}
EXPORT_SYMBOL_GPL(bcm_dma_wait_idle);

extern bool bcm_dma_is_busy(void __iomem *dma_chan_base)
{
	dsb(sy);

	return readl(dma_chan_base + BCM2708_DMA_CS) & BCM2708_DMA_ACTIVE;
}
EXPORT_SYMBOL_GPL(bcm_dma_is_busy);

/* Complete an ongoing DMA (assuming its results are to be ignored)
   Does nothing if there is no DMA in progress.
   This routine waits for the current AXI transfer to complete before
   terminating the current DMA. If the current transfer is hung on a DREQ used
   by an uncooperative peripheral the AXI transfer may never complete.	In this
   case the routine times out and return a non-zero error code.
   Use of this routine doesn't guarantee that the ongoing or aborted DMA
   does not produce an interrupt.
*/
extern int bcm_dma_abort(void __iomem *dma_chan_base)
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

 /* DMA Manager Device Methods */

static void vc_dmaman_init(struct vc_dmaman *dmaman, void __iomem *dma_base,
			   u32 chans_available)
{
	dmaman->dma_base = dma_base;
	dmaman->chan_available = chans_available;
	dmaman->has_feature[BCM_DMA_FEATURE_FAST_ORD] = 0x0c;  /* 2 & 3 */
	dmaman->has_feature[BCM_DMA_FEATURE_BULK_ORD] = 0x01;  /* 0 */
	dmaman->has_feature[BCM_DMA_FEATURE_NORMAL_ORD] = 0xfe;  /* 1 to 7 */
	dmaman->has_feature[BCM_DMA_FEATURE_LITE_ORD] = 0x7f00;  /* 8 to 14 */
}

static int vc_dmaman_chan_alloc(struct vc_dmaman *dmaman,
				unsigned required_feature_set)
{
	u32 chans;
	int chan = 0;
	int feature;

	chans = dmaman->chan_available;
	for (feature = 0; feature < BCM_DMA_FEATURE_COUNT; feature++)
		/* select the subset of available channels with the desired
		   features */
		if (required_feature_set & (1 << feature))
			chans &= dmaman->has_feature[feature];

	if (!chans)
		return -ENOENT;

	/* return the ordinal of the first channel in the bitmap */
	while (chans != 0 && (chans & 1) == 0) {
		chans >>= 1;
		chan++;
	}
	/* claim the channel */
	dmaman->chan_available &= ~(1 << chan);

	return chan;
}

static int vc_dmaman_chan_free(struct vc_dmaman *dmaman, int chan)
{
	if (chan < 0)
		return -EINVAL;

	if ((1 << chan) & dmaman->chan_available)
		return -EIDRM;

	dmaman->chan_available |= (1 << chan);

	return 0;
}

/* DMA Manager Monitor */

extern int bcm_dma_chan_alloc(unsigned required_feature_set,
			      void __iomem **out_dma_base, int *out_dma_irq)
{
	struct vc_dmaman *dmaman = g_dmaman;
	struct platform_device *pdev = to_platform_device(dmaman_dev);
	int chan;
	int irq;

	if (!dmaman_dev)
		return -ENODEV;

	mutex_lock(&dmaman->lock);
	chan = vc_dmaman_chan_alloc(dmaman, required_feature_set);
	if (chan < 0)
		goto out;

	irq = platform_get_irq(pdev, (unsigned int)chan);
	if (irq < 0) {
		dev_err(dmaman_dev, "failed to get irq for DMA channel %d\n",
			chan);
		vc_dmaman_chan_free(dmaman, chan);
		chan = -ENOENT;
		goto out;
	}

	*out_dma_base = BCM2708_DMA_CHANIO(dmaman->dma_base, chan);
	*out_dma_irq = irq;
	dev_dbg(dmaman_dev,
		"Legacy API allocated channel=%d, base=%p, irq=%i\n",
		chan, *out_dma_base, *out_dma_irq);

out:
	mutex_unlock(&dmaman->lock);

	return chan;
}
EXPORT_SYMBOL_GPL(bcm_dma_chan_alloc);

extern int bcm_dma_chan_free(int channel)
{
	struct vc_dmaman *dmaman = g_dmaman;
	int rc;

	if (!dmaman_dev)
		return -ENODEV;

	mutex_lock(&dmaman->lock);
	rc = vc_dmaman_chan_free(dmaman, channel);
	mutex_unlock(&dmaman->lock);

	return rc;
}
EXPORT_SYMBOL_GPL(bcm_dma_chan_free);

int bcm_dmaman_probe(struct platform_device *pdev, void __iomem *base,
		     u32 chans_available)
{
	struct device *dev = &pdev->dev;
	struct vc_dmaman *dmaman;

	dmaman = devm_kzalloc(dev, sizeof(*dmaman), GFP_KERNEL);
	if (!dmaman)
		return -ENOMEM;

	mutex_init(&dmaman->lock);
	vc_dmaman_init(dmaman, base, chans_available);
	g_dmaman = dmaman;
	dmaman_dev = dev;

	dev_info(dev, "DMA legacy API manager, dmachans=0x%x\n",
		 chans_available);

	return 0;
}
EXPORT_SYMBOL(bcm_dmaman_probe);

int bcm_dmaman_remove(struct platform_device *pdev)
{
	dmaman_dev = NULL;

	return 0;
}
EXPORT_SYMBOL(bcm_dmaman_remove);

MODULE_LICENSE("GPL");
