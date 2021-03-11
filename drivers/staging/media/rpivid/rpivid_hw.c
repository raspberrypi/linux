// SPDX-License-Identifier: GPL-2.0
/*
 * Raspberry Pi HEVC driver
 *
 * Copyright (C) 2020 Raspberry Pi (Trading) Ltd
 *
 * Based on the Cedrus VPU driver, that is:
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 */
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <media/videobuf2-core.h>
#include <media/v4l2-mem2mem.h>

#include "rpivid.h"
#include "rpivid_hw.h"

static void pre_irq(struct rpivid_dev *dev, struct rpivid_hw_irq_ent *ient,
		    rpivid_irq_callback cb, void *v,
		    struct rpivid_hw_irq_ctrl *ictl)
{
	unsigned long flags;

	if (ictl->irq) {
		v4l2_err(&dev->v4l2_dev, "Attempt to claim IRQ when already claimed\n");
		return;
	}

	ient->cb = cb;
	ient->v = v;

	spin_lock_irqsave(&ictl->lock, flags);
	ictl->irq = ient;
	ictl->no_sched++;
	spin_unlock_irqrestore(&ictl->lock, flags);
}

/* Should be called from inside ictl->lock */
static inline bool sched_enabled(const struct rpivid_hw_irq_ctrl * const ictl)
{
	return ictl->no_sched <= 0 && ictl->enable;
}

/* Should be called from inside ictl->lock & after checking sched_enabled() */
static inline void set_claimed(struct rpivid_hw_irq_ctrl * const ictl)
{
	if (ictl->enable > 0)
		--ictl->enable;
	ictl->no_sched = 1;
}

/* Should be called from inside ictl->lock */
static struct rpivid_hw_irq_ent *get_sched(struct rpivid_hw_irq_ctrl * const ictl)
{
	struct rpivid_hw_irq_ent *ient;

	if (!sched_enabled(ictl))
		return NULL;

	ient = ictl->claim;
	if (!ient)
		return NULL;
	ictl->claim = ient->next;

	set_claimed(ictl);
	return ient;
}

/* Run a callback & check to see if there is anything else to run */
static void sched_cb(struct rpivid_dev * const dev,
		     struct rpivid_hw_irq_ctrl * const ictl,
		     struct rpivid_hw_irq_ent *ient)
{
	while (ient) {
		unsigned long flags;

		ient->cb(dev, ient->v);

		spin_lock_irqsave(&ictl->lock, flags);

		/* Always dec no_sched after cb exec - must have been set
		 * on entry to cb
		 */
		--ictl->no_sched;
		ient = get_sched(ictl);

		spin_unlock_irqrestore(&ictl->lock, flags);
	}
}

/* Should only ever be called from its own IRQ cb so no lock required */
static void pre_thread(struct rpivid_dev *dev,
		       struct rpivid_hw_irq_ent *ient,
		       rpivid_irq_callback cb, void *v,
		       struct rpivid_hw_irq_ctrl *ictl)
{
	ient->cb = cb;
	ient->v = v;
	ictl->irq = ient;
	ictl->thread_reqed = true;
	ictl->no_sched++;	/* This is unwound in do_thread */
}

// Called in irq context
static void do_irq(struct rpivid_dev * const dev,
		   struct rpivid_hw_irq_ctrl * const ictl)
{
	struct rpivid_hw_irq_ent *ient;
	unsigned long flags;

	spin_lock_irqsave(&ictl->lock, flags);
	ient = ictl->irq;
	ictl->irq = NULL;
	spin_unlock_irqrestore(&ictl->lock, flags);

	sched_cb(dev, ictl, ient);
}

static void do_claim(struct rpivid_dev * const dev,
		     struct rpivid_hw_irq_ent *ient,
		     const rpivid_irq_callback cb, void * const v,
		     struct rpivid_hw_irq_ctrl * const ictl)
{
	unsigned long flags;

	ient->next = NULL;
	ient->cb = cb;
	ient->v = v;

