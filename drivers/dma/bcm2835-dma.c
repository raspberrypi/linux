// SPDX-License-Identifier: GPL-2.0+
/*
 * BCM2835 DMA engine support
 *
 * Author:      Florian Meier <florian.meier@koalo.de>
 *              Copyright 2013
 *
 * Based on
 *	OMAP DMAengine support by Russell King
 *
 *	BCM2708 DMA Driver
 *	Copyright (C) 2010 Broadcom
 *
 *	Raspberry Pi PCM I2S ALSA Driver
 *	Copyright (c) by Phil Poole 2013
 *
 *	MARVELL MMP Peripheral DMA Driver
 *	Copyright 2012 Marvell International Ltd.
 */
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_data/dma-bcm2708.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/of.h>
#include <linux/of_dma.h>

#include "virt-dma.h"

#define BCM2835_DMA_MAX_DMA_CHAN_SUPPORTED 14
#define BCM2835_DMA_CHAN_NAME_SIZE 8
#define BCM2835_DMA_BULK_MASK  BIT(0)
#define BCM2711_DMA_MEMCPY_CHAN 14

struct bcm2835_dma_cfg_data {
	u32	chan_40bit_mask;
};

/**
 * struct bcm2835_dmadev - BCM2835 DMA controller
 * @ddev: DMA device
 * @base: base address of register map
 * @zero_page: bus address of zero page (to detect transactions copying from
 *	zero page and avoid accessing memory if so)
 */
struct bcm2835_dmadev {
	struct dma_device ddev;
	void __iomem *base;
	dma_addr_t zero_page;
	const struct bcm2835_dma_cfg_data *cfg_data;
};

struct bcm2835_dma_cb {
	uint32_t info;
	uint32_t src;
	uint32_t dst;
	uint32_t length;
	uint32_t stride;
	uint32_t next;
	uint32_t pad[2];
};

struct bcm2711_dma40_scb {
	uint32_t ti;
	uint32_t src;
	uint32_t srci;
	uint32_t dst;
	uint32_t dsti;
	uint32_t len;
	uint32_t next_cb;
	uint32_t rsvd;
};

struct bcm2835_cb_entry {
	struct bcm2835_dma_cb *cb;
	dma_addr_t paddr;
};

struct bcm2835_chan {
	struct virt_dma_chan vc;

	struct dma_slave_config	cfg;
	unsigned int dreq;

	int ch;
	struct bcm2835_desc *desc;
	struct dma_pool *cb_pool;

	void __iomem *chan_base;
	int irq_number;
	unsigned int irq_flags;

	bool is_lite_channel;
	bool is_40bit_channel;
};

struct bcm2835_desc {
	struct bcm2835_chan *c;
	struct virt_dma_desc vd;
	enum dma_transfer_direction dir;

	unsigned int frames;
	size_t size;

	bool cyclic;

	struct bcm2835_cb_entry cb_list[];
};

#define BCM2835_DMA_CS		0x00
#define BCM2835_DMA_ADDR	0x04
#define BCM2835_DMA_TI		0x08
#define BCM2835_DMA_SOURCE_AD	0x0c
#define BCM2835_DMA_DEST_AD	0x10
#define BCM2835_DMA_LEN		0x14
#define BCM2835_DMA_STRIDE	0x18
#define BCM2835_DMA_NEXTCB	0x1c
#define BCM2835_DMA_DEBUG	0x20

/* DMA CS Control and Status bits */
#define BCM2835_DMA_ACTIVE	BIT(0)  /* activate the DMA */
#define BCM2835_DMA_END		BIT(1)  /* current CB has ended */
#define BCM2835_DMA_INT		BIT(2)  /* interrupt status */
#define BCM2835_DMA_DREQ	BIT(3)  /* DREQ state */
#define BCM2835_DMA_ISPAUSED	BIT(4)  /* Pause requested or not active */
#define BCM2835_DMA_ISHELD	BIT(5)  /* Is held by DREQ flow control */
#define BCM2835_DMA_WAITING_FOR_WRITES BIT(6) /* waiting for last
					       * AXI-write to ack
					       */
#define BCM2835_DMA_ERR		BIT(8)
#define BCM2835_DMA_PRIORITY(x) ((x & 15) << 16) /* AXI priority */
#define BCM2835_DMA_PANIC_PRIORITY(x) ((x & 15) << 20) /* panic priority */
/* current value of TI.BCM2835_DMA_WAIT_RESP */
#define BCM2835_DMA_WAIT_FOR_WRITES BIT(28)
#define BCM2835_DMA_DIS_DEBUG	BIT(29) /* disable debug pause signal */
#define BCM2835_DMA_ABORT	BIT(30) /* Stop current CB, go to next, WO */
#define BCM2835_DMA_RESET	BIT(31) /* WO, self clearing */

/* Transfer information bits - also bcm2835_cb.info field */
#define BCM2835_DMA_INT_EN	BIT(0)
#define BCM2835_DMA_TDMODE	BIT(1) /* 2D-Mode */
#define BCM2835_DMA_WAIT_RESP	BIT(3) /* wait for AXI-write to be acked */
#define BCM2835_DMA_D_INC	BIT(4)
#define BCM2835_DMA_D_WIDTH	BIT(5) /* 128bit writes if set */
#define BCM2835_DMA_D_DREQ	BIT(6) /* enable DREQ for destination */
#define BCM2835_DMA_D_IGNORE	BIT(7) /* ignore destination writes */
#define BCM2835_DMA_S_INC	BIT(8)
#define BCM2835_DMA_S_WIDTH	BIT(9) /* 128bit writes if set */
#define BCM2835_DMA_S_DREQ	BIT(10) /* enable SREQ for source */
#define BCM2835_DMA_S_IGNORE	BIT(11) /* ignore source reads - read 0 */
#define BCM2835_DMA_BURST_LENGTH(x) ((x & 15) << 12)
#define BCM2835_DMA_CS_FLAGS(x) (x & (BCM2835_DMA_PRIORITY(15) | \
				      BCM2835_DMA_PANIC_PRIORITY(15) | \
				      BCM2835_DMA_WAIT_FOR_WRITES | \
				      BCM2835_DMA_DIS_DEBUG))
#define BCM2835_DMA_PER_MAP(x)	((x & 31) << 16) /* REQ source */
#define BCM2835_DMA_WAIT(x)	((x & 31) << 21) /* add DMA-wait cycles */
#define BCM2835_DMA_NO_WIDE_BURSTS BIT(26) /* no 2 beat write bursts */

/* A fake bit to request that the driver doesn't set the WAIT_RESP bit. */
#define BCM2835_DMA_NO_WAIT_RESP BIT(27)
#define WAIT_RESP(x) ((x & BCM2835_DMA_NO_WAIT_RESP) ? \
		      0 : BCM2835_DMA_WAIT_RESP)

