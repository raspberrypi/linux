#ifndef _KERNEL_SCHED_ALT_SCHED_H
#define _KERNEL_SCHED_ALT_SCHED_H

#include <linux/context_tracking.h>
#include <linux/profile.h>
#include <linux/stop_machine.h>
#include <linux/syscalls.h>
#include <linux/tick.h>

#include <trace/events/power.h>
#include <trace/events/sched.h>

#include "../workqueue_internal.h"

#include "cpupri.h"

#ifdef CONFIG_CGROUP_SCHED
/* task group related information */
struct task_group {
	struct cgroup_subsys_state css;

	struct rcu_head rcu;
	struct list_head list;

	struct task_group *parent;
	struct list_head siblings;
	struct list_head children;
};

extern struct task_group *sched_create_group(struct task_group *parent);
extern void sched_online_group(struct task_group *tg,
			       struct task_group *parent);
extern void sched_destroy_group(struct task_group *tg);
extern void sched_release_group(struct task_group *tg);
#endif /* CONFIG_CGROUP_SCHED */

#define MIN_SCHED_NORMAL_PRIO	(32)
/*
 * levels: RT(0-24), reserved(25-31), NORMAL(32-63), cpu idle task(64)
 *
 * -- BMQ --
 * NORMAL: (lower boost range 12, NICE_WIDTH 40, higher boost range 12) / 2
 * -- PDS --
 * NORMAL: SCHED_EDGE_DELTA + ((NICE_WIDTH 40) / 2)
 */
#define SCHED_LEVELS		(64 + 1)

#define IDLE_TASK_SCHED_PRIO	(SCHED_LEVELS - 1)

#ifdef CONFIG_SCHED_DEBUG
# define SCHED_WARN_ON(x)	WARN_ONCE(x, #x)
extern void resched_latency_warn(int cpu, u64 latency);
#else
# define SCHED_WARN_ON(x)	({ (void)(x), 0; })
static inline void resched_latency_warn(int cpu, u64 latency) {}
#endif

/*
 * Increase resolution of nice-level calculations for 64-bit architectures.
 * The extra resolution improves shares distribution and load balancing of
 * low-weight task groups (eg. nice +19 on an autogroup), deeper taskgroup
 * hierarchies, especially on larger systems. This is not a user-visible change
 * and does not change the user-interface for setting shares/weights.
 *
 * We increase resolution only if we have enough bits to allow this increased
 * resolution (i.e. 64-bit). The costs for increasing resolution when 32-bit
 * are pretty high and the returns do not justify the increased costs.
 *
 * Really only required when CONFIG_FAIR_GROUP_SCHED=y is also set, but to
 * increase coverage and consistency always enable it on 64-bit platforms.
 */
#ifdef CONFIG_64BIT
# define NICE_0_LOAD_SHIFT	(SCHED_FIXEDPOINT_SHIFT + SCHED_FIXEDPOINT_SHIFT)
# define scale_load(w)		((w) << SCHED_FIXEDPOINT_SHIFT)
# define scale_load_down(w) \
({ \
	unsigned long __w = (w); \
	if (__w) \
		__w = max(2UL, __w >> SCHED_FIXEDPOINT_SHIFT); \
	__w; \
})
#else
# define NICE_0_LOAD_SHIFT	(SCHED_FIXEDPOINT_SHIFT)
# define scale_load(w)		(w)
# define scale_load_down(w)	(w)
#endif

/*
 * Tunables that become constants when CONFIG_SCHED_DEBUG is off:
 */
#ifdef CONFIG_SCHED_DEBUG
# define const_debug __read_mostly
#else
# define const_debug const
#endif

/* task_struct::on_rq states: */
#define TASK_ON_RQ_QUEUED	1
#define TASK_ON_RQ_MIGRATING	2

static inline int task_on_rq_queued(struct task_struct *p)
{
	return p->on_rq == TASK_ON_RQ_QUEUED;
}

static inline int task_on_rq_migrating(struct task_struct *p)
{
	return READ_ONCE(p->on_rq) == TASK_ON_RQ_MIGRATING;
}

/* Wake flags. The first three directly map to some SD flag value */
#define WF_EXEC         0x02 /* Wakeup after exec; maps to SD_BALANCE_EXEC */
#define WF_FORK         0x04 /* Wakeup after fork; maps to SD_BALANCE_FORK */
#define WF_TTWU         0x08 /* Wakeup;            maps to SD_BALANCE_WAKE */