	spin_lock_irqsave(&ictl->lock, flags);

	if (ictl->claim) {
		// If we have a Q then add to end
		ictl->tail->next = ient;
		ictl->tail = ient;
		ient = NULL;
	} else if (!sched_enabled(ictl)) {
		// Empty Q but other activity in progress so Q
		ictl->claim = ient;
		ictl->tail = ient;
		ient = NULL;
	} else {
		// Nothing else going on - schedule immediately and
		// prevent anything else scheduling claims
		set_claimed(ictl);
	}

	spin_unlock_irqrestore(&ictl->lock, flags);

	sched_cb(dev, ictl, ient);
}

/* Enable n claims.
 * n < 0   set to unlimited (default on init)
 * n = 0   if previously unlimited then disable otherwise nop
 * n > 0   if previously unlimited then set to n enables
 *         otherwise add n enables
 * The enable count is automatically decremented every time a claim is run
 */
static void do_enable_claim(struct rpivid_dev * const dev,
			    int n,
			    struct rpivid_hw_irq_ctrl * const ictl)
{
	unsigned long flags;
	struct rpivid_hw_irq_ent *ient;

	spin_lock_irqsave(&ictl->lock, flags);
	ictl->enable = n < 0 ? -1 : ictl->enable <= 0 ? n : ictl->enable + n;
	ient = get_sched(ictl);
	spin_unlock_irqrestore(&ictl->lock, flags);

	sched_cb(dev, ictl, ient);
}

static void ictl_init(struct rpivid_hw_irq_ctrl * const ictl)
{
	spin_lock_init(&ictl->lock);
	ictl->claim = NULL;
	ictl->tail = NULL;
	ictl->irq = NULL;
	ictl->no_sched = 0;
	ictl->enable = -1;
	ictl->thread_reqed = false;
}

static void ictl_uninit(struct rpivid_hw_irq_ctrl * const ictl)
{
	// Nothing to do
}

#if !OPT_DEBUG_POLL_IRQ
static irqreturn_t rpivid_irq_irq(int irq, void *data)
{
	struct rpivid_dev * const dev = data;
	__u32 ictrl;

	ictrl = irq_read(dev, ARG_IC_ICTRL);
	if (!(ictrl & ARG_IC_ICTRL_ALL_IRQ_MASK)) {
		v4l2_warn(&dev->v4l2_dev, "IRQ but no IRQ bits set\n");
		return IRQ_NONE;
	}

	// Cancel any/all irqs
	irq_write(dev, ARG_IC_ICTRL, ictrl & ~ARG_IC_ICTRL_SET_ZERO_MASK);

	// Service Active2 before Active1 so Phase 1 can transition to Phase 2
	// without delay
	if (ictrl & ARG_IC_ICTRL_ACTIVE2_INT_SET)
		do_irq(dev, &dev->ic_active2);
	if (ictrl & ARG_IC_ICTRL_ACTIVE1_INT_SET)
		do_irq(dev, &dev->ic_active1);

	return dev->ic_active1.thread_reqed || dev->ic_active2.thread_reqed ?
		IRQ_WAKE_THREAD : IRQ_HANDLED;
}

static void do_thread(struct rpivid_dev * const dev,
		      struct rpivid_hw_irq_ctrl *const ictl)
{
	unsigned long flags;
	struct rpivid_hw_irq_ent *ient = NULL;

	spin_lock_irqsave(&ictl->lock, flags);

	if (ictl->thread_reqed) {
		ient = ictl->irq;
		ictl->thread_reqed = false;
		ictl->irq = NULL;
	}

	spin_unlock_irqrestore(&ictl->lock, flags);

	sched_cb(dev, ictl, ient);
}

static irqreturn_t rpivid_irq_thread(int irq, void *data)
{
	struct rpivid_dev * const dev = data;

	do_thread(dev, &dev->ic_active1);
	do_thread(dev, &dev->ic_active2);

	return IRQ_HANDLED;
}
#endif

