/*
 *  linux/arch/arm/mach-bcm2708/armctrl.c
 *
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/init.h>
#include <linux/list.h>
#include <linux/io.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,39)
#include <linux/syscore_ops.h>
#else
#include <linux/sysdev.h>
#endif
#include <linux/interrupt.h>

#include <asm/mach/irq.h>
#include <mach/hardware.h>
#include "armctrl.h"

/* For support of kernels >= 3.0 assume only one VIC for now*/
static unsigned int remap_irqs[(INTERRUPT_ARASANSDIO + 1) - INTERRUPT_JPEG] = {
	INTERRUPT_VC_JPEG,
	INTERRUPT_VC_USB,
	INTERRUPT_VC_3D,
	INTERRUPT_VC_DMA2,
	INTERRUPT_VC_DMA3,
	INTERRUPT_VC_I2C,
	INTERRUPT_VC_SPI,
	INTERRUPT_VC_I2SPCM,
	INTERRUPT_VC_SDIO,
	INTERRUPT_VC_UART,
	INTERRUPT_VC_ARASANSDIO
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
static void armctrl_mask_irq(struct irq_data *d)
#else
static void armctrl_mask_irq(unsigned int irq)
#endif
{
	static const unsigned int disables[4] = {
		IO_ADDRESS(ARM_IRQ_DIBL1),
		IO_ADDRESS(ARM_IRQ_DIBL2),
		IO_ADDRESS(ARM_IRQ_DIBL3),
		0
	};
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
	unsigned int data = (unsigned int)irq_get_chip_data(d->irq);
#else
	unsigned int data = (unsigned int)get_irq_chip_data(irq);
#endif
	writel(1 << (data & 0x1f), __io(disables[(data >> 5) & 0x3]));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
static void armctrl_unmask_irq(struct irq_data *d)
#else
static void armctrl_unmask_irq(unsigned int irq)
#endif
{
	static const unsigned int enables[4] = {
		IO_ADDRESS(ARM_IRQ_ENBL1),
		IO_ADDRESS(ARM_IRQ_ENBL2),
		IO_ADDRESS(ARM_IRQ_ENBL3),
		0
	};
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
	unsigned int data = (unsigned int)irq_get_chip_data(d->irq);
#else
	unsigned int data = (unsigned int)get_irq_chip_data(irq);
#endif
	writel(1 << (data & 0x1f), __io(enables[(data >> 5) & 0x3]));
}

#if defined(CONFIG_PM)

/* for kernels 3.xx use the new syscore_ops apis but for older kernels use the sys dev class */

/* Static defines
 * struct armctrl_device - VIC PM device (< 3.xx)
 * @sysdev: The system device which is registered. (< 3.xx)
 * @irq: The IRQ number for the base of the VIC.
 * @base: The register base for the VIC.
 * @resume_sources: A bitmask of interrupts for resume.
 * @resume_irqs: The IRQs enabled for resume.
 * @int_select: Save for VIC_INT_SELECT.
 * @int_enable: Save for VIC_INT_ENABLE.
 * @soft_int: Save for VIC_INT_SOFT.
 * @protect: Save for VIC_PROTECT.
 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,39)
struct armctrl_device {
	struct sys_device sysdev;
#else
	struct armctrl_info {
#endif
		void __iomem *base;
		int irq;
		u32 resume_sources;
		u32 resume_irqs;
		u32 int_select;
		u32 int_enable;
		u32 soft_int;
		u32 protect;
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,39)
};
#else
	} armctrl;
#endif

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,39)

static struct armctrl_device armctrl_devices[1];

static inline struct armctrl_device *to_vic(struct sys_device *sys)
{
	return container_of(sys, struct armctrl_device, sysdev);
}

static int armctrl_id;

static int armctrl_class_resume(struct sys_device *dev)
{
#if 0  // FIXME
	struct armctrl_device *armctrl = to_vic(dev);
	void __iomem *base = armctrl->base;

	printk(KERN_DEBUG "%s: resuming armctrl at %p\n", __func__, base);

	writel(armctrl->int_select, base + VIC_INT_SELECT);
	writel(armctrl->protect, base + VIC_PROTECT);

	/* set the enabled ints and then clear the non-enabled */
	writel(armctrl->int_enable, base + VIC_INT_ENABLE);
	writel(~armctrl->int_enable, base + VIC_INT_ENABLE_CLEAR);

	/* and the same for the soft-int register */

	writel(armctrl->soft_int, base + VIC_INT_SOFT);
	writel(~armctrl->soft_int, base + VIC_INT_SOFT_CLEAR);
#endif
	return 0;
}

static int armctrl_class_suspend(struct sys_device *dev, pm_message_t state)
{
#if 0				// FIXME
	struct armctrl_device *armctrl = to_vic(dev);
	void __iomem *base = armctrl->base;

	printk(KERN_DEBUG "%s: suspending armctrl at %p\n", __func__, base);

	armctrl->int_select = readl(base + VIC_INT_SELECT);
	armctrl->int_enable = readl(base + VIC_INT_ENABLE);
	armctrl->soft_int = readl(base + VIC_INT_SOFT);
	armctrl->protect = readl(base + VIC_PROTECT);

	/* set the interrupts (if any) that are used for
	 * resuming the system */

	writel(armctrl->resume_irqs, base + VIC_INT_ENABLE);
	writel(~armctrl->resume_irqs, base + VIC_INT_ENABLE_CLEAR);
#endif
	return 0;
}

struct sysdev_class armctrl_class = {
	.name = "armctrl",
	.suspend = armctrl_class_suspend,
	.resume = armctrl_class_resume,
};

#endif // < 2.6.39

static int armctrl_suspend(void)
{
	return 0;
}

static void armctrl_resume(void)
{
	return;
}


/**
 * armctrl_pm_register - Register a VIC for later power management control
 * @base: The base address of the VIC.
 * @irq: The base IRQ for the VIC.
 * @resume_sources: bitmask of interrupts allowed for resume sources.
 *
 * For older kernels (< 3.xx) do -
 * Register the VIC with the system device tree so that it can be notified
 * of suspend and resume requests and ensure that the correct actions are
 * taken to re-instate the settings on resume.
 */
static void __init armctrl_pm_register(void __iomem * base, unsigned int irq,
				       u32 resume_sources)
{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,39)
	struct armctrl_device *v;

	if (armctrl_id >= ARRAY_SIZE(armctrl_devices))
		printk(KERN_ERR
		       "%s: too few VICs, increase CONFIG_ARM_VIC_NR\n",
		       __func__);
	else {
		v = &armctrl_devices[armctrl_id];
		v->base = base;
		v->resume_sources = resume_sources;
		v->irq = irq;
		armctrl_id++;
	}
#else
	armctrl.base = base;
	armctrl.resume_sources = resume_sources;
	armctrl.irq = irq;
#endif
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,39)