/* debug register bits */
#define BCM2835_DMA_DEBUG_LAST_NOT_SET_ERR	BIT(0)
#define BCM2835_DMA_DEBUG_FIFO_ERR		BIT(1)
#define BCM2835_DMA_DEBUG_READ_ERR		BIT(2)
#define BCM2835_DMA_DEBUG_OUTSTANDING_WRITES_SHIFT 4
#define BCM2835_DMA_DEBUG_OUTSTANDING_WRITES_BITS 4
#define BCM2835_DMA_DEBUG_ID_SHIFT		16
#define BCM2835_DMA_DEBUG_ID_BITS		9
#define BCM2835_DMA_DEBUG_STATE_SHIFT		16
#define BCM2835_DMA_DEBUG_STATE_BITS		9
#define BCM2835_DMA_DEBUG_VERSION_SHIFT		25
#define BCM2835_DMA_DEBUG_VERSION_BITS		3
#define BCM2835_DMA_DEBUG_LITE			BIT(28)

/* shared registers for all dma channels */
#define BCM2835_DMA_INT_STATUS         0xfe0
#define BCM2835_DMA_ENABLE             0xff0

#define BCM2835_DMA_DATA_TYPE_S8	1
#define BCM2835_DMA_DATA_TYPE_S16	2
#define BCM2835_DMA_DATA_TYPE_S32	4
#define BCM2835_DMA_DATA_TYPE_S128	16

/* Valid only for channels 0 - 14, 15 has its own base address */
#define BCM2835_DMA_CHAN_SIZE	0x100
#define BCM2835_DMA_CHAN(n)	((n) * BCM2835_DMA_CHAN_SIZE) /* Base address */
#define BCM2835_DMA_CHANIO(base, n) ((base) + BCM2835_DMA_CHAN(n))

/* the max dma length for different channels */
#define MAX_DMA_LEN SZ_1G
#define MAX_LITE_DMA_LEN (SZ_64K - 4)

/* 40-bit DMA support */
#define BCM2711_DMA40_CS	0x00
#define BCM2711_DMA40_CB	0x04
#define BCM2711_DMA40_DEBUG	0x0c
#define BCM2711_DMA40_TI	0x10
#define BCM2711_DMA40_SRC	0x14
#define BCM2711_DMA40_SRCI	0x18
#define BCM2711_DMA40_DEST	0x1c
#define BCM2711_DMA40_DESTI	0x20
#define BCM2711_DMA40_LEN	0x24
#define BCM2711_DMA40_NEXT_CB	0x28
#define BCM2711_DMA40_DEBUG2	0x2c

#define BCM2711_DMA40_ACTIVE		BIT(0)
#define BCM2711_DMA40_END		BIT(1)
#define BCM2711_DMA40_INT		BIT(2)
#define BCM2711_DMA40_DREQ		BIT(3)  /* DREQ state */
#define BCM2711_DMA40_RD_PAUSED		BIT(4)  /* Reading is paused */
#define BCM2711_DMA40_WR_PAUSED		BIT(5)  /* Writing is paused */
#define BCM2711_DMA40_DREQ_PAUSED	BIT(6)  /* Is paused by DREQ flow control */
#define BCM2711_DMA40_WAITING_FOR_WRITES BIT(7)  /* Waiting for last write */
#define BCM2711_DMA40_ERR		BIT(10)
#define BCM2711_DMA40_QOS(x)		(((x) & 0x1f) << 16)
#define BCM2711_DMA40_PANIC_QOS(x)	(((x) & 0x1f) << 20)
#define BCM2711_DMA40_WAIT_FOR_WRITES	BIT(28)
#define BCM2711_DMA40_DISDEBUG		BIT(29)
#define BCM2711_DMA40_ABORT		BIT(30)
#define BCM2711_DMA40_HALT		BIT(31)
#define BCM2711_DMA40_CS_FLAGS(x) (x & (BCM2711_DMA40_QOS(15) | \
					BCM2711_DMA40_PANIC_QOS(15) | \
					BCM2711_DMA40_WAIT_FOR_WRITES |	\
					BCM2711_DMA40_DISDEBUG))

/* Transfer information bits */
#define BCM2711_DMA40_INTEN		BIT(0)
#define BCM2711_DMA40_TDMODE		BIT(1) /* 2D-Mode */
#define BCM2711_DMA40_WAIT_RESP		BIT(2) /* wait for AXI write to be acked */
#define BCM2711_DMA40_WAIT_RD_RESP	BIT(3) /* wait for AXI read to complete */
#define BCM2711_DMA40_PER_MAP(x)	((x & 31) << 9) /* REQ source */
#define BCM2711_DMA40_S_DREQ		BIT(14) /* enable SREQ for source */
#define BCM2711_DMA40_D_DREQ		BIT(15) /* enable DREQ for destination */
#define BCM2711_DMA40_S_WAIT(x)		((x & 0xff) << 16) /* add DMA read-wait cycles */
#define BCM2711_DMA40_D_WAIT(x)		((x & 0xff) << 24) /* add DMA write-wait cycles */

/* debug register bits */
#define BCM2711_DMA40_DEBUG_WRITE_ERR		BIT(0)
#define BCM2711_DMA40_DEBUG_FIFO_ERR		BIT(1)
#define BCM2711_DMA40_DEBUG_READ_ERR		BIT(2)
#define BCM2711_DMA40_DEBUG_READ_CB_ERR		BIT(3)
#define BCM2711_DMA40_DEBUG_IN_ON_ERR		BIT(8)
#define BCM2711_DMA40_DEBUG_ABORT_ON_ERR	BIT(9)
#define BCM2711_DMA40_DEBUG_HALT_ON_ERR		BIT(10)
#define BCM2711_DMA40_DEBUG_DISABLE_CLK_GATE	BIT(11)
#define BCM2711_DMA40_DEBUG_RSTATE_SHIFT	14
#define BCM2711_DMA40_DEBUG_RSTATE_BITS		4
#define BCM2711_DMA40_DEBUG_WSTATE_SHIFT	18
#define BCM2711_DMA40_DEBUG_WSTATE_BITS		4
#define BCM2711_DMA40_DEBUG_RESET		BIT(23)
#define BCM2711_DMA40_DEBUG_ID_SHIFT		24
#define BCM2711_DMA40_DEBUG_ID_BITS		4
#define BCM2711_DMA40_DEBUG_VERSION_SHIFT	28
#define BCM2711_DMA40_DEBUG_VERSION_BITS	4

/* Valid only for channels 0 - 3 (11 - 14) */
#define BCM2711_DMA40_CHAN(n)	(((n) + 11) << 8) /* Base address */
#define BCM2711_DMA40_CHANIO(base, n) ((base) + BCM2711_DMA_CHAN(n))

