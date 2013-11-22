/*
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _PLAT_BCM2708_DMA_H
#define _PLAT_BCM2708_DMA_H

/* DMA CS Control and Status bits */
#define BCM2708_DMA_ACTIVE	BIT(0)
#define BCM2708_DMA_INT		BIT(2)
#define BCM2708_DMA_ISPAUSED	BIT(4)  /* Pause requested or not active */
#define BCM2708_DMA_ISHELD	BIT(5)  /* Is held by DREQ flow control */
#define BCM2708_DMA_ERR		BIT(8)
#define BCM2708_DMA_ABORT	BIT(30) /* stop current CB, go to next, WO */
#define BCM2708_DMA_RESET	BIT(31) /* WO, self clearing */

/* DMA control block "info" field bits */
#define BCM2708_DMA_INT_EN	BIT(0)
#define BCM2708_DMA_TDMODE	BIT(1)
#define BCM2708_DMA_WAIT_RESP	BIT(3)
#define BCM2708_DMA_D_INC	BIT(4)
#define BCM2708_DMA_D_WIDTH	BIT(5)
#define BCM2708_DMA_D_DREQ	BIT(6)
#define BCM2708_DMA_S_INC	BIT(8)
#define BCM2708_DMA_S_WIDTH	BIT(9)
#define BCM2708_DMA_S_DREQ	BIT(10)

#define	BCM2708_DMA_BURST(x)	(((x) & 0xf) << 12)
#define	BCM2708_DMA_PER_MAP(x)	((x) << 16)
#define	BCM2708_DMA_WAITS(x)	(((x) & 0x1f) << 21)

#define BCM2708_DMA_DREQ_EMMC	11
#define BCM2708_DMA_DREQ_SDHOST	13

#define BCM2708_DMA_CS		0x00 /* Control and Status */
#define BCM2708_DMA_ADDR	0x04
/* the current control block appears in the following registers - read only */
#define BCM2708_DMA_INFO	0x08
#define BCM2708_DMA_SOURCE_AD	0x0c
#define BCM2708_DMA_DEST_AD	0x10
#define BCM2708_DMA_NEXTCB	0x1C
#define BCM2708_DMA_DEBUG	0x20

#define BCM2708_DMA4_CS		(BCM2708_DMA_CHAN(4) + BCM2708_DMA_CS)
#define BCM2708_DMA4_ADDR	(BCM2708_DMA_CHAN(4) + BCM2708_DMA_ADDR)

#define BCM2708_DMA_TDMODE_LEN(w, h) ((h) << 16 | (w))

/* When listing features we can ask for when allocating DMA channels give
   those with higher priority smaller ordinal numbers */
#define BCM_DMA_FEATURE_FAST_ORD	0
#define BCM_DMA_FEATURE_BULK_ORD	1
#define BCM_DMA_FEATURE_NORMAL_ORD	2
#define BCM_DMA_FEATURE_LITE_ORD	3
#define BCM_DMA_FEATURE_FAST		BIT(BCM_DMA_FEATURE_FAST_ORD)
#define BCM_DMA_FEATURE_BULK		BIT(BCM_DMA_FEATURE_BULK_ORD)
#define BCM_DMA_FEATURE_NORMAL		BIT(BCM_DMA_FEATURE_NORMAL_ORD)
#define BCM_DMA_FEATURE_LITE		BIT(BCM_DMA_FEATURE_LITE_ORD)
#define BCM_DMA_FEATURE_COUNT		4

struct bcm2708_dma_cb {
	unsigned long info;
	unsigned long src;
	unsigned long dst;
	unsigned long length;
	unsigned long stride;
	unsigned long next;
	unsigned long pad[2];
};

struct scatterlist;

#ifdef CONFIG_DMA_BCM2708_LEGACY

int bcm_sg_suitable_for_dma(struct scatterlist *sg_ptr, int sg_len);
void bcm_dma_start(void __iomem *dma_chan_base, dma_addr_t control_block);
void bcm_dma_wait_idle(void __iomem *dma_chan_base);
bool bcm_dma_is_busy(void __iomem *dma_chan_base);
int bcm_dma_abort(void __iomem *dma_chan_base);

/* return channel no or -ve error */
int bcm_dma_chan_alloc(unsigned preferred_feature_set,
		       void __iomem **out_dma_base, int *out_dma_irq);
int bcm_dma_chan_free(int channel);

#else /* CONFIG_DMA_BCM2708_LEGACY */

static inline int bcm_sg_suitable_for_dma(struct scatterlist *sg_ptr,
					  int sg_len)
{
	return 0;
}

static inline void bcm_dma_start(void __iomem *dma_chan_base,
				 dma_addr_t control_block) { }

static inline void bcm_dma_wait_idle(void __iomem *dma_chan_base) { }

static inline bool bcm_dma_is_busy(void __iomem *dma_chan_base)
{
	return false;
}

static inline int bcm_dma_abort(void __iomem *dma_chan_base)
{
	return -EINVAL;
}

static inline int bcm_dma_chan_alloc(unsigned preferred_feature_set,
				     void __iomem **out_dma_base,
				     int *out_dma_irq)
{
	return -EINVAL;
}

static inline int bcm_dma_chan_free(int channel)
{
	return -EINVAL;
}

#endif /* CONFIG_DMA_BCM2708_LEGACY */

#endif /* _PLAT_BCM2708_DMA_H */
