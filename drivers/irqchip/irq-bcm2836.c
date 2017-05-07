/*
 * Root interrupt controller for the BCM2836 (Raspberry Pi 2).
 *
 * Copyright 2015 Broadcom
 * Repark CPU modifications copyright 2017 Tadeusz Kijkowski
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
 */

#include <linux/cpu.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <asm/exception.h>

/*
 * CONFIG_BCM2836_CPU_REPARK can only be enabled when CPU_ARM is also
 * enabled, unless I messed up Kconfig file.
 */
#ifdef CONFIG_BCM2836_CPU_REPARK
#include <linux/cpumask.h>
#include <asm/delay.h>
#include <asm/cacheflush.h>

#include "bcm2836-reparkcpu.h"
#endif /* CONFIG_BCM2836_CPU_REPARK */

#define LOCAL_CONTROL			0x000
#define LOCAL_PRESCALER			0x008

/*
 * The low 2 bits identify the CPU that the GPU IRQ goes to, and the
 * next 2 bits identify the CPU that the GPU FIQ goes to.
 */
#define LOCAL_GPU_ROUTING		0x00c
/* When setting bits 0-3, enables PMU interrupts on that CPU. */
#define LOCAL_PM_ROUTING_SET		0x010
/* When setting bits 0-3, disables PMU interrupts on that CPU. */
#define LOCAL_PM_ROUTING_CLR		0x014
/*
 * The low 4 bits of this are the CPU's timer IRQ enables, and the
 * next 4 bits are the CPU's timer FIQ enables (which override the IRQ
 * bits).
 */
#define LOCAL_TIMER_INT_CONTROL0	0x040
/*
 * The low 4 bits of this are the CPU's per-mailbox IRQ enables, and
 * the next 4 bits are the CPU's per-mailbox FIQ enables (which
 * override the IRQ bits).
 */
#define LOCAL_MAILBOX_INT_CONTROL0	0x050
/*
 * The CPU's interrupt status register.  Bits are defined by the the
 * LOCAL_IRQ_* bits below.
 */
#define LOCAL_IRQ_PENDING0		0x060
/* Same status bits as above, but for FIQ. */
#define LOCAL_FIQ_PENDING0		0x070
/*
 * Mailbox write-to-set bits.  There are 16 mailboxes, 4 per CPU, and
 * these bits are organized by mailbox number and then CPU number.  We
 * use mailbox 0 for IPIs.  The mailbox's interrupt is raised while
 * any bit is set.
 */
#define LOCAL_MAILBOX0_SET0		0x080
#define LOCAL_MAILBOX3_SET0		0x08c
/* Mailbox write-to-clear bits. */
#define LOCAL_MAILBOX0_CLR0		0x0c0
#define LOCAL_MAILBOX3_CLR0		0x0cc

#define LOCAL_IRQ_CNTPSIRQ	0
#define LOCAL_IRQ_CNTPNSIRQ	1
#define LOCAL_IRQ_CNTHPIRQ	2
#define LOCAL_IRQ_CNTVIRQ	3
#define LOCAL_IRQ_MAILBOX0	4
#define LOCAL_IRQ_MAILBOX1	5
#define LOCAL_IRQ_MAILBOX2	6
#define LOCAL_IRQ_MAILBOX3	7
#define LOCAL_IRQ_GPU_FAST	8
#define LOCAL_IRQ_PMU_FAST	9
#define LAST_IRQ		LOCAL_IRQ_PMU_FAST

struct bcm2836_arm_irqchip_intc {
	struct irq_domain *domain;
	void __iomem *base;
};

static struct bcm2836_arm_irqchip_intc intc  __read_mostly;

#ifdef CONFIG_BCM2836_CPU_REPARK
struct bcm2836_arm_cpu_repark_data bcm2836_repark_data;
#endif

