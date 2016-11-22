/**
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

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/pagemap.h>
#include <linux/dma-mapping.h>
#include <linux/version.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <asm/pgtable.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define TOTAL_SLOTS (VCHIQ_SLOT_ZERO_SLOTS + 2 * 32)

#include "vchiq_arm.h"
#include "vchiq_2835.h"
#include "vchiq_connected.h"
#include "vchiq_killable.h"

#define BELL0	0x00
#define BELL2	0x08

typedef struct vchiq_2835_state_struct {
   int inited;
   VCHIQ_ARM_STATE_T arm_state;
} VCHIQ_2835_ARM_STATE_T;

typedef struct 
{
    void *data;
    dma_addr_t dmaaddr;
    size_t size;
    enum dma_data_direction direction;
} VCHIQ_2835_ARM_BULK_DATA_T;

static void __iomem *g_regs;
static struct device *g_dev;

extern int vchiq_arm_log_level;

static irqreturn_t
vchiq_doorbell_irq(int irq, void *dev_id);

int vchiq_platform_init(struct platform_device *pdev, VCHIQ_STATE_T *state)
{
	struct device *dev = &pdev->dev;
	struct rpi_firmware *fw = platform_get_drvdata(pdev);
	VCHIQ_SLOT_ZERO_T *vchiq_slot_zero;
	struct resource *res;
	void *slot_mem;
	dma_addr_t slot_phys;
	u32 channelbase;
	int slot_mem_size;
	int err, irq, i;

	dma_set_mask(dev, DMA_BIT_MASK(32));
	dma_set_coherent_mask(dev, DMA_BIT_MASK(32));

	/* Allocate space for the channels in coherent memory */
	slot_mem_size = PAGE_ALIGN(TOTAL_SLOTS * VCHIQ_SLOT_SIZE);

	slot_mem = dmam_alloc_coherent(dev, slot_mem_size,
				       &slot_phys, GFP_KERNEL);

        dev_err(dev, "slot mem %p\n", slot_phys);

	if (!slot_mem) {
		dev_err(dev, "could not allocate DMA memory\n");
		return -ENOMEM;
	}

	WARN_ON(((int)slot_mem & (PAGE_SIZE - 1)) != 0);

	vchiq_slot_zero = vchiq_init_slots(slot_mem, slot_mem_size);
	if (!vchiq_slot_zero)
		return -EINVAL;

	vchiq_slot_zero->platform_data[VCHIQ_PLATFORM_FRAGMENTS_OFFSET_IDX] = 0;
	vchiq_slot_zero->platform_data[VCHIQ_PLATFORM_FRAGMENTS_COUNT_IDX] = 0;

	if (vchiq_init_state(state, vchiq_slot_zero, 0) != VCHIQ_SUCCESS)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	g_regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(g_regs))
		return PTR_ERR(g_regs);

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0) {
		dev_err(dev, "failed to get IRQ\n");
		return irq;
	}

	err = devm_request_irq(dev, irq, vchiq_doorbell_irq, IRQF_IRQPOLL,
			       "VCHIQ doorbell", state);
	if (err) {
		dev_err(dev, "failed to register irq=%d\n", irq);
		return err;
	}

	/* Send the base address of the slots to VideoCore */
	channelbase = (u32)slot_phys;
	err = rpi_firmware_property(fw, RPI_FIRMWARE_VCHIQ_INIT,
				    &channelbase, sizeof(channelbase));
	if (err || channelbase) {
		dev_err(dev, "failed to set channelbase\n");
		return err ? : -ENXIO;
	}

	vchiq_log_info(vchiq_arm_log_level,
		"vchiq_init - done (slots %x, phys %pad)",
		(unsigned int)vchiq_slot_zero, &slot_phys);

	vchiq_call_connected_callbacks();

        g_dev = dev;

   return 0;
}

VCHIQ_STATUS_T
vchiq_platform_init_state(VCHIQ_STATE_T *state)
{
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;
   state->platform_state = kzalloc(sizeof(VCHIQ_2835_ARM_STATE_T), GFP_KERNEL);
   ((VCHIQ_2835_ARM_STATE_T*)state->platform_state)->inited = 1;
   status = vchiq_arm_init_state(state, &((VCHIQ_2835_ARM_STATE_T*)state->platform_state)->arm_state);
   if(status != VCHIQ_SUCCESS)
   {
      ((VCHIQ_2835_ARM_STATE_T*)state->platform_state)->inited = 0;
   }
   return status;
}

VCHIQ_ARM_STATE_T*
vchiq_platform_get_arm_state(VCHIQ_STATE_T *state)
{
   if(!((VCHIQ_2835_ARM_STATE_T*)state->platform_state)->inited)
   {
      BUG();
   }
   return &((VCHIQ_2835_ARM_STATE_T*)state->platform_state)->arm_state;
}

void
remote_event_signal(REMOTE_EVENT_T *event)
{
	wmb();

	event->fired = 1;

	wmb();         /* data barrier operation */

	if (event->armed)
		writel(0, g_regs + BELL2); /* trigger vc interrupt */
}

int
vchiq_copy_from_user(void *dst, const void *src, int size)
{
	if ((u64)src < TASK_SIZE) {
		return copy_from_user(dst, src, size);
	} else {
		memcpy(dst, src, size);
		return 0;
	}
}