#define WF_SYNC         0x10 /* Waker goes to sleep after wakeup */
#define WF_MIGRATED     0x20 /* Internal use, task got migrated */
#define WF_CURRENT_CPU  0x40 /* Prefer to move the wakee to the current CPU. */

#ifdef CONFIG_SMP
static_assert(WF_EXEC == SD_BALANCE_EXEC);
static_assert(WF_FORK == SD_BALANCE_FORK);
static_assert(WF_TTWU == SD_BALANCE_WAKE);
#endif

#define SCHED_QUEUE_BITS	(SCHED_LEVELS - 1)

struct sched_queue {
	DECLARE_BITMAP(bitmap, SCHED_QUEUE_BITS);
	struct list_head heads[SCHED_LEVELS];
};

struct rq;
struct cpuidle_state;

struct balance_callback {
	struct balance_callback *next;
	void (*func)(struct rq *rq);
};

typedef void (*balance_func_t)(struct rq *rq, int cpu);
typedef void (*set_idle_mask_func_t)(unsigned int cpu, struct cpumask *dstp);
typedef void (*clear_idle_mask_func_t)(int cpu, struct cpumask *dstp);

struct balance_arg {
	struct task_struct	*task;
	int			active;
	cpumask_t		*cpumask;
};

/*
 * This is the main, per-CPU runqueue data structure.
 * This data should only be modified by the local cpu.
 */
struct rq {
	/* runqueue lock: */
	raw_spinlock_t			lock;

	struct task_struct __rcu	*curr;
	struct task_struct		*idle;
	struct task_struct		*stop;
	struct mm_struct		*prev_mm;

	struct sched_queue		queue		____cacheline_aligned;

	int				prio;
#ifdef CONFIG_SCHED_PDS
	int				prio_idx;
	u64				time_edge;
#endif

	/* switch count */
	u64 nr_switches;

	atomic_t nr_iowait;

#ifdef CONFIG_SCHED_DEBUG
	u64 last_seen_need_resched_ns;
	int ticks_without_resched;
#endif

#ifdef CONFIG_MEMBARRIER
	int membarrier_state;
#endif

	set_idle_mask_func_t	set_idle_mask_func;
	clear_idle_mask_func_t	clear_idle_mask_func;

#ifdef CONFIG_SMP
	int cpu;		/* cpu of this runqueue */
	bool online;

	unsigned int		ttwu_pending;
	unsigned char		nohz_idle_balance;
	unsigned char		idle_balance;

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	struct sched_avg	avg_irq;
#endif

	balance_func_t		balance_func;
	struct balance_arg	active_balance_arg		____cacheline_aligned;
	struct cpu_stop_work	active_balance_work;

	struct balance_callback	*balance_callback;
#ifdef CONFIG_HOTPLUG_CPU
	struct rcuwait		hotplug_wait;
#endif
	unsigned int		nr_pinned;

#endif /* CONFIG_SMP */
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	u64 prev_irq_time;
#endif /* CONFIG_IRQ_TIME_ACCOUNTING */
#ifdef CONFIG_PARAVIRT
	u64 prev_steal_time;
#endif /* CONFIG_PARAVIRT */
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	u64 prev_steal_time_rq;
#endif /* CONFIG_PARAVIRT_TIME_ACCOUNTING */

	/* For genenal cpu load util */
	s32 load_history;
	u64 load_block;
	u64 load_stamp;

	/* calc_load related fields */
	unsigned long calc_load_update;
	long calc_load_active;

	/* Ensure that all clocks are in the same cache line */
	u64			clock ____cacheline_aligned;
	u64			clock_task;

	unsigned int  nr_running;
	unsigned long nr_uninterruptible;

#ifdef CONFIG_SCHED_HRTICK
#ifdef CONFIG_SMP
	call_single_data_t hrtick_csd;
#endif
	struct hrtimer		hrtick_timer;
	ktime_t			hrtick_time;
#endif

#ifdef CONFIG_SCHEDSTATS

	/* latency stats */
	struct sched_info rq_sched_info;
	unsigned long long rq_cpu_time;
	/* could above be rq->cfs_rq.exec_clock + rq->rt_rq.rt_runtime ? */

	/* sys_sched_yield() stats */
	unsigned int yld_count;

