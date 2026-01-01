// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cpufreq extern governors common code
 *
 * Copyright	(C) Yipeng Zou <zouyipeng@huawei.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/percpu-defs.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/bpf_verifier.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include "cpufreq_governor.h"

#define DEAFULT_FREQUENCY_UP_THRESHOLD 30
#define CPUFREQ_EXT_NAME_LEN 128
#define CPUFREQ_EXT_DEFAULT_NAME "performance"
#define CPUFREQ_EXT_DESCR(m)	{ .ext_stat = m, .name = #m }

static DEFINE_MUTEX(ext_mutex);

enum {
	CPUFREQ_EXT_NONE = 0,
	CPUFREQ_EXT_INIT,
	CPUFREQ_EXT_LOADED,
};

struct cpufreq_ext_status_desc {
	unsigned int ext_stat;
	char *name;
};

static const struct cpufreq_ext_status_desc cfe_stats[] = {
	CPUFREQ_EXT_DESCR(CPUFREQ_EXT_NONE),
	CPUFREQ_EXT_DESCR(CPUFREQ_EXT_INIT),
	CPUFREQ_EXT_DESCR(CPUFREQ_EXT_LOADED),
};

struct ext_tuner {
	unsigned int ext_gov_stat;
};

struct ext_policy {
	struct policy_dbs_info policy_dbs;
	unsigned long next_freq;
};

struct cpufreq_governor_ext_ops {
	/**
	 * get_next_freq - Update the next cpufreq
	 * @policy: current cpufreq_policy
	 *
	 * Make decision how to adjust cpufreq here.
	 */
	unsigned long (*get_next_freq)(struct cpufreq_policy *policy);

	/**
	 * get_sampling_rate - Update the sampling_rate
	 * @policy: current cpufreq_policy
	 *
	 * Make decision how to adjust sampling_rate here.
	 */
	unsigned int (*get_sampling_rate)(struct cpufreq_policy *policy);

	/**
	 * init - Initialize the BPF cpufreq governor
	 */
	unsigned int (*init)(void);

	/**
	 * exit - Clean up the BPF cpufreq governor
	 */
	void (*exit)(void);

	/**
	 * BPF based cpufreq governor name.
	 */
	char name[CPUFREQ_EXT_NAME_LEN];
};

/*
 * get_next_freq_nop : Default get_next_freq function
 * Allways keep max cpufreq in default.
 */
static unsigned long get_next_freq_nop(struct cpufreq_policy *policy) { return policy->max; }

/*
 * get_sampling_rate_nop : Default get_sampling_rate function
 * Keep sampling_rate no modified.
 */
static unsigned int get_sampling_rate_nop(struct cpufreq_policy *policy) { return 0; }

/*
 * init_nop : Default init function
 */
static unsigned int init_nop(void) { return 0; }

/*
 * exit_nop : Default exit function
 */
static void exit_nop(void) { }

/*
 * Singleton Pattern, Only have one ext_ops in global.
 */
static struct cpufreq_governor_ext_ops bpf_ext_ops = {
	.get_next_freq = get_next_freq_nop,
	.get_sampling_rate = get_sampling_rate_nop,
	.init = init_nop,
	.exit = exit_nop,
};

static struct static_key_false ext_gov_load;

static struct cpufreq_governor_ext_ops ext_ops_global;

static struct ext_tuner *ext_global_tuner;

/************************** bpf interface ************************/
static const struct btf_type *cpufreq_policy_type;
static u32 cpufreq_policy_type_id;

static int ext_struct_access(struct bpf_verifier_log *log,
			     const struct bpf_reg_state *reg, int off,
			     int size)
{
	const struct btf_type *t;

	t = btf_type_by_id(reg->btf, reg->btf_id);

	if (t == cpufreq_policy_type) {
		/* Support all struct cpufreq_policy access */
		if (off >= offsetof(struct cpufreq_policy, cpus) &&
		    off + size <= offsetofend(struct cpufreq_policy, nb_max))
			return SCALAR_VALUE;
	}

	pr_err("%s : Access unsupport struct.\n", __func__);
	return -EACCES;
}

static const struct bpf_verifier_ops ext_verifier_ops = {
	.get_func_proto = bpf_base_func_proto,
	.is_valid_access = btf_ctx_access,
	.btf_struct_access = ext_struct_access,
};

