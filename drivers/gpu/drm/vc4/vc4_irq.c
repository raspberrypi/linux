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
	struct vc4_bo *bo;

	bo = vc4_bo_create(dev, 256 * 1024);
	if (!bo) {
		DRM_ERROR("Couldn't allocate binner overflow mem\n");
		return;
	}

	/* If there's a job executing currently, then our previous
	 * overflow allocation is getting used in that job and we need
	 * to queue it to be released when the job is done.  But if no
	 * job is executing at all, then we can free the old overflow
	 * object direcctly.
	 *
	 * No lock necessary for this pointer since we're the only
	 * ones that update the pointer, and our workqueue won't
	 * reenter.
	 */
	if (vc4->overflow_mem) {
		struct vc4_exec_info *current_exec;
		spin_lock(&vc4->job_lock);
		current_exec = vc4_first_job(vc4);
		if (current_exec) {
			vc4->overflow_mem->seqno = vc4->finished_seqno + 1;
			list_add_tail(&vc4->overflow_mem->unref_head,
				      &current_exec->unref_list);
			vc4->overflow_mem = NULL;
		}
		spin_unlock(&vc4->job_lock);
	}

	if (vc4->overflow_mem) {
		drm_gem_object_unreference_unlocked(&vc4->overflow_mem->base.base);
	}
	vc4->overflow_mem = bo;

	V3D_WRITE(V3D_BPOA, bo->base.paddr);
	V3D_WRITE(V3D_BPOS, bo->base.base.size);
	V3D_WRITE(V3D_INTDIS, 0);
	V3D_WRITE(V3D_INTENA, V3D_DRIVER_IRQS);
	V3D_WRITE(V3D_INTCTL, V3D_INT_OUTOMEM);
}

static void
vc4_irq_finish_job(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_exec_info *exec = vc4_first_job(vc4);

	spin_lock(&vc4->job_lock);
	if (!exec) {
		spin_unlock(&vc4->job_lock);
		return;
	}

	vc4->finished_seqno++;
	list_move_tail(&exec->head, &vc4->job_done_list);
	vc4_submit_next_job(dev);

	spin_unlock(&vc4->job_lock);

	wake_up_all(&vc4->job_wait_queue);
	schedule_work(&vc4->job_done_work);
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
		vc4_irq_finish_job(dev);
	}

	return intctl ? IRQ_HANDLED : IRQ_NONE;
}

void
vc4_irq_preinstall(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	init_waitqueue_head(&vc4->job_wait_queue);
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

	vc4_irq_finish_job(dev);
}
