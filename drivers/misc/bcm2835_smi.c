/**
 * Broadcom Secondary Memory Interface driver
 *
 * Written by Luke Wren <luke@raspberrypi.org>
 * Copyright (c) 2015, Raspberry Pi (Trading) Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2, as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/io.h>

#define BCM2835_SMI_IMPLEMENTATION
#include <linux/broadcom/bcm2835_smi.h>

#define DRIVER_NAME "smi-bcm2835"

#define N_PAGES_FROM_BYTES(n) ((n + PAGE_SIZE-1) / PAGE_SIZE)

#define DMA_WRITE_TO_MEM true
#define DMA_READ_FROM_MEM false

struct bcm2835_smi_instance {
	struct device *dev;
	struct smi_settings settings;
	__iomem void *smi_regs_ptr;
	dma_addr_t smi_regs_busaddr;

	struct dma_chan *dma_chan;
	struct dma_slave_config dma_config;

	struct bcm2835_smi_bounce_info bounce;

	struct scatterlist buffer_sgl;

	struct clk *clk;

	/* Sometimes we are called into in an atomic context (e.g. by
	   JFFS2 + MTD) so we can't use a mutex */
	spinlock_t transaction_lock;
};

/****************************************************************************
*
*   SMI peripheral setup
*
***************************************************************************/

static inline void write_smi_reg(struct bcm2835_smi_instance *inst,
	u32 val, unsigned reg)
{
	writel(val, inst->smi_regs_ptr + reg);
}

static inline u32 read_smi_reg(struct bcm2835_smi_instance *inst, unsigned reg)
{
	return readl(inst->smi_regs_ptr + reg);
}

/* Token-paste macro for e.g SMIDSR_RSTROBE ->  value of SMIDSR_RSTROBE_MASK */
#define _CONCAT(x, y) x##y
#define CONCAT(x, y) _CONCAT(x, y)

#define SET_BIT_FIELD(dest, field, bits) ((dest) = \
	((dest) & ~CONCAT(field, _MASK)) | (((bits) << CONCAT(field, _OFFS))& \
	 CONCAT(field, _MASK)))
#define GET_BIT_FIELD(src, field) (((src) & \
	CONCAT(field, _MASK)) >> CONCAT(field, _OFFS))

static void smi_dump_context_labelled(struct bcm2835_smi_instance *inst,
	const char *label)
{
	dev_err(inst->dev, "SMI context dump: %s", label);
	dev_err(inst->dev, "SMICS:  0x%08x", read_smi_reg(inst, SMICS));
	dev_err(inst->dev, "SMIL:   0x%08x", read_smi_reg(inst, SMIL));
	dev_err(inst->dev, "SMIDSR: 0x%08x", read_smi_reg(inst, SMIDSR0));
	dev_err(inst->dev, "SMIDSW: 0x%08x", read_smi_reg(inst, SMIDSW0));
	dev_err(inst->dev, "SMIDC:  0x%08x", read_smi_reg(inst, SMIDC));
	dev_err(inst->dev, "SMIFD:  0x%08x", read_smi_reg(inst, SMIFD));
	dev_err(inst->dev, " ");
}

static inline void smi_dump_context(struct bcm2835_smi_instance *inst)
{
	smi_dump_context_labelled(inst, "");
}

static void smi_get_default_settings(struct bcm2835_smi_instance *inst)
{
	struct smi_settings *settings = &inst->settings;

	settings->data_width = SMI_WIDTH_16BIT;
	settings->pack_data = true;

	settings->read_setup_time = 1;
	settings->read_hold_time = 1;
	settings->read_pace_time = 1;
	settings->read_strobe_time = 3;

	settings->write_setup_time = settings->read_setup_time;
	settings->write_hold_time = settings->read_hold_time;
	settings->write_pace_time = settings->read_pace_time;
	settings->write_strobe_time = settings->read_strobe_time;

	settings->dma_enable = true;
	settings->dma_passthrough_enable = false;
	settings->dma_read_thresh = 0x01;
	settings->dma_write_thresh = 0x3f;
	settings->dma_panic_read_thresh = 0x20;
	settings->dma_panic_write_thresh = 0x20;
}