	/* schedule() stats */
	unsigned int sched_switch;
	unsigned int sched_count;
	unsigned int sched_goidle;

	/* try_to_wake_up() stats */
	unsigned int ttwu_count;
	unsigned int ttwu_local;
#endif /* CONFIG_SCHEDSTATS */

#ifdef CONFIG_CPU_IDLE
	/* Must be inspected within a rcu lock section */
	struct cpuidle_state *idle_state;
#endif

#ifdef CONFIG_NO_HZ_COMMON
#ifdef CONFIG_SMP
	call_single_data_t	nohz_csd;
#endif
	atomic_t		nohz_flags;
#endif /* CONFIG_NO_HZ_COMMON */

	/* Scratch cpumask to be temporarily used under rq_lock */
	cpumask_var_t		scratch_mask;
};

extern unsigned int sysctl_sched_base_slice;

extern unsigned long rq_load_util(struct rq *rq, unsigned long max);

extern unsigned long calc_load_update;
extern atomic_long_t calc_load_tasks;

extern void calc_global_load_tick(struct rq *this_rq);
extern long calc_load_fold_active(struct rq *this_rq, long adjust);

DECLARE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);
#define cpu_rq(cpu)		(&per_cpu(runqueues, (cpu)))
#define this_rq()		this_cpu_ptr(&runqueues)
#define task_rq(p)		cpu_rq(task_cpu(p))
#define cpu_curr(cpu)		(cpu_rq(cpu)->curr)
#define raw_rq()		raw_cpu_ptr(&runqueues)

#ifdef CONFIG_SMP
#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_SYSCTL)
void register_sched_domain_sysctl(void);
void unregister_sched_domain_sysctl(void);
#else
static inline void register_sched_domain_sysctl(void)
{
}
static inline void unregister_sched_domain_sysctl(void)
{
}
#endif

extern bool sched_smp_initialized;

enum {
#ifdef CONFIG_SCHED_SMT
	SMT_LEVEL_SPACE_HOLDER,
#endif
	COREGROUP_LEVEL_SPACE_HOLDER,
	CORE_LEVEL_SPACE_HOLDER,
	OTHER_LEVEL_SPACE_HOLDER,
	NR_CPU_AFFINITY_LEVELS
};

DECLARE_PER_CPU_ALIGNED(cpumask_t [NR_CPU_AFFINITY_LEVELS], sched_cpu_topo_masks);

static inline int
__best_mask_cpu(const cpumask_t *cpumask, const cpumask_t *mask)
{
	int cpu;

	while ((cpu = cpumask_any_and(cpumask, mask)) >= nr_cpu_ids)
		mask++;

	return cpu;
}

static inline int best_mask_cpu(int cpu, const cpumask_t *mask)
{
	return __best_mask_cpu(mask, per_cpu(sched_cpu_topo_masks, cpu));
}

#endif

#ifndef arch_scale_freq_tick
static __always_inline
void arch_scale_freq_tick(void)
{
}
#endif

