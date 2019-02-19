// SPDX-License-Identifier: GPL-2.0
/* Copyright © 2015-2017 Broadcom */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/sizes.h>
#include <soc/brcmstb/memory_api.h>

/* Macro to help extract property data */
#define DT_PROP_DATA_TO_U32(b, offs) (fdt32_to_cpu(*(u32 *)(b + offs)))

/* Constants used when retrieving memc info */
#define NUM_BUS_RANGES 10
#define BUS_RANGE_ULIMIT_SHIFT 4
#define BUS_RANGE_LLIMIT_SHIFT 4
#define BUS_RANGE_PA_SHIFT 12

enum {
	BUSNUM_MCP0 = 0x4,
	BUSNUM_MCP1 = 0x5,
	BUSNUM_MCP2 = 0x6,
};

/*
 * If the DT nodes are handy, determine which MEMC holds the specified
 * physical address.
 */
#ifdef CONFIG_ARCH_BRCMSTB
int __brcmstb_memory_phys_addr_to_memc(phys_addr_t pa, void __iomem *base)
{
	int memc = -1;
	int i;

	for (i = 0; i < NUM_BUS_RANGES; i++, base += 8) {
		const u64 ulimit_raw = readl(base);
		const u64 llimit_raw = readl(base + 4);
		const u64 ulimit =
			((ulimit_raw >> BUS_RANGE_ULIMIT_SHIFT)
			 << BUS_RANGE_PA_SHIFT) | 0xfff;
		const u64 llimit = (llimit_raw >> BUS_RANGE_LLIMIT_SHIFT)
				   << BUS_RANGE_PA_SHIFT;
		const u32 busnum = (u32)(ulimit_raw & 0xf);

		if (pa >= llimit && pa <= ulimit) {
			if (busnum >= BUSNUM_MCP0 && busnum <= BUSNUM_MCP2) {
				memc = busnum - BUSNUM_MCP0;
				break;
			}
		}
	}

	return memc;
}

int brcmstb_memory_phys_addr_to_memc(phys_addr_t pa)
{
	int memc = -1;
	struct device_node *np;
	void __iomem *cpubiuctrl;

	np = of_find_compatible_node(NULL, NULL, "brcm,brcmstb-cpu-biu-ctrl");
	if (!np)
		return memc;

	cpubiuctrl = of_iomap(np, 0);
	if (!cpubiuctrl)
		goto cleanup;

	memc = __brcmstb_memory_phys_addr_to_memc(pa, cpubiuctrl);
	iounmap(cpubiuctrl);

cleanup:
	of_node_put(np);

	return memc;
}

#elif defined(CONFIG_MIPS)
int brcmstb_memory_phys_addr_to_memc(phys_addr_t pa)
{
	/* The logic here is fairly simple and hardcoded: if pa <= 0x5000_0000,
	 * then this is MEMC0, else MEMC1.
	 *
	 * For systems with 2GB on MEMC0, MEMC1 starts at 9000_0000, with 1GB
	 * on MEMC0, MEMC1 starts at 6000_0000.
	 */
	if (pa >= 0x50000000ULL)
		return 1;
	else
		return 0;
}
#endif

u64 brcmstb_memory_memc_size(int memc)
{
	const void *fdt = initial_boot_params;
	const int mem_offset = fdt_path_offset(fdt, "/memory");
	int addr_cells = 1, size_cells = 1;
	const struct fdt_property *prop;
	int proplen, cellslen;
	u64 memc_size = 0;
	int i;

	/* Get root size and address cells if specified */
	prop = fdt_get_property(fdt, 0, "#size-cells", &proplen);
	if (prop)
		size_cells = DT_PROP_DATA_TO_U32(prop->data, 0);

	prop = fdt_get_property(fdt, 0, "#address-cells", &proplen);
	if (prop)
		addr_cells = DT_PROP_DATA_TO_U32(prop->data, 0);

	if (mem_offset < 0)
		return -1;

	prop = fdt_get_property(fdt, mem_offset, "reg", &proplen);
	cellslen = (int)sizeof(u32) * (addr_cells + size_cells);
	if ((proplen % cellslen) != 0)
		return -1;

	for (i = 0; i < proplen / cellslen; ++i) {
		u64 addr = 0;
		u64 size = 0;
		int memc_idx;
		int j;

		for (j = 0; j < addr_cells; ++j) {
			int offset = (cellslen * i) + (sizeof(u32) * j);

			addr |= (u64)DT_PROP_DATA_TO_U32(prop->data, offset) <<
				((addr_cells - j - 1) * 32);
		}
		for (j = 0; j < size_cells; ++j) {
			int offset = (cellslen * i) +
				(sizeof(u32) * (j + addr_cells));

			size |= (u64)DT_PROP_DATA_TO_U32(prop->data, offset) <<
				((size_cells - j - 1) * 32);
		}

		if ((phys_addr_t)addr != addr) {
			pr_err("phys_addr_t is smaller than provided address 0x%llx!\n",
			       addr);
			return -1;
		}

		memc_idx = brcmstb_memory_phys_addr_to_memc((phys_addr_t)addr);
		if (memc_idx == memc)
			memc_size += size;
	}

	return memc_size;
}
EXPORT_SYMBOL_GPL(brcmstb_memory_memc_size);

