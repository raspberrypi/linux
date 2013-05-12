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

#include <asm/uaccess.h>


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
		down(&mbox->sema[chan]);
		*data28 = MBOX_DATA28(mbox->msg[chan]);
		mbox->msg[chan] = 0;
		rc = 0;
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

static int mbox_copy_from_user(void *dst, const void *src, int size)
{
	if ( (uint32_t)src < TASK_SIZE)
	{
		return copy_from_user(dst, src, size);
	}
	else
	{
		memcpy( dst, src, size );
		return 0;
	}
}

static int mbox_copy_to_user(void *dst, const void *src, int size)
{
	if ( (uint32_t)dst < TASK_SIZE)
	{
		return copy_to_user(dst, src, size);
	}
	else
	{
		memcpy( dst, src, size );
		return 0;
	}
}

static DEFINE_MUTEX(mailbox_lock);
extern int bcm_mailbox_property(void *data, int size)
{
	uint32_t success;
	dma_addr_t mem_bus;				/* the memory address accessed from videocore */
	void *mem_kern;					/* the memory address accessed from driver */
	int s = 0;

        mutex_lock(&mailbox_lock);
	/* allocate some memory for the messages communicating with GPU */
	mem_kern = dma_alloc_coherent(NULL, PAGE_ALIGN(size), &mem_bus, GFP_ATOMIC);
	if (mem_kern) {
		/* create the message */
		mbox_copy_from_user(mem_kern, data, size);

		/* send the message */
		wmb();
		s = bcm_mailbox_write(MBOX_CHAN_PROPERTY, (uint32_t)mem_bus);
		if (s == 0) {
			s = bcm_mailbox_read(MBOX_CHAN_PROPERTY, &success);
		}
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
		printk(KERN_ERR DRIVER_NAME ": %s failed (%d)\n", __func__, s);

        mutex_unlock(&mailbox_lock);
	return s;
}
EXPORT_SYMBOL_GPL(bcm_mailbox_property);

/* ----------------------------------------------------------------------
 *	Platform Device for Mailbox
 * -------------------------------------------------------------------- */

/*
 * Is the device open right now? Used to prevent
 * concurent access into the same device
 */
static int Device_Open = 0;

/*
 * This is called whenever a process attempts to open the device file
 */
static int device_open(struct inode *inode, struct file *file)
{
	/*
	 * We don't want to talk to two processes at the same time
	 */
	if (Device_Open)
		return -EBUSY;

	Device_Open++;
	/*
	 * Initialize the message
	 */
	try_module_get(THIS_MODULE);
	return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
	/*
	 * We're now ready for our next caller
	 */
	Device_Open--;

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
static long device_ioctl(struct file *file,	/* see include/linux/fs.h */
		 unsigned int ioctl_num,	/* number and param for ioctl */
		 unsigned long ioctl_param)
{
	unsigned size;
	/*
	 * Switch according to the ioctl called
	 */
	switch (ioctl_num) {
	case IOCTL_MBOX_PROPERTY:
		/*
		 * Receive a pointer to a message (in user space) and set that
		 * to be the device's message.  Get the parameter given to
		 * ioctl by the process.
		 */
		mbox_copy_from_user(&size, (void *)ioctl_param, sizeof size);
		return bcm_mailbox_property((void *)ioctl_param, size);
		break;
	default:
		printk(KERN_ERR DRIVER_NAME "unknown ioctl: %d\n", ioctl_num);
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
struct file_operations fops = {
	.unlocked_ioctl = device_ioctl,
	.open = device_open,
	.release = device_release,	/* a.k.a. close */
};

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

	if (ret == 0) {
		/*
		 * Register the character device
		 */
		ret = register_chrdev(MAJOR_NUM, DEVICE_FILE_NAME, &fops);

		/*
		 * Negative values signify an error
		 */
		if (ret < 0) {
			printk(KERN_ERR DRIVER_NAME
			       "Failed registering the character device %d\n", ret);
			return ret;
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