static void bcm2836_arm_irqchip_mask_per_cpu_irq(unsigned int reg_offset,
						 unsigned int bit,
						 int cpu)
{
	void __iomem *reg = intc.base + reg_offset + 4 * cpu;

	writel(readl(reg) & ~BIT(bit), reg);
}

static void bcm2836_arm_irqchip_unmask_per_cpu_irq(unsigned int reg_offset,
						   unsigned int bit,
						 int cpu)
{
	void __iomem *reg = intc.base + reg_offset + 4 * cpu;

	writel(readl(reg) | BIT(bit), reg);
}

static void bcm2836_arm_irqchip_mask_timer_irq(struct irq_data *d)
{
	bcm2836_arm_irqchip_mask_per_cpu_irq(LOCAL_TIMER_INT_CONTROL0,
					     d->hwirq - LOCAL_IRQ_CNTPSIRQ,
					     smp_processor_id());
}

static void bcm2836_arm_irqchip_unmask_timer_irq(struct irq_data *d)
{
	bcm2836_arm_irqchip_unmask_per_cpu_irq(LOCAL_TIMER_INT_CONTROL0,
					       d->hwirq - LOCAL_IRQ_CNTPSIRQ,
					       smp_processor_id());
}

static struct irq_chip bcm2836_arm_irqchip_timer = {
	.name		= "bcm2836-timer",
	.irq_mask	= bcm2836_arm_irqchip_mask_timer_irq,
	.irq_unmask	= bcm2836_arm_irqchip_unmask_timer_irq,
};

static void bcm2836_arm_irqchip_mask_pmu_irq(struct irq_data *d)
{
	writel(1 << smp_processor_id(), intc.base + LOCAL_PM_ROUTING_CLR);
}

static void bcm2836_arm_irqchip_unmask_pmu_irq(struct irq_data *d)
{
	writel(1 << smp_processor_id(), intc.base + LOCAL_PM_ROUTING_SET);
}

static struct irq_chip bcm2836_arm_irqchip_pmu = {
	.name		= "bcm2836-pmu",
	.irq_mask	= bcm2836_arm_irqchip_mask_pmu_irq,
	.irq_unmask	= bcm2836_arm_irqchip_unmask_pmu_irq,
};

static void bcm2836_arm_irqchip_mask_gpu_irq(struct irq_data *d)
{
}

static void bcm2836_arm_irqchip_unmask_gpu_irq(struct irq_data *d)
{
}

#ifdef CONFIG_ARM64

void bcm2836_arm_irqchip_spin_gpu_irq(void)
{
	u32 i;
	void __iomem *gpurouting = (intc.base + LOCAL_GPU_ROUTING);
	u32 routing_val = readl(gpurouting);

	for (i = 1; i <= 3; i++) {
		u32 new_routing_val = (routing_val + i) & 3;

		if (cpu_active(new_routing_val)) {
			writel(new_routing_val, gpurouting);
			return;
		}
	}
}
EXPORT_SYMBOL(bcm2836_arm_irqchip_spin_gpu_irq);

#endif

static struct irq_chip bcm2836_arm_irqchip_gpu = {
	.name		= "bcm2836-gpu",
	.irq_mask	= bcm2836_arm_irqchip_mask_gpu_irq,
	.irq_unmask	= bcm2836_arm_irqchip_unmask_gpu_irq,
};

static void bcm2836_arm_irqchip_register_irq(int hwirq, struct irq_chip *chip)
{
	int irq = irq_create_mapping(intc.domain, hwirq);

	irq_set_percpu_devid(irq);
	irq_set_chip_and_handler(irq, chip, handle_percpu_devid_irq);
	irq_set_status_flags(irq, IRQ_NOAUTOEN | IRQ_TYPE_LEVEL_LOW);
}