static int ext_init_member(const struct btf_type *t, const struct btf_member *member,
			   void *kdata, const void *udata)
{
	const struct cpufreq_governor_ext_ops *uops = udata;
	struct cpufreq_governor_ext_ops *ops = kdata;
	u32 offset = __btf_member_bit_offset(t, member) / 8;
	int ret;

	switch (offset) {
	case offsetof(struct cpufreq_governor_ext_ops, name):
		ret = bpf_obj_name_cpy(ops->name, uops->name,
				       sizeof(ops->name));
		if (ret <= 0) {
			pr_err("%s : offset %d : copy name fail(%d) (%s)\n",
			       __func__, offset, ret, uops->name);
			return -EINVAL;
		}
		return 1;
	}
	return 0;
}

static int ext_check_member(const struct btf_type *t,
			    const struct btf_member *member,
			    const struct bpf_prog *prog)
{
	u32 offset = __btf_member_bit_offset(t, member) / 8;

	switch (offset) {
	case offsetof(struct cpufreq_governor_ext_ops, get_next_freq):
	case offsetof(struct cpufreq_governor_ext_ops, get_sampling_rate):
	case offsetof(struct cpufreq_governor_ext_ops, init):
	case offsetof(struct cpufreq_governor_ext_ops, exit):
	case offsetof(struct cpufreq_governor_ext_ops, name):
		break;
	default:
		pr_err("%s: Unsupport offset %d\n", __func__, offset);
		return -EINVAL;
	}

	return 0;
}

static void ext_disable(void)
{
	pr_info("%s : Unregister ext governor(%s).\n", __func__, ext_ops_global.name);
	static_branch_disable(&ext_gov_load);
	ext_global_tuner->ext_gov_stat = CPUFREQ_EXT_INIT;
	ext_ops_global.get_next_freq = get_next_freq_nop;
	ext_ops_global.get_sampling_rate = get_sampling_rate_nop;
	ext_ops_global.init = init_nop;
	ext_ops_global.exit = exit_nop;
	strscpy(ext_ops_global.name, CPUFREQ_EXT_DEFAULT_NAME, strlen(CPUFREQ_EXT_DEFAULT_NAME));
}

static int ext_reg(void *kdata, struct bpf_link *link)
{
	struct cpufreq_governor_ext_ops *ops = (struct cpufreq_governor_ext_ops *)kdata;

	mutex_lock(&ext_mutex);

	/*
	 * Only can register when ext governor initialled and there is no other has been register.
	 */
	if (ext_global_tuner == NULL) {
		pr_err("%s : Need set ext governor first.\n", __func__);
		mutex_unlock(&ext_mutex);
		return -EEXIST;
	}

	if (ext_global_tuner->ext_gov_stat != CPUFREQ_EXT_INIT) {
		pr_err("%s : Already register.\n", __func__);
		mutex_unlock(&ext_mutex);
		return -EEXIST;
	}

	ext_global_tuner->ext_gov_stat = CPUFREQ_EXT_LOADED;
	ext_ops_global = *ops;

	if (ext_ops_global.init && ext_ops_global.init()) {
		ext_disable();
		mutex_unlock(&ext_mutex);
		return -EINVAL;
	}

	static_branch_enable(&ext_gov_load);
	pr_info("%s: Register ext governor(%s).\n", __func__, ext_ops_global.name);
	mutex_unlock(&ext_mutex);
	return 0;
}

static void ext_unreg(void *kdata, struct bpf_link *link)
{
	mutex_lock(&ext_mutex);

	if (ext_ops_global.exit)
		ext_ops_global.exit();

	ext_disable();
	mutex_unlock(&ext_mutex);
}

static int ext_init(struct btf *btf)
{
	s32 type_id;

	type_id = btf_find_by_name_kind(btf, "cpufreq_policy", BTF_KIND_STRUCT);
	if (type_id < 0)
		return -EINVAL;

	cpufreq_policy_type = btf_type_by_id(btf, type_id);
	cpufreq_policy_type_id = type_id;
	return 0;
}

static int ext_update(void *kdata, void *old_kdata, struct bpf_link *link)
{
	/*
	 * Not support update
	 */
	return -EOPNOTSUPP;
}

static int ext_validate(void *kdata)
{
	return 0;
}

