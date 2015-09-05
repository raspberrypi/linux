/**
 * Declarations and definitions for Broadcom's Secondary Memory Interface
 *
 * Written by Luke Wren <luke@raspberrypi.org>
 * Copyright (c) 2015, Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2010-2012 Broadcom. All rights reserved.
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

#ifndef BCM2835_SMI_H
#define BCM2835_SMI_H

#include <linux/ioctl.h>

#ifndef __KERNEL__
#include <stdint.h>
#include <stdbool.h>
#endif

#define BCM2835_SMI_IOC_MAGIC 0x1
#define BCM2835_SMI_INVALID_HANDLE (~0)

/* IOCTLs 0x100...0x1ff are not device-specific - we can use them */
#define BCM2835_SMI_IOC_GET_SETTINGS    _IO(BCM2835_SMI_IOC_MAGIC, 0)
#define BCM2835_SMI_IOC_WRITE_SETTINGS  _IO(BCM2835_SMI_IOC_MAGIC, 1)
#define BCM2835_SMI_IOC_ADDRESS	 _IO(BCM2835_SMI_IOC_MAGIC, 2)
#define BCM2835_SMI_IOC_MAX	     2

#define SMI_WIDTH_8BIT 0
#define SMI_WIDTH_16BIT 1
#define SMI_WIDTH_9BIT 2
#define SMI_WIDTH_18BIT 3

/* max number of bytes where DMA will not be used */
#define DMA_THRESHOLD_BYTES 128
#define DMA_BOUNCE_BUFFER_SIZE (1024 * 1024 / 2)
#define DMA_BOUNCE_BUFFER_COUNT 3


struct smi_settings {
	int data_width;
	/* Whether or not to pack multiple SMI transfers into a
	   single 32 bit FIFO word */
	bool pack_data;

	/* Timing for reads (writes the same but for WE)
	 *
	 * OE ----------+	   +--------------------
	 *		|	   |
	 *		+----------+
	 * SD -<==============================>-----------
	 * SA -<=========================================>-
	 *    <-setup->  <-strobe ->  <-hold ->  <- pace ->
	 */

	int read_setup_time;
	int read_hold_time;
	int read_pace_time;
	int read_strobe_time;

	int write_setup_time;
	int write_hold_time;
	int write_pace_time;
	int write_strobe_time;

	bool dma_enable;		/* DREQs */
	bool dma_passthrough_enable;	/* External DREQs */
	int dma_read_thresh;
	int dma_write_thresh;
	int dma_panic_read_thresh;
	int dma_panic_write_thresh;
};

/****************************************************************************
*
*   Declare exported SMI functions
*
***************************************************************************/

#ifdef __KERNEL__

#include <linux/dmaengine.h> /* for enum dma_transfer_direction */
#include <linux/of.h>
#include <linux/semaphore.h>

struct bcm2835_smi_instance;

struct bcm2835_smi_bounce_info {
	struct semaphore callback_sem;
	void *buffer[DMA_BOUNCE_BUFFER_COUNT];
	dma_addr_t phys[DMA_BOUNCE_BUFFER_COUNT];
	struct scatterlist sgl[DMA_BOUNCE_BUFFER_COUNT];
};


void bcm2835_smi_set_regs_from_settings(struct bcm2835_smi_instance *);

struct smi_settings *bcm2835_smi_get_settings_from_regs(
	struct bcm2835_smi_instance *inst);

void bcm2835_smi_write_buf(
	struct bcm2835_smi_instance *inst,
	const void *buf,
	size_t n_bytes);

void bcm2835_smi_read_buf(
	struct bcm2835_smi_instance *inst,
	void *buf,
	size_t n_bytes);

void bcm2835_smi_set_address(struct bcm2835_smi_instance *inst,
	unsigned int address);

ssize_t bcm2835_smi_user_dma(
	struct bcm2835_smi_instance *inst,
	enum dma_transfer_direction dma_dir,
	char __user *user_ptr,
	size_t count,
	struct bcm2835_smi_bounce_info **bounce);

struct bcm2835_smi_instance *bcm2835_smi_get(struct device_node *node);

#endif /* __KERNEL__ */

/****************************************************************
*
*	Implementation-only declarations
*
****************************************************************/

#ifdef BCM2835_SMI_IMPLEMENTATION