#ifndef arch_scale_freq_capacity
static __always_inline
unsigned long arch_scale_freq_capacity(int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

static inline u64 __rq_clock_broken(struct rq *rq)
{
	return READ_ONCE(rq->clock);
}

static inline u64 rq_clock(struct rq *rq)
{
	/*
	 * Relax lockdep_assert_held() checking as in VRQ, call to
	 * sched_info_xxxx() may not held rq->lock
	 * lockdep_assert_held(&rq->lock);
	 */
	return rq->clock;
}

static inline u64 rq_clock_task(struct rq *rq)
{
	/*
	 * Relax lockdep_assert_held() checking as in VRQ, call to
	 * sched_info_xxxx() may not held rq->lock
	 * lockdep_assert_held(&rq->lock);
	 */
	return rq->clock_task;
}

/*
 * {de,en}queue flags:
 *
 * DEQUEUE_SLEEP  - task is no longer runnable
 * ENQUEUE_WAKEUP - task just became runnable
 *
 */

#define DEQUEUE_SLEEP		0x01

#define ENQUEUE_WAKEUP		0x01


/*
 * Below are scheduler API which using in other kernel code
 * It use the dummy rq_flags
 * ToDo : BMQ need to support these APIs for compatibility with mainline
 * scheduler code.
 */
struct rq_flags {
	unsigned long flags;
};

struct rq *__task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(rq->lock);

struct rq *task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(p->pi_lock)
	__acquires(rq->lock);

static inline void __task_rq_unlock(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	raw_spin_unlock(&rq->lock);
}

static inline void
task_rq_unlock(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
	__releases(rq->lock)
	__releases(p->pi_lock)
{
	raw_spin_unlock(&rq->lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, rf->flags);
}

static inline void
rq_lock(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_lock(&rq->lock);
}

static inline void
rq_unlock(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	raw_spin_unlock(&rq->lock);
}

static inline void
rq_lock_irq(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_lock_irq(&rq->lock);
}

static inline void
rq_unlock_irq(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	raw_spin_unlock_irq(&rq->lock);
}

static inline struct rq *
this_rq_lock_irq(struct rq_flags *rf)
	__acquires(rq->lock)
{
	struct rq *rq;

	local_irq_disable();
	rq = this_rq();
	raw_spin_lock(&rq->lock);

	return rq;
}

static inline raw_spinlock_t *__rq_lockp(struct rq *rq)
{
	return &rq->lock;
}

static inline raw_spinlock_t *rq_lockp(struct rq *rq)
{
	return __rq_lockp(rq);
}

static inline void lockdep_assert_rq_held(struct rq *rq)
{
	lockdep_assert_held(__rq_lockp(rq));
}

extern void raw_spin_rq_lock_nested(struct rq *rq, int subclass);
extern void raw_spin_rq_unlock(struct rq *rq);

static inline void raw_spin_rq_lock(struct rq *rq)
{
	raw_spin_rq_lock_nested(rq, 0);
}

static inline void raw_spin_rq_lock_irq(struct rq *rq)
{
	local_irq_disable();
	raw_spin_rq_lock(rq);
}

static inline void raw_spin_rq_unlock_irq(struct rq *rq)
{
	raw_spin_rq_unlock(rq);
	local_irq_enable();
}

static inline int task_current(struct rq *rq, struct task_struct *p)
{
	return rq->curr == p;
}

static inline bool task_on_cpu(struct task_struct *p)
{
	return p->on_cpu;
}

extern struct static_key_false sched_schedstats;

#ifdef CONFIG_CPU_IDLE
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
	rq->idle_state = idle_state;
}

static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	WARN_ON(!rcu_read_lock_held());
	return rq->idle_state;
}
#else
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
}

static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	return NULL;
}
#endif

static inline int cpu_of(const struct rq *rq)
{
#ifdef CONFIG_SMP
	return rq->cpu;
#else
	return 0;
#endif
}

extern void resched_cpu(int cpu);

#include "stats.h"

#ifdef CONFIG_NO_HZ_COMMON
#define NOHZ_BALANCE_KICK_BIT	0
#define NOHZ_STATS_KICK_BIT	1

#define NOHZ_BALANCE_KICK	BIT(NOHZ_BALANCE_KICK_BIT)
#define NOHZ_STATS_KICK		BIT(NOHZ_STATS_KICK_BIT)

#define NOHZ_KICK_MASK	(NOHZ_BALANCE_KICK | NOHZ_STATS_KICK)

#define nohz_flags(cpu)	(&cpu_rq(cpu)->nohz_flags)

/* TODO: needed?
extern void nohz_balance_exit_idle(struct rq *rq);
#else
static inline void nohz_balance_exit_idle(struct rq *rq) { }
*/
#endif

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
struct irqtime {
	u64			total;
	u64			tick_delta;
	u64			irq_start_time;
	struct u64_stats_sync	sync;
};

DECLARE_PER_CPU(struct irqtime, cpu_irqtime);

/*
 * Returns the irqtime minus the softirq time computed by ksoftirqd.
 * Otherwise ksoftirqd's sum_exec_runtime is substracted its own runtime
 * and never move forward.
 */
static inline u64 irq_time_read(int cpu)
{
	struct irqtime *irqtime = &per_cpu(cpu_irqtime, cpu);
	unsigned int seq;
	u64 total;

	do {
		seq = __u64_stats_fetch_begin(&irqtime->sync);
		total = irqtime->total;
	} while (__u64_stats_fetch_retry(&irqtime->sync, seq));

	return total;
}
#endif /* CONFIG_IRQ_TIME_ACCOUNTING */

#ifdef CONFIG_CPU_FREQ
DECLARE_PER_CPU(struct update_util_data __rcu *, cpufreq_update_util_data);
#endif /* CONFIG_CPU_FREQ */