static struct bpf_struct_ops bpf_cpufreq_governor_ext_ops = {
	.verifier_ops = &ext_verifier_ops,
	.reg = ext_reg,
	.unreg = ext_unreg,
	.check_member = ext_check_member,
	.init_member = ext_init_member,
	.init = ext_init,
	.update = ext_update,
	.validate = ext_validate,
	.name = "cpufreq_governor_ext_ops",
	.owner = THIS_MODULE,
	.cfi_stubs = &bpf_ext_ops
};

/************************** cpufreq_ext bpf kfunc ************************/
__bpf_kfunc_start_defs();
__bpf_kfunc s32 ext_helper_update_io_busy(struct cpufreq_policy *policy, unsigned int is_busy)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct dbs_data *dbs_data = policy_dbs->dbs_data;

	dbs_data->io_is_busy = is_busy;
	return 0;
}

__bpf_kfunc s32 ext_helper_update_up_threshold(struct cpufreq_policy *policy,
					       unsigned int threshold)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct dbs_data *dbs_data = policy_dbs->dbs_data;

	dbs_data->up_threshold = threshold;
	return 0;
}

__bpf_kfunc s32 ext_helper_update_ignore_nice_load(struct cpufreq_policy *policy,
						   unsigned int ignore_nice_load)
{
	struct policy_dbs_info *policy_dbs = policy->governor_data;
	struct dbs_data *dbs_data = policy_dbs->dbs_data;

	dbs_data->ignore_nice_load = ignore_nice_load;
	return 0;
}

__bpf_kfunc bool ext_helper_is_cpu_in_policy(unsigned int cpu,
					     struct cpufreq_policy *policy)
{
	if (policy == NULL)
		return false;

	return cpumask_test_cpu(cpu, policy->cpus);
}
__bpf_kfunc_end_defs();

BTF_KFUNCS_START(ext_bpf_helpers)
BTF_ID_FLAGS(func, ext_helper_update_io_busy)
BTF_ID_FLAGS(func, ext_helper_update_up_threshold)
BTF_ID_FLAGS(func, ext_helper_update_ignore_nice_load)
BTF_ID_FLAGS(func, ext_helper_is_cpu_in_policy)
BTF_KFUNCS_END(ext_bpf_helpers)

static const struct btf_kfunc_id_set ext_helpers = {
	.owner			= THIS_MODULE,
	.set			= &ext_bpf_helpers,
};

static int __init cpufreq_ext_bpf_init(void)
{
	int ret;

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &ext_helpers);
	if (ret) {
		pr_err("Failed to register kfunc sets (%d)\n", ret);
		return ret;
	}

	ret = register_bpf_struct_ops(&bpf_cpufreq_governor_ext_ops, cpufreq_governor_ext_ops);
	if (ret) {
		pr_err("Register struct_ops fail: %d\n", ret);
		return ret;
	}

	return 0;
}
device_initcall(cpufreq_ext_bpf_init);

/************************** sysfs interface ************************/
static inline struct ext_tuner *to_ext_tuner(struct gov_attr_set *attr_set)
{
	struct dbs_data *dbs_data = to_dbs_data(attr_set);

	return dbs_data->tuners;
}

static ssize_t stat_show(struct gov_attr_set *attr_set, char *buf)
{
	struct ext_tuner *tuner = to_ext_tuner(attr_set);
	int len = 0;

	len += sprintf(buf + len, "Stat: %s\n", cfe_stats[tuner->ext_gov_stat].name);
	len += sprintf(buf + len, "BPF governor: %s\n", ext_ops_global.name);
	return len;
}
gov_attr_ro(stat);

static struct attribute *ext_gov_attrs[] = {
	&stat.attr,
	NULL
};
ATTRIBUTE_GROUPS(ext_gov);

/************************** cpufreq interface ************************/
static inline struct ext_policy *to_ext_policy(struct policy_dbs_info *policy_dbs)
{
	return container_of(policy_dbs, struct ext_policy, policy_dbs);
}

static unsigned int ext_get_next_freq_default(struct cpufreq_policy *policy)
{
	/*
	 * Set cpu freq to max as default.
	 */
	return policy->max;
}