/* Clock manager registers for SMI clock: */
#define CM_SMI_BASE_ADDRESS ((BCM2708_PERI_BASE) + 0x1010b0)
/* Clock manager "password" to protect registers from spurious writes */
#define CM_PWD (0x5a << 24)

#define CM_SMI_CTL	0x00
#define CM_SMI_DIV	0x04

#define CM_SMI_CTL_FLIP (1 << 8)
#define CM_SMI_CTL_BUSY (1 << 7)
#define CM_SMI_CTL_KILL (1 << 5)
#define CM_SMI_CTL_ENAB (1 << 4)
#define CM_SMI_CTL_SRC_MASK (0xf)
#define CM_SMI_CTL_SRC_OFFS (0)

#define CM_SMI_DIV_DIVI_MASK (0xf <<  12)
#define CM_SMI_DIV_DIVI_OFFS (12)
#define CM_SMI_DIV_DIVF_MASK (0xff << 4)
#define CM_SMI_DIV_DIVF_OFFS (4)

/* SMI register mapping:*/
#define SMI_BASE_ADDRESS ((BCM2708_PERI_BASE) + 0x600000)

#define SMICS	0x00	/* control + status register		*/
#define SMIL	0x04	/* length/count (n external txfers)	*/
#define SMIA	0x08	/* address register			*/
#define SMID	0x0c	/* data register			*/
#define SMIDSR0	0x10	/* device 0 read settings		*/
#define SMIDSW0	0x14	/* device 0 write settings		*/
#define SMIDSR1	0x18	/* device 1 read settings		*/
#define SMIDSW1	0x1c	/* device 1 write settings		*/
#define SMIDSR2	0x20	/* device 2 read settings		*/
#define SMIDSW2	0x24	/* device 2 write settings		*/
#define SMIDSR3	0x28	/* device 3 read settings		*/
#define SMIDSW3	0x2c	/* device 3 write settings		*/
#define SMIDC	0x30	/* DMA control registers		*/
#define SMIDCS	0x34	/* direct control/status register	*/
#define SMIDA	0x38	/* direct address register		*/
#define SMIDD	0x3c	/* direct data registers		*/
#define SMIFD	0x40	/* FIFO debug register			*/



/* Control and Status register bits:
 * SMICS_RXF	: RX fifo full: 1 when RX fifo is full
 * SMICS_TXE	: TX fifo empty: 1 when empty.
 * SMICS_RXD	: RX fifo contains data: 1 when there is data.
 * SMICS_TXD	: TX fifo can accept data: 1 when true.
 * SMICS_RXR	: RX fifo needs reading: 1 when fifo more than 3/4 full, or
 *		  when "DONE" and fifo not emptied.
 * SMICS_TXW	: TX fifo needs writing: 1 when less than 1/4 full.
 * SMICS_AFERR	: AXI FIFO error: 1 when fifo read when empty or written
 *		  when full. Write 1 to clear.
 * SMICS_EDREQ	: 1 when external DREQ received.
 * SMICS_PXLDAT	:  Pixel data:	write 1 to enable pixel transfer modes.
 * SMICS_SETERR	: 1 if there was an error writing to setup regs (e.g.
 *		  tx was in progress). Write 1 to clear.
 * SMICS_PVMODE	: Set to 1 to enable pixel valve mode.
 * SMICS_INTR	: Set to 1 to enable interrupt on RX.
 * SMICS_INTT	: Set to 1 to enable interrupt on TX.
 * SMICS_INTD	: Set to 1 to enable interrupt on DONE condition.
 * SMICS_TEEN	: Tear effect mode enabled: Programmed transfers will wait
 *		  for a TE trigger before writing.
 * SMICS_PAD1	: Padding settings for external transfers. For writes: the
 *		  number of bytes initially written to  the TX fifo that
 * SMICS_PAD0	: should be ignored. For reads: the number of bytes that will
 *		  be read before the data, and should be dropped.
 * SMICS_WRITE	: Transfer direction: 1 = write to external device, 0 = read
 * SMICS_CLEAR	: Write 1 to clear the FIFOs.
 * SMICS_START	: Write 1 to start the programmed transfer.
 * SMICS_ACTIVE	: Reads as 1 when a programmed transfer is underway.
 * SMICS_DONE	: Reads as 1 when transfer finished. For RX, not set until
 *		  FIFO emptied.
 * SMICS_ENABLE	: Set to 1 to enable the SMI peripheral, 0 to disable.
 */

