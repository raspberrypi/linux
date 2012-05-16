/*
 *  linux/arch/arm/mach-bcm2708/vcio.c
 *
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This device provides a shared mechanism for writing to the mailboxes,
 * semaphores, doorbells etc. that are shared between the ARM and the
 * VideoCore processor
 */

#if defined(CONFIG_SERIAL_BCM_MBOX_CONSOLE) && defined(CONFIG_MAGIC_SYSRQ)
#define SUPPORT_SYSRQ
#endif

#include <linux/module.h>
#include <linux/console.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/sysrq.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/irq.h>

#include <linux/io.h>

#include <mach/vcio.h>
#include <mach/platform.h>

#define DRIVER_NAME BCM_VCIO_DRIVER_NAME

/* ----------------------------------------------------------------------
 *	Mailbox
 * -------------------------------------------------------------------- */

/* offsets from a mail box base address */
#define MAIL_WRT	0x00	/* write - and next 4 words */
#define MAIL_RD		0x00	/* read - and next 4 words */
#define MAIL_POL	0x10	/* read without popping the fifo */
#define MAIL_SND	0x14	/* sender ID (bottom two bits) */
#define MAIL_STA	0x18	/* status */
#define MAIL_CNF	0x1C	/* configuration */

#define MBOX_MSG(chan, data28)		(((data28) & ~0xf) | ((chan) & 0xf))
#define MBOX_MSG_LSB(chan, data28) (((data28) << 4) | ((chan) & 0xf))
#define MBOX_CHAN(msg)			((msg) & 0xf)
#define MBOX_DATA28(msg)		((msg) & ~0xf)
#define MBOX_DATA28_LSB(msg)		(((uint32_t)msg) >> 4)

#define MBOX_MAGIC 0xd0d0c0de

struct vc_mailbox {
	struct device *dev;	/* parent device */
	void __iomem *status;
	void __iomem *config;
	void __iomem *read;
	void __iomem *write;
	uint32_t msg[MBOX_CHAN_COUNT];
	struct semaphore sema[MBOX_CHAN_COUNT];
	uint32_t magic;
};

static void mbox_init(struct vc_mailbox *mbox_out, struct device *dev,
		      uint32_t addr_mbox)
{
	int i;

	mbox_out->dev = dev;
	mbox_out->status = __io_address(addr_mbox + MAIL_STA);
	mbox_out->config = __io_address(addr_mbox + MAIL_CNF);
	mbox_out->read = __io_address(addr_mbox + MAIL_RD);
	/* Write to the other mailbox */
	mbox_out->write =
	    __io_address((addr_mbox ^ ARM_0_MAIL0_WRT ^ ARM_0_MAIL1_WRT) +
			 MAIL_WRT);

	for (i = 0; i < MBOX_CHAN_COUNT; i++) {
		mbox_out->msg[i] = 0;
		sema_init(&mbox_out->sema[i], 0);
	}

	/* Enable the interrupt on data reception */
	writel(ARM_MC_IHAVEDATAIRQEN, mbox_out->config);

	mbox_out->magic = MBOX_MAGIC;
}

static int mbox_write(struct vc_mailbox *mbox, unsigned chan, uint32_t data28)
{
	int rc;

	if (mbox->magic != MBOX_MAGIC)
		rc = -EINVAL;
	else {
		/* wait for the mailbox FIFO to have some space in it */
		while (0 != (readl(mbox->status) & ARM_MS_FULL))
			cpu_relax();

		writel(MBOX_MSG(chan, data28), mbox->write);
		rc = 0;
	}
	return rc;
}

static int mbox_read(struct vc_mailbox *mbox, unsigned chan, uint32_t *data28)
{
	int rc;

	if (mbox->magic != MBOX_MAGIC)
		rc = -EINVAL;
	else {
		if (down_interruptible(&mbox->sema[chan]) == 0) {
			*data28 = MBOX_DATA28(mbox->msg[chan]);
			mbox->msg[chan] = 0;
			rc = 0;
		} else {
			/* The wait was interrupted */
			rc = -EINTR;
		}
	}
	return rc;
}

static irqreturn_t mbox_irq(int irq, void *dev_id)
{
	/* wait for the mailbox FIFO to have some data in it */
	struct vc_mailbox *mbox = (struct vc_mailbox *) dev_id;
	int status = readl(mbox->status);
	int ret = IRQ_NONE;

	while (!(status & ARM_MS_EMPTY)) {
		uint32_t msg = readl(mbox->read);
		int chan = MBOX_CHAN(msg);
		if (chan < MBOX_CHAN_COUNT) {
			if (mbox->msg[chan]) {
				/* Overflow */
				printk(KERN_ERR DRIVER_NAME
				       ": mbox chan %d overflow - drop %08x\n",
				       chan, msg);
			} else {
				mbox->msg[chan] = (msg | 0xf);
				up(&mbox->sema[chan]);
			}
		} else {
			printk(KERN_ERR DRIVER_NAME
			       ": invalid channel selector (msg %08x)\n", msg);
		}
		ret = IRQ_HANDLED;
		status = readl(mbox->status);
	}
	return ret;
}