static unsigned int ext_gov_update(struct cpufreq_policy *policy)
{
	struct ext_policy *ext;
	struct policy_dbs_info *policy_dbs;
	unsigned int update_sampling_rate = 0;
	struct dbs_governor *gov = dbs_governor_of(policy);

	/* Only need to update current policy freq */
	policy_dbs = container_of((void *)policy, struct policy_dbs_info, policy);

	ext = to_ext_policy(policy_dbs);

	if (static_branch_likely(&ext_gov_load) &&
	    (ext_ops_global.get_next_freq != get_next_freq_nop))
		ext->next_freq = ext_ops_global.get_next_freq(policy);
	else
		ext->next_freq = ext_get_next_freq_default(policy);

	if (ext->next_freq != policy->cur)
		__cpufreq_driver_target(policy, ext->next_freq, CPUFREQ_RELATION_H);

	if (static_branch_likely(&ext_gov_load) &&
	    (ext_ops_global.get_sampling_rate != get_sampling_rate_nop))
		update_sampling_rate = ext_ops_global.get_sampling_rate(policy);

	/* If get_sampling_rate return 0, means we don't modify sampling_rate any more. */
	return update_sampling_rate == 0 ? gov->gdbs_data->sampling_rate : update_sampling_rate;
}

static struct policy_dbs_info *ext_gov_alloc(void)
{
	struct ext_policy *ext;

	ext = kzalloc(sizeof(*ext), GFP_KERNEL);
	return ext ? &ext->policy_dbs : NULL;
}

static void ext_gov_free(struct policy_dbs_info *policy_dbs)
{
	kfree(to_ext_policy(policy_dbs));
}

static int ext_gov_init(struct dbs_data *dbs_data)
{
	struct ext_tuner *tuner;

	if (!dbs_data)
		return -EPERM;

	tuner = kzalloc(sizeof(*tuner), GFP_KERNEL);
	if (!tuner)
		return -ENOMEM;

	tuner->ext_gov_stat = CPUFREQ_EXT_INIT;
	dbs_data->io_is_busy = 0;
	dbs_data->ignore_nice_load = 0;
	dbs_data->tuners = tuner;
	dbs_data->up_threshold = DEAFULT_FREQUENCY_UP_THRESHOLD;
	ext_global_tuner = tuner;
	/* Disable ext_gov_load as default */
	static_branch_disable(&ext_gov_load);

	/* As default, set ext_ops_global to nop function */
	ext_ops_global.get_next_freq = get_next_freq_nop;
	ext_ops_global.get_sampling_rate = get_sampling_rate_nop;
	ext_ops_global.init = init_nop;
	ext_ops_global.exit = exit_nop;
	strscpy(ext_ops_global.name, CPUFREQ_EXT_DEFAULT_NAME, strlen(CPUFREQ_EXT_DEFAULT_NAME));
	return 0;
}

static void ext_gov_exit(struct dbs_data *dbs_data)
{
	struct ext_tuner *tuner;

	if (!dbs_data || !dbs_data->tuners)
		return;

	tuner = (struct ext_tuner *)dbs_data->tuners;
	tuner->ext_gov_stat = CPUFREQ_EXT_NONE;
	kfree(dbs_data->tuners);
	dbs_data->tuners = NULL;
	ext_global_tuner = NULL;
}

static void ext_gov_start(struct cpufreq_policy *policy)
{
	struct ext_policy *ext = to_ext_policy(policy->governor_data);

	ext->next_freq = cpufreq_driver_resolve_freq(policy, policy->cur);
}

static struct dbs_governor ext_dbs_gov = {
	.gov = CPUFREQ_DBS_GOVERNOR_INITIALIZER("ext"),
	.kobj_type = { .default_groups = ext_gov_groups },
	.gov_dbs_update = ext_gov_update,
	.alloc = ext_gov_alloc,
	.free = ext_gov_free,
	.init = ext_gov_init,
	.exit = ext_gov_exit,
	.start = ext_gov_start,
};

#define CPU_FREQ_GOV_EXT	(ext_dbs_gov.gov)

MODULE_AUTHOR("Yipeng Zou <zouyipeng@huawei.com>");
MODULE_DESCRIPTION("'cpufreq_ext' - A bpf based cpufreq governor");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_EXT
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &CPU_FREQ_GOV_EXT;
}
#endif

cpufreq_governor_init(CPU_FREQ_GOV_EXT);
cpufreq_governor_exit(CPU_FREQ_GOV_EXT);