void bcm2835_smi_set_regs_from_settings(struct bcm2835_smi_instance *inst)
{
	struct smi_settings *settings = &inst->settings;
	int smidsr_temp = 0, smidsw_temp = 0, smics_temp,
	    smidcs_temp, smidc_temp = 0;

	spin_lock(&inst->transaction_lock);

	/* temporarily disable the peripheral: */
	smics_temp = read_smi_reg(inst, SMICS);
	write_smi_reg(inst, 0, SMICS);
	smidcs_temp = read_smi_reg(inst, SMIDCS);
	write_smi_reg(inst, 0, SMIDCS);

	if (settings->pack_data)
		smics_temp |= SMICS_PXLDAT;
	else
		smics_temp &= ~SMICS_PXLDAT;

	SET_BIT_FIELD(smidsr_temp, SMIDSR_RWIDTH, settings->data_width);
	SET_BIT_FIELD(smidsr_temp, SMIDSR_RSETUP, settings->read_setup_time);
	SET_BIT_FIELD(smidsr_temp, SMIDSR_RHOLD, settings->read_hold_time);
	SET_BIT_FIELD(smidsr_temp, SMIDSR_RPACE, settings->read_pace_time);
	SET_BIT_FIELD(smidsr_temp, SMIDSR_RSTROBE, settings->read_strobe_time);
	write_smi_reg(inst, smidsr_temp, SMIDSR0);

	SET_BIT_FIELD(smidsw_temp, SMIDSW_WWIDTH, settings->data_width);
	if (settings->data_width == SMI_WIDTH_8BIT)
		smidsw_temp |= SMIDSW_WSWAP;
	else
		smidsw_temp &= ~SMIDSW_WSWAP;
	SET_BIT_FIELD(smidsw_temp, SMIDSW_WSETUP, settings->write_setup_time);
	SET_BIT_FIELD(smidsw_temp, SMIDSW_WHOLD, settings->write_hold_time);
	SET_BIT_FIELD(smidsw_temp, SMIDSW_WPACE, settings->write_pace_time);
	SET_BIT_FIELD(smidsw_temp, SMIDSW_WSTROBE,
			settings->write_strobe_time);
	write_smi_reg(inst, smidsw_temp, SMIDSW0);

	SET_BIT_FIELD(smidc_temp, SMIDC_REQR, settings->dma_read_thresh);
	SET_BIT_FIELD(smidc_temp, SMIDC_REQW, settings->dma_write_thresh);
	SET_BIT_FIELD(smidc_temp, SMIDC_PANICR,
		      settings->dma_panic_read_thresh);
	SET_BIT_FIELD(smidc_temp, SMIDC_PANICW,
		      settings->dma_panic_write_thresh);
	if (settings->dma_passthrough_enable) {
		smidc_temp |= SMIDC_DMAP;
		smidsr_temp |= SMIDSR_RDREQ;
		write_smi_reg(inst, smidsr_temp, SMIDSR0);
		smidsw_temp |= SMIDSW_WDREQ;
		write_smi_reg(inst, smidsw_temp, SMIDSW0);
	} else
		smidc_temp &= ~SMIDC_DMAP;
	if (settings->dma_enable)
		smidc_temp |= SMIDC_DMAEN;
	else
		smidc_temp &= ~SMIDC_DMAEN;

	write_smi_reg(inst, smidc_temp, SMIDC);

	/* re-enable (if was previously enabled) */
	write_smi_reg(inst, smics_temp, SMICS);
	write_smi_reg(inst, smidcs_temp, SMIDCS);

	spin_unlock(&inst->transaction_lock);
}
EXPORT_SYMBOL(bcm2835_smi_set_regs_from_settings);

