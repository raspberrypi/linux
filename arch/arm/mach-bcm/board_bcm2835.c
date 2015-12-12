/*
 * Copyright (C) 2010 Broadcom
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

#include <linux/broadcom/vc_cma.h>
#include <linux/init.h>
#include <linux/irqchip.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/clk/bcm2835.h>
#include <linux/regmap.h>
#include <asm/system_info.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#define ARM_LOCAL_MAILBOX3_SET0 0x8c
#define ARM_LOCAL_MAILBOX3_CLR0 0xcc

#ifdef CONFIG_SMP
static int __init bcm2836_smp_boot_secondary(unsigned int cpu,
					     struct task_struct *idle)
{
	struct regmap *regmap;
	int timeout = 20;

	regmap = syscon_regmap_lookup_by_compatible("brcm,bcm2836-arm-local");
	if (IS_ERR(regmap)) {
		pr_err("Failed to get local register map for SMP\n");
		return -ENOSYS;
	}

	dsb();
	regmap_write(regmap, ARM_LOCAL_MAILBOX3_SET0 + 16 * cpu,
		     virt_to_phys(secondary_startup));

	while (true) {
		int val;
		int ret = regmap_read(regmap,
				      ARM_LOCAL_MAILBOX3_CLR0 + 16 * cpu, &val);
		if (ret)
			return ret;
		if (val == 0)
			return 0;
		if (timeout-- == 0)
			return -ETIMEDOUT;
		cpu_relax();
	}

	return 0;
}

struct smp_operations bcm2836_smp_ops __initdata = {
	.smp_boot_secondary	= bcm2836_smp_boot_secondary,
};
#endif

static void __init bcm2835_reserve(void)
{
	vc_cma_reserve();
}

/* Use this hack until a proper solution is agreed upon */
static void __init bcm2835_init_uart1(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "brcm,bcm2835-aux-uart");
	if (of_device_is_available(np)) {
		np = of_find_compatible_node(NULL, NULL,
					     "bcrm,bcm2835-aux-enable");
		if (np) {
			void __iomem *base = of_iomap(np, 0);

			if (!base) {
				pr_err("bcm2835: Failed enabling Mini UART\n");
				return;
			}

			writel(1, base);
			pr_info("bcm2835: Mini UART enabled\n");
		}
	}
}

static void __init bcm2835_init(void)
{
	struct device_node *np = of_find_node_by_path("/system");
	u32 val;
	u64 val64;
	int ret;

	vc_cma_early_init();

	bcm2835_init_clocks();

	ret = of_platform_populate(NULL, of_default_bus_match_table, NULL,
				   NULL);
	if (ret) {
		pr_err("of_platform_populate failed: %d\n", ret);
		BUG();
	}

	if (!of_property_read_u32(np, "linux,revision", &val))
		system_rev = val;
	if (!of_property_read_u64(np, "linux,serial", &val64))
		system_serial_low = val64;

	bcm2835_init_uart1();
}

static const char * const bcm2835_compat[] = {
	"brcm,bcm2835",
	NULL
};

DT_MACHINE_START(BCM2835, "BCM2835")
	.reserve = bcm2835_reserve,
	.init_machine = bcm2835_init,
	.dt_compat = bcm2835_compat
MACHINE_END

static const char * const bcm2836_compat[] = {
	"brcm,bcm2836",
	NULL
};

DT_MACHINE_START(BCM2836, "BCM2836")
	.smp = smp_ops(bcm2836_smp_ops),
	.reserve = bcm2835_reserve,
	.init_machine = bcm2835_init,
	.dt_compat = bcm2836_compat
MACHINE_END