/* the max dma length for different channels */
#define MAX_DMA40_LEN SZ_1G

#define BCM2711_DMA40_BURST_LEN(x)	((min(x,16) - 1) << 8)
#define BCM2711_DMA40_INC		BIT(12)
#define BCM2711_DMA40_SIZE_32		(0 << 13)
#define BCM2711_DMA40_SIZE_64		(1 << 13)
#define BCM2711_DMA40_SIZE_128		(2 << 13)
#define BCM2711_DMA40_SIZE_256		(3 << 13)
#define BCM2711_DMA40_IGNORE		BIT(15)
#define BCM2711_DMA40_STRIDE(x)		((x) << 16) /* For 2D mode */

#define BCM2711_DMA40_MEMCPY_FLAGS \
	(BCM2711_DMA40_QOS(0) | \
	 BCM2711_DMA40_PANIC_QOS(0) | \
	 BCM2711_DMA40_WAIT_FOR_WRITES | \
	 BCM2711_DMA40_DISDEBUG)

#define BCM2711_DMA40_MEMCPY_XFER_INFO \
	(BCM2711_DMA40_SIZE_128 | \
	 BCM2711_DMA40_INC | \
	 BCM2711_DMA40_BURST_LEN(16))

struct bcm2835_dmadev *memcpy_parent;
static void __iomem *memcpy_chan;
static struct bcm2711_dma40_scb *memcpy_scb;
static dma_addr_t memcpy_scb_dma;
DEFINE_SPINLOCK(memcpy_lock);

static const struct bcm2835_dma_cfg_data bcm2835_dma_cfg = {
	.chan_40bit_mask = 0,
};

static const struct bcm2835_dma_cfg_data bcm2711_dma_cfg = {
	.chan_40bit_mask = BIT(11) | BIT(12) | BIT(13) | BIT(14),
};

static inline size_t bcm2835_dma_max_frame_length(struct bcm2835_chan *c)
{
	/* lite and normal channels have different max frame length */
	return c->is_lite_channel ? MAX_LITE_DMA_LEN : MAX_DMA_LEN;
}

/* how many frames of max_len size do we need to transfer len bytes */
static inline size_t bcm2835_dma_frames_for_length(size_t len,
						   size_t max_len)
{
	return DIV_ROUND_UP(len, max_len);
}

static inline struct bcm2835_dmadev *to_bcm2835_dma_dev(struct dma_device *d)
{
	return container_of(d, struct bcm2835_dmadev, ddev);
}

static inline struct bcm2835_chan *to_bcm2835_dma_chan(struct dma_chan *c)
{
	return container_of(c, struct bcm2835_chan, vc.chan);
}

static inline struct bcm2835_desc *to_bcm2835_dma_desc(
		struct dma_async_tx_descriptor *t)
{
	return container_of(t, struct bcm2835_desc, vd.tx);
}

static inline uint32_t to_bcm2711_ti(uint32_t info)
{
	return ((info & BCM2835_DMA_INT_EN) ? BCM2711_DMA40_INTEN : 0) |
		((info & BCM2835_DMA_WAIT_RESP) ? BCM2711_DMA40_WAIT_RESP : 0) |
		((info & BCM2835_DMA_S_DREQ) ?
		 (BCM2711_DMA40_S_DREQ | BCM2711_DMA40_WAIT_RD_RESP) : 0) |
		((info & BCM2835_DMA_D_DREQ) ? BCM2711_DMA40_D_DREQ : 0) |
		BCM2711_DMA40_PER_MAP((info >> 16) & 0x1f);
}

static inline uint32_t to_bcm2711_srci(uint32_t info)
{
	return ((info & BCM2835_DMA_S_INC) ? BCM2711_DMA40_INC : 0);
}

static inline uint32_t to_bcm2711_dsti(uint32_t info)
{
	return ((info & BCM2835_DMA_D_INC) ? BCM2711_DMA40_INC : 0);
}

static inline uint32_t to_bcm2711_cbaddr(dma_addr_t addr)
{
	BUG_ON(addr & 0x1f);
	return (addr >> 5);
}

static void bcm2835_dma_free_cb_chain(struct bcm2835_desc *desc)
{
	size_t i;

	for (i = 0; i < desc->frames; i++)
		dma_pool_free(desc->c->cb_pool, desc->cb_list[i].cb,
			      desc->cb_list[i].paddr);

	kfree(desc);
}

static void bcm2835_dma_desc_free(struct virt_dma_desc *vd)
{
	bcm2835_dma_free_cb_chain(
		container_of(vd, struct bcm2835_desc, vd));
}

static void bcm2835_dma_create_cb_set_length(
	struct bcm2835_chan *c,
	struct bcm2835_dma_cb *control_block,
	size_t len,
	size_t period_len,
	size_t *total_len,
	u32 finalextrainfo)
{
	size_t max_len = bcm2835_dma_max_frame_length(c);
	uint32_t cb_len;

	/* set the length taking lite-channel limitations into account */
	cb_len = min_t(u32, len, max_len);

	if (period_len) {
		/*
		 * period_len means: that we need to generate
		 * transfers that are terminating at every
		 * multiple of period_len - this is typically
		 * used to set the interrupt flag in info
		 * which is required during cyclic transfers
		 */

		/* have we filled in period_length yet? */
		if (*total_len + cb_len < period_len) {
			/* update number of bytes in this period so far */
			*total_len += cb_len;
		} else {
			/* calculate the length that remains to reach period_len */
			cb_len = period_len - *total_len;

			/* reset total_length for next period */
			*total_len = 0;
		}
	}

	if (c->is_40bit_channel) {
		struct bcm2711_dma40_scb *scb =
			(struct bcm2711_dma40_scb *)control_block;

		scb->len = cb_len;
		/* add extrainfo bits to ti */
		scb->ti |= to_bcm2711_ti(finalextrainfo);
	} else {
		control_block->length = cb_len;
		/* add extrainfo bits to info */
		control_block->info |= finalextrainfo;
	}
}

static inline size_t bcm2835_dma_count_frames_for_sg(
	struct bcm2835_chan *c,
	struct scatterlist *sgl,
	unsigned int sg_len)
{
	size_t frames = 0;
	struct scatterlist *sgent;
	unsigned int i;
	size_t plength = bcm2835_dma_max_frame_length(c);

	for_each_sg(sgl, sgent, sg_len, i)
		frames += bcm2835_dma_frames_for_length(
			sg_dma_len(sgent), plength);

	return frames;
}

