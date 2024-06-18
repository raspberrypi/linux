// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 */

#include <linux/arch_topology.h>
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

#define REG_CUR_FREQ_KHZ_OFFSET 0x0
#define REG_SET_FREQ_KHZ_OFFSET 0x4
#define PER_CPU_OFFSET 0x8

static void __iomem *base;

static void android_v_virt_scale_freq_tick(void)
{
	int cpu = smp_processor_id();
	u32 max_freq = (u32)cpufreq_get_hw_max_freq(cpu);
	u64 cur_freq;
	long scale;

	cur_freq = (u64)readl_relaxed(base + cpu * PER_CPU_OFFSET
			+ REG_CUR_FREQ_KHZ_OFFSET);

	cur_freq <<= SCHED_CAPACITY_SHIFT;
	scale = (long)div_u64(cur_freq, max_freq);

	/*
	 * This is safe because clock_pelt is always scaled with freq and arch,
	 * and will ensure the scaling of lapsed time is < 1. Since there are
	 * use cases where the host frequency may exceed the frequency allowed
	 * in the guest, this allows us to maintain util invariance.
	 */
	scale = umin(scale,
		    SCHED_CAPACITY_SCALE * SCHED_CAPACITY_SCALE / arch_scale_cpu_capacity(cpu));

	this_cpu_write(arch_freq_scale, scale);
}

static struct scale_freq_data virt_sfd = {
	.source = SCALE_FREQ_SOURCE_VIRT,
	.set_freq_scale = android_v_virt_scale_freq_tick,
};

static unsigned int android_v_vcpufreq_set_perf(struct cpufreq_policy *policy,
					  unsigned int target_freq)
{
	writel_relaxed(target_freq,
		       base + policy->cpu * PER_CPU_OFFSET + REG_SET_FREQ_KHZ_OFFSET);
	return 0;
}

static unsigned int android_v_vcpufreq_fast_switch(struct cpufreq_policy *policy,
					     unsigned int target_freq)
{
	android_v_vcpufreq_set_perf(policy, target_freq);
	return target_freq;
}

static int android_v_vcpufreq_target_index(struct cpufreq_policy *policy,
				     unsigned int index)
{
	return android_v_vcpufreq_set_perf(policy,
				     policy->freq_table[index].frequency);
}

static int android_v_vcpufreq_cpu_init(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *table;
	struct device *cpu_dev;
	int ret;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev)
		return -ENODEV;

	ret = dev_pm_opp_of_add_table(cpu_dev);
	if (ret)
		return ret;

	ret = dev_pm_opp_get_opp_count(cpu_dev);
	if (ret <= 0) {
		dev_err(cpu_dev, "OPP table can't be empty\n");
		return -ENODEV;
	}

	ret = dev_pm_opp_init_cpufreq_table(cpu_dev, &table);
	if (ret) {
		dev_err(cpu_dev, "failed to init cpufreq table: %d\n", ret);
		return ret;
	}

	policy->freq_table = table;

	/*
	 * To simplify and improve latency of handling frequency requests on
	 * the host side, this ensures that the vCPU thread triggering the MMIO
	 * abort is the same thread whose performance constraints (Ex. uclamp
	 * settings) need to be updated. This simplifies the VMM (Virtual
	 * Machine Manager) having to find the correct vCPU thread and/or
	 * facing permission issues when configuring other threads.
	 */
	policy->dvfs_possible_from_any_cpu = false;
	policy->fast_switch_possible = true;

	/*
	 * Using the default SCALE_FREQ_SOURCE_CPUFREQ is insufficient since
	 * the actual physical CPU frequency may not match requested frequency
	 * from the vCPU thread due to frequency update latencies or other
	 * inputs to the physical CPU frequency selection. This additional FIE
	 * source allows for more accurate freq_scale updates and only takes
	 * effect if another FIE source such as AMUs have not been registered.
	 */
	topology_set_scale_freq_source(&virt_sfd, policy->cpus);

	return 0;
}

static int android_v_vcpufreq_cpu_exit(struct cpufreq_policy *policy)
{
	struct device *cpu_dev;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev)
		return -ENODEV;

	topology_clear_scale_freq_source(SCALE_FREQ_SOURCE_VIRT, policy->related_cpus);
	dev_pm_opp_free_cpufreq_table(cpu_dev, &policy->freq_table);
	return 0;
}

static int android_v_vcpufreq_online(struct cpufreq_policy *policy)
{
	/* Nothing to restore. */
	return 0;
}

static int android_v_vcpufreq_offline(struct cpufreq_policy *policy)
{
	/* Dummy offline() to avoid exit() being called and freeing resources. */
	return 0;
}

static int android_v_cpufreq_verify(struct cpufreq_policy_data *data)
{
	struct cpufreq_policy *policy;
	int ret;

	policy = cpufreq_cpu_get(data->cpu);
	if (!policy)
		return 0;

	/*
	 * Thermal pressure is being reworked as generic system pressure
	 * upstream, this won't be needed in future/newer kernels as cpufreq
	 * pressure will be applied automatically.
	 */
	arch_update_thermal_pressure(policy->related_cpus, data->max);
	cpufreq_cpu_put(policy);

	ret = cpufreq_frequency_table_verify(data, policy->freq_table);

	return ret;
}

static struct cpufreq_driver cpufreq_android_v_virt_driver = {
	.name		= "andr-v-vcpufreq",
	.init		= android_v_vcpufreq_cpu_init,
	.exit		= android_v_vcpufreq_cpu_exit,
	.online         = android_v_vcpufreq_online,
	.offline        = android_v_vcpufreq_offline,
	.verify		= android_v_cpufreq_verify,
	.target_index	= android_v_vcpufreq_target_index,
	.fast_switch	= android_v_vcpufreq_fast_switch,
	.attr		= cpufreq_generic_attr,
};

static int android_v_vcpufreq_driver_probe(struct platform_device *pdev)
{
	int ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	ret = cpufreq_register_driver(&cpufreq_android_v_virt_driver);
	if (ret) {
		dev_err(&pdev->dev, "Android-V Virt Cpufreq driver failed to register: %d\n", ret);
		return ret;
	}

	dev_dbg(&pdev->dev, "Android-V Virt CPUFreq driver initialized\n");
	return 0;
}

static int android_v_vcpufreq_driver_remove(struct platform_device *pdev)
{
	cpufreq_unregister_driver(&cpufreq_android_v_virt_driver);
	return 0;
}

static const struct of_device_id android_v_vcpufreq_match[] = {
	{ .compatible = "virtual,android-v-only-cpufreq", .data = NULL},
	{}
};
MODULE_DEVICE_TABLE(of, android_v_vcpufreq_match);

static struct platform_driver android_v_vcpufreq_driver = {
	.probe = android_v_vcpufreq_driver_probe,
	.remove = android_v_vcpufreq_driver_remove,
	.driver = {
		.name = "andr-v-vcpufreq",
		.of_match_table = android_v_vcpufreq_match,
	},
};

static int __init android_v_vcpufreq_init(void)
{
	return platform_driver_register(&android_v_vcpufreq_driver);
}
postcore_initcall(android_v_vcpufreq_init);

static void __exit android_v_vcpufreq_exit(void)
{
	platform_driver_unregister(&android_v_vcpufreq_driver);
}
module_exit(android_v_vcpufreq_exit);

MODULE_DESCRIPTION("Virtual cpufreq driver only for Android-V");
MODULE_LICENSE("GPL");