/**
 * armctrl_pm_init - initicall to register VIC pm
 *
 * This is called via late_initcall() to register
 * the resources for the VICs due to the early
 * nature of the VIC's registration.
*/
static int __init armctrl_pm_init(void)
{
	struct armctrl_device *dev = armctrl_devices;
	int err;
	int id;

	if (armctrl_id == 0)
		return 0;

	err = sysdev_class_register(&armctrl_class);
	if (err) {
		printk(KERN_ERR "%s: cannot register class\n", __func__);
		return err;
	}

	for (id = 0; id < armctrl_id; id++, dev++) {
		dev->sysdev.id = id;
		dev->sysdev.cls = &armctrl_class;

		err = sysdev_register(&dev->sysdev);
		if (err) {
			printk(KERN_ERR "%s: failed to register device\n",
			       __func__);
			return err;
		}
	}

	return 0;
}

late_initcall(armctrl_pm_init);

#endif // VERSION check

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
static int armctrl_set_wake(struct irq_data *d, unsigned int on)
#else
static int armctrl_set_wake(unsigned int irq, unsigned int on)
#endif
{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,39)
	struct armctrl_device *armctrl = &armctrl_devices[0];
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
	unsigned int off = d->irq & 31;
#else
	unsigned int off = irq & 31;
#endif
	u32 bit = 1 << off;

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,39)
	if (!armctrl)
		return -EINVAL;

	if (!(bit & armctrl->resume_sources))
		return -EINVAL;

	if (on)
		armctrl->resume_irqs |= bit;
	else
		armctrl->resume_irqs &= ~bit;
#else
	if (!(bit & armctrl.resume_sources))
		return -EINVAL;

	if (on)
		armctrl.resume_irqs |= bit;
	else
		armctrl.resume_irqs &= ~bit;
#endif

	return 0;
}

#else
static inline void armctrl_pm_register(void __iomem *base, unsigned int irq,
				       u32 arg1)
{
}
#define armctrl_suspend NULL
#define armctrl_resume NULL
#define armctrl_set_wake NULL
#endif /* CONFIG_PM */

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,39)

static struct syscore_ops armctrl_syscore_ops = {
	.suspend = armctrl_suspend,
        .resume = armctrl_resume,
};

/**
 * armctrl_syscore_init - initicall to register VIC pm functions
 *
 * This is called via late_initcall() to register
 * the resources for the VICs due to the early
 * nature of the VIC's registration.
*/
static int __init armctrl_syscore_init(void)
{
	register_syscore_ops(&armctrl_syscore_ops);
	return 0;
}

late_initcall(armctrl_syscore_init);

#endif

static struct irq_chip armctrl_chip = {
	.name = "ARMCTRL",
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
	.irq_ack = armctrl_mask_irq,
	.irq_mask = armctrl_mask_irq,
	.irq_unmask = armctrl_unmask_irq,
	.irq_set_wake = armctrl_set_wake,
#else
	.ack = armctrl_mask_irq,
	.mask = armctrl_mask_irq,
	.unmask = armctrl_unmask_irq,
	.set_wake = armctrl_set_wake,
#endif
};

/**
 * armctrl_init - initialise a vectored interrupt controller
 * @base: iomem base address
 * @irq_start: starting interrupt number, must be muliple of 32
 * @armctrl_sources: bitmask of interrupt sources to allow
 * @resume_sources: bitmask of interrupt sources to allow for resume
 */
int __init armctrl_init(void __iomem * base, unsigned int irq_start,
			u32 armctrl_sources, u32 resume_sources)
{
	unsigned int irq;

	for (irq = 0; irq < NR_IRQS; irq++) {
		unsigned int data = irq;
		if (irq >= INTERRUPT_JPEG)
			data = remap_irqs[irq - INTERRUPT_JPEG];

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
		irq_set_chip(irq, &armctrl_chip);
		irq_set_chip_data(irq, (void *)data);
		irq_set_handler(irq, handle_level_irq);
#else
		set_irq_chip(irq, &armctrl_chip);
		set_irq_chip_data(irq, (void *)data);
		set_irq_handler(irq, handle_level_irq);
#endif
		set_irq_flags(irq, IRQF_VALID | IRQF_PROBE | IRQF_DISABLED);
	}

	armctrl_pm_register(base, irq_start, resume_sources);
	return 0;
}