static void
__exception_irq_entry bcm2836_arm_irqchip_handle_irq(struct pt_regs *regs)
{
	int cpu = smp_processor_id();
	u32 stat;

	stat = readl_relaxed(intc.base + LOCAL_IRQ_PENDING0 + 4 * cpu);
	if (stat & BIT(LOCAL_IRQ_MAILBOX0)) {
#ifdef CONFIG_SMP
		void __iomem *mailbox0 = (intc.base +
					  LOCAL_MAILBOX0_CLR0 + 16 * cpu);
		u32 mbox_val = readl(mailbox0);
		u32 ipi = ffs(mbox_val) - 1;

		writel(1 << ipi, mailbox0);
		dsb(sy);
		handle_IPI(ipi, regs);
#endif
	} else if (stat) {
		u32 hwirq = ffs(stat) - 1;

		handle_domain_irq(intc.domain, hwirq, regs);
	}
}

#ifdef CONFIG_SMP
static void bcm2836_arm_irqchip_send_ipi(const struct cpumask *mask,
					 unsigned int ipi)
{
	int cpu;
	void __iomem *mailbox0_base = intc.base + LOCAL_MAILBOX0_SET0;

	/*
	 * Ensure that stores to normal memory are visible to the
	 * other CPUs before issuing the IPI.
	 */
	smp_wmb();

	for_each_cpu(cpu, mask)	{
		writel(1 << ipi, mailbox0_base + 16 * cpu);
	}
}

static int bcm2836_cpu_starting(unsigned int cpu)
{
	bcm2836_arm_irqchip_unmask_per_cpu_irq(LOCAL_MAILBOX_INT_CONTROL0, 0,
					       cpu);
	return 0;
}

static int bcm2836_cpu_dying(unsigned int cpu)
{
	bcm2836_arm_irqchip_mask_per_cpu_irq(LOCAL_MAILBOX_INT_CONTROL0, 0,
					     cpu);
	return 0;
}

#ifdef CONFIG_BCM2836_CPU_REPARK
static void __attribute__((unused)) repark_verify_offsets(void)
{
	BUILD_BUG_ON(offsetof(struct bcm2836_arm_cpu_repark_data,
		mailbox_rdclr_phys_base) != BCM2836_REPARK_PHYS_BASE_OFFSET);
	BUILD_BUG_ON(offsetof(struct bcm2836_arm_cpu_repark_data,
		mailbox_rdclr_virt_base) != BCM2836_REPARK_VIRT_BASE_OFFSET);
	BUILD_BUG_ON(offsetof(struct bcm2836_arm_cpu_repark_data,
		cpu_status) != BCM2836_REPARK_CPU_STATUS_OFFSET);
}

static bool bcm2836_cpu_is_irq_target(unsigned int cpunr)
{
	unsigned int gpu_int_routing;
	gpu_int_routing = readl(intc.base + LOCAL_GPU_ROUTING);
	return (gpu_int_routing & 3) == cpunr;
}

static bool bcm2836_cpu_is_fiq_target(unsigned int cpunr)
{
	unsigned int gpu_int_routing;
	gpu_int_routing = readl(intc.base + LOCAL_GPU_ROUTING);
	return ((gpu_int_routing >> 2) & 3) == cpunr;
}

/*
 * Slightly modified bcm2836_arm_irqchip_spin_gpu_irq which keeps FIQ routing
 */
static unsigned int bcm2836_safe_spin_gpu_irq(void)
{
	u32 i;
	void __iomem *gpurouting = (intc.base + LOCAL_GPU_ROUTING);
	u32 routing_val = readl(gpurouting);
	u32 fiq_routing = routing_val & ~3;

	for (i = 1; i <= 3; i++) {
		u32 new_routing_val = (routing_val + i) & 3;

		if (cpu_active(new_routing_val)) {
			writel(new_routing_val | fiq_routing, gpurouting);
			return i;
		}
	}
	return i;
}

static bool bcm2836_cpu_can_disable(unsigned int cpunr)
{
	if (cpunr == 0)
		return false;

	/*
	 * Unfortunatelly this function is called on startup, before GPU FIQs
	 * are re-routed.
	 * We know that irq-bcm2835.c will re-route FIQs to CPU#1 for dwc_otg
	 * (USB host), so just tell from the start, that disabling CPU#1 is
	 * not allowed
	 */
	if (cpunr == 1)
		return false;

	if (bcm2836_cpu_is_irq_target(cpunr)
			|| bcm2836_cpu_is_fiq_target(cpunr))
		return false;

	return true;
}