VCHIQ_STATUS_T
vchiq_prepare_bulk_data(VCHIQ_BULK_T *bulk, VCHI_MEM_HANDLE_T memhandle,
	void *offset, int size, int dir)
{
	WARN_ON(memhandle != VCHI_MEM_HANDLE_INVALID);

	u32 pageoffset = (u32)offset & (PAGE_SIZE - 1);
	u32 num_pages = (size + pageoffset + PAGE_SIZE - 1) / PAGE_SIZE;

	PAGELIST_T *pagelist = kmalloc(sizeof(PAGELIST_T) +
                                       sizeof(VCHIQ_2835_ARM_BULK_DATA_T),
                                       GFP_KERNEL);

	if (!pagelist)
		return VCHIQ_ERROR;

        VCHIQ_2835_ARM_BULK_DATA_T *bulkdata = (VCHIQ_2835_ARM_BULK_DATA_T *)(pagelist + 1);

	pagelist->length = size;
	pagelist->type   = (dir == VCHIQ_BULK_RECEIVE) ? PAGELIST_READ : PAGELIST_WRITE;
	pagelist->offset = pageoffset;
	
        bulkdata->data = (char*)offset - pageoffset;
        bulkdata->size = pageoffset + size;
        bulkdata->direction = (dir == VCHIQ_BULK_RECEIVE) ?  DMA_FROM_DEVICE : DMA_TO_DEVICE;

	bulkdata->dmaaddr = 
		dma_map_single(g_dev, 
                               bulkdata->data, 
                               bulkdata->size,
                               bulkdata->direction);

	if (!bulkdata->dmaaddr)
        { 
            kfree(pagelist);
            return VCHIQ_ERROR;
        }

        pagelist->addrs[0] = (u32)(bulkdata->dmaaddr) | (u32)num_pages;
        

	bulk->handle = memhandle;
	bulk->data = (u32)dma_map_single(g_dev, pagelist, 
                                         sizeof(PAGELIST_T) + sizeof(VCHIQ_2835_ARM_BULK_DATA_T),
                                         DMA_BIDIRECTIONAL);

        if (!bulkdata->data)
        {
            dma_unmap_single(g_dev,(dma_addr_t)bulk->data, 
                             sizeof(PAGELIST_T) + sizeof(VCHIQ_2835_ARM_BULK_DATA_T),
                             DMA_BIDIRECTIONAL);

	    dma_unmap_single(g_dev, 
                             bulkdata->dmaaddr, 
                             bulkdata->size,
                             bulkdata->direction);

            kfree(pagelist);
            return VCHIQ_ERROR;
        }

	/* Store the pagelist address in remote_data, which isn't used by the
	   slave. */
	bulk->remote_data = pagelist;

	return VCHIQ_SUCCESS;
}

void
vchiq_complete_bulk(VCHIQ_BULK_T *bulk)
{
	if (bulk && bulk->remote_data && bulk->actual)
	{
	     PAGELIST_T *pagelist = bulk->remote_data;
             VCHIQ_2835_ARM_BULK_DATA_T *bulkdata = (VCHIQ_2835_ARM_BULK_DATA_T *)(pagelist + 1);

            dma_unmap_single(g_dev,(dma_addr_t)bulk->data, 
                             sizeof(PAGELIST_T) + sizeof(VCHIQ_2835_ARM_BULK_DATA_T),
                             DMA_BIDIRECTIONAL);

	    dma_unmap_single(g_dev, 
                             bulkdata->dmaaddr, 
                             bulkdata->size,
                             bulkdata->direction);

            kfree(pagelist);

	}
}

void
vchiq_transfer_bulk(VCHIQ_BULK_T *bulk)
{
	/*
	 * This should only be called on the master (VideoCore) side, but
	 * provide an implementation to avoid the need for ifdefery.
	 */
	BUG();
}

void
vchiq_dump_platform_state(void *dump_context)
{
	char buf[80];
	int len;
	len = snprintf(buf, sizeof(buf),
		"  Platform: 2835 (VC master)");
	vchiq_dump(dump_context, buf, len + 1);
}

VCHIQ_STATUS_T
vchiq_platform_suspend(VCHIQ_STATE_T *state)
{
   return VCHIQ_ERROR;
}

VCHIQ_STATUS_T
vchiq_platform_resume(VCHIQ_STATE_T *state)
{
   return VCHIQ_SUCCESS;
}

void
vchiq_platform_paused(VCHIQ_STATE_T *state)
{
}

void
vchiq_platform_resumed(VCHIQ_STATE_T *state)
{
}

int
vchiq_platform_videocore_wanted(VCHIQ_STATE_T* state)
{
   return 1; // autosuspend not supported - videocore always wanted
}

int
vchiq_platform_use_suspend_timer(void)
{
   return 0;
}
void
vchiq_dump_platform_use_state(VCHIQ_STATE_T *state)
{
	vchiq_log_info(vchiq_arm_log_level, "Suspend timer not in use");
}
void
vchiq_platform_handle_timeout(VCHIQ_STATE_T *state)
{
	(void)state;
}
/*
 * Local functions
 */

static irqreturn_t
vchiq_doorbell_irq(int irq, void *dev_id)
{
	VCHIQ_STATE_T *state = dev_id;
	irqreturn_t ret = IRQ_NONE;
	unsigned int status;

	/* Read (and clear) the doorbell */
	status = readl(g_regs + BELL0);

	if (status & 0x4) {  /* Was the doorbell rung? */
		remote_event_pollall(state);
		ret = IRQ_HANDLED;
	}

	return ret;
}

