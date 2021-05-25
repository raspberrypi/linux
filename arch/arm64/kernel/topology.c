/*
 * arch/arm64/kernel/topology.c
 *
 * Copyright (C) 2011,2013,2014 Linaro Limited.
 *
 * Based on the arm32 version written by Vincent Guittot in turn based on
 * arch/sh/kernel/topology.c
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/acpi.h>
#include <linux/arch_topology.h>
#include <linux/cacheinfo.h>
#include <linux/init.h>
#include <linux/percpu.h>

#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/topology.h>

void store_cpu_topology(unsigned int cpuid)
{
	struct cpu_topology *cpuid_topo = &cpu_topology[cpuid];
	u64 mpidr;

	if (cpuid_topo->package_id != -1)
		goto topology_populated;

	mpidr = read_cpuid_mpidr();

	/* Uniprocessor systems can rely on default topology values */
	if (mpidr & MPIDR_UP_BITMASK)
		return;

	/*
	 * This would be the place to create cpu topology based on MPIDR.
	 *
	 * However, it cannot be trusted to depict the actual topology; some
	 * pieces of the architecture enforce an artificial cap on Aff0 values
	 * (e.g. GICv3's ICC_SGI1R_EL1 limits it to 15), leading to an
	 * artificial cycling of Aff1, Aff2 and Aff3 values. IOW, these end up
	 * having absolutely no relationship to the actual underlying system
	 * topology, and cannot be reasonably used as core / package ID.
	 *
	 * If the MT bit is set, Aff0 *could* be used to define a thread ID, but
	 * we still wouldn't be able to obtain a sane core ID. This means we
	 * need to entirely ignore MPIDR for any topology deduction.
	 */
	cpuid_topo->thread_id  = -1;
	cpuid_topo->core_id    = cpuid;
	cpuid_topo->package_id = cpu_to_node(cpuid);

	pr_debug("CPU%u: cluster %d core %d thread %d mpidr %#016llx\n",
		 cpuid, cpuid_topo->package_id, cpuid_topo->core_id,
		 cpuid_topo->thread_id, mpidr);

topology_populated:
	update_siblings_masks(cpuid);
}

#ifdef CONFIG_ACPI
static bool __init acpi_cpu_is_threaded(int cpu)
{
	int is_threaded = acpi_pptt_cpu_is_thread(cpu);

	/*
	 * if the PPTT doesn't have thread information, assume a homogeneous
	 * machine and return the current CPU's thread state.
	 */
	if (is_threaded < 0)
		is_threaded = read_cpuid_mpidr() & MPIDR_MT_BITMASK;

	return !!is_threaded;
}

/*
 * Propagate the topology information of the processor_topology_node tree to the
 * cpu_topology array.
 */
int __init parse_acpi_topology(void)
{
	int cpu, topology_id;

	if (acpi_disabled)
		return 0;

	for_each_possible_cpu(cpu) {
		int i, cache_id;

		topology_id = find_acpi_cpu_topology(cpu, 0);
		if (topology_id < 0)
			return topology_id;

		if (acpi_cpu_is_threaded(cpu)) {
			cpu_topology[cpu].thread_id = topology_id;
			topology_id = find_acpi_cpu_topology(cpu, 1);
			cpu_topology[cpu].core_id   = topology_id;
		} else {
			cpu_topology[cpu].thread_id  = -1;
			cpu_topology[cpu].core_id    = topology_id;
		}
		topology_id = find_acpi_cpu_topology_package(cpu);
		cpu_topology[cpu].package_id = topology_id;

		i = acpi_find_last_cache_level(cpu);

		if (i > 0) {
			/*
			 * this is the only part of cpu_topology that has
			 * a direct relationship with the cache topology
			 */
			cache_id = find_acpi_cpu_cache_topology(cpu, i);
			if (cache_id > 0)
				cpu_topology[cpu].llc_id = cache_id;
		}
	}

	return 0;
}
#endif


