// SPDX-License-Identifier: GPL-2.0-only
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/*
 * When VIP task is running switching to max speed
 */
static s32 vip_task_pid[] = {
	324, // Stub, need to be replacing
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__type(key, u32);
	__type(value, int);
	__uint(max_entries, 1);
} exit_stat SEC(".maps");

#define READ_KERNEL(P)								\
	({									\
		typeof(P) val;							\
		bpf_probe_read_kernel(&val, sizeof(val), &(P));			\
		val;								\
	})

#define TASK_RUNNING 0x00000000

#define task_is_running(task)	(READ_KERNEL((task)->__state) == TASK_RUNNING)

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct task_struct *bpf_task_from_pid(s32 pid) __ksym;
bool bpf_cpumask_test_cpu(u32 cpu, const struct cpumask *cpumask) __ksym;
void bpf_task_release(struct task_struct *p) __ksym;
bool ext_helper_is_cpu_in_policy(unsigned int cpu, struct cpufreq_policy *policy) __ksym;

static bool is_vip_task_running_on_cpus(struct cpufreq_policy *policy)
{
	struct task_struct *task = NULL;
	bool is_vip_running = false;
	struct thread_info info;
	s32 cpu;

	for (unsigned int index = 0; index < ARRAY_SIZE(vip_task_pid); index++) {
		task = bpf_task_from_pid(vip_task_pid[index]);
		if (!task)
			continue;

		is_vip_running = task_is_running(task);
		info = READ_KERNEL(task->thread_info);
		cpu = READ_KERNEL(info.cpu);
		bpf_task_release(task);

		/* Only task running on target CPU can update policy freq */
		if (is_vip_running && ext_helper_is_cpu_in_policy(cpu, policy))
			return true;
	}

	return false;
}

SEC("struct_ops.s/get_next_freq")
unsigned long BPF_PROG(update_next_freq, struct cpufreq_policy *policy)
{
	unsigned int max_freq = READ_KERNEL(policy->max);
	unsigned int min_freq = READ_KERNEL(policy->min);
	unsigned int cur_freq = READ_KERNEL(policy->cur);
	unsigned int policy_cpu = READ_KERNEL(policy->cpu);

	if (is_vip_task_running_on_cpus(policy) == false) {
		if (cur_freq != min_freq)
			bpf_printk("No VIP Set Freq(%d) On Policy%d.\n", min_freq, policy_cpu);
		return min_freq;
	}

	if (cur_freq != max_freq)
		bpf_printk("VIP running Set Freq(%d) On Policy%d.\n", max_freq, policy_cpu);
	return max_freq;
}

SEC("struct_ops.s/get_sampling_rate")
unsigned int BPF_PROG(update_sampling_rate, struct cpufreq_policy *policy)
{
	/* Return 0 means keep smapling_rate no modified */
	return 0;
}

SEC("struct_ops.s/init")
unsigned int BPF_PROG(ext_init)
{
	return 0;
}

SEC("struct_ops.s/exit")
void BPF_PROG(ext_exit)
{
	unsigned int index = 0;
	int code = 1;

	bpf_map_update_elem(&exit_stat, &index, &code, BPF_EXIST);
}

SEC(".struct_ops.link")
struct cpufreq_governor_ext_ops cpufreq_ext_demo_ops = {
	.get_next_freq		= (void *)update_next_freq,
	.get_sampling_rate	= (void *)update_sampling_rate,
	.init			= (void *)ext_init,
	.exit			= (void *)ext_exit,
	.name			= "VIP"
};

char _license[] SEC("license") = "GPL";
