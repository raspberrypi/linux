/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NUMA emulation header
 *
 * Copyright © 2024 Raspberry Pi Ltd
 */

#ifdef CONFIG_GENERIC_ARCH_NUMA_EMULATION
int numa_emu_cmdline(char *str);
int __init numa_emu_init(void);
#else
static inline int numa_emu_cmdline(char *str)
{
	return -EINVAL;
}

static int __init numa_emu_init(void)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_NUMA_EMU */