struct smi_settings *bcm2835_smi_get_settings_from_regs
	(struct bcm2835_smi_instance *inst)
{
	struct smi_settings *settings = &inst->settings;
	int smidsr, smidsw, smidc;

	spin_lock(&inst->transaction_lock);

	smidsr = read_smi_reg(inst, SMIDSR0);
	smidsw = read_smi_reg(inst, SMIDSW0);
	smidc = read_smi_reg(inst, SMIDC);

	settings->pack_data = (read_smi_reg(inst, SMICS) & SMICS_PXLDAT) ?
	    true : false;

	settings->data_width = GET_BIT_FIELD(smidsr, SMIDSR_RWIDTH);
	settings->read_setup_time = GET_BIT_FIELD(smidsr, SMIDSR_RSETUP);
	settings->read_hold_time = GET_BIT_FIELD(smidsr, SMIDSR_RHOLD);
	settings->read_pace_time = GET_BIT_FIELD(smidsr, SMIDSR_RPACE);
	settings->read_strobe_time = GET_BIT_FIELD(smidsr, SMIDSR_RSTROBE);

	settings->write_setup_time = GET_BIT_FIELD(smidsw, SMIDSW_WSETUP);
	settings->write_hold_time = GET_BIT_FIELD(smidsw, SMIDSW_WHOLD);
	settings->write_pace_time = GET_BIT_FIELD(smidsw, SMIDSW_WPACE);
	settings->write_strobe_time = GET_BIT_FIELD(smidsw, SMIDSW_WSTROBE);

	settings->dma_read_thresh = GET_BIT_FIELD(smidc, SMIDC_REQR);
	settings->dma_write_thresh = GET_BIT_FIELD(smidc, SMIDC_REQW);
	settings->dma_panic_read_thresh = GET_BIT_FIELD(smidc, SMIDC_PANICR);
	settings->dma_panic_write_thresh = GET_BIT_FIELD(smidc, SMIDC_PANICW);
	settings->dma_passthrough_enable = (smidc & SMIDC_DMAP) ? true : false;
	settings->dma_enable = (smidc & SMIDC_DMAEN) ? true : false;

	spin_unlock(&inst->transaction_lock);

	return settings;
}
EXPORT_SYMBOL(bcm2835_smi_get_settings_from_regs);

static inline void smi_set_address(struct bcm2835_smi_instance *inst,
	unsigned int address)
{
	int smia_temp = 0, smida_temp = 0;

	SET_BIT_FIELD(smia_temp, SMIA_ADDR, address);
	SET_BIT_FIELD(smida_temp, SMIDA_ADDR, address);

	/* Write to both address registers - user doesn't care whether we're
	   doing programmed or direct transfers. */
	write_smi_reg(inst, smia_temp, SMIA);
	write_smi_reg(inst, smida_temp, SMIDA);
}

static void smi_setup_regs(struct bcm2835_smi_instance *inst)
{

	dev_dbg(inst->dev, "Initialising SMI registers...");
	/* Disable the peripheral if already enabled */
	write_smi_reg(inst, 0, SMICS);
	write_smi_reg(inst, 0, SMIDCS);

	smi_get_default_settings(inst);
	bcm2835_smi_set_regs_from_settings(inst);
	smi_set_address(inst, 0);

	write_smi_reg(inst, read_smi_reg(inst, SMICS) | SMICS_ENABLE, SMICS);
	write_smi_reg(inst, read_smi_reg(inst, SMIDCS) | SMIDCS_ENABLE,
		SMIDCS);
}

/****************************************************************************
*
*   Low-level SMI access functions
*   Other modules should use the exported higher-level functions e.g.
*   bcm2835_smi_write_buf() unless they have a good reason to use these
*
***************************************************************************/

static inline uint32_t smi_read_single_word(struct bcm2835_smi_instance *inst)
{
	int timeout = 0;

	write_smi_reg(inst, SMIDCS_ENABLE, SMIDCS);
	write_smi_reg(inst, SMIDCS_ENABLE | SMIDCS_START, SMIDCS);
	/* Make sure things happen in the right order...*/
	mb();
	while (!(read_smi_reg(inst, SMIDCS) & SMIDCS_DONE) &&
		++timeout < 10000)
		;
	if (timeout < 10000)
		return read_smi_reg(inst, SMIDD);

	dev_err(inst->dev,
		"SMI direct read timed out (is the clock set up correctly?)");
	return 0;
}

static inline void smi_write_single_word(struct bcm2835_smi_instance *inst,
	uint32_t data)
{
	int timeout = 0;

	write_smi_reg(inst, SMIDCS_ENABLE | SMIDCS_WRITE, SMIDCS);
	write_smi_reg(inst, data, SMIDD);
	write_smi_reg(inst, SMIDCS_ENABLE | SMIDCS_WRITE | SMIDCS_START,
		SMIDCS);

	while (!(read_smi_reg(inst, SMIDCS) & SMIDCS_DONE) &&
		++timeout < 10000)
		;
	if (timeout >= 10000)
		dev_err(inst->dev,
		"SMI direct write timed out (is the clock set up correctly?)");
}