/**
 * bcm2835_dma_create_cb_chain - create a control block and fills data in
 *
 * @c:              the @bcm2835_chan for which we run this
 * @direction:      the direction in which we transfer
 * @cyclic:         it is a cyclic transfer
 * @info:           the default info bits to apply per controlblock
 * @frames:         number of controlblocks to allocate
 * @src:            the src address to assign (if the S_INC bit is set
 *                  in @info, then it gets incremented)
 * @dst:            the dst address to assign (if the D_INC bit is set
 *                  in @info, then it gets incremented)
 * @buf_len:        the full buffer length (may also be 0)
 * @period_len:     the period length when to apply @finalextrainfo
 *                  in addition to the last transfer
 *                  this will also break some control-blocks early
 * @finalextrainfo: additional bits in last controlblock
 *                  (or when period_len is reached in case of cyclic)
 * @gfp:            the GFP flag to use for allocation
 */
static struct bcm2835_desc *bcm2835_dma_create_cb_chain(
	struct bcm2835_chan *c, enum dma_transfer_direction direction,
	bool cyclic, u32 info, u32 finalextrainfo, size_t frames,
	dma_addr_t src, dma_addr_t dst, size_t buf_len,
	size_t period_len, gfp_t gfp)
{
	size_t len = buf_len, total_len;
	size_t frame;
	struct bcm2835_desc *d;
	struct bcm2835_cb_entry *cb_entry;
	struct bcm2835_dma_cb *control_block;

	if (!frames)
		return NULL;

	/* allocate and setup the descriptor. */
	d = kzalloc(struct_size(d, cb_list, frames), gfp);
	if (!d)
		return NULL;

	d->c = c;
	d->dir = direction;
	d->cyclic = cyclic;

	/*
	 * Iterate over all frames, create a control block
	 * for each frame and link them together.
	 */
	for (frame = 0, total_len = 0; frame < frames; d->frames++, frame++) {
		cb_entry = &d->cb_list[frame];
		cb_entry->cb = dma_pool_alloc(c->cb_pool, gfp,
					      &cb_entry->paddr);
		if (!cb_entry->cb)
			goto error_cb;

		/* fill in the control block */
		control_block = cb_entry->cb;
		if (c->is_40bit_channel) {
			struct bcm2711_dma40_scb *scb =
				(struct bcm2711_dma40_scb *)control_block;
			scb->ti = to_bcm2711_ti(info);
			scb->src = lower_32_bits(src);
			scb->srci= upper_32_bits(src) | to_bcm2711_srci(info);
			scb->dst = lower_32_bits(dst);
			scb->dsti = upper_32_bits(dst) | to_bcm2711_dsti(info);
			scb->next_cb = 0;
		} else {
			control_block->info = info;
			control_block->src = src;
			control_block->dst = dst;
			control_block->stride = 0;
			control_block->next = 0;
		}

		/* set up length in control_block if requested */
		if (buf_len) {
			/* calculate length honoring period_length */
			bcm2835_dma_create_cb_set_length(
				c, control_block,
				len, period_len, &total_len,
				cyclic ? finalextrainfo : 0);

			/* calculate new remaining length */
			len -= control_block->length;
		}

		/* link this the last controlblock */
		if (frame && c->is_40bit_channel)
			((struct bcm2711_dma40_scb *)
			 d->cb_list[frame - 1].cb)->next_cb =
				to_bcm2711_cbaddr(cb_entry->paddr);
		if (frame && !c->is_40bit_channel)
			d->cb_list[frame - 1].cb->next = cb_entry->paddr;

		/* update src and dst and length */
		if (src && (info & BCM2835_DMA_S_INC))
			src += control_block->length;
		if (dst && (info & BCM2835_DMA_D_INC))
			dst += control_block->length;

		/* Length of total transfer */
		if (c->is_40bit_channel)
			d->size += ((struct bcm2711_dma40_scb *)control_block)->len;
		else
			d->size += control_block->length;
	}

	/* the last frame requires extra flags */
	if (c->is_40bit_channel) {
		struct bcm2711_dma40_scb *scb =
			(struct bcm2711_dma40_scb *)d->cb_list[d->frames-1].cb;

		scb->ti |= to_bcm2711_ti(finalextrainfo);
	} else {
		d->cb_list[d->frames - 1].cb->info |= finalextrainfo;
	}

	/* detect a size missmatch */
	if (buf_len && (d->size != buf_len))
		goto error_cb;

	return d;
error_cb:
	bcm2835_dma_free_cb_chain(d);

	return NULL;
}

static void bcm2835_dma_fill_cb_chain_with_sg(
	struct bcm2835_chan *c,
	enum dma_transfer_direction direction,
	struct bcm2835_cb_entry *cb,
	struct scatterlist *sgl,
	unsigned int sg_len)
{
	size_t len, max_len;
	unsigned int i;
	dma_addr_t addr;
	struct scatterlist *sgent;

	max_len = bcm2835_dma_max_frame_length(c);
	for_each_sg(sgl, sgent, sg_len, i) {
		if (c->is_40bit_channel) {
			struct bcm2711_dma40_scb *scb;

			for (addr = sg_dma_address(sgent),
				     len = sg_dma_len(sgent);
				     len > 0;
			     addr += scb->len, len -= scb->len, cb++) {
				scb = (struct bcm2711_dma40_scb *)cb->cb;
				if (direction == DMA_DEV_TO_MEM) {
					scb->dst = lower_32_bits(addr);
					scb->dsti = upper_32_bits(addr) | BCM2711_DMA40_INC;
				} else {
					scb->src = lower_32_bits(addr);
					scb->srci = upper_32_bits(addr) | BCM2711_DMA40_INC;
				}
				scb->len = min(len, max_len);
			}
		} else {
			for (addr = sg_dma_address(sgent),
				     len = sg_dma_len(sgent);
			     len > 0;
			     addr += cb->cb->length, len -= cb->cb->length,
			     cb++) {
				if (direction == DMA_DEV_TO_MEM)
					cb->cb->dst = addr;
				else
					cb->cb->src = addr;
				cb->cb->length = min(len, max_len);
			}
		}
	}
}

static void bcm2835_dma_abort(struct bcm2835_chan *c)
{
	void __iomem *chan_base = c->chan_base;
	long int timeout = 10000;
	u32 wait_mask = BCM2835_DMA_WAITING_FOR_WRITES;

	if (c->is_40bit_channel)
		wait_mask = BCM2711_DMA40_WAITING_FOR_WRITES;

	/*
	 * A zero control block address means the channel is idle.
	 * (The ACTIVE flag in the CS register is not a reliable indicator.)
	 */
	if (!readl(chan_base + BCM2835_DMA_ADDR))
		return;

	/* Write 0 to the active bit - Pause the DMA */
	writel(0, chan_base + BCM2835_DMA_CS);

	/* Wait for any current AXI transfer to complete */
	while ((readl(chan_base + BCM2835_DMA_CS) & wait_mask) && --timeout)
		cpu_relax();

	/* Peripheral might be stuck and fail to signal AXI write responses */
	if (!timeout)
		dev_err(c->vc.chan.device->dev,
			"failed to complete outstanding writes\n");

	writel(BCM2835_DMA_RESET, chan_base + BCM2835_DMA_CS);
}