#define SMICS_RXF	(1 << 31)
#define SMICS_TXE	(1 << 30)
#define SMICS_RXD	(1 << 29)
#define SMICS_TXD	(1 << 28)
#define SMICS_RXR	(1 << 27)
#define SMICS_TXW	(1 << 26)
#define SMICS_AFERR	(1 << 25)
#define SMICS_EDREQ	(1 << 15)
#define SMICS_PXLDAT	(1 << 14)
#define SMICS_SETERR	(1 << 13)
#define SMICS_PVMODE	(1 << 12)
#define SMICS_INTR	(1 << 11)
#define SMICS_INTT	(1 << 10)
#define SMICS_INTD	(1 << 9)
#define SMICS_TEEN	(1 << 8)
#define SMICS_PAD1	(1 << 7)
#define SMICS_PAD0	(1 << 6)
#define SMICS_WRITE	(1 << 5)
#define SMICS_CLEAR	(1 << 4)
#define SMICS_START	(1 << 3)
#define SMICS_ACTIVE	(1 << 2)
#define SMICS_DONE	(1 << 1)
#define SMICS_ENABLE	(1 << 0)

/* Address register bits: */

#define SMIA_DEVICE_MASK ((1 << 9) | (1 << 8))
#define SMIA_DEVICE_OFFS (8)
#define SMIA_ADDR_MASK (0x3f)	/* bits 5 -> 0 */
#define SMIA_ADDR_OFFS (0)

/* DMA control register bits:
 * SMIDC_DMAEN	: DMA enable: set 1: DMA requests will be issued.
 * SMIDC_DMAP	: DMA passthrough: when set to 0, top two data pins are used by
 *		  SMI as usual. When set to 1, the top two pins are used for
 *		  external DREQs: pin 16 read request, 17 write.
 * SMIDC_PANIC*	: Threshold at which DMA will panic during read/write.
 * SMIDC_REQ*	: Threshold at which DMA will generate a DREQ.
 */

#define SMIDC_DMAEN		(1 << 28)
#define SMIDC_DMAP		(1 << 24)
#define SMIDC_PANICR_MASK	(0x3f << 18)
#define SMIDC_PANICR_OFFS	(18)
#define SMIDC_PANICW_MASK	(0x3f << 12)
#define SMIDC_PANICW_OFFS	(12)
#define SMIDC_REQR_MASK		(0x3f << 6)
#define SMIDC_REQR_OFFS		(6)
#define SMIDC_REQW_MASK		(0x3f)
#define SMIDC_REQW_OFFS		(0)

/* Device settings register bits: same for all 4 (or 3?) device register sets.
 * Device read settings:
 * SMIDSR_RWIDTH	: Read transfer width. 00 = 8bit, 01 = 16bit,
 *			  10 = 18bit, 11 = 9bit.
 * SMIDSR_RSETUP	: Read setup time: number of core cycles between chip
 *			  select/address and read strobe. Min 1, max 64.
 * SMIDSR_MODE68	: 1 for System 68 mode (i.e. enable + direction pins,
 *			  rather than OE + WE pin)
 * SMIDSR_FSETUP	: If set to 1, setup time only applies to first
 *			  transfer after address change.
 * SMIDSR_RHOLD		: Number of core cycles between read strobe going
 *			  inactive and CS/address going inactive. Min 1, max 64
 * SMIDSR_RPACEALL	: When set to 1, this device's RPACE value will always
 *			  be used for the next transaction, even if it is not
 *			  to this device.
 * SMIDSR_RPACE		: Number of core cycles spent waiting between CS
 *			  deassert and start of next transfer. Min 1, max 128
 * SMIDSR_RDREQ		: 1 = use external DMA request on SD16 to pace reads
 *			  from device. Must also set DMAP in SMICS.
 * SMIDSR_RSTROBE	: Number of cycles to assert the read strobe.
 *			  min 1, max 128.
 */
