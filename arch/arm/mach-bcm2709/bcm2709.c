/*
 *  linux/arch/arm/mach-bcm2709/bcm2709.c
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
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_platform.h>

#include <asm/system_info.h>
#include <asm/mach-types.h>
#include <asm/cputype.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include <mach/system.h>

#include <linux/broadcom/vc_cma.h>

/* Effectively we have an IOMMU (ARM<->VideoCore map) that is set up to
 * give us IO access only to 64Mbytes of physical memory (26 bits).  We could
 * represent this window by setting our dmamasks to 26 bits but, in fact
 * we're not going to use addresses outside this range (they're not in real
 * memory) so we don't bother.
 *
 * In the future we might include code to use this IOMMU to remap other
 * physical addresses onto VideoCore memory then the use of 32-bits would be
 * more legitimate.
 */

/* command line parameters */
static unsigned boardrev, serial;
static unsigned reboot_part = 0;

static struct map_desc bcm2709_io_desc[] __initdata = {
	{
	 .virtual = IO_ADDRESS(ARMCTRL_BASE),
	 .pfn = __phys_to_pfn(ARMCTRL_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(UART0_BASE),
	 .pfn = __phys_to_pfn(UART0_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(UART1_BASE),
	 .pfn = __phys_to_pfn(UART1_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(DMA_BASE),
	 .pfn = __phys_to_pfn(DMA_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(MCORE_BASE),
	 .pfn = __phys_to_pfn(MCORE_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(ST_BASE),
	 .pfn = __phys_to_pfn(ST_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(USB_BASE),
	 .pfn = __phys_to_pfn(USB_BASE),
	 .length = SZ_128K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(PM_BASE),
	 .pfn = __phys_to_pfn(PM_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(GPIO_BASE),
	 .pfn = __phys_to_pfn(GPIO_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(ARM_LOCAL_BASE),
	 .pfn = __phys_to_pfn(ARM_LOCAL_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
};

void __init bcm2709_map_io(void)
{
	iotable_init(bcm2709_io_desc, ARRAY_SIZE(bcm2709_io_desc));
}

int calc_rsts(int partition)
{
	return PM_PASSWORD |
		((partition & (1 << 0))  << 0) |
		((partition & (1 << 1))  << 1) |
		((partition & (1 << 2))  << 2) |
		((partition & (1 << 3))  << 3) |
		((partition & (1 << 4))  << 4) |
		((partition & (1 << 5))  << 5);
}

static void bcm2709_restart(enum reboot_mode mode, const char *cmd)
{
	extern char bcm2708_reboot_mode;
	uint32_t pm_rstc, pm_wdog;
	uint32_t timeout = 10;
	uint32_t pm_rsts = 0;

	if(bcm2708_reboot_mode == 'q')
	{
		// NOOBS < 1.3 booting with reboot=q
		pm_rsts = readl(__io_address(PM_RSTS));
		pm_rsts = PM_PASSWORD | pm_rsts | PM_RSTS_HADWRQ_SET;
	}
	else if(bcm2708_reboot_mode == 'p')
	{
		// NOOBS < 1.3 halting
		pm_rsts = readl(__io_address(PM_RSTS));
		pm_rsts = PM_PASSWORD | pm_rsts | PM_RSTS_HADWRH_SET;
	}
	else
	{
		pm_rsts = calc_rsts(reboot_part);
	}

	writel(pm_rsts, __io_address(PM_RSTS));

	/* Setup watchdog for reset */
	pm_rstc = readl(__io_address(PM_RSTC));

	pm_wdog = PM_PASSWORD | (timeout & PM_WDOG_TIME_SET); // watchdog timer = timer clock / 16; need password (31:16) + value (11:0)
	pm_rstc = PM_PASSWORD | (pm_rstc & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET;

	writel(pm_wdog, __io_address(PM_WDOG));
	writel(pm_rstc, __io_address(PM_RSTC));
}

/* We can't really power off, but if we do the normal reset scheme, and indicate to bootcode.bin not to reboot, then most of the chip will be powered off */
static void bcm2709_power_off(void)
{
	extern char bcm2708_reboot_mode;
	if(bcm2708_reboot_mode == 'q')
	{
		// NOOBS < v1.3
		bcm2709_restart('p', "");
	}
	else
	{
		/* partition 63 is special code for HALT the bootloader knows not to boot*/
		reboot_part = 63;
		/* continue with normal reset mechanism */
		bcm2709_restart(0, "");
	}
}

static void __init bcm2709_init_uart1(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "brcm,bcm2835-aux-uart");
	if (of_device_is_available(np)) {
		pr_info("bcm2709: Mini UART enabled\n");
		writel(1, __io_address(UART1_BASE + 0x4));
	}
}

void __init bcm2709_init(void)
{
	int ret;

	vc_cma_early_init();

	pm_power_off = bcm2709_power_off;

	ret = of_platform_populate(NULL, of_default_bus_match_table, NULL,
				   NULL);
	if (ret) {
		pr_err("of_platform_populate failed: %d\n", ret);
		BUG();
	}

	bcm2709_init_uart1();

	system_rev = boardrev;
	system_serial_low = serial;
}

static void __init bcm2709_timer_init(void)
{
	// timer control
	writel(0, __io_address(ARM_LOCAL_CONTROL));
	// timer pre_scaler
	writel(0x80000000, __io_address(ARM_LOCAL_PRESCALER)); // 19.2MHz
	//writel(0x06AAAAAB, __io_address(ARM_LOCAL_PRESCALER)); // 1MHz

	of_clk_init(NULL);
	clocksource_probe();
}


void __init bcm2709_init_early(void)
{
	/*
	 * Some devices allocate their coherent buffers from atomic
	 * context. Increase size of atomic coherent pool to make sure such
	 * the allocations won't fail.
	 */
	init_dma_coherent_pool_size(SZ_4M);
}

static void __init board_reserve(void)
{
	vc_cma_reserve();
}


#ifdef CONFIG_SMP
#include <linux/smp.h>

#include <asm/cacheflush.h>
#include <asm/smp_plat.h>
int dc4=0;
//void dc4_log(unsigned x) { if (dc4) writel((x), __io_address(ST_BASE+10 + raw_smp_processor_id()*4)); }
void dc4_log_dead(unsigned x) { if (dc4) writel((readl(__io_address(ST_BASE+0x10 + raw_smp_processor_id()*4)) & 0xffff) | ((x)<<16), __io_address(ST_BASE+0x10 + raw_smp_processor_id()*4)); }

static void bcm2835_send_doorbell(const struct cpumask *mask, unsigned int irq)
{
        int cpu;
        /*
         * Ensure that stores to Normal memory are visible to the
         * other CPUs before issuing the IPI.
         */
        dsb();

        /* Convert our logical CPU mask into a physical one. */
        for_each_cpu(cpu, mask)
	{
		/* submit softirq */
		writel(1<<irq, __io_address(ARM_LOCAL_MAILBOX0_SET0 + 0x10 * MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0)));
	}
}

void __init bcm2709_smp_init_cpus(void)
{
	void secondary_startup(void);
	unsigned int i, ncores;

	ncores = 4; // xxx scu_get_core_count(NULL);
	printk("[%s] enter (%x->%x)\n", __FUNCTION__, (unsigned)virt_to_phys((void *)secondary_startup), (unsigned)__io_address(ST_BASE + 0x10));
	printk("[%s] ncores=%d\n", __FUNCTION__, ncores);

	for (i = 0; i < ncores; i++) {
		set_cpu_possible(i, true);
		/* enable IRQ (not FIQ) */
		writel(0x1, __io_address(ARM_LOCAL_MAILBOX_INT_CONTROL0 + 0x4 * i));
		//writel(0xf, __io_address(ARM_LOCAL_TIMER_INT_CONTROL0   + 0x4 * i));
	}
	set_smp_cross_call(bcm2835_send_doorbell);
}

/*
 * for arch/arm/kernel/smp.c:smp_prepare_cpus(unsigned int max_cpus)
 */
void __init bcm2709_smp_prepare_cpus(unsigned int max_cpus)
{
    //void __iomem *scu_base;

    printk("[%s] enter\n", __FUNCTION__);
    //scu_base = scu_base_addr();
    //scu_enable(scu_base);
}

/*
 * for linux/arch/arm/kernel/smp.c:secondary_start_kernel(void)
 */
void __init bcm2709_secondary_init(unsigned int cpu)
{
    printk("[%s] enter cpu:%d\n", __FUNCTION__, cpu);
    //gic_secondary_init(0);
}

/*
 * for linux/arch/arm/kernel/smp.c:__cpu_up(..)
 */
int __init bcm2709_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
    void secondary_startup(void);
    void *mbox_set = __io_address(ARM_LOCAL_MAILBOX3_SET0 + 0x10 * MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0));
    void *mbox_clr = __io_address(ARM_LOCAL_MAILBOX3_CLR0 + 0x10 * MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0));
    unsigned secondary_boot = (unsigned)virt_to_phys((void *)secondary_startup);
    int timeout=20;
    unsigned t = -1;
    //printk("[%s] enter cpu:%d (%x->%p) %x\n", __FUNCTION__, cpu, secondary_boot, wake, readl(wake));

    dsb();
    BUG_ON(readl(mbox_clr) != 0);
    writel(secondary_boot, mbox_set);

    while (--timeout > 0) {
	t = readl(mbox_clr);
	if (t == 0) break;
	cpu_relax();
    }
    if (timeout==0)
        printk("[%s] cpu:%d failed to start (%x)\n", __FUNCTION__, cpu, t);
    else
        printk("[%s] cpu:%d started (%x) %d\n", __FUNCTION__, cpu, t, timeout);

    return 0;
}


struct smp_operations  bcm2709_smp_ops __initdata = {
	.smp_init_cpus		= bcm2709_smp_init_cpus,
	.smp_prepare_cpus	= bcm2709_smp_prepare_cpus,
	.smp_secondary_init	= bcm2709_secondary_init,
	.smp_boot_secondary	= bcm2709_boot_secondary,
};
#endif

static const char * const bcm2709_compat[] = {
	"brcm,bcm2709",
	"brcm,bcm2708", /* Could use bcm2708 in a pinch */
	NULL
};

MACHINE_START(BCM2709, "BCM2709")
    /* Maintainer: Broadcom Europe Ltd. */
#ifdef CONFIG_SMP
	.smp		= smp_ops(bcm2709_smp_ops),
#endif
	.map_io = bcm2709_map_io,
	.init_time = bcm2709_timer_init,
	.init_machine = bcm2709_init,
	.init_early = bcm2709_init_early,
	.reserve = board_reserve,
	.restart	= bcm2709_restart,
	.dt_compat = bcm2709_compat,
MACHINE_END

MACHINE_START(BCM2708, "BCM2709")
    /* Maintainer: Broadcom Europe Ltd. */
#ifdef CONFIG_SMP
	.smp		= smp_ops(bcm2709_smp_ops),
#endif
	.map_io = bcm2709_map_io,
	.init_time = bcm2709_timer_init,
	.init_machine = bcm2709_init,
	.init_early = bcm2709_init_early,
	.reserve = board_reserve,
	.restart	= bcm2709_restart,
	.dt_compat = bcm2709_compat,
MACHINE_END

module_param(boardrev, uint, 0644);
module_param(serial, uint, 0644);
module_param(reboot_part, uint, 0644);
