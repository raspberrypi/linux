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

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/platform_data/mailbox-bcm2708.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#define DRIVER_NAME "bcm2708_vcio"
#define DEVICE_FILE_NAME "vcio"

/* offsets from a mail box base address */
#define MAIL0_RD	0x00	/* read - and next 4 words */
#define MAIL0_POL	0x10	/* read without popping the fifo */
#define MAIL0_SND	0x14	/* sender ID (bottom two bits) */
#define MAIL0_STA	0x18	/* status */
#define MAIL0_CNF	0x1C	/* configuration */
#define MAIL1_WRT	0x20	/* write - and next 4 words */
#define MAIL1_STA	0x38	/* status */

/* On MACH_BCM270x these come through <linux/interrupt.h> (arm_control.h ) */
#ifndef ARM_MS_EMPTY
#define ARM_MS_EMPTY   BIT(30)
#define ARM_MS_FULL    BIT(31)

#define ARM_MC_IHAVEDATAIRQEN  BIT(0)
#endif

#define MBOX_MSG(chan, data28)		(((data28) & ~0xf) | ((chan) & 0xf))
#define MBOX_MSG_LSB(chan, data28)	(((data28) << 4) | ((chan) & 0xf))
#define MBOX_CHAN(msg)			((msg) & 0xf)
#define MBOX_DATA28(msg)		((msg) & ~0xf)
#define MBOX_DATA28_LSB(msg)		(((uint32_t)msg) >> 4)

#define MBOX_MAGIC 0xd0d0c0de

#define MAJOR_NUM 100
#define IOCTL_MBOX_PROPERTY _IOWR(MAJOR_NUM, 0, char *)

static struct class *vcio_class;

struct vc_mailbox {
	void __iomem *regs;
	uint32_t msg[MBOX_CHAN_COUNT];
	struct semaphore sema[MBOX_CHAN_COUNT];
	uint32_t magic;
};

static void mbox_init(struct vc_mailbox *mbox_out)
{
	int i;

	for (i = 0; i < MBOX_CHAN_COUNT; i++) {
		mbox_out->msg[i] = 0;
		sema_init(&mbox_out->sema[i], 0);
	}

	/* Enable the interrupt on data reception */
	writel(ARM_MC_IHAVEDATAIRQEN, mbox_out->regs + MAIL0_CNF);

	mbox_out->magic = MBOX_MAGIC;
}

static int mbox_write(struct vc_mailbox *mbox, unsigned chan, uint32_t data28)
{
	if (mbox->magic != MBOX_MAGIC)
		return -EINVAL;

	/* wait for the mailbox FIFO to have some space in it */
	while (0 != (readl(mbox->regs + MAIL1_STA) & ARM_MS_FULL))
		cpu_relax();

	writel(MBOX_MSG(chan, data28), mbox->regs + MAIL1_WRT);

	return 0;
}

static int mbox_read(struct vc_mailbox *mbox, unsigned chan, uint32_t *data28)
{
	if (mbox->magic != MBOX_MAGIC)
		return -EINVAL;

	down(&mbox->sema[chan]);
	*data28 = MBOX_DATA28(mbox->msg[chan]);
	mbox->msg[chan] = 0;

	return 0;
}

static irqreturn_t mbox_irq_handler(int irq, void *dev_id)
{
	/* wait for the mailbox FIFO to have some data in it */
	struct vc_mailbox *mbox = (struct vc_mailbox *)dev_id;
	int status = readl(mbox->regs + MAIL0_STA);
	int ret = IRQ_NONE;

	while (!(status & ARM_MS_EMPTY)) {
		uint32_t msg = readl(mbox->regs + MAIL0_RD);
		int chan = MBOX_CHAN(msg);

		if (chan < MBOX_CHAN_COUNT) {
			if (mbox->msg[chan]) {
				pr_err(DRIVER_NAME
				       ": mbox chan %d overflow - drop %08x\n",
				       chan, msg);
			} else {
				mbox->msg[chan] = (msg | 0xf);
				up(&mbox->sema[chan]);
			}
		} else {
			pr_err(DRIVER_NAME
			       ": invalid channel selector (msg %08x)\n", msg);
		}
		ret = IRQ_HANDLED;
		status = readl(mbox->regs + MAIL0_STA);
	}
	return ret;
}