/* Initiates a programmed read into the read FIFO. It is up to the caller to
 * read data from the FIFO -  either via paced DMA transfer,
 * or polling SMICS_RXD to check whether data is available.
 * SMICS_ACTIVE will go low upon completion. */
static void smi_init_programmed_read(struct bcm2835_smi_instance *inst,
	int num_transfers)
{
	int smics_temp;

	/* Disable the peripheral: */
	smics_temp = read_smi_reg(inst, SMICS) & ~(SMICS_ENABLE | SMICS_WRITE);
	write_smi_reg(inst, smics_temp, SMICS);
	while (read_smi_reg(inst, SMICS) & SMICS_ENABLE)
		;

	/* Program the transfer count: */
	write_smi_reg(inst, num_transfers, SMIL);

	/* re-enable and start: */
	smics_temp |= SMICS_ENABLE;
	write_smi_reg(inst, smics_temp, SMICS);
	smics_temp |= SMICS_CLEAR;
	/* Just to be certain: */
	mb();
	while (read_smi_reg(inst, SMICS) & SMICS_ACTIVE)
		;
	write_smi_reg(inst, smics_temp, SMICS);
	smics_temp |= SMICS_START;
	write_smi_reg(inst, smics_temp, SMICS);
}

/* Initiates a programmed write sequence, using data from the write FIFO.
 * It is up to the caller to initiate a DMA transfer before calling,
 * or use another method to keep the write FIFO topped up.
 * SMICS_ACTIVE will go low upon completion.
 */
static void smi_init_programmed_write(struct bcm2835_smi_instance *inst,
	int num_transfers)
{
	int smics_temp;

	/* Disable the peripheral: */
	smics_temp = read_smi_reg(inst, SMICS) & ~SMICS_ENABLE;
	write_smi_reg(inst, smics_temp, SMICS);
	while (read_smi_reg(inst, SMICS) & SMICS_ENABLE)
		;

	/* Program the transfer count: */
	write_smi_reg(inst, num_transfers, SMIL);

	/* setup, re-enable and start: */
	smics_temp |= SMICS_WRITE | SMICS_ENABLE;
	write_smi_reg(inst, smics_temp, SMICS);
	smics_temp |= SMICS_START;
	write_smi_reg(inst, smics_temp, SMICS);
}

/* Initiate a read and then poll FIFO for data, reading out as it appears. */
static void smi_read_fifo(struct bcm2835_smi_instance *inst,
	uint32_t *dest, int n_bytes)
{
	if (read_smi_reg(inst, SMICS) & SMICS_RXD) {
		smi_dump_context_labelled(inst,
			"WARNING: read FIFO not empty at start of read call.");
		while (read_smi_reg(inst, SMICS))
			;
	}

	/* Dispatch the read: */
	if (inst->settings.data_width == SMI_WIDTH_8BIT)
		smi_init_programmed_read(inst, n_bytes);
	else if (inst->settings.data_width == SMI_WIDTH_16BIT)
		smi_init_programmed_read(inst, n_bytes / 2);
	else {
		dev_err(inst->dev, "Unsupported data width for read.");
		return;
	}

	/* Poll FIFO to keep it empty */
	while (!(read_smi_reg(inst, SMICS) & SMICS_DONE))
		if (read_smi_reg(inst, SMICS) & SMICS_RXD)
			*dest++ = read_smi_reg(inst, SMID);

	/* Ensure that the FIFO is emptied */
	if (read_smi_reg(inst, SMICS) & SMICS_RXD) {
		int fifo_count;

		fifo_count = GET_BIT_FIELD(read_smi_reg(inst, SMIFD),
			SMIFD_FCNT);
		while (fifo_count--)
			*dest++ = read_smi_reg(inst, SMID);
	}

	if (!(read_smi_reg(inst, SMICS) & SMICS_DONE))
		smi_dump_context_labelled(inst,
			"WARNING: transaction finished but done bit not set.");

	if (read_smi_reg(inst, SMICS) & SMICS_RXD)
		smi_dump_context_labelled(inst,
			"WARNING: read FIFO not empty at end of read call.");

}