static void bcm2836_cpu_die(unsigned int cpunr)
{
	if (!bcm2836_cpu_is_irq_target(cpunr)) {
		unsigned int next_cpunr = bcm2836_safe_spin_gpu_irq();
		pr_notice("CPU%d: re-routed GPU IRQs to CPU%d\n",
				cpunr, next_cpunr);
	}

	if (!bcm2836_cpu_is_fiq_target(cpunr)) {
		/*
		 * It's not that easy to re-route FIQs, though.
		 * (We could, but need to take care of FIQ mode registers)
		 */
		pr_err("CPU%d: disabling CPU with GPU FIQs routed\n",
				cpunr);
		/* Too late to turn back */
	}

	/* Disable all timer interrupts */
	writel(0, intc.base + LOCAL_TIMER_INT_CONTROL0 + 4 * cpunr);

	/* Disable all mailbox interrupts */
	writel(0, intc.base + LOCAL_MAILBOX_INT_CONTROL0 + 4 * cpunr);

	bcm2836_repark_loop();
}

static void bcm2836_smp_repark_cpu(unsigned int cpunr)
{
	unsigned long repark_loop_phys =
		(unsigned long)virt_to_phys((void *)bcm2836_repark_loop);

	pr_info("bcm2836: reparking offline CPU#%d\n", cpunr);

	smp_wmb();

	writel(repark_loop_phys,
	       intc.base + LOCAL_MAILBOX3_SET0 + 16 * cpunr);
}

static void bcm2836_smp_prepare_cpus(unsigned int max_cpus)
{
	int cpunr;

	pr_info("bcm2836: prepare cpus called with max_cpus = %u\n", max_cpus);

	for_each_present_cpu(cpunr) {
		if (cpunr >= max_cpus)
			bcm2836_smp_repark_cpu(cpunr);
	}
}

static void bcm2836_smp_init_repark(struct device_node *node)
{
	struct resource res;

	/* This should never fail since of_iomap succeeded earlier */
	if (of_address_to_resource(node, 0, &res))
		panic("%s: unable to get local interrupt registers address\n",
			node->full_name);

	bcm2836_repark_data.mailbox_rdclr_phys_base =
		res.start + LOCAL_MAILBOX3_CLR0;
	bcm2836_repark_data.mailbox_rdclr_virt_base =
		intc.base + LOCAL_MAILBOX3_CLR0;
	sync_cache_w(&bcm2836_repark_data);
}

#endif

#ifdef CONFIG_ARM
static int bcm2836_smp_boot_secondary(unsigned int cpu,
					     struct task_struct *idle)
{
	unsigned long secondary_startup_phys =
		(unsigned long)virt_to_phys((void *)secondary_startup);

#ifdef CONFIG_BCM2836_CPU_REPARK
	int cpu_status = bcm2836_repark_data.cpu_status[cpu];
	smp_rmb();
	if (cpu_status == CPU_REPARK_STATUS_NOT_PARKED
			|| cpu_status == CPU_REPARK_STATUS_NOMMU) {
		writel(secondary_startup_phys,
		       intc.base + LOCAL_MAILBOX3_SET0 + 16 * cpu);
	} else if (cpu_status == CPU_REPARK_STATUS_MMU) {
		writel((unsigned int) secondary_startup,
		       intc.base + LOCAL_MAILBOX3_SET0 + 16 * cpu);
	} else {
		pr_err("bcm2836: CPU%d already online\n", cpu);
		return -EBUSY;
	}
#else
	writel(secondary_startup_phys,
	       intc.base + LOCAL_MAILBOX3_SET0 + 16 * cpu);
#endif