static void bcm2835_dma_start_desc(struct bcm2835_chan *c)
{
	struct virt_dma_desc *vd = vchan_next_desc(&c->vc);
	struct bcm2835_desc *d;

	if (!vd) {
		c->desc = NULL;
		return;
	}

	list_del(&vd->node);

	c->desc = d = to_bcm2835_dma_desc(&vd->tx);

	if (c->is_40bit_channel) {
		writel(to_bcm2711_cbaddr(d->cb_list[0].paddr),
		       c->chan_base + BCM2711_DMA40_CB);
		writel(BCM2711_DMA40_ACTIVE | BCM2711_DMA40_CS_FLAGS(c->dreq),
		       c->chan_base + BCM2711_DMA40_CS);
	} else {
		writel(d->cb_list[0].paddr, c->chan_base + BCM2835_DMA_ADDR);
		writel(BCM2835_DMA_ACTIVE | BCM2835_DMA_CS_FLAGS(c->dreq),
		       c->chan_base + BCM2835_DMA_CS);
	}
}

static irqreturn_t bcm2835_dma_callback(int irq, void *data)
{
	struct bcm2835_chan *c = data;
	struct bcm2835_desc *d;
	unsigned long flags;

	/* check the shared interrupt */
	if (c->irq_flags & IRQF_SHARED) {
		/* check if the interrupt is enabled */
		flags = readl(c->chan_base + BCM2835_DMA_CS);
		/* if not set then we are not the reason for the irq */
		if (!(flags & BCM2835_DMA_INT))
			return IRQ_NONE;
	}

	spin_lock_irqsave(&c->vc.lock, flags);

	/*
	 * Clear the INT flag to receive further interrupts. Keep the channel
	 * active in case the descriptor is cyclic or in case the client has
	 * already terminated the descriptor and issued a new one. (May happen
	 * if this IRQ handler is threaded.) If the channel is finished, it
	 * will remain idle despite the ACTIVE flag being set.
	 */
	writel(BCM2835_DMA_INT | BCM2835_DMA_ACTIVE,
	       c->chan_base + BCM2835_DMA_CS);

	d = c->desc;

	if (d) {
		if (d->cyclic) {
			/* call the cyclic callback */
			vchan_cyclic_callback(&d->vd);
		} else if (!readl(c->chan_base + BCM2835_DMA_ADDR)) {
			vchan_cookie_complete(&c->desc->vd);
			bcm2835_dma_start_desc(c);
		}
	}

	spin_unlock_irqrestore(&c->vc.lock, flags);

	return IRQ_HANDLED;
}

static int bcm2835_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct device *dev = c->vc.chan.device->dev;

	dev_dbg(dev, "Allocating DMA channel %d\n", c->ch);

	/*
	 * Control blocks are 256 bit in length and must start at a 256 bit
	 * (32 byte) aligned address (BCM2835 ARM Peripherals, sec. 4.2.1.1).
	 */
	c->cb_pool = dma_pool_create(dev_name(dev), dev,
				     sizeof(struct bcm2835_dma_cb), 32, 0);
	if (!c->cb_pool) {
		dev_err(dev, "unable to allocate descriptor pool\n");
		return -ENOMEM;
	}

	return request_irq(c->irq_number, bcm2835_dma_callback,
			   c->irq_flags, "DMA IRQ", c);
}

static void bcm2835_dma_free_chan_resources(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);

	vchan_free_chan_resources(&c->vc);
	free_irq(c->irq_number, c);
	dma_pool_destroy(c->cb_pool);

	dev_dbg(c->vc.chan.device->dev, "Freeing DMA channel %u\n", c->ch);
}

static size_t bcm2835_dma_desc_size(struct bcm2835_desc *d)
{
	return d->size;
}

static size_t bcm2835_dma_desc_size_pos(struct bcm2835_desc *d, dma_addr_t addr)
{
	unsigned int i;
	size_t size;

	for (size = i = 0; i < d->frames; i++) {
		struct bcm2835_dma_cb *control_block = d->cb_list[i].cb;
		size_t this_size = control_block->length;
		dma_addr_t dma;

		if (d->dir == DMA_DEV_TO_MEM)
			dma = control_block->dst;
		else
			dma = control_block->src;

		if (size)
			size += this_size;
		else if (addr >= dma && addr < dma + this_size)
			size += dma + this_size - addr;
	}

	return size;
}

static enum dma_status bcm2835_dma_tx_status(struct dma_chan *chan,
	dma_cookie_t cookie, struct dma_tx_state *txstate)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct virt_dma_desc *vd;
	enum dma_status ret;
	unsigned long flags;

	ret = dma_cookie_status(chan, cookie, txstate);
	if (ret == DMA_COMPLETE || !txstate)
		return ret;

	spin_lock_irqsave(&c->vc.lock, flags);
	vd = vchan_find_desc(&c->vc, cookie);
	if (vd) {
		txstate->residue =
			bcm2835_dma_desc_size(to_bcm2835_dma_desc(&vd->tx));
	} else if (c->desc && c->desc->vd.tx.cookie == cookie) {
		struct bcm2835_desc *d = c->desc;
		dma_addr_t pos;

		if (d->dir == DMA_MEM_TO_DEV && c->is_40bit_channel)
			pos = readl(c->chan_base + BCM2711_DMA40_SRC) +
				((readl(c->chan_base + BCM2711_DMA40_SRCI) &
				  0xff) << 8);
		else if (d->dir == DMA_MEM_TO_DEV && !c->is_40bit_channel)
			pos = readl(c->chan_base + BCM2835_DMA_SOURCE_AD);
		else if (d->dir == DMA_DEV_TO_MEM && c->is_40bit_channel)
			pos = readl(c->chan_base + BCM2711_DMA40_DEST) +
				((readl(c->chan_base + BCM2711_DMA40_DESTI) &
				  0xff) << 8);
		else if (d->dir == DMA_DEV_TO_MEM && !c->is_40bit_channel)
			pos = readl(c->chan_base + BCM2835_DMA_DEST_AD);
		else
			pos = 0;

		txstate->residue = bcm2835_dma_desc_size_pos(d, pos);
	} else {
		txstate->residue = 0;
	}

	spin_unlock_irqrestore(&c->vc.lock, flags);

	return ret;
}

static void bcm2835_dma_issue_pending(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);
	if (vchan_issue_pending(&c->vc) && !c->desc)
		bcm2835_dma_start_desc(c);

	spin_unlock_irqrestore(&c->vc.lock, flags);
}