/* Initiate a write, and then keep the FIFO topped up. */
static void smi_write_fifo(struct bcm2835_smi_instance *inst,
	uint32_t *src, int n_bytes)
{
	int i, timeout = 0;

	/* Empty FIFOs if not already so */
	if (!(read_smi_reg(inst, SMICS) & SMICS_TXE)) {
		smi_dump_context_labelled(inst,
		    "WARNING: write fifo not empty at start of write call.");
		write_smi_reg(inst, read_smi_reg(inst, SMICS) | SMICS_CLEAR,
			SMICS);
	}

	/* Initiate the transfer */
	if (inst->settings.data_width == SMI_WIDTH_8BIT)
		smi_init_programmed_write(inst, n_bytes);
	else if (inst->settings.data_width == SMI_WIDTH_16BIT)
		smi_init_programmed_write(inst, n_bytes / 2);
	else {
		dev_err(inst->dev, "Unsupported data width for write.");
		return;
	}
	/* Fill the FIFO: */
	for (i = 0; i < (n_bytes - 1) / 4 + 1; ++i) {
		while (!(read_smi_reg(inst, SMICS) & SMICS_TXD))
			;
		write_smi_reg(inst, *src++, SMID);
	}
	/* Busy wait... */
	while (!(read_smi_reg(inst, SMICS) & SMICS_DONE) && ++timeout <
		1000000)
		;
	if (timeout >= 1000000)
		smi_dump_context_labelled(inst,
			"Timed out on write operation!");
	if (!(read_smi_reg(inst, SMICS) & SMICS_TXE))
		smi_dump_context_labelled(inst,
			"WARNING: FIFO not empty at end of write operation.");
}

/****************************************************************************
*
*   SMI DMA operations
*
***************************************************************************/

/* Disable SMI and put it into the correct direction before doing DMA setup.
   Stops spurious DREQs during setup. Peripheral is re-enabled by init_*() */
static void smi_disable(struct bcm2835_smi_instance *inst,
	enum dma_transfer_direction direction)
{
	int smics_temp = read_smi_reg(inst, SMICS) & ~SMICS_ENABLE;

	if (direction == DMA_DEV_TO_MEM)
		smics_temp &= ~SMICS_WRITE;
	else
		smics_temp |= SMICS_WRITE;
	write_smi_reg(inst, smics_temp, SMICS);
	while (read_smi_reg(inst, SMICS) & SMICS_ACTIVE)
		;
}

static struct scatterlist *smi_scatterlist_from_buffer(
	struct bcm2835_smi_instance *inst,
	dma_addr_t buf,
	size_t len,
	struct scatterlist *sg)
{
	sg_init_table(sg, 1);
	sg_dma_address(sg) = buf;
	sg_dma_len(sg) = len;
	return sg;
}

static void smi_dma_callback_user_copy(void *param)
{
	/* Notify the bottom half that a chunk is ready for user copy */
	struct bcm2835_smi_instance *inst =
		(struct bcm2835_smi_instance *)param;

	up(&inst->bounce.callback_sem);
}

/* Creates a descriptor, assigns the given callback, and submits the
   descriptor to dmaengine. Does not block - can queue up multiple
   descriptors and then wait for them all to complete.
   sg_len is the number of control blocks, NOT the number of bytes.
   dir can be DMA_MEM_TO_DEV or DMA_DEV_TO_MEM.
   callback can be NULL - in this case it is not called. */
static inline struct dma_async_tx_descriptor *smi_dma_submit_sgl(
	struct bcm2835_smi_instance *inst,
	struct scatterlist *sgl,
	size_t sg_len,
	enum dma_transfer_direction dir,
	dma_async_tx_callback callback)
{
	struct dma_async_tx_descriptor *desc;

	desc = dmaengine_prep_slave_sg(inst->dma_chan,
				       sgl,
				       sg_len,
				       dir,
				       DMA_PREP_INTERRUPT | DMA_CTRL_ACK |
				       DMA_PREP_FENCE);
	if (!desc) {
		dev_err(inst->dev, "read_sgl: dma slave preparation failed!");
		write_smi_reg(inst, read_smi_reg(inst, SMICS) & ~SMICS_ACTIVE,
			SMICS);
		while (read_smi_reg(inst, SMICS) & SMICS_ACTIVE)
			cpu_relax();
		write_smi_reg(inst, read_smi_reg(inst, SMICS) | SMICS_ACTIVE,
			SMICS);
		return NULL;
	}
	desc->callback = callback;
	desc->callback_param = inst;
	if (dmaengine_submit(desc) < 0)
		return NULL;
	return desc;
}