#define SMIDSR_RWIDTH_MASK	((1<<31)|(1<<30))
#define SMIDSR_RWIDTH_OFFS	(30)
#define SMIDSR_RSETUP_MASK	(0x3f << 24)
#define SMIDSR_RSETUP_OFFS	(24)
#define SMIDSR_MODE68		(1 << 23)
#define SMIDSR_FSETUP		(1 << 22)
#define SMIDSR_RHOLD_MASK	(0x3f << 16)
#define SMIDSR_RHOLD_OFFS	(16)
#define SMIDSR_RPACEALL		(1 << 15)
#define SMIDSR_RPACE_MASK	(0x7f << 8)
#define SMIDSR_RPACE_OFFS	(8)
#define SMIDSR_RDREQ		(1 << 7)
#define SMIDSR_RSTROBE_MASK	(0x7f)
#define SMIDSR_RSTROBE_OFFS	(0)

/* Device write settings:
 * SMIDSW_WWIDTH	: Write transfer width. 00 = 8bit, 01 = 16bit,
 *			  10= 18bit, 11 = 9bit.
 * SMIDSW_WSETUP	: Number of cycles between CS assert and write strobe.
 *			  Min 1, max 64.
 * SMIDSW_WFORMAT	: Pixel format of input. 0 = 16bit RGB 565,
 *			  1 = 32bit RGBA 8888
 * SMIDSW_WSWAP		: 1 = swap pixel data bits. (Use with SMICS_PXLDAT)
 * SMIDSW_WHOLD		: Time between WE deassert and CS deassert. 1 to 64
 * SMIDSW_WPACEALL	: 1: this device's WPACE will be used for the next
 *			  transfer, regardless of that transfer's device.
 * SMIDSW_WPACE		: Cycles between CS deassert and next CS assert.
 *			  Min 1, max 128
 * SMIDSW_WDREQ		: Use external DREQ on pin 17 to pace writes. DMAP must
 *			  be set in SMICS.
 * SMIDSW_WSTROBE	: Number of cycles to assert the write strobe.
 *			  Min 1, max 128
 */
#define SMIDSW_WWIDTH_MASK	 ((1<<31)|(1<<30))
#define SMIDSW_WWIDTH_OFFS	(30)
#define SMIDSW_WSETUP_MASK	(0x3f << 24)
#define SMIDSW_WSETUP_OFFS	(24)
#define SMIDSW_WFORMAT		(1 << 23)
#define SMIDSW_WSWAP		(1 << 22)
#define SMIDSW_WHOLD_MASK	(0x3f << 16)
#define SMIDSW_WHOLD_OFFS	(16)
#define SMIDSW_WPACEALL		(1 << 15)
#define SMIDSW_WPACE_MASK	(0x7f << 8)
#define SMIDSW_WPACE_OFFS	(8)
#define SMIDSW_WDREQ		(1 << 7)
#define SMIDSW_WSTROBE_MASK	 (0x7f)
#define SMIDSW_WSTROBE_OFFS	 (0)

/* Direct transfer control + status register
 * SMIDCS_WRITE	: Direction of transfer: 1 -> write, 0 -> read
 * SMIDCS_DONE	: 1 when a transfer has finished. Write 1 to clear.
 * SMIDCS_START	: Write 1 to start a transfer, if one is not already underway.
 * SMIDCE_ENABLE: Write 1 to enable SMI in direct mode.
 */

#define SMIDCS_WRITE		(1 << 3)
#define SMIDCS_DONE		(1 << 2)
#define SMIDCS_START		(1 << 1)
#define SMIDCS_ENABLE		(1 << 0)

/* Direct transfer address register
 * SMIDA_DEVICE	: Indicates which of the device settings banks should be used.
 * SMIDA_ADDR	: The value to be asserted on the address pins.
 */

#define SMIDA_DEVICE_MASK	((1<<9)|(1<<8))
#define SMIDA_DEVICE_OFFS	(8)
#define SMIDA_ADDR_MASK		(0x3f)
#define SMIDA_ADDR_OFFS		(0)

/* FIFO debug register
 * SMIFD_FLVL	: The high-tide mark of FIFO count during the most recent txfer
 * SMIFD_FCNT	: The current FIFO count.
 */
#define SMIFD_FLVL_MASK		(0x3f << 8)
#define SMIFD_FLVL_OFFS		(8)
#define SMIFD_FCNT_MASK		(0x3f)
#define SMIFD_FCNT_OFFS		(0)

#endif /* BCM2835_SMI_IMPLEMENTATION */

#endif /* BCM2835_SMI_H */