static struct dma_async_tx_descriptor *bcm2835_dma_prep_dma_memcpy(
	struct dma_chan *chan, dma_addr_t dst, dma_addr_t src,
	size_t len, unsigned long flags)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct bcm2835_desc *d;
	u32 info = BCM2835_DMA_D_INC | BCM2835_DMA_S_INC;
	u32 extra = BCM2835_DMA_INT_EN | WAIT_RESP(c->dreq);
	size_t max_len = bcm2835_dma_max_frame_length(c);
	size_t frames;

	/* if src, dst or len is not given return with an error */
	if (!src || !dst || !len)
		return NULL;

	/* calculate number of frames */
	frames = bcm2835_dma_frames_for_length(len, max_len);

	/* allocate the CB chain - this also fills in the pointers */
	d = bcm2835_dma_create_cb_chain(c, DMA_MEM_TO_MEM, false,
					info, extra, frames,
					src, dst, len, 0, GFP_KERNEL);
	if (!d)
		return NULL;

	return vchan_tx_prep(&c->vc, &d->vd, flags);
}

static struct dma_async_tx_descriptor *bcm2835_dma_prep_slave_sg(
	struct dma_chan *chan,
	struct scatterlist *sgl, unsigned int sg_len,
	enum dma_transfer_direction direction,
	unsigned long flags, void *context)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct bcm2835_desc *d;
	dma_addr_t src = 0, dst = 0;
	u32 info = WAIT_RESP(c->dreq);
	u32 extra = BCM2835_DMA_INT_EN;
	size_t frames;

	if (!is_slave_direction(direction)) {
		dev_err(chan->device->dev,
			"%s: bad direction?\n", __func__);
		return NULL;
	}

	if (c->dreq != 0)
		info |= BCM2835_DMA_PER_MAP(c->dreq);

	if (direction == DMA_DEV_TO_MEM) {
		if (c->cfg.src_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES)
			return NULL;
		src = c->cfg.src_addr;
		/*
		 * One would think it ought to be possible to get the physical
		 * to dma address mapping information from the dma-ranges DT
		 * property, but I've not found a way yet that doesn't involve
		 * open-coding the whole thing.
		 */
		if (c->is_40bit_channel)
		    src |= 0x400000000ull;
		info |= BCM2835_DMA_S_DREQ | BCM2835_DMA_D_INC;
	} else {
		if (c->cfg.dst_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES)
			return NULL;
		dst = c->cfg.dst_addr;
		if (c->is_40bit_channel)
		    dst |= 0x400000000ull;
		info |= BCM2835_DMA_D_DREQ | BCM2835_DMA_S_INC;
	}

	/* count frames in sg list */
	frames = bcm2835_dma_count_frames_for_sg(c, sgl, sg_len);

	/* allocate the CB chain */
	d = bcm2835_dma_create_cb_chain(c, direction, false,
					info, extra,
					frames, src, dst, 0, 0,
					GFP_NOWAIT);
	if (!d)
		return NULL;

	/* fill in frames with scatterlist pointers */
	bcm2835_dma_fill_cb_chain_with_sg(c, direction, d->cb_list,
					  sgl, sg_len);

	return vchan_tx_prep(&c->vc, &d->vd, flags);
}

static struct dma_async_tx_descriptor *bcm2835_dma_prep_dma_cyclic(
	struct dma_chan *chan, dma_addr_t buf_addr, size_t buf_len,
	size_t period_len, enum dma_transfer_direction direction,
	unsigned long flags)
{
	struct bcm2835_dmadev *od = to_bcm2835_dma_dev(chan->device);
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct bcm2835_desc *d;
	dma_addr_t src, dst;
	u32 info = WAIT_RESP(c->dreq);
	u32 extra = 0;
	size_t max_len = bcm2835_dma_max_frame_length(c);
	size_t frames;

	/* Grab configuration */
	if (!is_slave_direction(direction)) {
		dev_err(chan->device->dev, "%s: bad direction?\n", __func__);
		return NULL;
	}

	if (!buf_len) {
		dev_err(chan->device->dev,
			"%s: bad buffer length (= 0)\n", __func__);
		return NULL;
	}

	if (flags & DMA_PREP_INTERRUPT)
		extra |= BCM2835_DMA_INT_EN;
	else
		period_len = buf_len;

	/*
	 * warn if buf_len is not a multiple of period_len - this may leed
	 * to unexpected latencies for interrupts and thus audiable clicks
	 */
	if (buf_len % period_len)
		dev_warn_once(chan->device->dev,
			      "%s: buffer_length (%zd) is not a multiple of period_len (%zd)\n",
			      __func__, buf_len, period_len);

	/* Setup DREQ channel */
	if (c->dreq != 0)
		info |= BCM2835_DMA_PER_MAP(c->dreq);

	if (direction == DMA_DEV_TO_MEM) {
		if (c->cfg.src_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES)
			return NULL;
		src = c->cfg.src_addr;
		if (c->is_40bit_channel)
		    src |= 0x400000000ull;
		dst = buf_addr;
		info |= BCM2835_DMA_S_DREQ | BCM2835_DMA_D_INC;
	} else {
		if (c->cfg.dst_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES)
			return NULL;
		dst = c->cfg.dst_addr;
		if (c->is_40bit_channel)
		    dst |= 0x400000000ull;
		src = buf_addr;
		info |= BCM2835_DMA_D_DREQ | BCM2835_DMA_S_INC;

		/* non-lite channels can write zeroes w/o accessing memory */
		if (buf_addr == od->zero_page && !c->is_lite_channel)
			info |= BCM2835_DMA_S_IGNORE;
	}

	/* calculate number of frames */
	frames = /* number of periods */
		 DIV_ROUND_UP(buf_len, period_len) *
		 /* number of frames per period */
		 bcm2835_dma_frames_for_length(period_len, max_len);

	/*
	 * allocate the CB chain
	 * note that we need to use GFP_NOWAIT, as the ALSA i2s dmaengine
	 * implementation calls prep_dma_cyclic with interrupts disabled.
	 */
	d = bcm2835_dma_create_cb_chain(c, direction, true,
					info, extra,
					frames, src, dst, buf_len,
					period_len, GFP_NOWAIT);
	if (!d)
		return NULL;

	/* wrap around into a loop */
	if (c->is_40bit_channel)
		((struct bcm2711_dma40_scb *)
		 d->cb_list[frames - 1].cb)->next_cb =
			to_bcm2711_cbaddr(d->cb_list[0].paddr);
	else
		d->cb_list[d->frames - 1].cb->next = d->cb_list[0].paddr;

	return vchan_tx_prep(&c->vc, &d->vd, flags);
}

