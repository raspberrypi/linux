/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "vc4_drv.h"
#include "vc4_regs.h"

#define V3D_DRIVER_IRQS (V3D_INT_OUTOMEM | \
			 V3D_INT_FRDONE)

DECLARE_WAIT_QUEUE_HEAD(render_wait);

static void
vc4_overflow_mem_work(struct work_struct *work)
{
	struct vc4_dev *vc4 =
		container_of(work, struct vc4_dev, overflow_mem_work);
	struct drm_device *dev = vc4->dev;
	struct vc4_bo_list_entry *entry = kmalloc(sizeof(*entry), GFP_KERNEL);

	if (!entry) {
		DRM_ERROR("Couldn't allocate binner overflow mem record\n");
		return;
	}

	entry->bo = drm_gem_cma_create(dev, 256 * 1024);
	if (IS_ERR(entry->bo)) {
		DRM_ERROR("Couldn't allocate binner overflow mem\n");
		kfree(entry);
		return;
	}

	list_add_tail(&entry->head, &vc4->overflow_list);

	V3D_WRITE(V3D_BPOA, entry->bo->paddr);
	V3D_WRITE(V3D_BPOS, entry->bo->base.size);
	V3D_WRITE(V3D_INTDIS, 0);
	V3D_WRITE(V3D_INTCTL, V3D_INT_OUTOMEM);
}

irqreturn_t
vc4_irq(int irq, void *arg)
{
	struct drm_device *dev = arg;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	uint32_t intctl;

	barrier();
	intctl = V3D_READ(V3D_INTCTL);
	V3D_WRITE(V3D_INTCTL, intctl);

	if (intctl & V3D_INT_OUTOMEM) {
		V3D_WRITE(V3D_INTDIS, V3D_INT_OUTOMEM);
		schedule_work(&vc4->overflow_mem_work);
	}

	if (intctl & V3D_INT_FRDONE) {
		vc4->frame_done = true;
		wake_up_all(&vc4->frame_done_queue);
	}

	return intctl ? IRQ_HANDLED : IRQ_NONE;
}

void
vc4_irq_preinstall(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	init_waitqueue_head(&vc4->frame_done_queue);
	INIT_WORK(&vc4->overflow_mem_work, vc4_overflow_mem_work);

	/* Clear any pending interrupts someone might have left around
	 * for us.
	 */
	V3D_WRITE(V3D_INTCTL, V3D_DRIVER_IRQS);
}

int
vc4_irq_postinstall(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	/* Enable both the bin and render done interrupts, as well as
	 * out of memory.  Eventually, we'll have the bin use internal
	 * semaphores with render to sync between the two, but for now
	 * we're driving that from the ARM.
	 */
	V3D_WRITE(V3D_INTENA, V3D_DRIVER_IRQS);

	/* No interrupts disabled. */
	V3D_WRITE(V3D_INTDIS, 0);

	return 0;
}

void
vc4_irq_uninstall(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	cancel_work_sync(&vc4->overflow_mem_work);

	V3D_WRITE(V3D_INTENA, 0);
	V3D_WRITE(V3D_INTDIS, 0);

	/* Clear any pending interrupts we might have left. */
	V3D_WRITE(V3D_INTCTL, V3D_DRIVER_IRQS);
}

/** Reinitializes interrupt registers when a GPU reset is performed. */
void vc4_irq_reset(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	V3D_WRITE(V3D_INTCTL, V3D_DRIVER_IRQS);
	V3D_WRITE(V3D_INTDIS, 0);
	V3D_WRITE(V3D_INTENA, V3D_DRIVER_IRQS);

	vc4->frame_done = true;
	wake_up_all(&vc4->frame_done_queue);
}
