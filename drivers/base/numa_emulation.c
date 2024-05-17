// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple NUMA emulation.
 *
 * Copyright © 2024 Raspberry Pi Ltd
 *
 * Author: Maíra Canal <mcanal@igalia.com>
 * Author: Tvrtko Ursulin <tursulin@igalia.com>
 */
#include <linux/memblock.h>

#include "numa_emulation.h"

static unsigned int emu_nodes;

int __init numa_emu_cmdline(char *str)
{
	int ret;

	ret = kstrtouint(str, 10, &emu_nodes);
	if (ret)
		return ret;

	if (emu_nodes > MAX_NUMNODES) {
		pr_notice("numa=fake=%u too large, reducing to %u\n",
			  emu_nodes, MAX_NUMNODES);
		emu_nodes = MAX_NUMNODES;
	}

	return 0;
}

int __init numa_emu_init(void)
{
	phys_addr_t start, end;
	unsigned long size;
	unsigned int i;
	int ret;

	if (!emu_nodes)
		return -EINVAL;

	start = memblock_start_of_DRAM();
	end = memblock_end_of_DRAM() - 1;

	size = DIV_ROUND_DOWN_ULL(end - start + 1, emu_nodes);
	size = PAGE_ALIGN_DOWN(size);

	for (i = 0; i < emu_nodes; i++) {
		u64 s, e;

		s = start + i * size;
		e = s + size - 1;

		if (i == (emu_nodes - 1) && e != end)
			e = end;

		pr_info("Faking a node at [mem %pap-%pap]\n", &s, &e);
		ret = numa_add_memblk(i, s, e + 1);
		if (ret) {
			pr_err("Failed to add fake NUMA node %d!\n", i);
			break;
		}
	}

	return ret;
}