/* Mailbox Methods */

static struct device *mbox_dev;	/* we assume there's only one! */

static int dev_mbox_write(struct device *dev, unsigned chan, uint32_t data28)
{
	struct vc_mailbox *mailbox = dev_get_drvdata(dev);
	int rc;

	device_lock(dev);
	rc = mbox_write(mailbox, chan, data28);
	device_unlock(dev);

	return rc;
}

static int dev_mbox_read(struct device *dev, unsigned chan, uint32_t *data28)
{
	struct vc_mailbox *mailbox = dev_get_drvdata(dev);
	int rc;

	device_lock(dev);
	rc = mbox_read(mailbox, chan, data28);
	device_unlock(dev);

	return rc;
}

extern int bcm_mailbox_write(unsigned chan, uint32_t data28)
{
	if (!mbox_dev)
		return -ENODEV;

	return dev_mbox_write(mbox_dev, chan, data28);
}
EXPORT_SYMBOL_GPL(bcm_mailbox_write);

extern int bcm_mailbox_read(unsigned chan, uint32_t *data28)
{
	if (!mbox_dev)
		return -ENODEV;

	return dev_mbox_read(mbox_dev, chan, data28);
}
EXPORT_SYMBOL_GPL(bcm_mailbox_read);

static int mbox_copy_from_user(void *dst, const void *src, int size)
{
	if ((uint32_t)src < TASK_SIZE)
		return copy_from_user(dst, src, size);

	memcpy(dst, src, size);

	return 0;
}

static int mbox_copy_to_user(void *dst, const void *src, int size)
{
	if ((uint32_t)dst < TASK_SIZE)
		return copy_to_user(dst, src, size);

	memcpy(dst, src, size);

	return 0;
}

static DEFINE_MUTEX(mailbox_lock);
extern int bcm_mailbox_property(void *data, int size)
{
	uint32_t success;
	dma_addr_t mem_bus; /* the memory address accessed from videocore */
	void *mem_kern;     /* the memory address accessed from driver */
	int s = 0;

	mutex_lock(&mailbox_lock);
	/* allocate some memory for the messages communicating with GPU */
	mem_kern = dma_alloc_coherent(NULL, PAGE_ALIGN(size), &mem_bus,
				      GFP_KERNEL);
	if (mem_kern) {
		/* create the message */
		mbox_copy_from_user(mem_kern, data, size);

		/* send the message */
		wmb();
		s = bcm_mailbox_write(MBOX_CHAN_PROPERTY, (uint32_t)mem_bus);
		if (s == 0)
			s = bcm_mailbox_read(MBOX_CHAN_PROPERTY, &success);
		if (s == 0) {
			/* copy the response */
			rmb();
			mbox_copy_to_user(data, mem_kern, size);
		}
		dma_free_coherent(NULL, PAGE_ALIGN(size), mem_kern, mem_bus);
	} else {
		s = -ENOMEM;
	}
	if (s != 0)
		pr_err(DRIVER_NAME ": %s failed (%d)\n", __func__, s);

	mutex_unlock(&mailbox_lock);
	return s;
}
EXPORT_SYMBOL_GPL(bcm_mailbox_property);

/* Platform Device for Mailbox */

/*
 * Is the device open right now? Used to prevent
 * concurent access into the same device
 */
static bool device_is_open;

/* This is called whenever a process attempts to open the device file */
static int device_open(struct inode *inode, struct file *file)
{
	/* We don't want to talk to two processes at the same time */
	if (device_is_open)
		return -EBUSY;

	device_is_open = true;
	try_module_get(THIS_MODULE);

	return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
	/* We're now ready for our next caller */
	device_is_open = false;

	module_put(THIS_MODULE);

	return 0;
}