/* NB this function blocks until the transfer is complete */
static void
smi_dma_read_sgl(struct bcm2835_smi_instance *inst,
	struct scatterlist *sgl, size_t sg_len, size_t n_bytes)
{
	struct dma_async_tx_descriptor *desc;

	/* Disable SMI and set to read before dispatching DMA - if SMI is in
	 * write mode and TX fifo is empty, it will generate a DREQ which may
	 * cause the read DMA to complete before the SMI read command is even
	 * dispatched! We want to dispatch DMA before SMI read so that reading
	 * is gapless, for logic analyser.
	 */

	smi_disable(inst, DMA_DEV_TO_MEM);

	desc = smi_dma_submit_sgl(inst, sgl, sg_len, DMA_DEV_TO_MEM, NULL);
	dma_async_issue_pending(inst->dma_chan);

	if (inst->settings.data_width == SMI_WIDTH_8BIT)
		smi_init_programmed_read(inst, n_bytes);
	else
		smi_init_programmed_read(inst, n_bytes / 2);

	if (dma_wait_for_async_tx(desc) == DMA_ERROR)
		smi_dump_context_labelled(inst, "DMA timeout!");
}

static void
smi_dma_write_sgl(struct bcm2835_smi_instance *inst,
	struct scatterlist *sgl, size_t sg_len, size_t n_bytes)
{
	struct dma_async_tx_descriptor *desc;

	if (inst->settings.data_width == SMI_WIDTH_8BIT)
		smi_init_programmed_write(inst, n_bytes);
	else
		smi_init_programmed_write(inst, n_bytes / 2);

	desc = smi_dma_submit_sgl(inst, sgl, sg_len, DMA_MEM_TO_DEV, NULL);
	dma_async_issue_pending(inst->dma_chan);

	if (dma_wait_for_async_tx(desc) == DMA_ERROR)
		smi_dump_context_labelled(inst, "DMA timeout!");
	else
		/* Wait for SMI to finish our writes */
		while (!(read_smi_reg(inst, SMICS) & SMICS_DONE))
			cpu_relax();
}

ssize_t bcm2835_smi_user_dma(
	struct bcm2835_smi_instance *inst,
	enum dma_transfer_direction dma_dir,
	char __user *user_ptr, size_t count,
	struct bcm2835_smi_bounce_info **bounce)
{
	int chunk_no = 0, chunk_size, count_left = count;
	struct scatterlist *sgl;
	void (*init_trans_func)(struct bcm2835_smi_instance *, int);

	spin_lock(&inst->transaction_lock);

	if (dma_dir == DMA_DEV_TO_MEM)
		init_trans_func = smi_init_programmed_read;
	else
		init_trans_func = smi_init_programmed_write;

	smi_disable(inst, dma_dir);

	sema_init(&inst->bounce.callback_sem, 0);
	if (bounce)
		*bounce = &inst->bounce;
	while (count_left) {
		chunk_size = count_left > DMA_BOUNCE_BUFFER_SIZE ?
			DMA_BOUNCE_BUFFER_SIZE : count_left;
		if (chunk_size == DMA_BOUNCE_BUFFER_SIZE) {
			sgl =
			&inst->bounce.sgl[chunk_no % DMA_BOUNCE_BUFFER_COUNT];
		} else {
			sgl = smi_scatterlist_from_buffer(
				inst,
				inst->bounce.phys[
					chunk_no % DMA_BOUNCE_BUFFER_COUNT],
				chunk_size,
				&inst->buffer_sgl);
		}

		if (!smi_dma_submit_sgl(inst, sgl, 1, dma_dir,
			smi_dma_callback_user_copy
		)) {
			dev_err(inst->dev, "sgl submit failed");
			count = 0;
			goto out;
		}
		count_left -= chunk_size;
		chunk_no++;
	}
	dma_async_issue_pending(inst->dma_chan);

	if (inst->settings.data_width == SMI_WIDTH_8BIT)
		init_trans_func(inst, count);
	else if (inst->settings.data_width == SMI_WIDTH_16BIT)
		init_trans_func(inst, count / 2);
out:
	spin_unlock(&inst->transaction_lock);
	return count;
}
EXPORT_SYMBOL(bcm2835_smi_user_dma);


/****************************************************************************
*
*   High level buffer transfer functions - for use by other drivers
*
***************************************************************************/