static struct irqaction mbox_irqaction = {
	.name = "ARM Mailbox IRQ",
	.flags = IRQF_DISABLED | IRQF_IRQPOLL,
	.handler = mbox_irq,
};

/* ----------------------------------------------------------------------
 *	Mailbox Methods
 * -------------------------------------------------------------------- */

static struct device *mbox_dev;	/* we assume there's only one! */

static int dev_mbox_write(struct device *dev, unsigned chan, uint32_t data28)
{
	int rc;

	struct vc_mailbox *mailbox = dev_get_drvdata(dev);
	device_lock(dev);
	rc = mbox_write(mailbox, chan, data28);
	device_unlock(dev);

	return rc;
}

static int dev_mbox_read(struct device *dev, unsigned chan, uint32_t *data28)
{
	int rc;

	struct vc_mailbox *mailbox = dev_get_drvdata(dev);
	device_lock(dev);
	rc = mbox_read(mailbox, chan, data28);
	device_unlock(dev);

	return rc;
}

extern int bcm_mailbox_write(unsigned chan, uint32_t data28)
{
	if (mbox_dev)
		return dev_mbox_write(mbox_dev, chan, data28);
	else
		return -ENODEV;
}
EXPORT_SYMBOL_GPL(bcm_mailbox_write);

extern int bcm_mailbox_read(unsigned chan, uint32_t *data28)
{
	if (mbox_dev)
		return dev_mbox_read(mbox_dev, chan, data28);
	else
		return -ENODEV;
}
EXPORT_SYMBOL_GPL(bcm_mailbox_read);

static void dev_mbox_register(const char *dev_name, struct device *dev)
{
	mbox_dev = dev;
}

/* ----------------------------------------------------------------------
 *	Platform Device for Mailbox
 * -------------------------------------------------------------------- */

static int bcm_vcio_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct vc_mailbox *mailbox;

	mailbox = kzalloc(sizeof(*mailbox), GFP_KERNEL);
	if (NULL == mailbox) {
		printk(KERN_ERR DRIVER_NAME ": failed to allocate "
		       "mailbox memory\n");
		ret = -ENOMEM;
	} else {
		struct resource *res;

		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (res == NULL) {
			printk(KERN_ERR DRIVER_NAME ": failed to obtain memory "
			       "resource\n");
			ret = -ENODEV;
			kfree(mailbox);
		} else {
			/* should be based on the registers from res really */
			mbox_init(mailbox, &pdev->dev, ARM_0_MAIL0_RD);

			platform_set_drvdata(pdev, mailbox);
			dev_mbox_register(DRIVER_NAME, &pdev->dev);

			mbox_irqaction.dev_id = mailbox;
			setup_irq(IRQ_ARM_MAILBOX, &mbox_irqaction);
			printk(KERN_INFO DRIVER_NAME ": mailbox at %p\n",
			       __io_address(ARM_0_MAIL0_RD));
		}
	}
	return ret;
}

static int bcm_vcio_remove(struct platform_device *pdev)
{
	struct vc_mailbox *mailbox = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	kfree(mailbox);

	return 0;
}

static struct platform_driver bcm_mbox_driver = {
	.probe = bcm_vcio_probe,
	.remove = bcm_vcio_remove,

	.driver = {
		   .name = DRIVER_NAME,
		   .owner = THIS_MODULE,
		   },
};

static int __init bcm_mbox_init(void)
{
	int ret;

	printk(KERN_INFO "mailbox: Broadcom VideoCore Mailbox driver\n");

	ret = platform_driver_register(&bcm_mbox_driver);
	if (ret != 0) {
		printk(KERN_ERR DRIVER_NAME ": failed to register "
		       "on platform\n");
	} 

	return ret;
}

static void __exit bcm_mbox_exit(void)
{
	platform_driver_unregister(&bcm_mbox_driver);
}

arch_initcall(bcm_mbox_init);	/* Initialize early */
module_exit(bcm_mbox_exit);

MODULE_AUTHOR("Gray Girling");
MODULE_DESCRIPTION("ARM I/O to VideoCore processor");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:bcm-mbox");
