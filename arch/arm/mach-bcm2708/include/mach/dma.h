/*
 *  linux/arch/arm/mach-bcm2708/include/mach/dma.h
 *
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */


#ifndef _MACH_BCM2708_DMA_H
#define _MACH_BCM2708_DMA_H

#define BCM_DMAMAN_DRIVER_NAME "bcm2708_dma"

/* DMA CS Control and Status bits */
#define BCM2708_DMA_ACTIVE	(1 << 0)
#define BCM2708_DMA_INT		(1 << 2)
#define BCM2708_DMA_ISPAUSED	(1 << 4)  /* Pause requested or not active */
#define BCM2708_DMA_ISHELD	(1 << 5)  /* Is held by DREQ flow control */
#define BCM2708_DMA_ERR		(1 << 8)
#define BCM2708_DMA_ABORT	(1 << 30) /* stop current CB, go to next, WO */
#define BCM2708_DMA_RESET	(1 << 31) /* WO, self clearing */

/* DMA control block "info" field bits */
#define BCM2708_DMA_INT_EN	(1 << 0)
#define BCM2708_DMA_TDMODE	(1 << 1)
#define BCM2708_DMA_WAIT_RESP	(1 << 3)
#define BCM2708_DMA_D_INC	(1 << 4)
#define BCM2708_DMA_D_WIDTH	(1 << 5)
#define BCM2708_DMA_D_DREQ	(1 << 6)
#define BCM2708_DMA_S_INC	(1 << 8)
#define BCM2708_DMA_S_WIDTH	(1 << 9)
#define BCM2708_DMA_S_DREQ	(1 << 10)

#define	BCM2708_DMA_BURST(x)	(((x)&0xf) << 12)
#define	BCM2708_DMA_PER_MAP(x)	((x) << 16)
#define	BCM2708_DMA_WAITS(x)	(((x)&0x1f) << 21)

#define BCM2708_DMA_DREQ_EMMC	11
#define BCM2708_DMA_DREQ_SDHOST	13

#define BCM2708_DMA_CS		0x00 /* Control and Status */
#define BCM2708_DMA_ADDR	0x04
/* the current control block appears in the following registers - read only */
#define BCM2708_DMA_INFO	0x08
#define BCM2708_DMA_NEXTCB	0x1C
#define BCM2708_DMA_DEBUG	0x20

#define BCM2708_DMA4_CS		(BCM2708_DMA_CHAN(4)+BCM2708_DMA_CS)
#define BCM2708_DMA4_ADDR	(BCM2708_DMA_CHAN(4)+BCM2708_DMA_ADDR)

#define BCM2708_DMA_TDMODE_LEN(w, h) ((h) << 16 | (w))

struct bcm2708_dma_cb {
	unsigned long info;
	unsigned long src;
	unsigned long dst;
	unsigned long length;
	unsigned long stride;
	unsigned long next;
	unsigned long pad[2];
};

extern int bcm_sg_suitable_for_dma(struct scatterlist *sg_ptr, int sg_len);
extern void bcm_dma_start(void __iomem *dma_chan_base,
			  dma_addr_t control_block);
extern void bcm_dma_wait_idle(void __iomem *dma_chan_base);
extern int /*rc*/ bcm_dma_abort(void __iomem *dma_chan_base);

/* When listing features we can ask for when allocating DMA channels give
   those with higher priority smaller ordinal numbers */
#define BCM_DMA_FEATURE_FAST_ORD 0
#define BCM_DMA_FEATURE_BULK_ORD 1
#define BCM_DMA_FEATURE_FAST	 (1<<BCM_DMA_FEATURE_FAST_ORD)
#define BCM_DMA_FEATURE_BULK	 (1<<BCM_DMA_FEATURE_BULK_ORD)
#define BCM_DMA_FEATURE_COUNT	 2

/* return channel no or -ve error */
extern int bcm_dma_chan_alloc(unsigned preferred_feature_set,
			      void __iomem **out_dma_base, int *out_dma_irq);
extern int bcm_dma_chan_free(int channel);


#endif /* _MACH_BCM2708_DMA_H */