static int bcm2835_dma_slave_config(struct dma_chan *chan,
				    struct dma_slave_config *cfg)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);

	c->cfg = *cfg;

	return 0;
}

static int bcm2835_dma_terminate_all(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&c->vc.lock, flags);

	/* stop DMA activity */
	if (c->desc) {
		vchan_terminate_vdesc(&c->desc->vd);
		c->desc = NULL;
		bcm2835_dma_abort(c);
	}

	vchan_get_all_descriptors(&c->vc, &head);
	spin_unlock_irqrestore(&c->vc.lock, flags);
	vchan_dma_desc_free_list(&c->vc, &head);

	return 0;
}

static void bcm2835_dma_synchronize(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);

	vchan_synchronize(&c->vc);
}

static int bcm2835_dma_chan_init(struct bcm2835_dmadev *d, int chan_id,
				 int irq, unsigned int irq_flags)
{
	struct bcm2835_chan *c;

	c = devm_kzalloc(d->ddev.dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	c->vc.desc_free = bcm2835_dma_desc_free;
	vchan_init(&c->vc, &d->ddev);

	c->chan_base = BCM2835_DMA_CHANIO(d->base, chan_id);
	c->ch = chan_id;
	c->irq_number = irq;
	c->irq_flags = irq_flags;

	/* check for 40bit and lite channels */
	if (d->cfg_data->chan_40bit_mask & BIT(chan_id))
		c->is_40bit_channel = true;
	else if (readl(c->chan_base + BCM2835_DMA_DEBUG) &
		 BCM2835_DMA_DEBUG_LITE)
		c->is_lite_channel = true;

	return 0;
}

static void bcm2835_dma_free(struct bcm2835_dmadev *od)
{
	struct bcm2835_chan *c, *next;

	list_for_each_entry_safe(c, next, &od->ddev.channels,
				 vc.chan.device_node) {
		list_del(&c->vc.chan.device_node);
		tasklet_kill(&c->vc.task);
	}

	dma_unmap_page_attrs(od->ddev.dev, od->zero_page, PAGE_SIZE,
			     DMA_TO_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);
}

int bcm2711_dma40_memcpy_init(void)
{
	if (!memcpy_parent)
		return -EPROBE_DEFER;

	if (!memcpy_chan)
		return -EINVAL;

	if (!memcpy_scb)
		return -ENOMEM;

	return 0;
}
EXPORT_SYMBOL(bcm2711_dma40_memcpy_init);

void bcm2711_dma40_memcpy(dma_addr_t dst, dma_addr_t src, size_t size)
{
	struct bcm2711_dma40_scb *scb = memcpy_scb;
	unsigned long flags;

	if (!scb) {
		pr_err("bcm2711_dma40_memcpy not initialised!\n");
		return;
	}

	spin_lock_irqsave(&memcpy_lock, flags);

	scb->ti = 0;
	scb->src = lower_32_bits(src);
	scb->srci = upper_32_bits(src) | BCM2711_DMA40_MEMCPY_XFER_INFO;
	scb->dst = lower_32_bits(dst);
	scb->dsti = upper_32_bits(dst) | BCM2711_DMA40_MEMCPY_XFER_INFO;
	scb->len = size;
	scb->next_cb = 0;

	writel((u32)(memcpy_scb_dma >> 5), memcpy_chan + BCM2711_DMA40_CB);
	writel(BCM2711_DMA40_MEMCPY_FLAGS + BCM2711_DMA40_ACTIVE,
	       memcpy_chan + BCM2711_DMA40_CS);

	/* Poll for completion */
	while (!(readl(memcpy_chan + BCM2711_DMA40_CS) & BCM2711_DMA40_END))
		cpu_relax();

	writel(BCM2711_DMA40_END, memcpy_chan + BCM2711_DMA40_CS);

	spin_unlock_irqrestore(&memcpy_lock, flags);
}
EXPORT_SYMBOL(bcm2711_dma40_memcpy);

static const struct of_device_id bcm2835_dma_of_match[] = {
	{ .compatible = "brcm,bcm2835-dma", .data = &bcm2835_dma_cfg },
	{ .compatible = "brcm,bcm2711-dma", .data = &bcm2711_dma_cfg },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_dma_of_match);

static struct dma_chan *bcm2835_dma_xlate(struct of_phandle_args *spec,
					   struct of_dma *ofdma)
{
	struct bcm2835_dmadev *d = ofdma->of_dma_data;
	struct dma_chan *chan;

	chan = dma_get_any_slave_channel(&d->ddev);
	if (!chan)
		return NULL;

	/* Set DREQ from param */
	to_bcm2835_dma_chan(chan)->dreq = spec->args[0];

	return chan;
}

static int bcm2835_dma_probe(struct platform_device *pdev)
{
	struct bcm2835_dmadev *od;
	struct resource *res;
	void __iomem *base;
	int rc;
	int i, j;
	int irq[BCM2835_DMA_MAX_DMA_CHAN_SUPPORTED + 1];
	int irq_flags;
	uint32_t chans_available;
	char chan_name[BCM2835_DMA_CHAN_NAME_SIZE];
	const struct of_device_id *of_id;
	int chan_count, chan_start, chan_end;

	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (rc) {
		dev_err(&pdev->dev, "Unable to set DMA mask\n");
		return rc;
	}

	od = devm_kzalloc(&pdev->dev, sizeof(*od), GFP_KERNEL);
	if (!od)
		return -ENOMEM;

	dma_set_max_seg_size(&pdev->dev, 0x3FFFFFFF);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/* The set of channels can be split across multiple instances. */
	chan_start = ((u32)(uintptr_t)base / BCM2835_DMA_CHAN_SIZE) & 0xf;
	base -= BCM2835_DMA_CHAN(chan_start);
	chan_count = resource_size(res) / BCM2835_DMA_CHAN_SIZE;
	chan_end = min(chan_start + chan_count,
			 BCM2835_DMA_MAX_DMA_CHAN_SUPPORTED + 1);

	od->base = base;

	dma_cap_set(DMA_SLAVE, od->ddev.cap_mask);
	dma_cap_set(DMA_PRIVATE, od->ddev.cap_mask);
	dma_cap_set(DMA_CYCLIC, od->ddev.cap_mask);
	dma_cap_set(DMA_MEMCPY, od->ddev.cap_mask);
	od->ddev.device_alloc_chan_resources = bcm2835_dma_alloc_chan_resources;
	od->ddev.device_free_chan_resources = bcm2835_dma_free_chan_resources;
	od->ddev.device_tx_status = bcm2835_dma_tx_status;
	od->ddev.device_issue_pending = bcm2835_dma_issue_pending;
	od->ddev.device_prep_dma_cyclic = bcm2835_dma_prep_dma_cyclic;
	od->ddev.device_prep_slave_sg = bcm2835_dma_prep_slave_sg;
	od->ddev.device_prep_dma_memcpy = bcm2835_dma_prep_dma_memcpy;
	od->ddev.device_config = bcm2835_dma_slave_config;
	od->ddev.device_terminate_all = bcm2835_dma_terminate_all;
	od->ddev.device_synchronize = bcm2835_dma_synchronize;
	od->ddev.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	od->ddev.dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	od->ddev.directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV) |
			      BIT(DMA_MEM_TO_MEM);
	od->ddev.residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;
	od->ddev.descriptor_reuse = true;
	od->ddev.dev = &pdev->dev;
	INIT_LIST_HEAD(&od->ddev.channels);

	platform_set_drvdata(pdev, od);

	od->zero_page = dma_map_page_attrs(od->ddev.dev, ZERO_PAGE(0), 0,
					   PAGE_SIZE, DMA_TO_DEVICE,
					   DMA_ATTR_SKIP_CPU_SYNC);
	if (dma_mapping_error(od->ddev.dev, od->zero_page)) {
		dev_err(&pdev->dev, "Failed to map zero page\n");
		return -ENOMEM;
	}

	of_id = of_match_node(bcm2835_dma_of_match, pdev->dev.of_node);
	if (!of_id) {
		dev_err(&pdev->dev, "Failed to match compatible string\n");
		return -EINVAL;
	}

	od->cfg_data = of_id->data;

	/* Request DMA channel mask from device tree */
	if (of_property_read_u32(pdev->dev.of_node,
			"brcm,dma-channel-mask",
			&chans_available)) {
		dev_err(&pdev->dev, "Failed to get channel mask\n");
		rc = -EINVAL;
		goto err_no_dma;
	}

	/* One channel is reserved for the legacy API */
	if (chans_available & BCM2835_DMA_BULK_MASK) {
		rc = bcm_dmaman_probe(pdev, base,
				      chans_available & BCM2835_DMA_BULK_MASK);
		if (rc)
			dev_err(&pdev->dev,
				"Failed to initialize the legacy API\n");

		chans_available &= ~BCM2835_DMA_BULK_MASK;
	}

	/* And possibly one for the 40-bit DMA memcpy API */
	if (chans_available & od->cfg_data->chan_40bit_mask &
	    BIT(BCM2711_DMA_MEMCPY_CHAN)) {
		memcpy_parent = od;
		memcpy_chan = BCM2835_DMA_CHANIO(base, BCM2711_DMA_MEMCPY_CHAN);
		memcpy_scb = dma_alloc_coherent(memcpy_parent->ddev.dev,
						sizeof(*memcpy_scb),
						&memcpy_scb_dma, GFP_KERNEL);
		if (!memcpy_scb)
			dev_warn(&pdev->dev,
				 "Failed to allocated memcpy scb\n");

		chans_available &= ~BIT(BCM2711_DMA_MEMCPY_CHAN);
	}

	/* get irqs for each channel that we support */
	for (i = chan_start; i < chan_end; i++) {
		/* skip masked out channels */
		if (!(chans_available & (1 << i))) {
			irq[i] = -1;
			continue;
		}

		/* get the named irq */
		snprintf(chan_name, sizeof(chan_name), "dma%i", i);
		irq[i] = platform_get_irq_byname(pdev, chan_name);
		if (irq[i] >= 0)
			continue;

		/* legacy device tree case handling */
		dev_warn_once(&pdev->dev,
			      "missing interrupt-names property in device tree - legacy interpretation is used\n");
		/*
		 * in case of channel >= 11
		 * use the 11th interrupt and that is shared
		 */
		irq[i] = platform_get_irq(pdev, i < 11 ? i : 11);
	}

	chan_count = 0;

	/* get irqs for each channel */
	for (i = chan_start; i < chan_end; i++) {
		/* skip channels without irq */
		if (irq[i] < 0)
			continue;

		/* check if there are other channels that also use this irq */
		/* FIXME: This will fail if interrupts are shared across
		   instances */
		irq_flags = 0;
		for (j = 0; j <= BCM2835_DMA_MAX_DMA_CHAN_SUPPORTED; j++)
			if ((i != j) && (irq[j] == irq[i])) {
				irq_flags = IRQF_SHARED;
				break;
			}

		/* initialize the channel */
		rc = bcm2835_dma_chan_init(od, i, irq[i], irq_flags);
		if (rc)
			goto err_no_dma;
		chan_count++;
	}

	dev_dbg(&pdev->dev, "Initialized %i DMA channels\n", chan_count);

	/* Device-tree DMA controller registration */
	rc = of_dma_controller_register(pdev->dev.of_node,
			bcm2835_dma_xlate, od);
	if (rc) {
		dev_err(&pdev->dev, "Failed to register DMA controller\n");
		goto err_no_dma;
	}

	rc = dma_async_device_register(&od->ddev);
	if (rc) {
		dev_err(&pdev->dev,
			"Failed to register slave DMA engine device: %d\n", rc);
		goto err_no_dma;
	}

	dev_dbg(&pdev->dev, "Load BCM2835 DMA engine driver\n");

	return 0;

err_no_dma:
	bcm2835_dma_free(od);
	return rc;
}

