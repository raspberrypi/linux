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

#include <linux/init.h>
#include <linux/irqchip.h>
#include <linux/of_address.h>
#include <linux/clk/bcm2835.h>
#include <linux/broadcom/vc_cma.h>
#include <asm/system_info.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include <linux/dma-mapping.h>

static void __init bcm2835_init(void)
{
	struct device_node *np = of_find_node_by_path("/system");
	u32 val;
	u64 val64;

	vc_cma_early_init();
	bcm2835_init_clocks();

	if (!of_property_read_u32(np, "linux,revision", &val))
		system_rev = val;
	if (!of_property_read_u64(np, "linux,serial", &val64))
		system_serial_low = val64;
}

static void __init bcm2835_init_early(void)
{
	/* dwc_otg needs this for bounce buffers on non-aligned transfers */
	init_dma_coherent_pool_size(SZ_1M);
}

static void __init bcm2835_board_reserve(void)
{
	vc_cma_reserve();
}

static const char * const bcm2835_compat[] = {
#ifdef CONFIG_ARCH_MULTI_V6
	"brcm,bcm2835",
#endif
#ifdef CONFIG_ARCH_MULTI_V7
	"brcm,bcm2836",
#endif
	NULL
};

DT_MACHINE_START(BCM2835, "BCM2835")
	.init_machine = bcm2835_init,
	.reserve = bcm2835_board_reserve,
	.init_early = bcm2835_init_early,
	.dt_compat = bcm2835_compat
MACHINE_END

#ifdef CONFIG_ARCH_BCM2708
static const char * const bcm2708_compat[] = {
	"brcm,bcm2708",
	NULL
};

DT_MACHINE_START(BCM2708, "BCM2708")
	.init_machine = bcm2835_init,
	.reserve = bcm2835_board_reserve,
	.init_early = bcm2835_init_early,
	.dt_compat = bcm2708_compat,
MACHINE_END
#endif

#ifdef CONFIG_ARCH_BCM2709
static const char * const bcm2709_compat[] = {
	"brcm,bcm2709",
	NULL
};

DT_MACHINE_START(BCM2709, "BCM2709")
	.init_machine = bcm2835_init,
	.reserve = bcm2835_board_reserve,
	.init_early = bcm2835_init_early,
	.dt_compat = bcm2709_compat,
MACHINE_END
#endif