#ifdef CONFIG_NO_HZ_FULL
extern int __init sched_tick_offload_init(void);
#else
static inline int sched_tick_offload_init(void) { return 0; }
#endif

#ifdef arch_scale_freq_capacity
#ifndef arch_scale_freq_invariant
#define arch_scale_freq_invariant()	(true)
#endif
#else /* arch_scale_freq_capacity */
#define arch_scale_freq_invariant()	(false)
#endif

#ifdef CONFIG_SMP
unsigned long sugov_effective_cpu_perf(int cpu, unsigned long actual,
				 unsigned long min,
				 unsigned long max);
#endif /* CONFIG_SMP */

extern void schedule_idle(void);

#define cap_scale(v, s) ((v)*(s) >> SCHED_CAPACITY_SHIFT)

/*
 * !! For sched_setattr_nocheck() (kernel) only !!
 *
 * This is actually gross. :(
 *
 * It is used to make schedutil kworker(s) higher priority than SCHED_DEADLINE
 * tasks, but still be able to sleep. We need this on platforms that cannot
 * atomically change clock frequency. Remove once fast switching will be
 * available on such platforms.
 *
 * SUGOV stands for SchedUtil GOVernor.
 */
#define SCHED_FLAG_SUGOV	0x10000000

#ifdef CONFIG_MEMBARRIER
/*
 * The scheduler provides memory barriers required by membarrier between:
 * - prior user-space memory accesses and store to rq->membarrier_state,
 * - store to rq->membarrier_state and following user-space memory accesses.
 * In the same way it provides those guarantees around store to rq->curr.
 */
static inline void membarrier_switch_mm(struct rq *rq,
					struct mm_struct *prev_mm,
					struct mm_struct *next_mm)
{
	int membarrier_state;

	if (prev_mm == next_mm)
		return;

	membarrier_state = atomic_read(&next_mm->membarrier_state);
	if (READ_ONCE(rq->membarrier_state) == membarrier_state)
		return;

	WRITE_ONCE(rq->membarrier_state, membarrier_state);
}
#else
static inline void membarrier_switch_mm(struct rq *rq,
					struct mm_struct *prev_mm,
					struct mm_struct *next_mm)
{
}
#endif

#ifdef CONFIG_NUMA
extern int sched_numa_find_closest(const struct cpumask *cpus, int cpu);
#else
static inline int sched_numa_find_closest(const struct cpumask *cpus, int cpu)
{
	return nr_cpu_ids;
}
#endif

extern void swake_up_all_locked(struct swait_queue_head *q);
extern void __prepare_to_swait(struct swait_queue_head *q, struct swait_queue *wait);

extern int try_to_wake_up(struct task_struct *tsk, unsigned int state, int wake_flags);

#ifdef CONFIG_PREEMPT_DYNAMIC
extern int preempt_dynamic_mode;
extern int sched_dynamic_mode(const char *str);
extern void sched_dynamic_update(int mode);
#endif

static inline void nohz_run_idle_balance(int cpu) { }

static inline unsigned long
uclamp_eff_value(struct task_struct *p, enum uclamp_id clamp_id)
{
	if (clamp_id == UCLAMP_MIN)
		return 0;

	return SCHED_CAPACITY_SCALE;
}

static inline bool uclamp_rq_is_capped(struct rq *rq) { return false; }

static inline bool uclamp_is_used(void)
{
	return false;
}

static inline unsigned long
uclamp_rq_get(struct rq *rq, enum uclamp_id clamp_id)
{
	if (clamp_id == UCLAMP_MIN)
		return 0;

	return SCHED_CAPACITY_SCALE;
}

static inline void
uclamp_rq_set(struct rq *rq, enum uclamp_id clamp_id, unsigned int value)
{
}

static inline bool uclamp_rq_is_idle(struct rq *rq)
{
	return false;
}

#ifdef CONFIG_SCHED_MM_CID

#define SCHED_MM_CID_PERIOD_NS	(100ULL * 1000000)	/* 100ms */
#define MM_CID_SCAN_DELAY	100			/* 100ms */

extern raw_spinlock_t cid_lock;
extern int use_cid_lock;

extern void sched_mm_cid_migrate_from(struct task_struct *t);
extern void sched_mm_cid_migrate_to(struct rq *dst_rq, struct task_struct *t);
extern void task_tick_mm_cid(struct rq *rq, struct task_struct *curr);
extern void init_sched_mm_cid(struct task_struct *t);

