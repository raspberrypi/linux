/*
 * Copyright 2017 Tadeusz Kijkowski
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

#ifndef _BCM2836_REPARKCPU_H
#define _BCM2836_REPARKCPU_H

/* Not parked - initally online */
#define CPU_REPARK_STATUS_NOT_PARKED	0
/* Parked - MMU disabled */
#define CPU_REPARK_STATUS_NOMMU		1
/* Parked - MMU enabled */
#define CPU_REPARK_STATUS_MMU		2
/* Not parked - online */
#define CPU_REPARK_STATUS_ONLINE	3

#define BCM2836_REPARK_PHYS_BASE_OFFSET		0
#define BCM2836_REPARK_VIRT_BASE_OFFSET 	4
#define BCM2836_REPARK_CPU_STATUS_OFFSET	8

#define BCM2836_MAX_CPUS	4

#ifndef __ASSEMBLY__
asmlinkage void bcm2836_repark_loop(void);

struct bcm2836_arm_cpu_repark_data {
	unsigned int mailbox_rdclr_phys_base;
	void* mailbox_rdclr_virt_base;
	volatile int cpu_status[BCM2836_MAX_CPUS];
};
#endif /* __ASSEMBLY__ */

#endif /* _BCM2836_REPARKCPU_H */