/* Buffer must be physically contiguous - i.e. kmalloc, not vmalloc! */
void bcm2835_smi_write_buf(
	struct bcm2835_smi_instance *inst,
	const void *buf, size_t n_bytes)
{
	int odd_bytes = n_bytes & 0x3;

	n_bytes -= odd_bytes;

	spin_lock(&inst->transaction_lock);

	if (n_bytes > DMA_THRESHOLD_BYTES) {
		dma_addr_t phy_addr = dma_map_single(
			inst->dev,
			(void *)buf,
			n_bytes,
			DMA_MEM_TO_DEV);
		struct scatterlist *sgl =
			smi_scatterlist_from_buffer(inst, phy_addr, n_bytes,
				&inst->buffer_sgl);

		if (!sgl) {
			smi_dump_context_labelled(inst,
			"Error: could not create scatterlist for write!");
			goto out;
		}
		smi_dma_write_sgl(inst, sgl, 1, n_bytes);

		dma_unmap_single
			(inst->dev, phy_addr, n_bytes, DMA_MEM_TO_DEV);
	} else if (n_bytes) {
		smi_write_fifo(inst, (uint32_t *) buf, n_bytes);
	}
	buf += n_bytes;

	if (inst->settings.data_width == SMI_WIDTH_8BIT) {
		while (odd_bytes--)
			smi_write_single_word(inst, *(uint8_t *) (buf++));
	} else {
		while (odd_bytes >= 2) {
			smi_write_single_word(inst, *(uint16_t *)buf);
			buf += 2;
			odd_bytes -= 2;
		}
		if (odd_bytes) {
			/* Reading an odd number of bytes on a 16 bit bus is
			   a user bug. It's kinder to fail early and tell them
			   than to e.g. transparently give them the bottom byte
			   of a 16 bit transfer. */
			dev_err(inst->dev,
		"WARNING: odd number of bytes specified for wide transfer.");
			dev_err(inst->dev,
		"At least one byte dropped as a result.");
			dump_stack();
		}
	}
out:
	spin_unlock(&inst->transaction_lock);
}
EXPORT_SYMBOL(bcm2835_smi_write_buf);

void bcm2835_smi_read_buf(struct bcm2835_smi_instance *inst,
	void *buf, size_t n_bytes)
{

	/* SMI is inherently 32-bit, which causes surprising amounts of mess
	   for bytes % 4 != 0. Easiest to avoid this mess altogether
	   by handling remainder separately. */
	int odd_bytes = n_bytes & 0x3;

	spin_lock(&inst->transaction_lock);
	n_bytes -= odd_bytes;
	if (n_bytes > DMA_THRESHOLD_BYTES) {
		dma_addr_t phy_addr = dma_map_single(inst->dev,
						     buf, n_bytes,
						     DMA_DEV_TO_MEM);
		struct scatterlist *sgl = smi_scatterlist_from_buffer(
			inst, phy_addr, n_bytes,
			&inst->buffer_sgl);
		if (!sgl) {
			smi_dump_context_labelled(inst,
			"Error: could not create scatterlist for read!");
			goto out;
		}
		smi_dma_read_sgl(inst, sgl, 1, n_bytes);
		dma_unmap_single(inst->dev, phy_addr, n_bytes, DMA_DEV_TO_MEM);
	} else if (n_bytes) {
		smi_read_fifo(inst, (uint32_t *)buf, n_bytes);
	}
	buf += n_bytes;

	if (inst->settings.data_width == SMI_WIDTH_8BIT) {
		while (odd_bytes--)
			*((uint8_t *) (buf++)) = smi_read_single_word(inst);
	} else {
		while (odd_bytes >= 2) {
			*(uint16_t *) buf = smi_read_single_word(inst);
			buf += 2;
			odd_bytes -= 2;
		}
		if (odd_bytes) {
			dev_err(inst->dev,
		"WARNING: odd number of bytes specified for wide transfer.");
			dev_err(inst->dev,
		"At least one byte dropped as a result.");
			dump_stack();
		}
	}
out:
	spin_unlock(&inst->transaction_lock);
}
EXPORT_SYMBOL(bcm2835_smi_read_buf);

void bcm2835_smi_set_address(struct bcm2835_smi_instance *inst,
	unsigned int address)
{
	spin_lock(&inst->transaction_lock);
	smi_set_address(inst, address);
	spin_unlock(&inst->transaction_lock);
}
EXPORT_SYMBOL(bcm2835_smi_set_address);