/* May only be called from Active1 CB
 * IRQs should not be expected until execution continues in the cb
 */
void rpivid_hw_irq_active1_thread(struct rpivid_dev *dev,
				  struct rpivid_hw_irq_ent *ient,
				  rpivid_irq_callback thread_cb, void *ctx)
{
	pre_thread(dev, ient, thread_cb, ctx, &dev->ic_active1);
}

void rpivid_hw_irq_active1_enable_claim(struct rpivid_dev *dev,
					int n)
{
	do_enable_claim(dev, n, &dev->ic_active1);
}

void rpivid_hw_irq_active1_claim(struct rpivid_dev *dev,
				 struct rpivid_hw_irq_ent *ient,
				 rpivid_irq_callback ready_cb, void *ctx)
{
	do_claim(dev, ient, ready_cb, ctx, &dev->ic_active1);
}

void rpivid_hw_irq_active1_irq(struct rpivid_dev *dev,
			       struct rpivid_hw_irq_ent *ient,
			       rpivid_irq_callback irq_cb, void *ctx)
{
	pre_irq(dev, ient, irq_cb, ctx, &dev->ic_active1);
}

void rpivid_hw_irq_active2_claim(struct rpivid_dev *dev,
				 struct rpivid_hw_irq_ent *ient,
				 rpivid_irq_callback ready_cb, void *ctx)
{
	do_claim(dev, ient, ready_cb, ctx, &dev->ic_active2);
}

void rpivid_hw_irq_active2_irq(struct rpivid_dev *dev,
			       struct rpivid_hw_irq_ent *ient,
			       rpivid_irq_callback irq_cb, void *ctx)
{
	pre_irq(dev, ient, irq_cb, ctx, &dev->ic_active2);
}

int rpivid_hw_probe(struct rpivid_dev *dev)
{
	struct resource *res;
	__u32 irq_stat;
	int irq_dec;
	int ret = 0;

	ictl_init(&dev->ic_active1);
	ictl_init(&dev->ic_active2);

	res = platform_get_resource_byname(dev->pdev, IORESOURCE_MEM, "intc");
	if (!res)
		return -ENODEV;

	dev->base_irq = devm_ioremap(dev->dev, res->start, resource_size(res));
	if (IS_ERR(dev->base_irq))
		return PTR_ERR(dev->base_irq);

	res = platform_get_resource_byname(dev->pdev, IORESOURCE_MEM, "hevc");
	if (!res)
		return -ENODEV;

	dev->base_h265 = devm_ioremap(dev->dev, res->start, resource_size(res));
	if (IS_ERR(dev->base_h265))
		return PTR_ERR(dev->base_h265);

	dev->clock = devm_clk_get(&dev->pdev->dev, "hevc");
	if (IS_ERR(dev->clock))
		return PTR_ERR(dev->clock);

	// Disable IRQs & reset anything pending
	irq_write(dev, 0,
		  ARG_IC_ICTRL_ACTIVE1_EN_SET | ARG_IC_ICTRL_ACTIVE2_EN_SET);
	irq_stat = irq_read(dev, 0);
	irq_write(dev, 0, irq_stat);

#if !OPT_DEBUG_POLL_IRQ
	irq_dec = platform_get_irq(dev->pdev, 0);
	if (irq_dec <= 0)
		return irq_dec;
	ret = devm_request_threaded_irq(dev->dev, irq_dec,
					rpivid_irq_irq,
					rpivid_irq_thread,
					0, dev_name(dev->dev), dev);
	if (ret) {
		dev_err(dev->dev, "Failed to request IRQ - %d\n", ret);

		return ret;
	}
#endif
	return ret;
}

void rpivid_hw_remove(struct rpivid_dev *dev)
{
	// IRQ auto freed on unload so no need to do it here
	ictl_uninit(&dev->ic_active1);
	ictl_uninit(&dev->ic_active2);
}