/*
 * This function is called whenever a process tries to do an ioctl on our
 * device file. We get two extra parameters (additional to the inode and file
 * structures, which all device functions get): the number of the ioctl called
 * and the parameter given to the ioctl function.
 *
 * If the ioctl is write or read/write (meaning output is returned to the
 * calling process), the ioctl call returns the output of this function.
 *
 */
static long device_ioctl(struct file *file, unsigned int ioctl_num,
			 unsigned long ioctl_param)
{
	unsigned size;

	switch (ioctl_num) {
	case IOCTL_MBOX_PROPERTY:
		/*
		 * Receive a pointer to a message (in user space) and set that
		 * to be the device's message.  Get the parameter given to
		 * ioctl by the process.
		 */
		mbox_copy_from_user(&size, (void *)ioctl_param, sizeof(size));
		return bcm_mailbox_property((void *)ioctl_param, size);
	default:
		pr_err(DRIVER_NAME "unknown ioctl: %d\n", ioctl_num);
		return -EINVAL;
	}

	return 0;
}

/* Module Declarations */

/*
 * This structure will hold the functions to be called
 * when a process does something to the device we
 * created. Since a pointer to this structure is kept in
 * the devices table, it can't be local to
 * init_module. NULL is for unimplemented functios.
 */
const struct file_operations fops = {
	.unlocked_ioctl = device_ioctl,
	.open = device_open,
	.release = device_release,	/* a.k.a. close */
};

static int bcm_vcio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *vdev;
	struct vc_mailbox *mailbox;
	struct resource *res;
	int irq, ret;

	mailbox = devm_kzalloc(dev, sizeof(*mailbox), GFP_KERNEL);
	if (!mailbox)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mailbox->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(mailbox->regs))
		return PTR_ERR(mailbox->regs);

	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(dev, irq, mbox_irq_handler,
			       IRQF_IRQPOLL,
			       dev_name(dev), mailbox);
	if (ret) {
		dev_err(dev, "Interrupt request failed %d\n", ret);
		return ret;
	}

	ret = register_chrdev(MAJOR_NUM, DEVICE_FILE_NAME, &fops);
	if (ret < 0) {
		pr_err("Character device registration failed %d\n", ret);
		return ret;
	}

	vcio_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(vcio_class)) {
		ret = PTR_ERR(vcio_class);
		pr_err("Class creation failed %d\n", ret);
		goto err_class;
	}

	vdev = device_create(vcio_class, NULL, MKDEV(MAJOR_NUM, 0), NULL,
			     "vcio");
	if (IS_ERR(vdev)) {
		ret = PTR_ERR(vdev);
		pr_err("Device creation failed %d\n", ret);
		goto err_dev;
	}

	mbox_init(mailbox);
	platform_set_drvdata(pdev, mailbox);
	mbox_dev = dev;

	dev_info(dev, "mailbox at %p\n", mailbox->regs);

	return 0;

err_dev:
	class_destroy(vcio_class);
err_class:
	unregister_chrdev(MAJOR_NUM, DEVICE_FILE_NAME);

	return ret;
}

static int bcm_vcio_remove(struct platform_device *pdev)
{
	mbox_dev = NULL;
	platform_set_drvdata(pdev, NULL);
	device_destroy(vcio_class, MKDEV(MAJOR_NUM, 0));
	class_destroy(vcio_class);
	unregister_chrdev(MAJOR_NUM, DEVICE_FILE_NAME);

	return 0;
}

static const struct of_device_id bcm_vcio_of_match_table[] = {
	{ .compatible = "brcm,bcm2708-vcio", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm_vcio_of_match_table);

static struct platform_driver bcm_mbox_driver = {
	.probe = bcm_vcio_probe,
	.remove = bcm_vcio_remove,

	.driver = {
		   .name = DRIVER_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = bcm_vcio_of_match_table,
		   },
};

static int __init bcm_mbox_init(void)
{
	return platform_driver_register(&bcm_mbox_driver);
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