struct bcm2835_smi_instance *bcm2835_smi_get(struct device_node *node)
{
	struct platform_device *pdev;

	if (!node)
		return NULL;

	pdev = of_find_device_by_node(node);
	if (!pdev)
		return NULL;

	return platform_get_drvdata(pdev);
}
EXPORT_SYMBOL(bcm2835_smi_get);

/****************************************************************************
*
*   bcm2835_smi_probe - called when the driver is loaded.
*
***************************************************************************/

static int bcm2835_smi_dma_setup(struct bcm2835_smi_instance *inst)
{
	int i, rv = 0;

	inst->dma_chan = dma_request_slave_channel(inst->dev, "rx-tx");

	inst->dma_config.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	inst->dma_config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	inst->dma_config.src_addr = inst->smi_regs_busaddr + SMID;
	inst->dma_config.dst_addr = inst->dma_config.src_addr;
	/* Direction unimportant - always overridden by prep_slave_sg */
	inst->dma_config.direction = DMA_DEV_TO_MEM;
	dmaengine_slave_config(inst->dma_chan, &inst->dma_config);
	/* Alloc and map bounce buffers */
	for (i = 0; i < DMA_BOUNCE_BUFFER_COUNT; ++i) {
		inst->bounce.buffer[i] =
		dmam_alloc_coherent(inst->dev, DMA_BOUNCE_BUFFER_SIZE,
				&inst->bounce.phys[i],
				GFP_KERNEL);
		if (!inst->bounce.buffer[i]) {
			dev_err(inst->dev, "Could not allocate buffer!");
			rv = -ENOMEM;
			break;
		}
		smi_scatterlist_from_buffer(
			inst,
			inst->bounce.phys[i],
			DMA_BOUNCE_BUFFER_SIZE,
			&inst->bounce.sgl[i]
		);
	}

	return rv;
}

static int bcm2835_smi_probe(struct platform_device *pdev)
{
	int err;
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct resource *ioresource;
	struct bcm2835_smi_instance *inst;
	const __be32 *addr;

	/* We require device tree support */
	if (!node)
		return -EINVAL;
	/* Allocate buffers and instance data */
	inst = devm_kzalloc(dev, sizeof(struct bcm2835_smi_instance),
		GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	inst->dev = dev;
	spin_lock_init(&inst->transaction_lock);

	ioresource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	inst->smi_regs_ptr = devm_ioremap_resource(dev, ioresource);
	if (IS_ERR(inst->smi_regs_ptr)) {
		err = PTR_ERR(inst->smi_regs_ptr);
		goto err;
	}
	addr = of_get_address(node, 0, NULL, NULL);
	inst->smi_regs_busaddr = be32_to_cpu(*addr);

	err = bcm2835_smi_dma_setup(inst);
	if (err)
		goto err;

	/* request clock */
	inst->clk = devm_clk_get(dev, NULL);
	if (!inst->clk)
		goto err;
	clk_prepare_enable(inst->clk);

	/* Finally, do peripheral setup */
	smi_setup_regs(inst);

	platform_set_drvdata(pdev, inst);

	dev_info(inst->dev, "initialised");

	return 0;
err:
	kfree(inst);
	return err;
}

/****************************************************************************
*
*   bcm2835_smi_remove - called when the driver is unloaded.
*
***************************************************************************/

static int bcm2835_smi_remove(struct platform_device *pdev)
{
	struct bcm2835_smi_instance *inst = platform_get_drvdata(pdev);
	struct device *dev = inst->dev;

	dmaengine_terminate_all(inst->dma_chan);
	dma_release_channel(inst->dma_chan);

	clk_disable_unprepare(inst->clk);

	dev_info(dev, "SMI device removed - OK");
	return 0;
}

/****************************************************************************
*
*   Register the driver with device tree
*
***************************************************************************/

static const struct of_device_id bcm2835_smi_of_match[] = {
	{.compatible = "brcm,bcm2835-smi",},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, bcm2835_smi_of_match);

static struct platform_driver bcm2835_smi_driver = {
	.probe = bcm2835_smi_probe,
	.remove = bcm2835_smi_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = bcm2835_smi_of_match,
		   },
};

module_platform_driver(bcm2835_smi_driver);

MODULE_ALIAS("platform:smi-bcm2835");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Device driver for BCM2835's secondary memory interface");
MODULE_AUTHOR("Luke Wren <luke@raspberrypi.org>");