static int bcm2835_dma_remove(struct platform_device *pdev)
{
	struct bcm2835_dmadev *od = platform_get_drvdata(pdev);

	bcm_dmaman_remove(pdev);
	dma_async_device_unregister(&od->ddev);
	if (memcpy_parent == od) {
		dma_free_coherent(&pdev->dev, sizeof(*memcpy_scb), memcpy_scb,
				  memcpy_scb_dma);
		memcpy_parent = NULL;
		memcpy_scb = NULL;
		memcpy_chan = NULL;
	}
	bcm2835_dma_free(od);

	return 0;
}

static struct platform_driver bcm2835_dma_driver = {
	.probe	= bcm2835_dma_probe,
	.remove	= bcm2835_dma_remove,
	.driver = {
		.name = "bcm2835-dma",
		.of_match_table = of_match_ptr(bcm2835_dma_of_match),
	},
};

static int bcm2835_dma_init(void)
{
	return platform_driver_register(&bcm2835_dma_driver);
}

static void bcm2835_dma_exit(void)
{
	platform_driver_unregister(&bcm2835_dma_driver);
}

/*
 * Load after serial driver (arch_initcall) so we see the messages if it fails,
 * but before drivers (module_init) that need a DMA channel.
 */
subsys_initcall(bcm2835_dma_init);
module_exit(bcm2835_dma_exit);

MODULE_ALIAS("platform:bcm2835-dma");
MODULE_DESCRIPTION("BCM2835 DMA engine driver");
MODULE_AUTHOR("Florian Meier <florian.meier@koalo.de>");
MODULE_LICENSE("GPL");