	return 0;
}

static const struct smp_operations bcm2836_smp_ops __initconst = {
#ifdef CONFIG_BCM2836_CPU_REPARK
        .smp_prepare_cpus	= bcm2836_smp_prepare_cpus,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die		= bcm2836_cpu_die,
	.cpu_can_disable	= bcm2836_cpu_can_disable,
#endif
#endif
	.smp_boot_secondary	= bcm2836_smp_boot_secondary,
};
#endif
#endif

static const struct irq_domain_ops bcm2836_arm_irqchip_intc_ops = {
	.xlate = irq_domain_xlate_onecell
};

static void
bcm2836_arm_irqchip_smp_init(void)
{
#ifdef CONFIG_SMP
	/* Unmask IPIs to the boot CPU. */
	cpuhp_setup_state(CPUHP_AP_IRQ_BCM2836_STARTING,
			  "AP_IRQ_BCM2836_STARTING", bcm2836_cpu_starting,
			  bcm2836_cpu_dying);

	set_smp_cross_call(bcm2836_arm_irqchip_send_ipi);

#ifdef CONFIG_ARM
	smp_set_ops(&bcm2836_smp_ops);
#endif
#endif
}

/*
 * The LOCAL_IRQ_CNT* timer firings are based off of the external
 * oscillator with some scaling.  The firmware sets up CNTFRQ to
 * report 19.2Mhz, but doesn't set up the scaling registers.
 */
static void bcm2835_init_local_timer_frequency(void)
{
	/*
	 * Set the timer to source from the 19.2Mhz crystal clock (bit
	 * 8 unset), and only increment by 1 instead of 2 (bit 9
	 * unset).
	 */
	writel(0, intc.base + LOCAL_CONTROL);

	/*
	 * Set the timer prescaler to 1:1 (timer freq = input freq *
	 * 2**31 / prescaler)
	 */
	writel(0x80000000, intc.base + LOCAL_PRESCALER);
}

static int __init bcm2836_arm_irqchip_l1_intc_of_init(struct device_node *node,
						      struct device_node *parent)
{
	intc.base = of_iomap(node, 0);
	if (!intc.base) {
		panic("%s: unable to map local interrupt registers\n",
			node->full_name);
	}

	bcm2835_init_local_timer_frequency();

	intc.domain = irq_domain_add_linear(node, LAST_IRQ + 1,
					    &bcm2836_arm_irqchip_intc_ops,
					    NULL);
	if (!intc.domain)
		panic("%s: unable to create IRQ domain\n", node->full_name);

#ifdef CONFIG_BCM2836_CPU_REPARK
	bcm2836_smp_init_repark(node);
#endif

	bcm2836_arm_irqchip_register_irq(LOCAL_IRQ_CNTPSIRQ,
					 &bcm2836_arm_irqchip_timer);
	bcm2836_arm_irqchip_register_irq(LOCAL_IRQ_CNTPNSIRQ,
					 &bcm2836_arm_irqchip_timer);
	bcm2836_arm_irqchip_register_irq(LOCAL_IRQ_CNTHPIRQ,
					 &bcm2836_arm_irqchip_timer);
	bcm2836_arm_irqchip_register_irq(LOCAL_IRQ_CNTVIRQ,
					 &bcm2836_arm_irqchip_timer);
	bcm2836_arm_irqchip_register_irq(LOCAL_IRQ_GPU_FAST,
					 &bcm2836_arm_irqchip_gpu);
	bcm2836_arm_irqchip_register_irq(LOCAL_IRQ_PMU_FAST,
					 &bcm2836_arm_irqchip_pmu);

	bcm2836_arm_irqchip_smp_init();

	set_handle_irq(bcm2836_arm_irqchip_handle_irq);
	return 0;
}

IRQCHIP_DECLARE(bcm2836_arm_irqchip_l1_intc, "brcm,bcm2836-l1-intc",
		bcm2836_arm_irqchip_l1_intc_of_init);