static inline void __mm_cid_put(struct mm_struct *mm, int cid)
{
	if (cid < 0)
		return;
	cpumask_clear_cpu(cid, mm_cidmask(mm));
}

/*
 * The per-mm/cpu cid can have the MM_CID_LAZY_PUT flag set or transition to
 * the MM_CID_UNSET state without holding the rq lock, but the rq lock needs to
 * be held to transition to other states.
 *
 * State transitions synchronized with cmpxchg or try_cmpxchg need to be
 * consistent across cpus, which prevents use of this_cpu_cmpxchg.
 */
static inline void mm_cid_put_lazy(struct task_struct *t)
{
	struct mm_struct *mm = t->mm;
	struct mm_cid __percpu *pcpu_cid = mm->pcpu_cid;
	int cid;

	lockdep_assert_irqs_disabled();
	cid = __this_cpu_read(pcpu_cid->cid);
	if (!mm_cid_is_lazy_put(cid) ||
	    !try_cmpxchg(&this_cpu_ptr(pcpu_cid)->cid, &cid, MM_CID_UNSET))
		return;
	__mm_cid_put(mm, mm_cid_clear_lazy_put(cid));
}

static inline int mm_cid_pcpu_unset(struct mm_struct *mm)
{
	struct mm_cid __percpu *pcpu_cid = mm->pcpu_cid;
	int cid, res;

	lockdep_assert_irqs_disabled();
	cid = __this_cpu_read(pcpu_cid->cid);
	for (;;) {
		if (mm_cid_is_unset(cid))
			return MM_CID_UNSET;
		/*
		 * Attempt transition from valid or lazy-put to unset.
		 */
		res = cmpxchg(&this_cpu_ptr(pcpu_cid)->cid, cid, MM_CID_UNSET);
		if (res == cid)
			break;
		cid = res;
	}
	return cid;
}

static inline void mm_cid_put(struct mm_struct *mm)
{
	int cid;

	lockdep_assert_irqs_disabled();
	cid = mm_cid_pcpu_unset(mm);
	if (cid == MM_CID_UNSET)
		return;
	__mm_cid_put(mm, mm_cid_clear_lazy_put(cid));
}

static inline int __mm_cid_try_get(struct mm_struct *mm)
{
	struct cpumask *cpumask;
	int cid;

	cpumask = mm_cidmask(mm);
	/*
	 * Retry finding first zero bit if the mask is temporarily
	 * filled. This only happens during concurrent remote-clear
	 * which owns a cid without holding a rq lock.
	 */
	for (;;) {
		cid = cpumask_first_zero(cpumask);
		if (cid < nr_cpu_ids)
			break;
		cpu_relax();
	}
	if (cpumask_test_and_set_cpu(cid, cpumask))
		return -1;
	return cid;
}

/*
 * Save a snapshot of the current runqueue time of this cpu
 * with the per-cpu cid value, allowing to estimate how recently it was used.
 */
static inline void mm_cid_snapshot_time(struct rq *rq, struct mm_struct *mm)
{
	struct mm_cid *pcpu_cid = per_cpu_ptr(mm->pcpu_cid, cpu_of(rq));

	lockdep_assert_rq_held(rq);
	WRITE_ONCE(pcpu_cid->time, rq->clock);
}

static inline int __mm_cid_get(struct rq *rq, struct mm_struct *mm)
{
	int cid;

	/*
	 * All allocations (even those using the cid_lock) are lock-free. If
	 * use_cid_lock is set, hold the cid_lock to perform cid allocation to
	 * guarantee forward progress.
	 */
	if (!READ_ONCE(use_cid_lock)) {
		cid = __mm_cid_try_get(mm);
		if (cid >= 0)
			goto end;
		raw_spin_lock(&cid_lock);
	} else {
		raw_spin_lock(&cid_lock);
		cid = __mm_cid_try_get(mm);
		if (cid >= 0)
			goto unlock;
	}

	/*
	 * cid concurrently allocated. Retry while forcing following
	 * allocations to use the cid_lock to ensure forward progress.
	 */
	WRITE_ONCE(use_cid_lock, 1);
	/*
	 * Set use_cid_lock before allocation. Only care about program order
	 * because this is only required for forward progress.
	 */
	barrier();
	/*
	 * Retry until it succeeds. It is guaranteed to eventually succeed once
	 * all newcoming allocations observe the use_cid_lock flag set.
	 */
	do {
		cid = __mm_cid_try_get(mm);
		cpu_relax();
	} while (cid < 0);
	/*
	 * Allocate before clearing use_cid_lock. Only care about
	 * program order because this is for forward progress.
	 */
	barrier();
	WRITE_ONCE(use_cid_lock, 0);
unlock:
	raw_spin_unlock(&cid_lock);
end:
	mm_cid_snapshot_time(rq, mm);
	return cid;
}

static inline int mm_cid_get(struct rq *rq, struct mm_struct *mm)
{
	struct mm_cid __percpu *pcpu_cid = mm->pcpu_cid;
	struct cpumask *cpumask;
	int cid;

	lockdep_assert_rq_held(rq);
	cpumask = mm_cidmask(mm);
	cid = __this_cpu_read(pcpu_cid->cid);
	if (mm_cid_is_valid(cid)) {
		mm_cid_snapshot_time(rq, mm);
		return cid;
	}
	if (mm_cid_is_lazy_put(cid)) {
		if (try_cmpxchg(&this_cpu_ptr(pcpu_cid)->cid, &cid, MM_CID_UNSET))
			__mm_cid_put(mm, mm_cid_clear_lazy_put(cid));
	}
	cid = __mm_cid_get(rq, mm);
	__this_cpu_write(pcpu_cid->cid, cid);
	return cid;
}

static inline void switch_mm_cid(struct rq *rq,
				 struct task_struct *prev,
				 struct task_struct *next)
{
	/*
	 * Provide a memory barrier between rq->curr store and load of
	 * {prev,next}->mm->pcpu_cid[cpu] on rq->curr->mm transition.
	 *
	 * Should be adapted if context_switch() is modified.
	 */
	if (!next->mm) {                                // to kernel
		/*
		 * user -> kernel transition does not guarantee a barrier, but
		 * we can use the fact that it performs an atomic operation in
		 * mmgrab().
		 */
		if (prev->mm)                           // from user
			smp_mb__after_mmgrab();
		/*
		 * kernel -> kernel transition does not change rq->curr->mm
		 * state. It stays NULL.
		 */
	} else {                                        // to user
		/*
		 * kernel -> user transition does not provide a barrier
		 * between rq->curr store and load of {prev,next}->mm->pcpu_cid[cpu].
		 * Provide it here.
		 */
		if (!prev->mm)                          // from kernel
			smp_mb();
		/*
		 * user -> user transition guarantees a memory barrier through
		 * switch_mm() when current->mm changes. If current->mm is
		 * unchanged, no barrier is needed.
		 */
	}
	if (prev->mm_cid_active) {
		mm_cid_snapshot_time(rq, prev->mm);
		mm_cid_put_lazy(prev);
		prev->mm_cid = -1;
	}
	if (next->mm_cid_active)
		next->last_mm_cid = next->mm_cid = mm_cid_get(rq, next->mm);
}

#else
static inline void switch_mm_cid(struct rq *rq, struct task_struct *prev, struct task_struct *next) { }
static inline void sched_mm_cid_migrate_from(struct task_struct *t) { }
static inline void sched_mm_cid_migrate_to(struct rq *dst_rq, struct task_struct *t) { }
static inline void task_tick_mm_cid(struct rq *rq, struct task_struct *curr) { }
static inline void init_sched_mm_cid(struct task_struct *t) { }
#endif

#ifdef CONFIG_SMP
extern struct balance_callback balance_push_callback;

static inline void
queue_balance_callback(struct rq *rq,
		       struct balance_callback *head,
		       void (*func)(struct rq *rq))
{
	lockdep_assert_rq_held(rq);

	/*
	 * Don't (re)queue an already queued item; nor queue anything when
	 * balance_push() is active, see the comment with
	 * balance_push_callback.
	 */
	if (unlikely(head->next || rq->balance_callback == &balance_push_callback))
		return;

	head->func = func;
	head->next = rq->balance_callback;
	rq->balance_callback = head;
}
#endif /* CONFIG_SMP */

#ifdef CONFIG_SCHED_BMQ
#include "bmq.h"
#endif
#ifdef CONFIG_SCHED_PDS
#include "pds.h"
#endif

#endif /* _KERNEL_SCHED_ALT_SCHED_H */
