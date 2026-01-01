/*
 *  kernel/sched/alt_core.c
 *
 *  Core alternative kernel scheduler code and related syscalls
 *
 *  Copyright (C) 1991-2002  Linus Torvalds
 *
 *  2009-08-13	Brainfuck deadline scheduling policy by Con Kolivas deletes
 *		a whole lot of those previous things.
 *  2017-09-06	Priority and Deadline based Skip list multiple queue kernel
 *		scheduler by Alfred Chen.
 *  2019-02-20	BMQ(BitMap Queue) kernel scheduler by Alfred Chen.
 */
#include <linux/sched/clock.h>
#include <linux/sched/cputime.h>
#include <linux/sched/debug.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/init.h>
#include <linux/sched/isolation.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/mm.h>
#include <linux/sched/nohz.h>
#include <linux/sched/stat.h>
#include <linux/sched/wake_q.h>

#include <linux/blkdev.h>
#include <linux/context_tracking.h>
#include <linux/cpuset.h>
#include <linux/delayacct.h>
#include <linux/init_task.h>
#include <linux/kcov.h>
#include <linux/kprobes.h>
#include <linux/nmi.h>
#include <linux/rseq.h>
#include <linux/scs.h>

#include <uapi/linux/sched/types.h>

#include <asm/irq_regs.h>
#include <asm/switch_to.h>

#define CREATE_TRACE_POINTS
#include <trace/events/sched.h>
#include <trace/events/ipi.h>
#undef CREATE_TRACE_POINTS

#include "sched.h"
#include "smp.h"

#include "pelt.h"

#include "../../io_uring/io-wq.h"
#include "../smpboot.h"

EXPORT_TRACEPOINT_SYMBOL_GPL(ipi_send_cpu);
EXPORT_TRACEPOINT_SYMBOL_GPL(ipi_send_cpumask);

/*
 * Export tracepoints that act as a bare tracehook (ie: have no trace event
 * associated with them) to allow external modules to probe them.
 */
EXPORT_TRACEPOINT_SYMBOL_GPL(pelt_irq_tp);

#ifdef CONFIG_SCHED_DEBUG
#define sched_feat(x)	(1)
/*
 * Print a warning if need_resched is set for the given duration (if
 * LATENCY_WARN is enabled).
 *
 * If sysctl_resched_latency_warn_once is set, only one warning will be shown
 * per boot.
 */
__read_mostly int sysctl_resched_latency_warn_ms = 100;
__read_mostly int sysctl_resched_latency_warn_once = 1;
#else
#define sched_feat(x)	(0)
#endif /* CONFIG_SCHED_DEBUG */

#define ALT_SCHED_VERSION "v6.12-r1"

#define STOP_PRIO		(MAX_RT_PRIO - 1)

/*
 * Time slice
 * (default: 4 msec, units: nanoseconds)
 */
unsigned int sysctl_sched_base_slice __read_mostly	= (4 << 20);

#include "alt_core.h"
#include "alt_topology.h"

/* Reschedule if less than this many μs left */
#define RESCHED_NS		(100 << 10)

/**
 * sched_yield_type - Type of sched_yield() will be performed.
 * 0: No yield.
 * 1: Requeue task. (default)
 */
int sched_yield_type __read_mostly = 1;

#ifdef CONFIG_SMP
cpumask_t sched_rq_pending_mask ____cacheline_aligned_in_smp;

DEFINE_PER_CPU_ALIGNED(cpumask_t [NR_CPU_AFFINITY_LEVELS], sched_cpu_topo_masks);
DEFINE_PER_CPU_ALIGNED(cpumask_t *, sched_cpu_llc_mask);
DEFINE_PER_CPU_ALIGNED(cpumask_t *, sched_cpu_topo_end_mask);

#ifdef CONFIG_SCHED_SMT
DEFINE_STATIC_KEY_FALSE(sched_smt_present);
EXPORT_SYMBOL_GPL(sched_smt_present);

cpumask_t sched_smt_mask ____cacheline_aligned_in_smp;
#endif

/*
 * Keep a unique ID per domain (we use the first CPUs number in the cpumask of
 * the domain), this allows us to quickly tell if two cpus are in the same cache
 * domain, see cpus_share_cache().
 */
DEFINE_PER_CPU(int, sd_llc_id);
#endif /* CONFIG_SMP */

DEFINE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);

#ifndef prepare_arch_switch
# define prepare_arch_switch(next)	do { } while (0)
#endif
#ifndef finish_arch_post_lock_switch
# define finish_arch_post_lock_switch()	do { } while (0)
#endif

static cpumask_t sched_preempt_mask[SCHED_QUEUE_BITS + 2] ____cacheline_aligned_in_smp;

cpumask_t *const sched_idle_mask = &sched_preempt_mask[SCHED_QUEUE_BITS - 1];
cpumask_t *const sched_sg_idle_mask = &sched_preempt_mask[SCHED_QUEUE_BITS];
cpumask_t *const sched_pcore_idle_mask = &sched_preempt_mask[SCHED_QUEUE_BITS];
cpumask_t *const sched_ecore_idle_mask = &sched_preempt_mask[SCHED_QUEUE_BITS + 1];

/* task function */
static inline const struct cpumask *task_user_cpus(struct task_struct *p)
{
	if (!p->user_cpus_ptr)
		return cpu_possible_mask; /* &init_task.cpus_mask */
	return p->user_cpus_ptr;
}

/* sched_queue related functions */
static inline void sched_queue_init(struct sched_queue *q)
{
	int i;

	bitmap_zero(q->bitmap, SCHED_QUEUE_BITS);
	for(i = 0; i < SCHED_LEVELS; i++)
		INIT_LIST_HEAD(&q->heads[i]);
}

/*
 * Init idle task and put into queue structure of rq
 * IMPORTANT: may be called multiple times for a single cpu
 */
static inline void sched_queue_init_idle(struct sched_queue *q,
					 struct task_struct *idle)
{
	INIT_LIST_HEAD(&q->heads[IDLE_TASK_SCHED_PRIO]);
	list_add_tail(&idle->sq_node, &q->heads[IDLE_TASK_SCHED_PRIO]);
	idle->on_rq = TASK_ON_RQ_QUEUED;
}

#define CLEAR_CACHED_PREEMPT_MASK(pr, low, high, cpu)		\
	if (low < pr && pr <= high)				\
		cpumask_clear_cpu(cpu, sched_preempt_mask + pr);

#define SET_CACHED_PREEMPT_MASK(pr, low, high, cpu)		\
	if (low < pr && pr <= high)				\
		cpumask_set_cpu(cpu, sched_preempt_mask + pr);

static atomic_t sched_prio_record = ATOMIC_INIT(0);

/* water mark related functions */
static inline void update_sched_preempt_mask(struct rq *rq)
{
	int prio = find_first_bit(rq->queue.bitmap, SCHED_QUEUE_BITS);
	int last_prio = rq->prio;
	int cpu, pr;

	if (prio == last_prio)
		return;

	rq->prio = prio;
#ifdef CONFIG_SCHED_PDS
	rq->prio_idx = sched_prio2idx(rq->prio, rq);
#endif
	cpu = cpu_of(rq);
	pr = atomic_read(&sched_prio_record);

	if (prio < last_prio) {
		if (IDLE_TASK_SCHED_PRIO == last_prio) {
			rq->clear_idle_mask_func(cpu, sched_idle_mask);
			last_prio -= 2;
		}
		CLEAR_CACHED_PREEMPT_MASK(pr, prio, last_prio, cpu);

		return;
	}
	/* last_prio < prio */
	if (IDLE_TASK_SCHED_PRIO == prio) {
		rq->set_idle_mask_func(cpu, sched_idle_mask);
		prio -= 2;
	}
	SET_CACHED_PREEMPT_MASK(pr, last_prio, prio, cpu);
}

/*
 * Serialization rules:
 *
 * Lock order:
 *
 *   p->pi_lock
 *     rq->lock
 *       hrtimer_cpu_base->lock (hrtimer_start() for bandwidth controls)
 *
 *  rq1->lock
 *    rq2->lock  where: rq1 < rq2
 *
 * Regular state:
 *
 * Normal scheduling state is serialized by rq->lock. __schedule() takes the
 * local CPU's rq->lock, it optionally removes the task from the runqueue and
 * always looks at the local rq data structures to find the most eligible task
 * to run next.
 *
 * Task enqueue is also under rq->lock, possibly taken from another CPU.
 * Wakeups from another LLC domain might use an IPI to transfer the enqueue to
 * the local CPU to avoid bouncing the runqueue state around [ see
 * ttwu_queue_wakelist() ]
 *
 * Task wakeup, specifically wakeups that involve migration, are horribly
 * complicated to avoid having to take two rq->locks.
 *
 * Special state:
 *
 * System-calls and anything external will use task_rq_lock() which acquires
 * both p->pi_lock and rq->lock. As a consequence the state they change is
 * stable while holding either lock:
 *
 *  - sched_setaffinity()/
 *    set_cpus_allowed_ptr():	p->cpus_ptr, p->nr_cpus_allowed
 *  - set_user_nice():		p->se.load, p->*prio
 *  - __sched_setscheduler():	p->sched_class, p->policy, p->*prio,
 *				p->se.load, p->rt_priority,
 *				p->dl.dl_{runtime, deadline, period, flags, bw, density}
 *  - sched_setnuma():		p->numa_preferred_nid
 *  - sched_move_task():        p->sched_task_group
 *  - uclamp_update_active()	p->uclamp*
 *
 * p->state <- TASK_*:
 *
 *   is changed locklessly using set_current_state(), __set_current_state() or
 *   set_special_state(), see their respective comments, or by
 *   try_to_wake_up(). This latter uses p->pi_lock to serialize against
 *   concurrent self.
 *
 * p->on_rq <- { 0, 1 = TASK_ON_RQ_QUEUED, 2 = TASK_ON_RQ_MIGRATING }:
 *
 *   is set by activate_task() and cleared by deactivate_task(), under
 *   rq->lock. Non-zero indicates the task is runnable, the special
 *   ON_RQ_MIGRATING state is used for migration without holding both
 *   rq->locks. It indicates task_cpu() is not stable, see task_rq_lock().
 *
 *   Additionally it is possible to be ->on_rq but still be considered not
 *   runnable when p->se.sched_delayed is true. These tasks are on the runqueue
 *   but will be dequeued as soon as they get picked again. See the
 *   task_is_runnable() helper.
 *
 * p->on_cpu <- { 0, 1 }:
 *
 *   is set by prepare_task() and cleared by finish_task() such that it will be
 *   set before p is scheduled-in and cleared after p is scheduled-out, both
 *   under rq->lock. Non-zero indicates the task is running on its CPU.
 *
 *   [ The astute reader will observe that it is possible for two tasks on one
 *     CPU to have ->on_cpu = 1 at the same time. ]
 *
 * task_cpu(p): is changed by set_task_cpu(), the rules are:
 *
 *  - Don't call set_task_cpu() on a blocked task:
 *
 *    We don't care what CPU we're not running on, this simplifies hotplug,
 *    the CPU assignment of blocked tasks isn't required to be valid.
 *
 *  - for try_to_wake_up(), called under p->pi_lock:
 *
 *    This allows try_to_wake_up() to only take one rq->lock, see its comment.
 *
 *  - for migration called under rq->lock:
 *    [ see task_on_rq_migrating() in task_rq_lock() ]
 *
 *    o move_queued_task()
 *    o detach_task()
 *
 *  - for migration called under double_rq_lock():
 *
 *    o __migrate_swap_task()
 *    o push_rt_task() / pull_rt_task()
 *    o push_dl_task() / pull_dl_task()
 *    o dl_task_offline_migration()
 *
 */

/*
 * Context: p->pi_lock
 */
static inline struct rq *
task_access_lock_irqsave(struct task_struct *p, raw_spinlock_t **plock, unsigned long *flags)
{
	struct rq *rq;
	for (;;) {
		rq = task_rq(p);
		if (p->on_cpu || task_on_rq_queued(p)) {
			raw_spin_lock_irqsave(&rq->lock, *flags);
			if (likely((p->on_cpu || task_on_rq_queued(p)) && rq == task_rq(p))) {
				*plock = &rq->lock;
				return rq;
			}
			raw_spin_unlock_irqrestore(&rq->lock, *flags);
		} else if (task_on_rq_migrating(p)) {
			do {
				cpu_relax();
			} while (unlikely(task_on_rq_migrating(p)));
		} else {
			raw_spin_lock_irqsave(&p->pi_lock, *flags);
			if (likely(!p->on_cpu && !p->on_rq && rq == task_rq(p))) {
				*plock = &p->pi_lock;
				return rq;
			}
			raw_spin_unlock_irqrestore(&p->pi_lock, *flags);
		}
	}
}

static inline void
task_access_unlock_irqrestore(struct task_struct *p, raw_spinlock_t *lock, unsigned long *flags)
{
	raw_spin_unlock_irqrestore(lock, *flags);
}

/*
 * __task_rq_lock - lock the rq @p resides on.
 */
struct rq *__task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(rq->lock)
{
	struct rq *rq;

	lockdep_assert_held(&p->pi_lock);

	for (;;) {
		rq = task_rq(p);
		raw_spin_lock(&rq->lock);
		if (likely(rq == task_rq(p) && !task_on_rq_migrating(p)))
			return rq;
		raw_spin_unlock(&rq->lock);

		while (unlikely(task_on_rq_migrating(p)))
			cpu_relax();
	}
}

/*
 * task_rq_lock - lock p->pi_lock and lock the rq @p resides on.
 */
struct rq *task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(p->pi_lock)
	__acquires(rq->lock)
{
	struct rq *rq;

	for (;;) {
		raw_spin_lock_irqsave(&p->pi_lock, rf->flags);
		rq = task_rq(p);
		raw_spin_lock(&rq->lock);
		/*
		 *	move_queued_task()		task_rq_lock()
		 *
		 *	ACQUIRE (rq->lock)
		 *	[S] ->on_rq = MIGRATING		[L] rq = task_rq()
		 *	WMB (__set_task_cpu())		ACQUIRE (rq->lock);
		 *	[S] ->cpu = new_cpu		[L] task_rq()
		 *					[L] ->on_rq
		 *	RELEASE (rq->lock)
		 *
		 * If we observe the old CPU in task_rq_lock(), the acquire of
		 * the old rq->lock will fully serialize against the stores.
		 *
		 * If we observe the new CPU in task_rq_lock(), the address
		 * dependency headed by '[L] rq = task_rq()' and the acquire
		 * will pair with the WMB to ensure we then also see migrating.
		 */
		if (likely(rq == task_rq(p) && !task_on_rq_migrating(p))) {
			return rq;
		}
		raw_spin_unlock(&rq->lock);
		raw_spin_unlock_irqrestore(&p->pi_lock, rf->flags);

		while (unlikely(task_on_rq_migrating(p)))
			cpu_relax();
	}
}

static inline void rq_lock_irqsave(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_lock_irqsave(&rq->lock, rf->flags);
}

static inline void rq_unlock_irqrestore(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	raw_spin_unlock_irqrestore(&rq->lock, rf->flags);
}

DEFINE_LOCK_GUARD_1(rq_lock_irqsave, struct rq,
		    rq_lock_irqsave(_T->lock, &_T->rf),
		    rq_unlock_irqrestore(_T->lock, &_T->rf),
		    struct rq_flags rf)

void raw_spin_rq_lock_nested(struct rq *rq, int subclass)
{
	raw_spinlock_t *lock;

	/* Matches synchronize_rcu() in __sched_core_enable() */
	preempt_disable();

	for (;;) {
		lock = __rq_lockp(rq);
		raw_spin_lock_nested(lock, subclass);
		if (likely(lock == __rq_lockp(rq))) {
			/* preempt_count *MUST* be > 1 */
			preempt_enable_no_resched();
			return;
		}
		raw_spin_unlock(lock);
	}
}

void raw_spin_rq_unlock(struct rq *rq)
{
	raw_spin_unlock(rq_lockp(rq));
}

/*
 * RQ-clock updating methods:
 */

static void update_rq_clock_task(struct rq *rq, s64 delta)
{
/*
 * In theory, the compile should just see 0 here, and optimize out the call
 * to sched_rt_avg_update. But I don't trust it...
 */
	s64 __maybe_unused steal = 0, irq_delta = 0;

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	irq_delta = irq_time_read(cpu_of(rq)) - rq->prev_irq_time;

	/*
	 * Since irq_time is only updated on {soft,}irq_exit, we might run into
	 * this case when a previous update_rq_clock() happened inside a
	 * {soft,}IRQ region.
	 *
	 * When this happens, we stop ->clock_task and only update the
	 * prev_irq_time stamp to account for the part that fit, so that a next
	 * update will consume the rest. This ensures ->clock_task is
	 * monotonic.
	 *
	 * It does however cause some slight miss-attribution of {soft,}IRQ
	 * time, a more accurate solution would be to update the irq_time using
	 * the current rq->clock timestamp, except that would require using
	 * atomic ops.
	 */
	if (irq_delta > delta)
		irq_delta = delta;

	rq->prev_irq_time += irq_delta;
	delta -= irq_delta;
	delayacct_irq(rq->curr, irq_delta);
#endif
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	if (static_key_false((&paravirt_steal_rq_enabled))) {
		steal = paravirt_steal_clock(cpu_of(rq));
		steal -= rq->prev_steal_time_rq;

		if (unlikely(steal > delta))
			steal = delta;

		rq->prev_steal_time_rq += steal;
		delta -= steal;
	}
#endif

	rq->clock_task += delta;

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	if ((irq_delta + steal))
		update_irq_load_avg(rq, irq_delta + steal);
#endif
}

static inline void update_rq_clock(struct rq *rq)
{
	s64 delta = sched_clock_cpu(cpu_of(rq)) - rq->clock;

	if (unlikely(delta <= 0))
		return;
	rq->clock += delta;
	sched_update_rq_clock(rq);
	update_rq_clock_task(rq, delta);
}

/*
 * RQ Load update routine
 */
#define RQ_LOAD_HISTORY_BITS		(sizeof(s32) * 8ULL)
#define RQ_UTIL_SHIFT			(8)
#define RQ_LOAD_HISTORY_TO_UTIL(l)	(((l) >> (RQ_LOAD_HISTORY_BITS - 1 - RQ_UTIL_SHIFT)) & 0xff)

#define LOAD_BLOCK(t)		((t) >> 17)
#define LOAD_HALF_BLOCK(t)	((t) >> 16)
#define BLOCK_MASK(t)		((t) & ((0x01 << 18) - 1))
#define LOAD_BLOCK_BIT(b)	(1UL << (RQ_LOAD_HISTORY_BITS - 1 - (b)))
#define CURRENT_LOAD_BIT	LOAD_BLOCK_BIT(0)

static inline void rq_load_update(struct rq *rq)
{
	u64 time = rq->clock;
	u64 delta = min(LOAD_BLOCK(time) - LOAD_BLOCK(rq->load_stamp), RQ_LOAD_HISTORY_BITS - 1);
	u64 prev = !!(rq->load_history & CURRENT_LOAD_BIT);
	u64 curr = !!rq->nr_running;

	if (delta) {
		rq->load_history = rq->load_history >> delta;

		if (delta < RQ_UTIL_SHIFT) {
			rq->load_block += (~BLOCK_MASK(rq->load_stamp)) * prev;
			if (!!LOAD_HALF_BLOCK(rq->load_block) ^ curr)
				rq->load_history ^= LOAD_BLOCK_BIT(delta);
		}

		rq->load_block = BLOCK_MASK(time) * prev;
	} else {
		rq->load_block += (time - rq->load_stamp) * prev;
	}
	if (prev ^ curr)
		rq->load_history ^= CURRENT_LOAD_BIT;
	rq->load_stamp = time;
}

unsigned long rq_load_util(struct rq *rq, unsigned long max)
{
	return RQ_LOAD_HISTORY_TO_UTIL(rq->load_history) * (max >> RQ_UTIL_SHIFT);
}

#ifdef CONFIG_SMP
unsigned long sched_cpu_util(int cpu)
{
	return rq_load_util(cpu_rq(cpu), arch_scale_cpu_capacity(cpu));
}
#endif /* CONFIG_SMP */

#ifdef CONFIG_CPU_FREQ
/**
 * cpufreq_update_util - Take a note about CPU utilization changes.
 * @rq: Runqueue to carry out the update for.
 * @flags: Update reason flags.
 *
 * This function is called by the scheduler on the CPU whose utilization is
 * being updated.
 *
 * It can only be called from RCU-sched read-side critical sections.
 *
 * The way cpufreq is currently arranged requires it to evaluate the CPU
 * performance state (frequency/voltage) on a regular basis to prevent it from
 * being stuck in a completely inadequate performance level for too long.
 * That is not guaranteed to happen if the updates are only triggered from CFS
 * and DL, though, because they may not be coming in if only RT tasks are
 * active all the time (or there are RT tasks only).
 *
 * As a workaround for that issue, this function is called periodically by the
 * RT sched class to trigger extra cpufreq updates to prevent it from stalling,
 * but that really is a band-aid.  Going forward it should be replaced with
 * solutions targeted more specifically at RT tasks.
 */
static inline void cpufreq_update_util(struct rq *rq, unsigned int flags)
{
	struct update_util_data *data;

#ifdef CONFIG_SMP
	rq_load_update(rq);
#endif
	data = rcu_dereference_sched(*per_cpu_ptr(&cpufreq_update_util_data, cpu_of(rq)));
	if (data)
		data->func(data, rq_clock(rq), flags);
}
#else
static inline void cpufreq_update_util(struct rq *rq, unsigned int flags)
{
#ifdef CONFIG_SMP
	rq_load_update(rq);
#endif
}
#endif /* CONFIG_CPU_FREQ */

#ifdef CONFIG_NO_HZ_FULL
/*
 * Tick may be needed by tasks in the runqueue depending on their policy and
 * requirements. If tick is needed, lets send the target an IPI to kick it out
 * of nohz mode if necessary.
 */
static inline void sched_update_tick_dependency(struct rq *rq)
{
	int cpu = cpu_of(rq);

	if (!tick_nohz_full_cpu(cpu))
		return;

	if (rq->nr_running < 2)
		tick_nohz_dep_clear_cpu(cpu, TICK_DEP_BIT_SCHED);
	else
		tick_nohz_dep_set_cpu(cpu, TICK_DEP_BIT_SCHED);
}
#else /* !CONFIG_NO_HZ_FULL */
static inline void sched_update_tick_dependency(struct rq *rq) { }
#endif

bool sched_task_on_rq(struct task_struct *p)
{
	return task_on_rq_queued(p);
}

unsigned long get_wchan(struct task_struct *p)
{
	unsigned long ip = 0;
	unsigned int state;

	if (!p || p == current)
		return 0;

	/* Only get wchan if task is blocked and we can keep it that way. */
	raw_spin_lock_irq(&p->pi_lock);
	state = READ_ONCE(p->__state);
	smp_rmb(); /* see try_to_wake_up() */
	if (state != TASK_RUNNING && state != TASK_WAKING && !p->on_rq)
		ip = __get_wchan(p);
	raw_spin_unlock_irq(&p->pi_lock);

	return ip;
}

/*
 * Add/Remove/Requeue task to/from the runqueue routines
 * Context: rq->lock
 */
#define __SCHED_DEQUEUE_TASK(p, rq, flags, func)					\
	sched_info_dequeue(rq, p);							\
											\
	__list_del_entry(&p->sq_node);							\
	if (p->sq_node.prev == p->sq_node.next) {					\
		clear_bit(sched_idx2prio(p->sq_node.next - &rq->queue.heads[0], rq),	\
			  rq->queue.bitmap);						\
		func;									\
	}

#define __SCHED_ENQUEUE_TASK(p, rq, flags, func)					\
	sched_info_enqueue(rq, p);							\
	{										\
	int idx, prio;									\
	TASK_SCHED_PRIO_IDX(p, rq, idx, prio);						\
	list_add_tail(&p->sq_node, &rq->queue.heads[idx]);				\
	if (list_is_first(&p->sq_node, &rq->queue.heads[idx])) {			\
		set_bit(prio, rq->queue.bitmap);					\
		func;									\
	}										\
	}

static inline void dequeue_task(struct task_struct *p, struct rq *rq, int flags)
{
#ifdef ALT_SCHED_DEBUG
	lockdep_assert_held(&rq->lock);

	/*printk(KERN_INFO "sched: dequeue(%d) %px %016llx\n", cpu_of(rq), p, p->deadline);*/
	WARN_ONCE(task_rq(p) != rq, "sched: dequeue task reside on cpu%d from cpu%d\n",
		  task_cpu(p), cpu_of(rq));
#endif

	__SCHED_DEQUEUE_TASK(p, rq, flags, update_sched_preempt_mask(rq));
	--rq->nr_running;
#ifdef CONFIG_SMP
	if (1 == rq->nr_running)
		cpumask_clear_cpu(cpu_of(rq), &sched_rq_pending_mask);
#endif

	sched_update_tick_dependency(rq);
}

static inline void enqueue_task(struct task_struct *p, struct rq *rq, int flags)
{
#ifdef ALT_SCHED_DEBUG
	lockdep_assert_held(&rq->lock);

	/*printk(KERN_INFO "sched: enqueue(%d) %px %d\n", cpu_of(rq), p, p->prio);*/
	WARN_ONCE(task_rq(p) != rq, "sched: enqueue task reside on cpu%d to cpu%d\n",
		  task_cpu(p), cpu_of(rq));
#endif

	__SCHED_ENQUEUE_TASK(p, rq, flags, update_sched_preempt_mask(rq));
	++rq->nr_running;
#ifdef CONFIG_SMP
	if (2 == rq->nr_running)
		cpumask_set_cpu(cpu_of(rq), &sched_rq_pending_mask);
#endif

	sched_update_tick_dependency(rq);
}

void requeue_task(struct task_struct *p, struct rq *rq)
{
	struct list_head *node = &p->sq_node;
	int deq_idx, idx, prio;

	TASK_SCHED_PRIO_IDX(p, rq, idx, prio);
#ifdef ALT_SCHED_DEBUG
	lockdep_assert_held(&rq->lock);
	/*printk(KERN_INFO "sched: requeue(%d) %px %016llx\n", cpu_of(rq), p, p->deadline);*/
	WARN_ONCE(task_rq(p) != rq, "sched: cpu[%d] requeue task reside on cpu%d\n",
		  cpu_of(rq), task_cpu(p));
#endif
	if (list_is_last(node, &rq->queue.heads[idx]))
		return;

	__list_del_entry(node);
	if (node->prev == node->next && (deq_idx = node->next - &rq->queue.heads[0]) != idx)
		clear_bit(sched_idx2prio(deq_idx, rq), rq->queue.bitmap);

	list_add_tail(node, &rq->queue.heads[idx]);
	if (list_is_first(node, &rq->queue.heads[idx]))
		set_bit(prio, rq->queue.bitmap);
	update_sched_preempt_mask(rq);
}

/*
 * try_cmpxchg based fetch_or() macro so it works for different integer types:
 */
#define fetch_or(ptr, mask)						\
	({								\
		typeof(ptr) _ptr = (ptr);				\
		typeof(mask) _mask = (mask);				\
		typeof(*_ptr) _val = *_ptr;				\
									\
		do {							\
		} while (!try_cmpxchg(_ptr, &_val, _val | _mask));	\
	_val;								\
})

#if defined(CONFIG_SMP) && defined(TIF_POLLING_NRFLAG)
/*
 * Atomically set TIF_NEED_RESCHED and test for TIF_POLLING_NRFLAG,
 * this avoids any races wrt polling state changes and thereby avoids
 * spurious IPIs.
 */
static inline bool set_nr_and_not_polling(struct task_struct *p)
{
	struct thread_info *ti = task_thread_info(p);
	return !(fetch_or(&ti->flags, _TIF_NEED_RESCHED) & _TIF_POLLING_NRFLAG);
}

/*
 * Atomically set TIF_NEED_RESCHED if TIF_POLLING_NRFLAG is set.
 *
 * If this returns true, then the idle task promises to call
 * sched_ttwu_pending() and reschedule soon.
 */
static bool set_nr_if_polling(struct task_struct *p)
{
	struct thread_info *ti = task_thread_info(p);
	typeof(ti->flags) val = READ_ONCE(ti->flags);

	do {
		if (!(val & _TIF_POLLING_NRFLAG))
			return false;
		if (val & _TIF_NEED_RESCHED)
			return true;
	} while (!try_cmpxchg(&ti->flags, &val, val | _TIF_NEED_RESCHED));

	return true;
}

#else
static inline bool set_nr_and_not_polling(struct task_struct *p)
{
	set_tsk_need_resched(p);
	return true;
}

#ifdef CONFIG_SMP
static inline bool set_nr_if_polling(struct task_struct *p)
{
	return false;
}
#endif
#endif

static bool __wake_q_add(struct wake_q_head *head, struct task_struct *task)
{
	struct wake_q_node *node = &task->wake_q;

	/*
	 * Atomically grab the task, if ->wake_q is !nil already it means
	 * it's already queued (either by us or someone else) and will get the
	 * wakeup due to that.
	 *
	 * In order to ensure that a pending wakeup will observe our pending
	 * state, even in the failed case, an explicit smp_mb() must be used.
	 */
	smp_mb__before_atomic();
	if (unlikely(cmpxchg_relaxed(&node->next, NULL, WAKE_Q_TAIL)))
		return false;

	/*
	 * The head is context local, there can be no concurrency.
	 */
	*head->lastp = node;
	head->lastp = &node->next;
	return true;
}

/**
 * wake_q_add() - queue a wakeup for 'later' waking.
 * @head: the wake_q_head to add @task to
 * @task: the task to queue for 'later' wakeup
 *
 * Queue a task for later wakeup, most likely by the wake_up_q() call in the
 * same context, _HOWEVER_ this is not guaranteed, the wakeup can come
 * instantly.
 *
 * This function must be used as-if it were wake_up_process(); IOW the task
 * must be ready to be woken at this location.
 */
void wake_q_add(struct wake_q_head *head, struct task_struct *task)
{
	if (__wake_q_add(head, task))
		get_task_struct(task);
}

/**
 * wake_q_add_safe() - safely queue a wakeup for 'later' waking.
 * @head: the wake_q_head to add @task to
 * @task: the task to queue for 'later' wakeup
 *
 * Queue a task for later wakeup, most likely by the wake_up_q() call in the
 * same context, _HOWEVER_ this is not guaranteed, the wakeup can come
 * instantly.
 *
 * This function must be used as-if it were wake_up_process(); IOW the task
 * must be ready to be woken at this location.
 *
 * This function is essentially a task-safe equivalent to wake_q_add(). Callers
 * that already hold reference to @task can call the 'safe' version and trust
 * wake_q to do the right thing depending whether or not the @task is already
 * queued for wakeup.
 */
void wake_q_add_safe(struct wake_q_head *head, struct task_struct *task)
{
	if (!__wake_q_add(head, task))
		put_task_struct(task);
}

void wake_up_q(struct wake_q_head *head)
{
	struct wake_q_node *node = head->first;

	while (node != WAKE_Q_TAIL) {
		struct task_struct *task;

		task = container_of(node, struct task_struct, wake_q);
		/* task can safely be re-inserted now: */
		node = node->next;
		task->wake_q.next = NULL;

		/*
		 * wake_up_process() executes a full barrier, which pairs with
		 * the queueing in wake_q_add() so as not to miss wakeups.
		 */
		wake_up_process(task);
		put_task_struct(task);
	}
}

/*
 * resched_curr - mark rq's current task 'to be rescheduled now'.
 *
 * On UP this means the setting of the need_resched flag, on SMP it
 * might also involve a cross-CPU call to trigger the scheduler on
 * the target CPU.
 */
static inline void resched_curr(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	int cpu;

	lockdep_assert_held(&rq->lock);

	if (test_tsk_need_resched(curr))
		return;

	cpu = cpu_of(rq);
	if (cpu == smp_processor_id()) {
		set_tsk_need_resched(curr);
		set_preempt_need_resched();
		return;
	}

	if (set_nr_and_not_polling(curr))
		smp_send_reschedule(cpu);
	else
		trace_sched_wake_idle_without_ipi(cpu);
}

void resched_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	if (cpu_online(cpu) || cpu == smp_processor_id())
		resched_curr(cpu_rq(cpu));
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

#ifdef CONFIG_SMP
#ifdef CONFIG_NO_HZ_COMMON
/*
 * This routine will record that the CPU is going idle with tick stopped.
 * This info will be used in performing idle load balancing in the future.
 */
void nohz_balance_enter_idle(int cpu) {}

/*
 * In the semi idle case, use the nearest busy CPU for migrating timers
 * from an idle CPU.  This is good for power-savings.
 *
 * We don't do similar optimization for completely idle system, as
 * selecting an idle CPU will add more delays to the timers than intended
 * (as that CPU's timer base may not be up to date wrt jiffies etc).
 */
int get_nohz_timer_target(void)
{
	int i, cpu = smp_processor_id(), default_cpu = -1;
	struct cpumask *mask;
	const struct cpumask *hk_mask;

	if (housekeeping_cpu(cpu, HK_TYPE_TIMER)) {
		if (!idle_cpu(cpu))
			return cpu;
		default_cpu = cpu;
	}

	hk_mask = housekeeping_cpumask(HK_TYPE_TIMER);

	for (mask = per_cpu(sched_cpu_topo_masks, cpu);
	     mask < per_cpu(sched_cpu_topo_end_mask, cpu); mask++)
		for_each_cpu_and(i, mask, hk_mask)
			if (!idle_cpu(i))
				return i;

	if (default_cpu == -1)
		default_cpu = housekeeping_any_cpu(HK_TYPE_TIMER);
	cpu = default_cpu;

	return cpu;
}

/*
 * When add_timer_on() enqueues a timer into the timer wheel of an
 * idle CPU then this timer might expire before the next timer event
 * which is scheduled to wake up that CPU. In case of a completely
 * idle system the next event might even be infinite time into the
 * future. wake_up_idle_cpu() ensures that the CPU is woken up and
 * leaves the inner idle loop so the newly added timer is taken into
 * account when the CPU goes back to idle and evaluates the timer
 * wheel for the next timer event.
 */
static inline void wake_up_idle_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (cpu == smp_processor_id())
		return;

	/*
	 * Set TIF_NEED_RESCHED and send an IPI if in the non-polling
	 * part of the idle loop. This forces an exit from the idle loop
	 * and a round trip to schedule(). Now this could be optimized
	 * because a simple new idle loop iteration is enough to
	 * re-evaluate the next tick. Provided some re-ordering of tick
	 * nohz functions that would need to follow TIF_NR_POLLING
	 * clearing:
	 *
	 * - On most architectures, a simple fetch_or on ti::flags with a
	 *   "0" value would be enough to know if an IPI needs to be sent.
	 *
	 * - x86 needs to perform a last need_resched() check between
	 *   monitor and mwait which doesn't take timers into account.
	 *   There a dedicated TIF_TIMER flag would be required to
	 *   fetch_or here and be checked along with TIF_NEED_RESCHED
	 *   before mwait().
	 *
	 * However, remote timer enqueue is not such a frequent event
	 * and testing of the above solutions didn't appear to report
	 * much benefits.
	 */
	if (set_nr_and_not_polling(rq->idle))
		smp_send_reschedule(cpu);
	else
		trace_sched_wake_idle_without_ipi(cpu);
}

static inline bool wake_up_full_nohz_cpu(int cpu)
{
	/*
	 * We just need the target to call irq_exit() and re-evaluate
	 * the next tick. The nohz full kick at least implies that.
	 * If needed we can still optimize that later with an
	 * empty IRQ.
	 */
	if (cpu_is_offline(cpu))
		return true;  /* Don't try to wake offline CPUs. */
	if (tick_nohz_full_cpu(cpu)) {
		if (cpu != smp_processor_id() ||
		    tick_nohz_tick_stopped())
			tick_nohz_full_kick_cpu(cpu);
		return true;
	}

	return false;
}

void wake_up_nohz_cpu(int cpu)
{
	if (!wake_up_full_nohz_cpu(cpu))
		wake_up_idle_cpu(cpu);
}

static void nohz_csd_func(void *info)
{
	struct rq *rq = info;
	int cpu = cpu_of(rq);
	unsigned int flags;

	/*
	 * Release the rq::nohz_csd.
	 */
	flags = atomic_fetch_andnot(NOHZ_KICK_MASK, nohz_flags(cpu));
	WARN_ON(!(flags & NOHZ_KICK_MASK));

	rq->idle_balance = idle_cpu(cpu);
	if (rq->idle_balance && !need_resched()) {
		rq->nohz_idle_balance = flags;
		raise_softirq_irqoff(SCHED_SOFTIRQ);
	}
}

#endif /* CONFIG_NO_HZ_COMMON */
#endif /* CONFIG_SMP */

static inline void wakeup_preempt(struct rq *rq)
{
	if (sched_rq_first_task(rq) != rq->curr)
		resched_curr(rq);
}

static __always_inline
int __task_state_match(struct task_struct *p, unsigned int state)
{
	if (READ_ONCE(p->__state) & state)
		return 1;

	if (READ_ONCE(p->saved_state) & state)
		return -1;

	return 0;
}

static __always_inline
int task_state_match(struct task_struct *p, unsigned int state)
{
	/*
	 * Serialize against current_save_and_set_rtlock_wait_state(),
	 * current_restore_rtlock_saved_state(), and __refrigerator().
	 */
	guard(raw_spinlock_irq)(&p->pi_lock);

	return __task_state_match(p, state);
}

/*
 * wait_task_inactive - wait for a thread to unschedule.
 *
 * Wait for the thread to block in any of the states set in @match_state.
 * If it changes, i.e. @p might have woken up, then return zero.  When we
 * succeed in waiting for @p to be off its CPU, we return a positive number
 * (its total switch count).  If a second call a short while later returns the
 * same number, the caller can be sure that @p has remained unscheduled the
 * whole time.
 *
 * The caller must ensure that the task *will* unschedule sometime soon,
 * else this function might spin for a *long* time. This function can't
 * be called with interrupts off, or it may introduce deadlock with
 * smp_call_function() if an IPI is sent by the same process we are
 * waiting to become inactive.
 */
unsigned long wait_task_inactive(struct task_struct *p, unsigned int match_state)
{
	unsigned long flags;
	int running, queued, match;
	unsigned long ncsw;
	struct rq *rq;
	raw_spinlock_t *lock;

	for (;;) {
		rq = task_rq(p);

		/*
		 * If the task is actively running on another CPU
		 * still, just relax and busy-wait without holding
		 * any locks.
		 *
		 * NOTE! Since we don't hold any locks, it's not
		 * even sure that "rq" stays as the right runqueue!
		 * But we don't care, since this will return false
		 * if the runqueue has changed and p is actually now
		 * running somewhere else!
		 */
		while (task_on_cpu(p)) {
			if (!task_state_match(p, match_state))
				return 0;
			cpu_relax();
		}

		/*
		 * Ok, time to look more closely! We need the rq
		 * lock now, to be *sure*. If we're wrong, we'll
		 * just go back and repeat.
		 */
		task_access_lock_irqsave(p, &lock, &flags);
		trace_sched_wait_task(p);
		running = task_on_cpu(p);
		queued = p->on_rq;
		ncsw = 0;
		if ((match = __task_state_match(p, match_state))) {
			/*
			 * When matching on p->saved_state, consider this task
			 * still queued so it will wait.
			 */
			if (match < 0)
				queued = 1;
			ncsw = p->nvcsw | LONG_MIN; /* sets MSB */
		}
		task_access_unlock_irqrestore(p, lock, &flags);

		/*
		 * If it changed from the expected state, bail out now.
		 */
		if (unlikely(!ncsw))
			break;

		/*
		 * Was it really running after all now that we
		 * checked with the proper locks actually held?
		 *
		 * Oops. Go back and try again..
		 */
		if (unlikely(running)) {
			cpu_relax();
			continue;
		}

		/*
		 * It's not enough that it's not actively running,
		 * it must be off the runqueue _entirely_, and not
		 * preempted!
		 *
		 * So if it was still runnable (but just not actively
		 * running right now), it's preempted, and we should
		 * yield - it could be a while.
		 */
		if (unlikely(queued)) {
			ktime_t to = NSEC_PER_SEC / HZ;

			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_hrtimeout(&to, HRTIMER_MODE_REL_HARD);
			continue;
		}

		/*
		 * Ahh, all good. It wasn't running, and it wasn't
		 * runnable, which means that it will never become
		 * running in the future either. We're all done!
		 */
		break;
	}

	return ncsw;
}

#ifdef CONFIG_SCHED_HRTICK
/*
 * Use HR-timers to deliver accurate preemption points.
 */

static void hrtick_clear(struct rq *rq)
{
	if (hrtimer_active(&rq->hrtick_timer))
		hrtimer_cancel(&rq->hrtick_timer);
}

/*
 * High-resolution timer tick.
 * Runs from hardirq context with interrupts disabled.
 */
static enum hrtimer_restart hrtick(struct hrtimer *timer)
{
	struct rq *rq = container_of(timer, struct rq, hrtick_timer);

	WARN_ON_ONCE(cpu_of(rq) != smp_processor_id());

	raw_spin_lock(&rq->lock);
	resched_curr(rq);
	raw_spin_unlock(&rq->lock);

	return HRTIMER_NORESTART;
}

/*
 * Use hrtick when:
 *  - enabled by features
 *  - hrtimer is actually high res
 */
static inline int hrtick_enabled(struct rq *rq)
{
	/**
	 * Alt schedule FW doesn't support sched_feat yet
	if (!sched_feat(HRTICK))
		return 0;
	*/
	if (!cpu_active(cpu_of(rq)))
		return 0;
	return hrtimer_is_hres_active(&rq->hrtick_timer);
}

#ifdef CONFIG_SMP

static void __hrtick_restart(struct rq *rq)
{
	struct hrtimer *timer = &rq->hrtick_timer;
	ktime_t time = rq->hrtick_time;

	hrtimer_start(timer, time, HRTIMER_MODE_ABS_PINNED_HARD);
}

/*
 * called from hardirq (IPI) context
 */
static void __hrtick_start(void *arg)
{
	struct rq *rq = arg;

	raw_spin_lock(&rq->lock);
	__hrtick_restart(rq);
	raw_spin_unlock(&rq->lock);
}

/*
 * Called to set the hrtick timer state.
 *
 * called with rq->lock held and IRQs disabled
 */
static inline void hrtick_start(struct rq *rq, u64 delay)
{
	struct hrtimer *timer = &rq->hrtick_timer;
	s64 delta;

	/*
	 * Don't schedule slices shorter than 10000ns, that just
	 * doesn't make sense and can cause timer DoS.
	 */
	delta = max_t(s64, delay, 10000LL);

	rq->hrtick_time = ktime_add_ns(timer->base->get_time(), delta);

	if (rq == this_rq())
		__hrtick_restart(rq);
	else
		smp_call_function_single_async(cpu_of(rq), &rq->hrtick_csd);
}

#else
/*
 * Called to set the hrtick timer state.
 *
 * called with rq->lock held and IRQs disabled
 */
static inline void hrtick_start(struct rq *rq, u64 delay)
{
	/*
	 * Don't schedule slices shorter than 10000ns, that just
	 * doesn't make sense. Rely on vruntime for fairness.
	 */
	delay = max_t(u64, delay, 10000LL);
	hrtimer_start(&rq->hrtick_timer, ns_to_ktime(delay),
		      HRTIMER_MODE_REL_PINNED_HARD);
}
#endif /* CONFIG_SMP */

static void hrtick_rq_init(struct rq *rq)
{
#ifdef CONFIG_SMP
	INIT_CSD(&rq->hrtick_csd, __hrtick_start, rq);
#endif

	hrtimer_init(&rq->hrtick_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	rq->hrtick_timer.function = hrtick;
}
#else	/* CONFIG_SCHED_HRTICK */
static inline int hrtick_enabled(struct rq *rq)
{
	return 0;
}

static inline void hrtick_clear(struct rq *rq)
{
}

static inline void hrtick_rq_init(struct rq *rq)
{
}
#endif	/* CONFIG_SCHED_HRTICK */

/*
 * activate_task - move a task to the runqueue.
 *
 * Context: rq->lock
 */
static void activate_task(struct task_struct *p, struct rq *rq)
{
	enqueue_task(p, rq, ENQUEUE_WAKEUP);

	WRITE_ONCE(p->on_rq, TASK_ON_RQ_QUEUED);
	ASSERT_EXCLUSIVE_WRITER(p->on_rq);

	/*
	 * If in_iowait is set, the code below may not trigger any cpufreq
	 * utilization updates, so do it here explicitly with the IOWAIT flag
	 * passed.
	 */
	cpufreq_update_util(rq, SCHED_CPUFREQ_IOWAIT * p->in_iowait);
}

static void block_task(struct rq *rq, struct task_struct *p)
{
	dequeue_task(p, rq, DEQUEUE_SLEEP);

	WRITE_ONCE(p->on_rq, 0);
	ASSERT_EXCLUSIVE_WRITER(p->on_rq);
	if (p->sched_contributes_to_load)
		rq->nr_uninterruptible++;

	if (p->in_iowait) {
		atomic_inc(&rq->nr_iowait);
		delayacct_blkio_start();
	}
}

static inline void __set_task_cpu(struct task_struct *p, unsigned int cpu)
{
#ifdef CONFIG_SMP
	/*
	 * After ->cpu is set up to a new value, task_access_lock(p, ...) can be
	 * successfully executed on another CPU. We must ensure that updates of
	 * per-task data have been completed by this moment.
	 */
	smp_wmb();

	WRITE_ONCE(task_thread_info(p)->cpu, cpu);
#endif
}

#ifdef CONFIG_SMP

void set_task_cpu(struct task_struct *p, unsigned int new_cpu)
{
#ifdef CONFIG_SCHED_DEBUG
	unsigned int state = READ_ONCE(p->__state);

	/*
	 * We should never call set_task_cpu() on a blocked task,
	 * ttwu() will sort out the placement.
	 */
	WARN_ON_ONCE(state != TASK_RUNNING && state != TASK_WAKING && !p->on_rq);

#ifdef CONFIG_LOCKDEP
	/*
	 * The caller should hold either p->pi_lock or rq->lock, when changing
	 * a task's CPU. ->pi_lock for waking tasks, rq->lock for runnable tasks.
	 *
	 * sched_move_task() holds both and thus holding either pins the cgroup,
	 * see task_group().
	 */
	WARN_ON_ONCE(debug_locks && !(lockdep_is_held(&p->pi_lock) ||
				      lockdep_is_held(&task_rq(p)->lock)));
#endif
	/*
	 * Clearly, migrating tasks to offline CPUs is a fairly daft thing.
	 */
	WARN_ON_ONCE(!cpu_online(new_cpu));

	WARN_ON_ONCE(is_migration_disabled(p));
#endif
	trace_sched_migrate_task(p, new_cpu);

	if (task_cpu(p) != new_cpu)
	{
		rseq_migrate(p);
		sched_mm_cid_migrate_from(p);
		perf_event_task_migrate(p);
	}

	__set_task_cpu(p, new_cpu);
}

static void
__do_set_cpus_ptr(struct task_struct *p, const struct cpumask *new_mask)
{
	/*
	 * This here violates the locking rules for affinity, since we're only
	 * supposed to change these variables while holding both rq->lock and
	 * p->pi_lock.
	 *
	 * HOWEVER, it magically works, because ttwu() is the only code that
	 * accesses these variables under p->pi_lock and only does so after
	 * smp_cond_load_acquire(&p->on_cpu, !VAL), and we're in __schedule()
	 * before finish_task().
	 *
	 * XXX do further audits, this smells like something putrid.
	 */
	SCHED_WARN_ON(!p->on_cpu);
	p->cpus_ptr = new_mask;
}

void migrate_disable(void)
{
	struct task_struct *p = current;
	int cpu;

	if (p->migration_disabled) {
#ifdef CONFIG_DEBUG_PREEMPT
		/*
		 * Warn about overflow half-way through the range.
		 */
		WARN_ON_ONCE((s16)p->migration_disabled < 0);
#endif
		p->migration_disabled++;
		return;
	}

	guard(preempt)();
	cpu = smp_processor_id();
	if (cpumask_test_cpu(cpu, &p->cpus_mask)) {
		cpu_rq(cpu)->nr_pinned++;
		p->migration_disabled = 1;
		/*
		 * Violates locking rules! see comment in __do_set_cpus_ptr().
		 */
		if (p->cpus_ptr == &p->cpus_mask)
			__do_set_cpus_ptr(p, cpumask_of(cpu));
	}
}
EXPORT_SYMBOL_GPL(migrate_disable);

void migrate_enable(void)
{
	struct task_struct *p = current;

#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Check both overflow from migrate_disable() and superfluous
	 * migrate_enable().
	 */
	if (WARN_ON_ONCE((s16)p->migration_disabled <= 0))
		return;
#endif

	if (p->migration_disabled > 1) {
		p->migration_disabled--;
		return;
	}

	/*
	 * Ensure stop_task runs either before or after this, and that
	 * __set_cpus_allowed_ptr(SCA_MIGRATE_ENABLE) doesn't schedule().
	 */
	guard(preempt)();
	/*
	 * Assumption: current should be running on allowed cpu
	 */
	WARN_ON_ONCE(!cpumask_test_cpu(smp_processor_id(), &p->cpus_mask));
	if (p->cpus_ptr != &p->cpus_mask)
		__do_set_cpus_ptr(p, &p->cpus_mask);
	/*
	 * Mustn't clear migration_disabled() until cpus_ptr points back at the
	 * regular cpus_mask, otherwise things that race (eg.
	 * select_fallback_rq) get confused.
	 */
	barrier();
	p->migration_disabled = 0;
	this_rq()->nr_pinned--;
}
EXPORT_SYMBOL_GPL(migrate_enable);

static void __migrate_force_enable(struct task_struct *p, struct rq *rq)
{
	if (likely(p->cpus_ptr != &p->cpus_mask))
		__do_set_cpus_ptr(p, &p->cpus_mask);
	p->migration_disabled = 0;
	/* When p is migrate_disabled, rq->lock should be held */
	rq->nr_pinned--;
}

static inline bool rq_has_pinned_tasks(struct rq *rq)
{
	return rq->nr_pinned;
}

/*
 * Per-CPU kthreads are allowed to run on !active && online CPUs, see
 * __set_cpus_allowed_ptr() and select_fallback_rq().
 */
static inline bool is_cpu_allowed(struct task_struct *p, int cpu)
{
	/* When not in the task's cpumask, no point in looking further. */
	if (!cpumask_test_cpu(cpu, p->cpus_ptr))
		return false;

	/* migrate_disabled() must be allowed to finish. */
	if (is_migration_disabled(p))
		return cpu_online(cpu);

	/* Non kernel threads are not allowed during either online or offline. */
	if (!(p->flags & PF_KTHREAD))
		return cpu_active(cpu) && task_cpu_possible(cpu, p);

	/* KTHREAD_IS_PER_CPU is always allowed. */
	if (kthread_is_per_cpu(p))
		return cpu_online(cpu);

	/* Regular kernel threads don't get to stay during offline. */
	if (cpu_dying(cpu))
		return false;

	/* But are allowed during online. */
	return cpu_online(cpu);
}

/*
 * This is how migration works:
 *
 * 1) we invoke migration_cpu_stop() on the target CPU using
 *    stop_one_cpu().
 * 2) stopper starts to run (implicitly forcing the migrated thread
 *    off the CPU)
 * 3) it checks whether the migrated task is still in the wrong runqueue.
 * 4) if it's in the wrong runqueue then the migration thread removes
 *    it and puts it into the right queue.
 * 5) stopper completes and stop_one_cpu() returns and the migration
 *    is done.
 */

/*
 * move_queued_task - move a queued task to new rq.
 *
 * Returns (locked) new rq. Old rq's lock is released.
 */
struct rq *move_queued_task(struct rq *rq, struct task_struct *p, int new_cpu)
{
	lockdep_assert_held(&rq->lock);

	WRITE_ONCE(p->on_rq, TASK_ON_RQ_MIGRATING);
	dequeue_task(p, rq, 0);
	set_task_cpu(p, new_cpu);
	raw_spin_unlock(&rq->lock);

	rq = cpu_rq(new_cpu);

	raw_spin_lock(&rq->lock);
	WARN_ON_ONCE(task_cpu(p) != new_cpu);

	sched_mm_cid_migrate_to(rq, p);

	sched_task_sanity_check(p, rq);
	enqueue_task(p, rq, 0);
	WRITE_ONCE(p->on_rq, TASK_ON_RQ_QUEUED);
	wakeup_preempt(rq);

	return rq;
}

struct migration_arg {
	struct task_struct *task;
	int dest_cpu;
};

/*
 * Move (not current) task off this CPU, onto the destination CPU. We're doing
 * this because either it can't run here any more (set_cpus_allowed()
 * away from this CPU, or CPU going down), or because we're
 * attempting to rebalance this task on exec (sched_exec).
 *
 * So we race with normal scheduler movements, but that's OK, as long
 * as the task is no longer on this CPU.
 */
static struct rq *__migrate_task(struct rq *rq, struct task_struct *p, int dest_cpu)
{
	/* Affinity changed (again). */
	if (!is_cpu_allowed(p, dest_cpu))
		return rq;

	return move_queued_task(rq, p, dest_cpu);
}

/*
 * migration_cpu_stop - this will be executed by a high-prio stopper thread
 * and performs thread migration by bumping thread off CPU then
 * 'pushing' onto another runqueue.
 */
static int migration_cpu_stop(void *data)
{
	struct migration_arg *arg = data;
	struct task_struct *p = arg->task;
	struct rq *rq = this_rq();
	unsigned long flags;

	/*
	 * The original target CPU might have gone down and we might
	 * be on another CPU but it doesn't matter.
	 */
	local_irq_save(flags);
	/*
	 * We need to explicitly wake pending tasks before running
	 * __migrate_task() such that we will not miss enforcing cpus_ptr
	 * during wakeups, see set_cpus_allowed_ptr()'s TASK_WAKING test.
	 */
	flush_smp_call_function_queue();

	raw_spin_lock(&p->pi_lock);
	raw_spin_lock(&rq->lock);
	/*
	 * If task_rq(p) != rq, it cannot be migrated here, because we're
	 * holding rq->lock, if p->on_rq == 0 it cannot get enqueued because
	 * we're holding p->pi_lock.
	 */
	if (task_rq(p) == rq && task_on_rq_queued(p)) {
		update_rq_clock(rq);
		rq = __migrate_task(rq, p, arg->dest_cpu);
	}
	raw_spin_unlock(&rq->lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	return 0;
}

static inline void
set_cpus_allowed_common(struct task_struct *p, struct affinity_context *ctx)
{
	cpumask_copy(&p->cpus_mask, ctx->new_mask);
	p->nr_cpus_allowed = cpumask_weight(ctx->new_mask);

	/*
	 * Swap in a new user_cpus_ptr if SCA_USER flag set
	 */
	if (ctx->flags & SCA_USER)
		swap(p->user_cpus_ptr, ctx->user_mask);
}

static void
__do_set_cpus_allowed(struct task_struct *p, struct affinity_context *ctx)
{
	lockdep_assert_held(&p->pi_lock);
	set_cpus_allowed_common(p, ctx);
}

/*
 * Used for kthread_bind() and select_fallback_rq(), in both cases the user
 * affinity (if any) should be destroyed too.
 */
void do_set_cpus_allowed(struct task_struct *p, const struct cpumask *new_mask)
{
	struct affinity_context ac = {
		.new_mask  = new_mask,
		.user_mask = NULL,
		.flags     = SCA_USER,	/* clear the user requested mask */
	};
	union cpumask_rcuhead {
		cpumask_t cpumask;
		struct rcu_head rcu;
	};

	__do_set_cpus_allowed(p, &ac);

	if (is_migration_disabled(p) && !cpumask_test_cpu(task_cpu(p), &p->cpus_mask))
		__migrate_force_enable(p, task_rq(p));

	/*
	 * Because this is called with p->pi_lock held, it is not possible
	 * to use kfree() here (when PREEMPT_RT=y), therefore punt to using
	 * kfree_rcu().
	 */
	kfree_rcu((union cpumask_rcuhead *)ac.user_mask, rcu);
}

int dup_user_cpus_ptr(struct task_struct *dst, struct task_struct *src,
		      int node)
{
	cpumask_t *user_mask;
	unsigned long flags;

	/*
	 * Always clear dst->user_cpus_ptr first as their user_cpus_ptr's
	 * may differ by now due to racing.
	 */
	dst->user_cpus_ptr = NULL;

	/*
	 * This check is racy and losing the race is a valid situation.
	 * It is not worth the extra overhead of taking the pi_lock on
	 * every fork/clone.
	 */
	if (data_race(!src->user_cpus_ptr))
		return 0;

	user_mask = alloc_user_cpus_ptr(node);
	if (!user_mask)
		return -ENOMEM;

	/*
	 * Use pi_lock to protect content of user_cpus_ptr
	 *
	 * Though unlikely, user_cpus_ptr can be reset to NULL by a concurrent
	 * do_set_cpus_allowed().
	 */
	raw_spin_lock_irqsave(&src->pi_lock, flags);
	if (src->user_cpus_ptr) {
		swap(dst->user_cpus_ptr, user_mask);
		cpumask_copy(dst->user_cpus_ptr, src->user_cpus_ptr);
	}
	raw_spin_unlock_irqrestore(&src->pi_lock, flags);

	if (unlikely(user_mask))
		kfree(user_mask);

	return 0;
}

static inline struct cpumask *clear_user_cpus_ptr(struct task_struct *p)
{
	struct cpumask *user_mask = NULL;

	swap(p->user_cpus_ptr, user_mask);

	return user_mask;
}

void release_user_cpus_ptr(struct task_struct *p)
{
	kfree(clear_user_cpus_ptr(p));
}

#endif

/**
 * task_curr - is this task currently executing on a CPU?
 * @p: the task in question.
 *
 * Return: 1 if the task is currently executing. 0 otherwise.
 */
inline int task_curr(const struct task_struct *p)
{
	return cpu_curr(task_cpu(p)) == p;
}

#ifdef CONFIG_SMP
/***
 * kick_process - kick a running thread to enter/exit the kernel
 * @p: the to-be-kicked thread
 *
 * Cause a process which is running on another CPU to enter
 * kernel-mode, without any delay. (to get signals handled.)
 *
 * NOTE: this function doesn't have to take the runqueue lock,
 * because all it wants to ensure is that the remote task enters
 * the kernel. If the IPI races and the task has been migrated
 * to another CPU then no harm is done and the purpose has been
 * achieved as well.
 */
void kick_process(struct task_struct *p)
{
	guard(preempt)();
	int cpu = task_cpu(p);

	if ((cpu != smp_processor_id()) && task_curr(p))
		smp_send_reschedule(cpu);
}
EXPORT_SYMBOL_GPL(kick_process);

/*
 * ->cpus_ptr is protected by both rq->lock and p->pi_lock
 *
 * A few notes on cpu_active vs cpu_online:
 *
 *  - cpu_active must be a subset of cpu_online
 *
 *  - on CPU-up we allow per-CPU kthreads on the online && !active CPU,
 *    see __set_cpus_allowed_ptr(). At this point the newly online
 *    CPU isn't yet part of the sched domains, and balancing will not
 *    see it.
 *
 *  - on cpu-down we clear cpu_active() to mask the sched domains and
 *    avoid the load balancer to place new tasks on the to be removed
 *    CPU. Existing tasks will remain running there and will be taken
 *    off.
 *
 * This means that fallback selection must not select !active CPUs.
 * And can assume that any active CPU must be online. Conversely
 * select_task_rq() below may allow selection of !active CPUs in order
 * to satisfy the above rules.
 */
static int select_fallback_rq(int cpu, struct task_struct *p)
{
	int nid = cpu_to_node(cpu);
	const struct cpumask *nodemask = NULL;
	enum { cpuset, possible, fail } state = cpuset;
	int dest_cpu;

	/*
	 * If the node that the CPU is on has been offlined, cpu_to_node()
	 * will return -1. There is no CPU on the node, and we should
	 * select the CPU on the other node.
	 */
	if (nid != -1) {
		nodemask = cpumask_of_node(nid);

		/* Look for allowed, online CPU in same node. */
		for_each_cpu(dest_cpu, nodemask) {
			if (is_cpu_allowed(p, dest_cpu))
				return dest_cpu;
		}
	}

	for (;;) {
		/* Any allowed, online CPU? */
		for_each_cpu(dest_cpu, p->cpus_ptr) {
			if (!is_cpu_allowed(p, dest_cpu))
				continue;
			goto out;
		}

		/* No more Mr. Nice Guy. */
		switch (state) {
		case cpuset:
			if (cpuset_cpus_allowed_fallback(p)) {
				state = possible;
				break;
			}
			fallthrough;
		case possible:
			/*
			 * XXX When called from select_task_rq() we only
			 * hold p->pi_lock and again violate locking order.
			 *
			 * More yuck to audit.
			 */
			do_set_cpus_allowed(p, task_cpu_possible_mask(p));
			state = fail;
			break;

		case fail:
			BUG();
			break;
		}
	}

out:
	if (state != cpuset) {
		/*
		 * Don't tell them about moving exiting tasks or
		 * kernel threads (both mm NULL), since they never
		 * leave kernel.
		 */
		if (p->mm && printk_ratelimit()) {
			printk_deferred("process %d (%s) no longer affine to cpu%d\n",
					task_pid_nr(p), p->comm, cpu);
		}
	}

	return dest_cpu;
}

static inline void
sched_preempt_mask_flush(cpumask_t *mask, int prio, int ref)
{
	int cpu;

	cpumask_copy(mask, sched_preempt_mask + ref);
	if (prio < ref) {
		for_each_clear_bit(cpu, cpumask_bits(mask), nr_cpumask_bits) {
			if (prio < cpu_rq(cpu)->prio)
				cpumask_set_cpu(cpu, mask);
		}
	} else {
		for_each_cpu_andnot(cpu, mask, sched_idle_mask) {
			if (prio >= cpu_rq(cpu)->prio)
				cpumask_clear_cpu(cpu, mask);
		}
	}
}

static inline int
preempt_mask_check(cpumask_t *preempt_mask, cpumask_t *allow_mask, int prio)
{
	cpumask_t *mask = sched_preempt_mask + prio;
	int pr = atomic_read(&sched_prio_record);

	if (pr != prio && SCHED_QUEUE_BITS - 1 != prio) {
		sched_preempt_mask_flush(mask, prio, pr);
		atomic_set(&sched_prio_record, prio);
	}

	return cpumask_and(preempt_mask, allow_mask, mask);
}

__read_mostly idle_select_func_t idle_select_func ____cacheline_aligned_in_smp = cpumask_and;

static inline int select_task_rq(struct task_struct *p)
{
	cpumask_t allow_mask, mask;

	if (unlikely(!cpumask_and(&allow_mask, p->cpus_ptr, cpu_active_mask)))
		return select_fallback_rq(task_cpu(p), p);

	if (idle_select_func(&mask, &allow_mask, sched_idle_mask)	||
	    preempt_mask_check(&mask, &allow_mask, task_sched_prio(p)))
		return best_mask_cpu(task_cpu(p), &mask);

	return best_mask_cpu(task_cpu(p), &allow_mask);
}

void sched_set_stop_task(int cpu, struct task_struct *stop)
{
	static struct lock_class_key stop_pi_lock;
	struct sched_param stop_param = { .sched_priority = STOP_PRIO };
	struct sched_param start_param = { .sched_priority = 0 };
	struct task_struct *old_stop = cpu_rq(cpu)->stop;

	if (stop) {
		/*
		 * Make it appear like a SCHED_FIFO task, its something
		 * userspace knows about and won't get confused about.
		 *
		 * Also, it will make PI more or less work without too
		 * much confusion -- but then, stop work should not
		 * rely on PI working anyway.
		 */
		sched_setscheduler_nocheck(stop, SCHED_FIFO, &stop_param);

		/*
		 * The PI code calls rt_mutex_setprio() with ->pi_lock held to
		 * adjust the effective priority of a task. As a result,
		 * rt_mutex_setprio() can trigger (RT) balancing operations,
		 * which can then trigger wakeups of the stop thread to push
		 * around the current task.
		 *
		 * The stop task itself will never be part of the PI-chain, it
		 * never blocks, therefore that ->pi_lock recursion is safe.
		 * Tell lockdep about this by placing the stop->pi_lock in its
		 * own class.
		 */
		lockdep_set_class(&stop->pi_lock, &stop_pi_lock);
	}

	cpu_rq(cpu)->stop = stop;

	if (old_stop) {
		/*
		 * Reset it back to a normal scheduling policy so that
		 * it can die in pieces.
		 */
		sched_setscheduler_nocheck(old_stop, SCHED_NORMAL, &start_param);
	}
}

static int affine_move_task(struct rq *rq, struct task_struct *p, int dest_cpu,
			    raw_spinlock_t *lock, unsigned long irq_flags)
	__releases(rq->lock)
	__releases(p->pi_lock)
{
	/* Can the task run on the task's current CPU? If so, we're done */
	if (!cpumask_test_cpu(task_cpu(p), &p->cpus_mask)) {
		if (is_migration_disabled(p))
			__migrate_force_enable(p, rq);

		if (task_on_cpu(p) || READ_ONCE(p->__state) == TASK_WAKING) {
			struct migration_arg arg = { p, dest_cpu };

			/* Need help from migration thread: drop lock and wait. */
			__task_access_unlock(p, lock);
			raw_spin_unlock_irqrestore(&p->pi_lock, irq_flags);
			stop_one_cpu(cpu_of(rq), migration_cpu_stop, &arg);
			return 0;
		}
		if (task_on_rq_queued(p)) {
			/*
			 * OK, since we're going to drop the lock immediately
			 * afterwards anyway.
			 */
			update_rq_clock(rq);
			rq = move_queued_task(rq, p, dest_cpu);
			lock = &rq->lock;
		}
	}
	__task_access_unlock(p, lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, irq_flags);
	return 0;
}

static int __set_cpus_allowed_ptr_locked(struct task_struct *p,
					 struct affinity_context *ctx,
					 struct rq *rq,
					 raw_spinlock_t *lock,
					 unsigned long irq_flags)
{
	const struct cpumask *cpu_allowed_mask = task_cpu_possible_mask(p);
	const struct cpumask *cpu_valid_mask = cpu_active_mask;
	bool kthread = p->flags & PF_KTHREAD;
	int dest_cpu;
	int ret = 0;

	if (kthread || is_migration_disabled(p)) {
		/*
		 * Kernel threads are allowed on online && !active CPUs,
		 * however, during cpu-hot-unplug, even these might get pushed
		 * away if not KTHREAD_IS_PER_CPU.
		 *
		 * Specifically, migration_disabled() tasks must not fail the
		 * cpumask_any_and_distribute() pick below, esp. so on
		 * SCA_MIGRATE_ENABLE, otherwise we'll not call
		 * set_cpus_allowed_common() and actually reset p->cpus_ptr.
		 */
		cpu_valid_mask = cpu_online_mask;
	}

	if (!kthread && !cpumask_subset(ctx->new_mask, cpu_allowed_mask)) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Must re-check here, to close a race against __kthread_bind(),
	 * sched_setaffinity() is not guaranteed to observe the flag.
	 */
	if ((ctx->flags & SCA_CHECK) && (p->flags & PF_NO_SETAFFINITY)) {
		ret = -EINVAL;
		goto out;
	}

	if (cpumask_equal(&p->cpus_mask, ctx->new_mask))
		goto out;

	dest_cpu = cpumask_any_and(cpu_valid_mask, ctx->new_mask);
	if (dest_cpu >= nr_cpu_ids) {
		ret = -EINVAL;
		goto out;
	}

	__do_set_cpus_allowed(p, ctx);

	return affine_move_task(rq, p, dest_cpu, lock, irq_flags);

out:
	__task_access_unlock(p, lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, irq_flags);

	return ret;
}

/*
 * Change a given task's CPU affinity. Migrate the thread to a
 * is removed from the allowed bitmask.
 *
 * NOTE: the caller must have a valid reference to the task, the
 * task must not exit() & deallocate itself prematurely. The
 * call is not atomic; no spinlocks may be held.
 */
int __set_cpus_allowed_ptr(struct task_struct *p,
			   struct affinity_context *ctx)
{
	unsigned long irq_flags;
	struct rq *rq;
	raw_spinlock_t *lock;

	raw_spin_lock_irqsave(&p->pi_lock, irq_flags);
	rq = __task_access_lock(p, &lock);
	/*
	 * Masking should be skipped if SCA_USER or any of the SCA_MIGRATE_*
	 * flags are set.
	 */
	if (p->user_cpus_ptr &&
	    !(ctx->flags & SCA_USER) &&
	    cpumask_and(rq->scratch_mask, ctx->new_mask, p->user_cpus_ptr))
		ctx->new_mask = rq->scratch_mask;


	return __set_cpus_allowed_ptr_locked(p, ctx, rq, lock, irq_flags);
}

int set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *new_mask)
{
	struct affinity_context ac = {
		.new_mask  = new_mask,
		.flags     = 0,
	};

	return __set_cpus_allowed_ptr(p, &ac);
}
EXPORT_SYMBOL_GPL(set_cpus_allowed_ptr);

/*
 * Change a given task's CPU affinity to the intersection of its current
 * affinity mask and @subset_mask, writing the resulting mask to @new_mask.
 * If user_cpus_ptr is defined, use it as the basis for restricting CPU
 * affinity or use cpu_online_mask instead.
 *
 * If the resulting mask is empty, leave the affinity unchanged and return
 * -EINVAL.
 */
static int restrict_cpus_allowed_ptr(struct task_struct *p,
				     struct cpumask *new_mask,
				     const struct cpumask *subset_mask)
{
	struct affinity_context ac = {
		.new_mask  = new_mask,
		.flags     = 0,
	};
	unsigned long irq_flags;
	raw_spinlock_t *lock;
	struct rq *rq;
	int err;

	raw_spin_lock_irqsave(&p->pi_lock, irq_flags);
	rq = __task_access_lock(p, &lock);

	if (!cpumask_and(new_mask, task_user_cpus(p), subset_mask)) {
		err = -EINVAL;
		goto err_unlock;
	}

	return __set_cpus_allowed_ptr_locked(p, &ac, rq, lock, irq_flags);

err_unlock:
	__task_access_unlock(p, lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, irq_flags);
	return err;
}

/*
 * Restrict the CPU affinity of task @p so that it is a subset of
 * task_cpu_possible_mask() and point @p->user_cpus_ptr to a copy of the
 * old affinity mask. If the resulting mask is empty, we warn and walk
 * up the cpuset hierarchy until we find a suitable mask.
 */
void force_compatible_cpus_allowed_ptr(struct task_struct *p)
{
	cpumask_var_t new_mask;
	const struct cpumask *override_mask = task_cpu_possible_mask(p);

	alloc_cpumask_var(&new_mask, GFP_KERNEL);

	/*
	 * __migrate_task() can fail silently in the face of concurrent
	 * offlining of the chosen destination CPU, so take the hotplug
	 * lock to ensure that the migration succeeds.
	 */
	cpus_read_lock();
	if (!cpumask_available(new_mask))
		goto out_set_mask;

	if (!restrict_cpus_allowed_ptr(p, new_mask, override_mask))
		goto out_free_mask;

	/*
	 * We failed to find a valid subset of the affinity mask for the
	 * task, so override it based on its cpuset hierarchy.
	 */
	cpuset_cpus_allowed(p, new_mask);
	override_mask = new_mask;

out_set_mask:
	if (printk_ratelimit()) {
		printk_deferred("Overriding affinity for process %d (%s) to CPUs %*pbl\n",
				task_pid_nr(p), p->comm,
				cpumask_pr_args(override_mask));
	}

	WARN_ON(set_cpus_allowed_ptr(p, override_mask));
out_free_mask:
	cpus_read_unlock();
	free_cpumask_var(new_mask);
}

/*
 * Restore the affinity of a task @p which was previously restricted by a
 * call to force_compatible_cpus_allowed_ptr().
 *
 * It is the caller's responsibility to serialise this with any calls to
 * force_compatible_cpus_allowed_ptr(@p).
 */
void relax_compatible_cpus_allowed_ptr(struct task_struct *p)
{
	struct affinity_context ac = {
		.new_mask  = task_user_cpus(p),
		.flags     = 0,
	};
	int ret;

	/*
	 * Try to restore the old affinity mask with __sched_setaffinity().
	 * Cpuset masking will be done there too.
	 */
	ret = __sched_setaffinity(p, &ac);
	WARN_ON_ONCE(ret);
}

#else /* CONFIG_SMP */

static inline int select_task_rq(struct task_struct *p)
{
	return 0;
}

static inline bool rq_has_pinned_tasks(struct rq *rq)
{
	return false;
}

#endif /* !CONFIG_SMP */

static void
ttwu_stat(struct task_struct *p, int cpu, int wake_flags)
{
	struct rq *rq;

	if (!schedstat_enabled())
		return;

	rq = this_rq();

#ifdef CONFIG_SMP
	if (cpu == rq->cpu) {
		__schedstat_inc(rq->ttwu_local);
		__schedstat_inc(p->stats.nr_wakeups_local);
	} else {
		/** Alt schedule FW ToDo:
		 * How to do ttwu_wake_remote
		 */
	}
#endif /* CONFIG_SMP */

	__schedstat_inc(rq->ttwu_count);
	__schedstat_inc(p->stats.nr_wakeups);
}

/*
 * Mark the task runnable.
 */
static inline void ttwu_do_wakeup(struct task_struct *p)
{
	WRITE_ONCE(p->__state, TASK_RUNNING);
	trace_sched_wakeup(p);
}

static inline void
ttwu_do_activate(struct rq *rq, struct task_struct *p, int wake_flags)
{
	if (p->sched_contributes_to_load)
		rq->nr_uninterruptible--;

	if (
#ifdef CONFIG_SMP
	    !(wake_flags & WF_MIGRATED) &&
#endif
	    p->in_iowait) {
		delayacct_blkio_end(p);
		atomic_dec(&task_rq(p)->nr_iowait);
	}

	activate_task(p, rq);
	wakeup_preempt(rq);

	ttwu_do_wakeup(p);
}

/*
 * Consider @p being inside a wait loop:
 *
 *   for (;;) {
 *      set_current_state(TASK_UNINTERRUPTIBLE);
 *
 *      if (CONDITION)
 *         break;
 *
 *      schedule();
 *   }
 *   __set_current_state(TASK_RUNNING);
 *
 * between set_current_state() and schedule(). In this case @p is still
 * runnable, so all that needs doing is change p->state back to TASK_RUNNING in
 * an atomic manner.
 *
 * By taking task_rq(p)->lock we serialize against schedule(), if @p->on_rq
 * then schedule() must still happen and p->state can be changed to
 * TASK_RUNNING. Otherwise we lost the race, schedule() has happened, and we
 * need to do a full wakeup with enqueue.
 *
 * Returns: %true when the wakeup is done,
 *          %false otherwise.
 */
static int ttwu_runnable(struct task_struct *p, int wake_flags)
{
	struct rq *rq;
	raw_spinlock_t *lock;
	int ret = 0;

	rq = __task_access_lock(p, &lock);
	if (task_on_rq_queued(p)) {
		if (!task_on_cpu(p)) {
			/*
			 * When on_rq && !on_cpu the task is preempted, see if
			 * it should preempt the task that is current now.
			 */
			update_rq_clock(rq);
			wakeup_preempt(rq);
		}
		ttwu_do_wakeup(p);
		ret = 1;
	}
	__task_access_unlock(p, lock);

	return ret;
}

#ifdef CONFIG_SMP
void sched_ttwu_pending(void *arg)
{
	struct llist_node *llist = arg;
	struct rq *rq = this_rq();
	struct task_struct *p, *t;
	struct rq_flags rf;

	if (!llist)
		return;

	rq_lock_irqsave(rq, &rf);
	update_rq_clock(rq);

	llist_for_each_entry_safe(p, t, llist, wake_entry.llist) {
		if (WARN_ON_ONCE(p->on_cpu))
			smp_cond_load_acquire(&p->on_cpu, !VAL);

		if (WARN_ON_ONCE(task_cpu(p) != cpu_of(rq)))
			set_task_cpu(p, cpu_of(rq));

		ttwu_do_activate(rq, p, p->sched_remote_wakeup ? WF_MIGRATED : 0);
	}

	/*
	 * Must be after enqueueing at least once task such that
	 * idle_cpu() does not observe a false-negative -- if it does,
	 * it is possible for select_idle_siblings() to stack a number
	 * of tasks on this CPU during that window.
	 *
	 * It is OK to clear ttwu_pending when another task pending.
	 * We will receive IPI after local IRQ enabled and then enqueue it.
	 * Since now nr_running > 0, idle_cpu() will always get correct result.
	 */
	WRITE_ONCE(rq->ttwu_pending, 0);
	rq_unlock_irqrestore(rq, &rf);
}

/*
 * Prepare the scene for sending an IPI for a remote smp_call
 *
 * Returns true if the caller can proceed with sending the IPI.
 * Returns false otherwise.
 */
bool call_function_single_prep_ipi(int cpu)
{
	if (set_nr_if_polling(cpu_rq(cpu)->idle)) {
		trace_sched_wake_idle_without_ipi(cpu);
		return false;
	}

	return true;
}

/*
 * Queue a task on the target CPUs wake_list and wake the CPU via IPI if
 * necessary. The wakee CPU on receipt of the IPI will queue the task
 * via sched_ttwu_wakeup() for activation so the wakee incurs the cost
 * of the wakeup instead of the waker.
 */
static void __ttwu_queue_wakelist(struct task_struct *p, int cpu, int wake_flags)
{
	struct rq *rq = cpu_rq(cpu);

	p->sched_remote_wakeup = !!(wake_flags & WF_MIGRATED);

	WRITE_ONCE(rq->ttwu_pending, 1);
	__smp_call_single_queue(cpu, &p->wake_entry.llist);
}

static inline bool ttwu_queue_cond(struct task_struct *p, int cpu)
{
	/*
	 * Do not complicate things with the async wake_list while the CPU is
	 * in hotplug state.
	 */
	if (!cpu_active(cpu))
		return false;

	/* Ensure the task will still be allowed to run on the CPU. */
	if (!cpumask_test_cpu(cpu, p->cpus_ptr))
		return false;

	/*
	 * If the CPU does not share cache, then queue the task on the
	 * remote rqs wakelist to avoid accessing remote data.
	 */
	if (!cpus_share_cache(smp_processor_id(), cpu))
		return true;

	if (cpu == smp_processor_id())
		return false;

	/*
	 * If the wakee cpu is idle, or the task is descheduling and the
	 * only running task on the CPU, then use the wakelist to offload
	 * the task activation to the idle (or soon-to-be-idle) CPU as
	 * the current CPU is likely busy. nr_running is checked to
	 * avoid unnecessary task stacking.
	 *
	 * Note that we can only get here with (wakee) p->on_rq=0,
	 * p->on_cpu can be whatever, we've done the dequeue, so
	 * the wakee has been accounted out of ->nr_running.
	 */
	if (!cpu_rq(cpu)->nr_running)
		return true;

	return false;
}

static bool ttwu_queue_wakelist(struct task_struct *p, int cpu, int wake_flags)
{
	if (__is_defined(ALT_SCHED_TTWU_QUEUE) && ttwu_queue_cond(p, cpu)) {
		sched_clock_cpu(cpu); /* Sync clocks across CPUs */
		__ttwu_queue_wakelist(p, cpu, wake_flags);
		return true;
	}

	return false;
}

void wake_up_if_idle(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	guard(rcu)();
	if (is_idle_task(rcu_dereference(rq->curr))) {
		guard(raw_spinlock_irqsave)(&rq->lock);
		if (is_idle_task(rq->curr))
			resched_curr(rq);
	}
}

extern struct static_key_false sched_asym_cpucapacity;

static __always_inline bool sched_asym_cpucap_active(void)
{
	return static_branch_unlikely(&sched_asym_cpucapacity);
}

bool cpus_equal_capacity(int this_cpu, int that_cpu)
{
	if (!sched_asym_cpucap_active())
		return true;

	if (this_cpu == that_cpu)
		return true;

	return arch_scale_cpu_capacity(this_cpu) == arch_scale_cpu_capacity(that_cpu);
}

bool cpus_share_cache(int this_cpu, int that_cpu)
{
	if (this_cpu == that_cpu)
		return true;

	return per_cpu(sd_llc_id, this_cpu) == per_cpu(sd_llc_id, that_cpu);
}
#else /* !CONFIG_SMP */

static inline bool ttwu_queue_wakelist(struct task_struct *p, int cpu, int wake_flags)
{
	return false;
}

#endif /* CONFIG_SMP */

static inline void ttwu_queue(struct task_struct *p, int cpu, int wake_flags)
{
	struct rq *rq = cpu_rq(cpu);

	if (ttwu_queue_wakelist(p, cpu, wake_flags))
		return;

	raw_spin_lock(&rq->lock);
	update_rq_clock(rq);
	ttwu_do_activate(rq, p, wake_flags);
	raw_spin_unlock(&rq->lock);
}

/*
 * Invoked from try_to_wake_up() to check whether the task can be woken up.
 *
 * The caller holds p::pi_lock if p != current or has preemption
 * disabled when p == current.
 *
 * The rules of saved_state:
 *
 *   The related locking code always holds p::pi_lock when updating
 *   p::saved_state, which means the code is fully serialized in both cases.
 *
 *  For PREEMPT_RT, the lock wait and lock wakeups happen via TASK_RTLOCK_WAIT.
 *  No other bits set. This allows to distinguish all wakeup scenarios.
 *
 *  For FREEZER, the wakeup happens via TASK_FROZEN. No other bits set. This
 *  allows us to prevent early wakeup of tasks before they can be run on
 *  asymmetric ISA architectures (eg ARMv9).
 */
static __always_inline
bool ttwu_state_match(struct task_struct *p, unsigned int state, int *success)
{
	int match;

	if (IS_ENABLED(CONFIG_DEBUG_PREEMPT)) {
		WARN_ON_ONCE((state & TASK_RTLOCK_WAIT) &&
			     state != TASK_RTLOCK_WAIT);
	}

	*success = !!(match = __task_state_match(p, state));

	/*
	 * Saved state preserves the task state across blocking on
	 * an RT lock or TASK_FREEZABLE tasks.  If the state matches,
	 * set p::saved_state to TASK_RUNNING, but do not wake the task
	 * because it waits for a lock wakeup or __thaw_task(). Also
	 * indicate success because from the regular waker's point of
	 * view this has succeeded.
	 *
	 * After acquiring the lock the task will restore p::__state
	 * from p::saved_state which ensures that the regular
	 * wakeup is not lost. The restore will also set
	 * p::saved_state to TASK_RUNNING so any further tests will
	 * not result in false positives vs. @success
	 */
	if (match < 0)
		p->saved_state = TASK_RUNNING;

	return match > 0;
}

/*
 * Notes on Program-Order guarantees on SMP systems.
 *
 *  MIGRATION
 *
 * The basic program-order guarantee on SMP systems is that when a task [t]
 * migrates, all its activity on its old CPU [c0] happens-before any subsequent
 * execution on its new CPU [c1].
 *
 * For migration (of runnable tasks) this is provided by the following means:
 *
 *  A) UNLOCK of the rq(c0)->lock scheduling out task t
 *  B) migration for t is required to synchronize *both* rq(c0)->lock and
 *     rq(c1)->lock (if not at the same time, then in that order).
 *  C) LOCK of the rq(c1)->lock scheduling in task
 *
 * Transitivity guarantees that B happens after A and C after B.
 * Note: we only require RCpc transitivity.
 * Note: the CPU doing B need not be c0 or c1
 *
 * Example:
 *
 *   CPU0            CPU1            CPU2
 *
 *   LOCK rq(0)->lock
 *   sched-out X
 *   sched-in Y
 *   UNLOCK rq(0)->lock
 *
 *                                   LOCK rq(0)->lock // orders against CPU0
 *                                   dequeue X
 *                                   UNLOCK rq(0)->lock
 *
 *                                   LOCK rq(1)->lock
 *                                   enqueue X
 *                                   UNLOCK rq(1)->lock
 *
 *                   LOCK rq(1)->lock // orders against CPU2
 *                   sched-out Z
 *                   sched-in X
 *                   UNLOCK rq(1)->lock
 *
 *
 *  BLOCKING -- aka. SLEEP + WAKEUP
 *
 * For blocking we (obviously) need to provide the same guarantee as for
 * migration. However the means are completely different as there is no lock
 * chain to provide order. Instead we do:
 *
 *   1) smp_store_release(X->on_cpu, 0)   -- finish_task()
 *   2) smp_cond_load_acquire(!X->on_cpu) -- try_to_wake_up()
 *
 * Example:
 *
 *   CPU0 (schedule)  CPU1 (try_to_wake_up) CPU2 (schedule)
 *
 *   LOCK rq(0)->lock LOCK X->pi_lock
 *   dequeue X
 *   sched-out X
 *   smp_store_release(X->on_cpu, 0);
 *
 *                    smp_cond_load_acquire(&X->on_cpu, !VAL);
 *                    X->state = WAKING
 *                    set_task_cpu(X,2)
 *
 *                    LOCK rq(2)->lock
 *                    enqueue X
 *                    X->state = RUNNING
 *                    UNLOCK rq(2)->lock
 *
 *                                          LOCK rq(2)->lock // orders against CPU1
 *                                          sched-out Z
 *                                          sched-in X
 *                                          UNLOCK rq(2)->lock
 *
 *                    UNLOCK X->pi_lock
 *   UNLOCK rq(0)->lock
 *
 *
 * However; for wakeups there is a second guarantee we must provide, namely we
 * must observe the state that lead to our wakeup. That is, not only must our
 * task observe its own prior state, it must also observe the stores prior to
 * its wakeup.
 *
 * This means that any means of doing remote wakeups must order the CPU doing
 * the wakeup against the CPU the task is going to end up running on. This,
 * however, is already required for the regular Program-Order guarantee above,
 * since the waking CPU is the one issueing the ACQUIRE (smp_cond_load_acquire).
 *
 */

/**
 * try_to_wake_up - wake up a thread
 * @p: the thread to be awakened
 * @state: the mask of task states that can be woken
 * @wake_flags: wake modifier flags (WF_*)
 *
 * Conceptually does:
 *
 *   If (@state & @p->state) @p->state = TASK_RUNNING.
 *
 * If the task was not queued/runnable, also place it back on a runqueue.
 *
 * This function is atomic against schedule() which would dequeue the task.
 *
 * It issues a full memory barrier before accessing @p->state, see the comment
 * with set_current_state().
 *
 * Uses p->pi_lock to serialize against concurrent wake-ups.
 *
 * Relies on p->pi_lock stabilizing:
 *  - p->sched_class
 *  - p->cpus_ptr
 *  - p->sched_task_group
 * in order to do migration, see its use of select_task_rq()/set_task_cpu().
 *
 * Tries really hard to only take one task_rq(p)->lock for performance.
 * Takes rq->lock in:
 *  - ttwu_runnable()    -- old rq, unavoidable, see comment there;
 *  - ttwu_queue()       -- new rq, for enqueue of the task;
 *  - psi_ttwu_dequeue() -- much sadness :-( accounting will kill us.
 *
 * As a consequence we race really badly with just about everything. See the
 * many memory barriers and their comments for details.
 *
 * Return: %true if @p->state changes (an actual wakeup was done),
 *	   %false otherwise.
 */
int try_to_wake_up(struct task_struct *p, unsigned int state, int wake_flags)
{
	guard(preempt)();
	int cpu, success = 0;

	if (p == current) {
		/*
		 * We're waking current, this means 'p->on_rq' and 'task_cpu(p)
		 * == smp_processor_id()'. Together this means we can special
		 * case the whole 'p->on_rq && ttwu_runnable()' case below
		 * without taking any locks.
		 *
		 * In particular:
		 *  - we rely on Program-Order guarantees for all the ordering,
		 *  - we're serialized against set_special_state() by virtue of
		 *    it disabling IRQs (this allows not taking ->pi_lock).
		 */
		if (!ttwu_state_match(p, state, &success))
			goto out;

		trace_sched_waking(p);
		ttwu_do_wakeup(p);
		goto out;
	}

	/*
	 * If we are going to wake up a thread waiting for CONDITION we
	 * need to ensure that CONDITION=1 done by the caller can not be
	 * reordered with p->state check below. This pairs with smp_store_mb()
	 * in set_current_state() that the waiting thread does.
	 */
	scoped_guard (raw_spinlock_irqsave, &p->pi_lock) {
		smp_mb__after_spinlock();
		if (!ttwu_state_match(p, state, &success))
			break;

		trace_sched_waking(p);

		/*
		 * Ensure we load p->on_rq _after_ p->state, otherwise it would
		 * be possible to, falsely, observe p->on_rq == 0 and get stuck
		 * in smp_cond_load_acquire() below.
		 *
		 * sched_ttwu_pending()			try_to_wake_up()
		 *   STORE p->on_rq = 1			  LOAD p->state
		 *   UNLOCK rq->lock
		 *
		 * __schedule() (switch to task 'p')
		 *   LOCK rq->lock			  smp_rmb();
		 *   smp_mb__after_spinlock();
		 *   UNLOCK rq->lock
		 *
		 * [task p]
		 *   STORE p->state = UNINTERRUPTIBLE	  LOAD p->on_rq
		 *
		 * Pairs with the LOCK+smp_mb__after_spinlock() on rq->lock in
		 * __schedule().  See the comment for smp_mb__after_spinlock().
		 *
		 * A similar smp_rmb() lives in __task_needs_rq_lock().
		 */
		smp_rmb();
		if (READ_ONCE(p->on_rq) && ttwu_runnable(p, wake_flags))
			break;

#ifdef CONFIG_SMP
		/*
		 * Ensure we load p->on_cpu _after_ p->on_rq, otherwise it would be
		 * possible to, falsely, observe p->on_cpu == 0.
		 *
		 * One must be running (->on_cpu == 1) in order to remove oneself
		 * from the runqueue.
		 *
		 * __schedule() (switch to task 'p')	try_to_wake_up()
		 *   STORE p->on_cpu = 1		  LOAD p->on_rq
		 *   UNLOCK rq->lock
		 *
		 * __schedule() (put 'p' to sleep)
		 *   LOCK rq->lock			  smp_rmb();
		 *   smp_mb__after_spinlock();
		 *   STORE p->on_rq = 0			  LOAD p->on_cpu
		 *
		 * Pairs with the LOCK+smp_mb__after_spinlock() on rq->lock in
		 * __schedule().  See the comment for smp_mb__after_spinlock().
		 *
		 * Form a control-dep-acquire with p->on_rq == 0 above, to ensure
		 * schedule()'s deactivate_task() has 'happened' and p will no longer
		 * care about it's own p->state. See the comment in __schedule().
		 */
		smp_acquire__after_ctrl_dep();

		/*
		 * We're doing the wakeup (@success == 1), they did a dequeue (p->on_rq
		 * == 0), which means we need to do an enqueue, change p->state to
		 * TASK_WAKING such that we can unlock p->pi_lock before doing the
		 * enqueue, such as ttwu_queue_wakelist().
		 */
		WRITE_ONCE(p->__state, TASK_WAKING);

		/*
		 * If the owning (remote) CPU is still in the middle of schedule() with
		 * this task as prev, considering queueing p on the remote CPUs wake_list
		 * which potentially sends an IPI instead of spinning on p->on_cpu to
		 * let the waker make forward progress. This is safe because IRQs are
		 * disabled and the IPI will deliver after on_cpu is cleared.
		 *
		 * Ensure we load task_cpu(p) after p->on_cpu:
		 *
		 * set_task_cpu(p, cpu);
		 *   STORE p->cpu = @cpu
		 * __schedule() (switch to task 'p')
		 *   LOCK rq->lock
		 *   smp_mb__after_spin_lock()          smp_cond_load_acquire(&p->on_cpu)
		 *   STORE p->on_cpu = 1                LOAD p->cpu
		 *
		 * to ensure we observe the correct CPU on which the task is currently
		 * scheduling.
		 */
		if (smp_load_acquire(&p->on_cpu) &&
		    ttwu_queue_wakelist(p, task_cpu(p), wake_flags))
			break;

		/*
		 * If the owning (remote) CPU is still in the middle of schedule() with
		 * this task as prev, wait until it's done referencing the task.
		 *
		 * Pairs with the smp_store_release() in finish_task().
		 *
		 * This ensures that tasks getting woken will be fully ordered against
		 * their previous state and preserve Program Order.
		 */
		smp_cond_load_acquire(&p->on_cpu, !VAL);

		sched_task_ttwu(p);

		if ((wake_flags & WF_CURRENT_CPU) &&
		    cpumask_test_cpu(smp_processor_id(), p->cpus_ptr))
			cpu = smp_processor_id();
		else
			cpu = select_task_rq(p);

		if (cpu != task_cpu(p)) {
			if (p->in_iowait) {
				delayacct_blkio_end(p);
				atomic_dec(&task_rq(p)->nr_iowait);
			}

			wake_flags |= WF_MIGRATED;
			set_task_cpu(p, cpu);
		}
#else
		sched_task_ttwu(p);

		cpu = task_cpu(p);
#endif /* CONFIG_SMP */

		ttwu_queue(p, cpu, wake_flags);
	}
out:
	if (success)
		ttwu_stat(p, task_cpu(p), wake_flags);

	return success;
}

static bool __task_needs_rq_lock(struct task_struct *p)
{
	unsigned int state = READ_ONCE(p->__state);

	/*
	 * Since pi->lock blocks try_to_wake_up(), we don't need rq->lock when
	 * the task is blocked. Make sure to check @state since ttwu() can drop
	 * locks at the end, see ttwu_queue_wakelist().
	 */
	if (state == TASK_RUNNING || state == TASK_WAKING)
		return true;

	/*
	 * Ensure we load p->on_rq after p->__state, otherwise it would be
	 * possible to, falsely, observe p->on_rq == 0.
	 *
	 * See try_to_wake_up() for a longer comment.
	 */
	smp_rmb();
	if (p->on_rq)
		return true;

#ifdef CONFIG_SMP
	/*
	 * Ensure the task has finished __schedule() and will not be referenced
	 * anymore. Again, see try_to_wake_up() for a longer comment.
	 */
	smp_rmb();
	smp_cond_load_acquire(&p->on_cpu, !VAL);
#endif

	return false;
}

/**
 * task_call_func - Invoke a function on task in fixed state
 * @p: Process for which the function is to be invoked, can be @current.
 * @func: Function to invoke.
 * @arg: Argument to function.
 *
 * Fix the task in it's current state by avoiding wakeups and or rq operations
 * and call @func(@arg) on it.  This function can use task_is_runnable() and
 * task_curr() to work out what the state is, if required.  Given that @func
 * can be invoked with a runqueue lock held, it had better be quite
 * lightweight.
 *
 * Returns:
 *   Whatever @func returns
 */
int task_call_func(struct task_struct *p, task_call_f func, void *arg)
{
	struct rq *rq = NULL;
	struct rq_flags rf;
	int ret;

	raw_spin_lock_irqsave(&p->pi_lock, rf.flags);

	if (__task_needs_rq_lock(p))
		rq = __task_rq_lock(p, &rf);

	/*
	 * At this point the task is pinned; either:
	 *  - blocked and we're holding off wakeups      (pi->lock)
	 *  - woken, and we're holding off enqueue       (rq->lock)
	 *  - queued, and we're holding off schedule     (rq->lock)
	 *  - running, and we're holding off de-schedule (rq->lock)
	 *
	 * The called function (@func) can use: task_curr(), p->on_rq and
	 * p->__state to differentiate between these states.
	 */
	ret = func(p, arg);

	if (rq)
		__task_rq_unlock(rq, &rf);

	raw_spin_unlock_irqrestore(&p->pi_lock, rf.flags);
	return ret;
}

/**
 * cpu_curr_snapshot - Return a snapshot of the currently running task
 * @cpu: The CPU on which to snapshot the task.
 *
 * Returns the task_struct pointer of the task "currently" running on
 * the specified CPU.  If the same task is running on that CPU throughout,
 * the return value will be a pointer to that task's task_struct structure.
 * If the CPU did any context switches even vaguely concurrently with the
 * execution of this function, the return value will be a pointer to the
 * task_struct structure of a randomly chosen task that was running on
 * that CPU somewhere around the time that this function was executing.
 *
 * If the specified CPU was offline, the return value is whatever it
 * is, perhaps a pointer to the task_struct structure of that CPU's idle
 * task, but there is no guarantee.  Callers wishing a useful return
 * value must take some action to ensure that the specified CPU remains
 * online throughout.
 *
 * This function executes full memory barriers before and after fetching
 * the pointer, which permits the caller to confine this function's fetch
 * with respect to the caller's accesses to other shared variables.
 */
struct task_struct *cpu_curr_snapshot(int cpu)
{
	struct task_struct *t;

	smp_mb(); /* Pairing determined by caller's synchronization design. */
	t = rcu_dereference(cpu_curr(cpu));
	smp_mb(); /* Pairing determined by caller's synchronization design. */
	return t;
}

/**
 * wake_up_process - Wake up a specific process
 * @p: The process to be woken up.
 *
 * Attempt to wake up the nominated process and move it to the set of runnable
 * processes.
 *
 * Return: 1 if the process was woken up, 0 if it was already running.
 *
 * This function executes a full memory barrier before accessing the task state.
 */
int wake_up_process(struct task_struct *p)
{
	return try_to_wake_up(p, TASK_NORMAL, 0);
}
EXPORT_SYMBOL(wake_up_process);

int wake_up_state(struct task_struct *p, unsigned int state)
{
	return try_to_wake_up(p, state, 0);
}

/*
 * Perform scheduler related setup for a newly forked process p.
 * p is forked by current.
 *
 * __sched_fork() is basic setup used by init_idle() too:
 */
static inline void __sched_fork(unsigned long clone_flags, struct task_struct *p)
{
	p->on_rq			= 0;
	p->on_cpu			= 0;
	p->utime			= 0;
	p->stime			= 0;
	p->sched_time			= 0;

#ifdef CONFIG_SCHEDSTATS
	/* Even if schedstat is disabled, there should not be garbage */
	memset(&p->stats, 0, sizeof(p->stats));
#endif

#ifdef CONFIG_PREEMPT_NOTIFIERS
	INIT_HLIST_HEAD(&p->preempt_notifiers);
#endif

#ifdef CONFIG_COMPACTION
	p->capture_control = NULL;
#endif
#ifdef CONFIG_SMP
	p->wake_entry.u_flags = CSD_TYPE_TTWU;
#endif
	init_sched_mm_cid(p);
}

/*
 * fork()/clone()-time setup:
 */
int sched_fork(unsigned long clone_flags, struct task_struct *p)
{
	__sched_fork(clone_flags, p);
	/*
	 * We mark the process as NEW here. This guarantees that
	 * nobody will actually run it, and a signal or other external
	 * event cannot wake it up and insert it on the runqueue either.
	 */
	p->__state = TASK_NEW;

	/*
	 * Make sure we do not leak PI boosting priority to the child.
	 */
	p->prio = current->normal_prio;

	/*
	 * Revert to default priority/policy on fork if requested.
	 */
	if (unlikely(p->sched_reset_on_fork)) {
		if (task_has_rt_policy(p)) {
			p->policy = SCHED_NORMAL;
			p->static_prio = NICE_TO_PRIO(0);
			p->rt_priority = 0;
		} else if (PRIO_TO_NICE(p->static_prio) < 0)
			p->static_prio = NICE_TO_PRIO(0);

		p->prio = p->normal_prio = p->static_prio;

		/*
		 * We don't need the reset flag anymore after the fork. It has
		 * fulfilled its duty:
		 */
		p->sched_reset_on_fork = 0;
	}

#ifdef CONFIG_SCHED_INFO
	if (unlikely(sched_info_on()))
		memset(&p->sched_info, 0, sizeof(p->sched_info));
#endif
	init_task_preempt_count(p);

	return 0;
}

int sched_cgroup_fork(struct task_struct *p, struct kernel_clone_args *kargs)
{
	unsigned long flags;
	struct rq *rq;

	/*
	 * Because we're not yet on the pid-hash, p->pi_lock isn't strictly
	 * required yet, but lockdep gets upset if rules are violated.
	 */
	raw_spin_lock_irqsave(&p->pi_lock, flags);
	/*
	 * Share the timeslice between parent and child, thus the
	 * total amount of pending timeslices in the system doesn't change,
	 * resulting in more scheduling fairness.
	 */
	rq = this_rq();
	raw_spin_lock(&rq->lock);

	rq->curr->time_slice /= 2;
	p->time_slice = rq->curr->time_slice;
#ifdef CONFIG_SCHED_HRTICK
	hrtick_start(rq, rq->curr->time_slice);
#endif

	if (p->time_slice < RESCHED_NS) {
		p->time_slice = sysctl_sched_base_slice;
		resched_curr(rq);
	}
	sched_task_fork(p, rq);
	raw_spin_unlock(&rq->lock);

	rseq_migrate(p);
	/*
	 * We're setting the CPU for the first time, we don't migrate,
	 * so use __set_task_cpu().
	 */
	__set_task_cpu(p, smp_processor_id());
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	return 0;
}

void sched_cancel_fork(struct task_struct *p)
{
}

void sched_post_fork(struct task_struct *p)
{
}

#ifdef CONFIG_SCHEDSTATS

DEFINE_STATIC_KEY_FALSE(sched_schedstats);

static void set_schedstats(bool enabled)
{
	if (enabled)
		static_branch_enable(&sched_schedstats);
	else
		static_branch_disable(&sched_schedstats);
}

void force_schedstat_enabled(void)
{
	if (!schedstat_enabled()) {
		pr_info("kernel profiling enabled schedstats, disable via kernel.sched_schedstats.\n");
		static_branch_enable(&sched_schedstats);
	}
}

static int __init setup_schedstats(char *str)
{
	int ret = 0;
	if (!str)
		goto out;

	if (!strcmp(str, "enable")) {
		set_schedstats(true);
		ret = 1;
	} else if (!strcmp(str, "disable")) {
		set_schedstats(false);
		ret = 1;
	}
out:
	if (!ret)
		pr_warn("Unable to parse schedstats=\n");

	return ret;
}
__setup("schedstats=", setup_schedstats);

#ifdef CONFIG_PROC_SYSCTL
static int sysctl_schedstats(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int err;
	int state = static_branch_likely(&sched_schedstats);

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &state;
	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;
	if (write)
		set_schedstats(state);
	return err;
}

static struct ctl_table sched_core_sysctls[] = {
	{
		.procname       = "sched_schedstats",
		.data           = NULL,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		.proc_handler   = sysctl_schedstats,
		.extra1         = SYSCTL_ZERO,
		.extra2         = SYSCTL_ONE,
	},
};
static int __init sched_core_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_core_sysctls);
	return 0;
}
late_initcall(sched_core_sysctl_init);
#endif /* CONFIG_PROC_SYSCTL */
#endif /* CONFIG_SCHEDSTATS */

/*
 * wake_up_new_task - wake up a newly created task for the first time.
 *
 * This function will do some initial scheduler statistics housekeeping
 * that must be done for every newly created context, then puts the task
 * on the runqueue and wakes it.
 */
void wake_up_new_task(struct task_struct *p)
{
	unsigned long flags;
	struct rq *rq;

	raw_spin_lock_irqsave(&p->pi_lock, flags);
	WRITE_ONCE(p->__state, TASK_RUNNING);
	rq = cpu_rq(select_task_rq(p));
#ifdef CONFIG_SMP
	rseq_migrate(p);
	/*
	 * Fork balancing, do it here and not earlier because:
	 * - cpus_ptr can change in the fork path
	 * - any previously selected CPU might disappear through hotplug
	 *
	 * Use __set_task_cpu() to avoid calling sched_class::migrate_task_rq,
	 * as we're not fully set-up yet.
	 */
	__set_task_cpu(p, cpu_of(rq));
#endif

	raw_spin_lock(&rq->lock);
	update_rq_clock(rq);

	activate_task(p, rq);
	trace_sched_wakeup_new(p);
	wakeup_preempt(rq);

	raw_spin_unlock(&rq->lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);
}

#ifdef CONFIG_PREEMPT_NOTIFIERS

static DEFINE_STATIC_KEY_FALSE(preempt_notifier_key);

void preempt_notifier_inc(void)
{
	static_branch_inc(&preempt_notifier_key);
}
EXPORT_SYMBOL_GPL(preempt_notifier_inc);

void preempt_notifier_dec(void)
{
	static_branch_dec(&preempt_notifier_key);
}
EXPORT_SYMBOL_GPL(preempt_notifier_dec);

/**
 * preempt_notifier_register - tell me when current is being preempted & rescheduled
 * @notifier: notifier struct to register
 */
void preempt_notifier_register(struct preempt_notifier *notifier)
{
	if (!static_branch_unlikely(&preempt_notifier_key))
		WARN(1, "registering preempt_notifier while notifiers disabled\n");

	hlist_add_head(&notifier->link, &current->preempt_notifiers);
}
EXPORT_SYMBOL_GPL(preempt_notifier_register);

/**
 * preempt_notifier_unregister - no longer interested in preemption notifications
 * @notifier: notifier struct to unregister
 *
 * This is *not* safe to call from within a preemption notifier.
 */
void preempt_notifier_unregister(struct preempt_notifier *notifier)
{
	hlist_del(&notifier->link);
}
EXPORT_SYMBOL_GPL(preempt_notifier_unregister);

static void __fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
	struct preempt_notifier *notifier;

	hlist_for_each_entry(notifier, &curr->preempt_notifiers, link)
		notifier->ops->sched_in(notifier, raw_smp_processor_id());
}

static __always_inline void fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
	if (static_branch_unlikely(&preempt_notifier_key))
		__fire_sched_in_preempt_notifiers(curr);
}

static void
__fire_sched_out_preempt_notifiers(struct task_struct *curr,
				   struct task_struct *next)
{
	struct preempt_notifier *notifier;

	hlist_for_each_entry(notifier, &curr->preempt_notifiers, link)
		notifier->ops->sched_out(notifier, next);
}

static __always_inline void
fire_sched_out_preempt_notifiers(struct task_struct *curr,
				 struct task_struct *next)
{
	if (static_branch_unlikely(&preempt_notifier_key))
		__fire_sched_out_preempt_notifiers(curr, next);
}

#else /* !CONFIG_PREEMPT_NOTIFIERS */

static inline void fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
}

static inline void
fire_sched_out_preempt_notifiers(struct task_struct *curr,
				 struct task_struct *next)
{
}

#endif /* CONFIG_PREEMPT_NOTIFIERS */

static inline void prepare_task(struct task_struct *next)
{
	/*
	 * Claim the task as running, we do this before switching to it
	 * such that any running task will have this set.
	 *
	 * See the smp_load_acquire(&p->on_cpu) case in ttwu() and
	 * its ordering comment.
	 */
	WRITE_ONCE(next->on_cpu, 1);
}

static inline void finish_task(struct task_struct *prev)
{
#ifdef CONFIG_SMP
	/*
	 * This must be the very last reference to @prev from this CPU. After
	 * p->on_cpu is cleared, the task can be moved to a different CPU. We
	 * must ensure this doesn't happen until the switch is completely
	 * finished.
	 *
	 * In particular, the load of prev->state in finish_task_switch() must
	 * happen before this.
	 *
	 * Pairs with the smp_cond_load_acquire() in try_to_wake_up().
	 */
	smp_store_release(&prev->on_cpu, 0);
#else
	prev->on_cpu = 0;
#endif
}

#ifdef CONFIG_SMP

static void do_balance_callbacks(struct rq *rq, struct balance_callback *head)
{
	void (*func)(struct rq *rq);
	struct balance_callback *next;

	lockdep_assert_held(&rq->lock);

	while (head) {
		func = (void (*)(struct rq *))head->func;
		next = head->next;
		head->next = NULL;
		head = next;

		func(rq);
	}
}

static void balance_push(struct rq *rq);

/*
 * balance_push_callback is a right abuse of the callback interface and plays
 * by significantly different rules.
 *
 * Where the normal balance_callback's purpose is to be ran in the same context
 * that queued it (only later, when it's safe to drop rq->lock again),
 * balance_push_callback is specifically targeted at __schedule().
 *
 * This abuse is tolerated because it places all the unlikely/odd cases behind
 * a single test, namely: rq->balance_callback == NULL.
 */
struct balance_callback balance_push_callback = {
	.next = NULL,
	.func = balance_push,
};

static inline struct balance_callback *
__splice_balance_callbacks(struct rq *rq, bool split)
{
	struct balance_callback *head = rq->balance_callback;

	if (likely(!head))
		return NULL;

	lockdep_assert_rq_held(rq);
	/*
	 * Must not take balance_push_callback off the list when
	 * splice_balance_callbacks() and balance_callbacks() are not
	 * in the same rq->lock section.
	 *
	 * In that case it would be possible for __schedule() to interleave
	 * and observe the list empty.
	 */
	if (split && head == &balance_push_callback)
		head = NULL;
	else
		rq->balance_callback = NULL;

	return head;
}

struct balance_callback *splice_balance_callbacks(struct rq *rq)
{
	return __splice_balance_callbacks(rq, true);
}

static void __balance_callbacks(struct rq *rq)
{
	do_balance_callbacks(rq, __splice_balance_callbacks(rq, false));
}

void balance_callbacks(struct rq *rq, struct balance_callback *head)
{
	unsigned long flags;

	if (unlikely(head)) {
		raw_spin_lock_irqsave(&rq->lock, flags);
		do_balance_callbacks(rq, head);
		raw_spin_unlock_irqrestore(&rq->lock, flags);
	}
}

#else

static inline void __balance_callbacks(struct rq *rq)
{
}
#endif

static inline void
prepare_lock_switch(struct rq *rq, struct task_struct *next)
{
	/*
	 * Since the runqueue lock will be released by the next
	 * task (which is an invalid locking op but in the case
	 * of the scheduler it's an obvious special-case), so we
	 * do an early lockdep release here:
	 */
	spin_release(&rq->lock.dep_map, _THIS_IP_);
#ifdef CONFIG_DEBUG_SPINLOCK
	/* this is a valid case when another task releases the spinlock */
	rq->lock.owner = next;
#endif
}

static inline void finish_lock_switch(struct rq *rq)
{
	/*
	 * If we are tracking spinlock dependencies then we have to
	 * fix up the runqueue lock - which gets 'carried over' from
	 * prev into current:
	 */
	spin_acquire(&rq->lock.dep_map, 0, 0, _THIS_IP_);
	__balance_callbacks(rq);
	raw_spin_unlock_irq(&rq->lock);
}

/*
 * NOP if the arch has not defined these:
 */

#ifndef prepare_arch_switch
# define prepare_arch_switch(next)	do { } while (0)
#endif

#ifndef finish_arch_post_lock_switch
# define finish_arch_post_lock_switch()	do { } while (0)
#endif

static inline void kmap_local_sched_out(void)
{
#ifdef CONFIG_KMAP_LOCAL
	if (unlikely(current->kmap_ctrl.idx))
		__kmap_local_sched_out();
#endif
}

static inline void kmap_local_sched_in(void)
{
#ifdef CONFIG_KMAP_LOCAL
	if (unlikely(current->kmap_ctrl.idx))
		__kmap_local_sched_in();
#endif
}

/**
 * prepare_task_switch - prepare to switch tasks
 * @rq: the runqueue preparing to switch
 * @next: the task we are going to switch to.
 *
 * This is called with the rq lock held and interrupts off. It must
 * be paired with a subsequent finish_task_switch after the context
 * switch.
 *
 * prepare_task_switch sets up locking and calls architecture specific
 * hooks.
 */
static inline void
prepare_task_switch(struct rq *rq, struct task_struct *prev,
		    struct task_struct *next)
{
	kcov_prepare_switch(prev);
	sched_info_switch(rq, prev, next);
	perf_event_task_sched_out(prev, next);
	rseq_preempt(prev);
	fire_sched_out_preempt_notifiers(prev, next);
	kmap_local_sched_out();
	prepare_task(next);
	prepare_arch_switch(next);
}

/**
 * finish_task_switch - clean up after a task-switch
 * @rq: runqueue associated with task-switch
 * @prev: the thread we just switched away from.
 *
 * finish_task_switch must be called after the context switch, paired
 * with a prepare_task_switch call before the context switch.
 * finish_task_switch will reconcile locking set up by prepare_task_switch,
 * and do any other architecture-specific cleanup actions.
 *
 * Note that we may have delayed dropping an mm in context_switch(). If
 * so, we finish that here outside of the runqueue lock.  (Doing it
 * with the lock held can cause deadlocks; see schedule() for
 * details.)
 *
 * The context switch have flipped the stack from under us and restored the
 * local variables which were saved when this task called schedule() in the
 * past. 'prev == current' is still correct but we need to recalculate this_rq
 * because prev may have moved to another CPU.
 */
static struct rq *finish_task_switch(struct task_struct *prev)
	__releases(rq->lock)
{
	struct rq *rq = this_rq();
	struct mm_struct *mm = rq->prev_mm;
	unsigned int prev_state;

	/*
	 * The previous task will have left us with a preempt_count of 2
	 * because it left us after:
	 *
	 *	schedule()
	 *	  preempt_disable();			// 1
	 *	  __schedule()
	 *	    raw_spin_lock_irq(&rq->lock)	// 2
	 *
	 * Also, see FORK_PREEMPT_COUNT.
	 */
	if (WARN_ONCE(preempt_count() != 2*PREEMPT_DISABLE_OFFSET,
		      "corrupted preempt_count: %s/%d/0x%x\n",
		      current->comm, current->pid, preempt_count()))
		preempt_count_set(FORK_PREEMPT_COUNT);

	rq->prev_mm = NULL;

	/*
	 * A task struct has one reference for the use as "current".
	 * If a task dies, then it sets TASK_DEAD in tsk->state and calls
	 * schedule one last time. The schedule call will never return, and
	 * the scheduled task must drop that reference.
	 *
	 * We must observe prev->state before clearing prev->on_cpu (in
	 * finish_task), otherwise a concurrent wakeup can get prev
	 * running on another CPU and we could rave with its RUNNING -> DEAD
	 * transition, resulting in a double drop.
	 */
	prev_state = READ_ONCE(prev->__state);
	vtime_task_switch(prev);
	perf_event_task_sched_in(prev, current);
	finish_task(prev);
	tick_nohz_task_switch();
	finish_lock_switch(rq);
	finish_arch_post_lock_switch();
	kcov_finish_switch(current);
	/*
	 * kmap_local_sched_out() is invoked with rq::lock held and
	 * interrupts disabled. There is no requirement for that, but the
	 * sched out code does not have an interrupt enabled section.
	 * Restoring the maps on sched in does not require interrupts being
	 * disabled either.
	 */
	kmap_local_sched_in();

	fire_sched_in_preempt_notifiers(current);
	/*
	 * When switching through a kernel thread, the loop in
	 * membarrier_{private,global}_expedited() may have observed that
	 * kernel thread and not issued an IPI. It is therefore possible to
	 * schedule between user->kernel->user threads without passing though
	 * switch_mm(). Membarrier requires a barrier after storing to
	 * rq->curr, before returning to userspace, so provide them here:
	 *
	 * - a full memory barrier for {PRIVATE,GLOBAL}_EXPEDITED, implicitly
	 *   provided by mmdrop(),
	 * - a sync_core for SYNC_CORE.
	 */
	if (mm) {
		membarrier_mm_sync_core_before_usermode(mm);
		mmdrop_sched(mm);
	}
	if (unlikely(prev_state == TASK_DEAD)) {
		/* Task is done with its stack. */
		put_task_stack(prev);

		put_task_struct_rcu_user(prev);
	}

	return rq;
}

/**
 * schedule_tail - first thing a freshly forked thread must call.
 * @prev: the thread we just switched away from.
 */
asmlinkage __visible void schedule_tail(struct task_struct *prev)
	__releases(rq->lock)
{
	/*
	 * New tasks start with FORK_PREEMPT_COUNT, see there and
	 * finish_task_switch() for details.
	 *
	 * finish_task_switch() will drop rq->lock() and lower preempt_count
	 * and the preempt_enable() will end up enabling preemption (on
	 * PREEMPT_COUNT kernels).
	 */

	finish_task_switch(prev);
	preempt_enable();

	if (current->set_child_tid)
		put_user(task_pid_vnr(current), current->set_child_tid);

	calculate_sigpending();
}

/*
 * context_switch - switch to the new MM and the new thread's register state.
 */
static __always_inline struct rq *
context_switch(struct rq *rq, struct task_struct *prev,
	       struct task_struct *next)
{
	prepare_task_switch(rq, prev, next);

	/*
	 * For paravirt, this is coupled with an exit in switch_to to
	 * combine the page table reload and the switch backend into
	 * one hypercall.
	 */
	arch_start_context_switch(prev);

	/*
	 * kernel -> kernel   lazy + transfer active
	 *   user -> kernel   lazy + mmgrab() active
	 *
	 * kernel ->   user   switch + mmdrop() active
	 *   user ->   user   switch
	 *
	 * switch_mm_cid() needs to be updated if the barriers provided
	 * by context_switch() are modified.
	 */
	if (!next->mm) {                                // to kernel
		enter_lazy_tlb(prev->active_mm, next);

		next->active_mm = prev->active_mm;
		if (prev->mm)                           // from user
			mmgrab(prev->active_mm);
		else
			prev->active_mm = NULL;
	} else {                                        // to user
		membarrier_switch_mm(rq, prev->active_mm, next->mm);
		/*
		 * sys_membarrier() requires an smp_mb() between setting
		 * rq->curr / membarrier_switch_mm() and returning to userspace.
		 *
		 * The below provides this either through switch_mm(), or in
		 * case 'prev->active_mm == next->mm' through
		 * finish_task_switch()'s mmdrop().
		 */
		switch_mm_irqs_off(prev->active_mm, next->mm, next);
		lru_gen_use_mm(next->mm);

		if (!prev->mm) {                        // from kernel
			/* will mmdrop() in finish_task_switch(). */
			rq->prev_mm = prev->active_mm;
			prev->active_mm = NULL;
		}
	}

	/* switch_mm_cid() requires the memory barriers above. */
	switch_mm_cid(rq, prev, next);

	prepare_lock_switch(rq, next);

	/* Here we just switch the register state and the stack. */
	switch_to(prev, next, prev);
	barrier();

	return finish_task_switch(prev);
}

/*
 * nr_running, nr_uninterruptible and nr_context_switches:
 *
 * externally visible scheduler statistics: current number of runnable
 * threads, total number of context switches performed since bootup.
 */
unsigned int nr_running(void)
{
	unsigned int i, sum = 0;

	for_each_online_cpu(i)
		sum += cpu_rq(i)->nr_running;

	return sum;
}

/*
 * Check if only the current task is running on the CPU.
 *
 * Caution: this function does not check that the caller has disabled
 * preemption, thus the result might have a time-of-check-to-time-of-use
 * race.  The caller is responsible to use it correctly, for example:
 *
 * - from a non-preemptible section (of course)
 *
 * - from a thread that is bound to a single CPU
 *
 * - in a loop with very short iterations (e.g. a polling loop)
 */
bool single_task_running(void)
{
	return raw_rq()->nr_running == 1;
}
EXPORT_SYMBOL(single_task_running);

unsigned long long nr_context_switches_cpu(int cpu)
{
	return cpu_rq(cpu)->nr_switches;
}

unsigned long long nr_context_switches(void)
{
	int i;
	unsigned long long sum = 0;

	for_each_possible_cpu(i)
		sum += cpu_rq(i)->nr_switches;

	return sum;
}

/*
 * Consumers of these two interfaces, like for example the cpuidle menu
 * governor, are using nonsensical data. Preferring shallow idle state selection
 * for a CPU that has IO-wait which might not even end up running the task when
 * it does become runnable.
 */

unsigned int nr_iowait_cpu(int cpu)
{
	return atomic_read(&cpu_rq(cpu)->nr_iowait);
}

/*
 * IO-wait accounting, and how it's mostly bollocks (on SMP).
 *
 * The idea behind IO-wait account is to account the idle time that we could
 * have spend running if it were not for IO. That is, if we were to improve the
 * storage performance, we'd have a proportional reduction in IO-wait time.
 *
 * This all works nicely on UP, where, when a task blocks on IO, we account
 * idle time as IO-wait, because if the storage were faster, it could've been
 * running and we'd not be idle.
 *
 * This has been extended to SMP, by doing the same for each CPU. This however
 * is broken.
 *
 * Imagine for instance the case where two tasks block on one CPU, only the one
 * CPU will have IO-wait accounted, while the other has regular idle. Even
 * though, if the storage were faster, both could've ran at the same time,
 * utilising both CPUs.
 *
 * This means, that when looking globally, the current IO-wait accounting on
 * SMP is a lower bound, by reason of under accounting.
 *
 * Worse, since the numbers are provided per CPU, they are sometimes
 * interpreted per CPU, and that is nonsensical. A blocked task isn't strictly
 * associated with any one particular CPU, it can wake to another CPU than it
 * blocked on. This means the per CPU IO-wait number is meaningless.
 *
 * Task CPU affinities can make all that even more 'interesting'.
 */

unsigned int nr_iowait(void)
{
	unsigned int i, sum = 0;

	for_each_possible_cpu(i)
		sum += nr_iowait_cpu(i);

	return sum;
}

#ifdef CONFIG_SMP

/*
 * sched_exec - execve() is a valuable balancing opportunity, because at
 * this point the task has the smallest effective memory and cache
 * footprint.
 */
void sched_exec(void)
{
}

#endif

DEFINE_PER_CPU(struct kernel_stat, kstat);
DEFINE_PER_CPU(struct kernel_cpustat, kernel_cpustat);

EXPORT_PER_CPU_SYMBOL(kstat);
EXPORT_PER_CPU_SYMBOL(kernel_cpustat);

static inline void update_curr(struct rq *rq, struct task_struct *p)
{
	s64 ns = rq->clock_task - p->last_ran;

	p->sched_time += ns;
	cgroup_account_cputime(p, ns);
	account_group_exec_runtime(p, ns);

	p->time_slice -= ns;
	p->last_ran = rq->clock_task;
}

/*
 * Return accounted runtime for the task.
 * Return separately the current's pending runtime that have not been
 * accounted yet.
 */
unsigned long long task_sched_runtime(struct task_struct *p)
{
	unsigned long flags;
	struct rq *rq;
	raw_spinlock_t *lock;
	u64 ns;

#if defined(CONFIG_64BIT) && defined(CONFIG_SMP)
	/*
	 * 64-bit doesn't need locks to atomically read a 64-bit value.
	 * So we have a optimization chance when the task's delta_exec is 0.
	 * Reading ->on_cpu is racy, but this is OK.
	 *
	 * If we race with it leaving CPU, we'll take a lock. So we're correct.
	 * If we race with it entering CPU, unaccounted time is 0. This is
	 * indistinguishable from the read occurring a few cycles earlier.
	 * If we see ->on_cpu without ->on_rq, the task is leaving, and has
	 * been accounted, so we're correct here as well.
	 */
	if (!p->on_cpu || !task_on_rq_queued(p))
		return tsk_seruntime(p);
#endif

	rq = task_access_lock_irqsave(p, &lock, &flags);
	/*
	 * Must be ->curr _and_ ->on_rq.  If dequeued, we would
	 * project cycles that may never be accounted to this
	 * thread, breaking clock_gettime().
	 */
	if (p == rq->curr && task_on_rq_queued(p)) {
		update_rq_clock(rq);
		update_curr(rq, p);
	}
	ns = tsk_seruntime(p);
	task_access_unlock_irqrestore(p, lock, &flags);

	return ns;
}

/* This manages tasks that have run out of timeslice during a scheduler_tick */
static inline void scheduler_task_tick(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	if (is_idle_task(p))
		return;

	update_curr(rq, p);
	cpufreq_update_util(rq, 0);

	/*
	 * Tasks have less than RESCHED_NS of time slice left they will be
	 * rescheduled.
	 */
	if (p->time_slice >= RESCHED_NS)
		return;
	set_tsk_need_resched(p);
	set_preempt_need_resched();
}

#ifdef CONFIG_SCHED_DEBUG
static u64 cpu_resched_latency(struct rq *rq)
{
	int latency_warn_ms = READ_ONCE(sysctl_resched_latency_warn_ms);
	u64 resched_latency, now = rq_clock(rq);
	static bool warned_once;

	if (sysctl_resched_latency_warn_once && warned_once)
		return 0;

	if (!need_resched() || !latency_warn_ms)
		return 0;

	if (system_state == SYSTEM_BOOTING)
		return 0;

	if (!rq->last_seen_need_resched_ns) {
		rq->last_seen_need_resched_ns = now;
		rq->ticks_without_resched = 0;
		return 0;
	}

	rq->ticks_without_resched++;
	resched_latency = now - rq->last_seen_need_resched_ns;
	if (resched_latency <= latency_warn_ms * NSEC_PER_MSEC)
		return 0;

	warned_once = true;

	return resched_latency;
}

static int __init setup_resched_latency_warn_ms(char *str)
{
	long val;

	if ((kstrtol(str, 0, &val))) {
		pr_warn("Unable to set resched_latency_warn_ms\n");
		return 1;
	}

	sysctl_resched_latency_warn_ms = val;
	return 1;
}
__setup("resched_latency_warn_ms=", setup_resched_latency_warn_ms);
#else
static inline u64 cpu_resched_latency(struct rq *rq) { return 0; }
#endif /* CONFIG_SCHED_DEBUG */

/*
 * This function gets called by the timer code, with HZ frequency.
 * We call it with interrupts disabled.
 */
void sched_tick(void)
{
	int cpu __maybe_unused = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	struct task_struct *curr = rq->curr;
	u64 resched_latency;

	if (housekeeping_cpu(cpu, HK_TYPE_TICK))
		arch_scale_freq_tick();

	sched_clock_tick();

	raw_spin_lock(&rq->lock);
	update_rq_clock(rq);

	scheduler_task_tick(rq);
	if (sched_feat(LATENCY_WARN))
		resched_latency = cpu_resched_latency(rq);
	calc_global_load_tick(rq);

	task_tick_mm_cid(rq, rq->curr);

	raw_spin_unlock(&rq->lock);

	if (sched_feat(LATENCY_WARN) && resched_latency)
		resched_latency_warn(cpu, resched_latency);

	perf_event_task_tick();

	if (curr->flags & PF_WQ_WORKER)
		wq_worker_tick(curr);
}

#ifdef CONFIG_NO_HZ_FULL

struct tick_work {
	int			cpu;
	atomic_t		state;
	struct delayed_work	work;
};
/* Values for ->state, see diagram below. */
#define TICK_SCHED_REMOTE_OFFLINE	0
#define TICK_SCHED_REMOTE_OFFLINING	1
#define TICK_SCHED_REMOTE_RUNNING	2

/*
 * State diagram for ->state:
 *
 *
 *          TICK_SCHED_REMOTE_OFFLINE
 *                    |   ^
 *                    |   |
 *                    |   | sched_tick_remote()
 *                    |   |
 *                    |   |
 *                    +--TICK_SCHED_REMOTE_OFFLINING
 *                    |   ^
 *                    |   |
 * sched_tick_start() |   | sched_tick_stop()
 *                    |   |
 *                    V   |
 *          TICK_SCHED_REMOTE_RUNNING
 *
 *
 * Other transitions get WARN_ON_ONCE(), except that sched_tick_remote()
 * and sched_tick_start() are happy to leave the state in RUNNING.
 */

static struct tick_work __percpu *tick_work_cpu;

static void sched_tick_remote(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct tick_work *twork = container_of(dwork, struct tick_work, work);
	int cpu = twork->cpu;
	struct rq *rq = cpu_rq(cpu);
	int os;

	/*
	 * Handle the tick only if it appears the remote CPU is running in full
	 * dynticks mode. The check is racy by nature, but missing a tick or
	 * having one too much is no big deal because the scheduler tick updates
	 * statistics and checks timeslices in a time-independent way, regardless
	 * of when exactly it is running.
	 */
	if (tick_nohz_tick_stopped_cpu(cpu)) {
		guard(raw_spinlock_irqsave)(&rq->lock);
		struct task_struct *curr = rq->curr;

		if (cpu_online(cpu)) {
			update_rq_clock(rq);

			if (!is_idle_task(curr)) {
				/*
				 * Make sure the next tick runs within a
				 * reasonable amount of time.
				 */
				u64 delta = rq_clock_task(rq) - curr->last_ran;
				WARN_ON_ONCE(delta > (u64)NSEC_PER_SEC * 3);
			}
			scheduler_task_tick(rq);

			calc_load_nohz_remote(rq);
		}
	}

	/*
	 * Run the remote tick once per second (1Hz). This arbitrary
	 * frequency is large enough to avoid overload but short enough
	 * to keep scheduler internal stats reasonably up to date.  But
	 * first update state to reflect hotplug activity if required.
	 */
	os = atomic_fetch_add_unless(&twork->state, -1, TICK_SCHED_REMOTE_RUNNING);
	WARN_ON_ONCE(os == TICK_SCHED_REMOTE_OFFLINE);
	if (os == TICK_SCHED_REMOTE_RUNNING)
		queue_delayed_work(system_unbound_wq, dwork, HZ);
}

static void sched_tick_start(int cpu)
{
	int os;
	struct tick_work *twork;

	if (housekeeping_cpu(cpu, HK_TYPE_TICK))
		return;

	WARN_ON_ONCE(!tick_work_cpu);

	twork = per_cpu_ptr(tick_work_cpu, cpu);
	os = atomic_xchg(&twork->state, TICK_SCHED_REMOTE_RUNNING);
	WARN_ON_ONCE(os == TICK_SCHED_REMOTE_RUNNING);
	if (os == TICK_SCHED_REMOTE_OFFLINE) {
		twork->cpu = cpu;
		INIT_DELAYED_WORK(&twork->work, sched_tick_remote);
		queue_delayed_work(system_unbound_wq, &twork->work, HZ);
	}
}

#ifdef CONFIG_HOTPLUG_CPU
static void sched_tick_stop(int cpu)
{
	struct tick_work *twork;
	int os;

	if (housekeeping_cpu(cpu, HK_TYPE_TICK))
		return;

	WARN_ON_ONCE(!tick_work_cpu);

	twork = per_cpu_ptr(tick_work_cpu, cpu);
	/* There cannot be competing actions, but don't rely on stop-machine. */
	os = atomic_xchg(&twork->state, TICK_SCHED_REMOTE_OFFLINING);
	WARN_ON_ONCE(os != TICK_SCHED_REMOTE_RUNNING);
	/* Don't cancel, as this would mess up the state machine. */
}
#endif /* CONFIG_HOTPLUG_CPU */

int __init sched_tick_offload_init(void)
{
	tick_work_cpu = alloc_percpu(struct tick_work);
	BUG_ON(!tick_work_cpu);
	return 0;
}

#else /* !CONFIG_NO_HZ_FULL */
static inline void sched_tick_start(int cpu) { }
static inline void sched_tick_stop(int cpu) { }
#endif

#if defined(CONFIG_PREEMPTION) && (defined(CONFIG_DEBUG_PREEMPT) || \
				defined(CONFIG_PREEMPT_TRACER))
/*
 * If the value passed in is equal to the current preempt count
 * then we just disabled preemption. Start timing the latency.
 */
static inline void preempt_latency_start(int val)
{
	if (preempt_count() == val) {
		unsigned long ip = get_lock_parent_ip();
#ifdef CONFIG_DEBUG_PREEMPT
		current->preempt_disable_ip = ip;
#endif
		trace_preempt_off(CALLER_ADDR0, ip);
	}
}

void preempt_count_add(int val)
{
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Underflow?
	 */
	if (DEBUG_LOCKS_WARN_ON((preempt_count() < 0)))
		return;
#endif
	__preempt_count_add(val);
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Spinlock count overflowing soon?
	 */
	DEBUG_LOCKS_WARN_ON((preempt_count() & PREEMPT_MASK) >=
				PREEMPT_MASK - 10);
#endif
	preempt_latency_start(val);
}
EXPORT_SYMBOL(preempt_count_add);
NOKPROBE_SYMBOL(preempt_count_add);

/*
 * If the value passed in equals to the current preempt count
 * then we just enabled preemption. Stop timing the latency.
 */
static inline void preempt_latency_stop(int val)
{
	if (preempt_count() == val)
		trace_preempt_on(CALLER_ADDR0, get_lock_parent_ip());
}

void preempt_count_sub(int val)
{
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Underflow?
	 */
	if (DEBUG_LOCKS_WARN_ON(val > preempt_count()))
		return;
	/*
	 * Is the spinlock portion underflowing?
	 */
	if (DEBUG_LOCKS_WARN_ON((val < PREEMPT_MASK) &&
			!(preempt_count() & PREEMPT_MASK)))
		return;
#endif

	preempt_latency_stop(val);
	__preempt_count_sub(val);
}
EXPORT_SYMBOL(preempt_count_sub);
NOKPROBE_SYMBOL(preempt_count_sub);

#else
static inline void preempt_latency_start(int val) { }
static inline void preempt_latency_stop(int val) { }
#endif

static inline unsigned long get_preempt_disable_ip(struct task_struct *p)
{
#ifdef CONFIG_DEBUG_PREEMPT
	return p->preempt_disable_ip;
#else
	return 0;
#endif
}

/*
 * Print scheduling while atomic bug:
 */
static noinline void __schedule_bug(struct task_struct *prev)
{
	/* Save this before calling printk(), since that will clobber it */
	unsigned long preempt_disable_ip = get_preempt_disable_ip(current);

	if (oops_in_progress)
		return;

	printk(KERN_ERR "BUG: scheduling while atomic: %s/%d/0x%08x\n",
		prev->comm, prev->pid, preempt_count());

	debug_show_held_locks(prev);
	print_modules();
	if (irqs_disabled())
		print_irqtrace_events(prev);
	if (IS_ENABLED(CONFIG_DEBUG_PREEMPT)) {
		pr_err("Preemption disabled at:");
		print_ip_sym(KERN_ERR, preempt_disable_ip);
	}
	check_panic_on_warn("scheduling while atomic");

	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}

/*
 * Various schedule()-time debugging checks and statistics:
 */
static inline void schedule_debug(struct task_struct *prev, bool preempt)
{
#ifdef CONFIG_SCHED_STACK_END_CHECK
	if (task_stack_end_corrupted(prev))
		panic("corrupted stack end detected inside scheduler\n");

	if (task_scs_end_corrupted(prev))
		panic("corrupted shadow stack detected inside scheduler\n");
#endif

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
	if (!preempt && READ_ONCE(prev->__state) && prev->non_block_count) {
		printk(KERN_ERR "BUG: scheduling in a non-blocking section: %s/%d/%i\n",
			prev->comm, prev->pid, prev->non_block_count);
		dump_stack();
		add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
	}
#endif

	if (unlikely(in_atomic_preempt_off())) {
		__schedule_bug(prev);
		preempt_count_set(PREEMPT_DISABLED);
	}
	rcu_sleep_check();
	SCHED_WARN_ON(ct_state() == CT_STATE_USER);

	profile_hit(SCHED_PROFILING, __builtin_return_address(0));

	schedstat_inc(this_rq()->sched_count);
}

#ifdef ALT_SCHED_DEBUG
void alt_sched_debug(void)
{
	printk(KERN_INFO "sched: pending: 0x%04lx, idle: 0x%04lx, sg_idle: 0x%04lx,"
	       " ecore_idle: 0x%04lx\n",
	       sched_rq_pending_mask.bits[0],
	       sched_idle_mask->bits[0],
	       sched_pcore_idle_mask->bits[0],
	       sched_ecore_idle_mask->bits[0]);
}
#endif

#ifdef	CONFIG_SMP

#ifdef CONFIG_PREEMPT_RT
#define SCHED_NR_MIGRATE_BREAK 8
#else
#define SCHED_NR_MIGRATE_BREAK 32
#endif

const_debug unsigned int sysctl_sched_nr_migrate = SCHED_NR_MIGRATE_BREAK;

/*
 * Migrate pending tasks in @rq to @dest_cpu
 */
static inline int
migrate_pending_tasks(struct rq *rq, struct rq *dest_rq, const int dest_cpu)
{
	struct task_struct *p, *skip = rq->curr;
	int nr_migrated = 0;
	int nr_tries = min(rq->nr_running / 2, sysctl_sched_nr_migrate);

	/* WA to check rq->curr is still on rq */
	if (!task_on_rq_queued(skip))
		return 0;

	while (skip != rq->idle && nr_tries &&
	       (p = sched_rq_next_task(skip, rq)) != rq->idle) {
		skip = sched_rq_next_task(p, rq);
		if (cpumask_test_cpu(dest_cpu, p->cpus_ptr)) {
			__SCHED_DEQUEUE_TASK(p, rq, 0, );
			set_task_cpu(p, dest_cpu);
			sched_task_sanity_check(p, dest_rq);
			sched_mm_cid_migrate_to(dest_rq, p);
			__SCHED_ENQUEUE_TASK(p, dest_rq, 0, );
			nr_migrated++;
		}
		nr_tries--;
	}

	return nr_migrated;
}

static inline int take_other_rq_tasks(struct rq *rq, int cpu)
{
	cpumask_t *topo_mask, *end_mask, chk;

	if (unlikely(!rq->online))
		return 0;

	if (cpumask_empty(&sched_rq_pending_mask))
		return 0;

	topo_mask = per_cpu(sched_cpu_topo_masks, cpu);
	end_mask = per_cpu(sched_cpu_topo_end_mask, cpu);
	do {
		int i;

		if (!cpumask_and(&chk, &sched_rq_pending_mask, topo_mask))
			continue;

		for_each_cpu_wrap(i, &chk, cpu) {
			int nr_migrated;
			struct rq *src_rq;

			src_rq = cpu_rq(i);
			if (!do_raw_spin_trylock(&src_rq->lock))
				continue;
			spin_acquire(&src_rq->lock.dep_map,
				     SINGLE_DEPTH_NESTING, 1, _RET_IP_);

			if ((nr_migrated = migrate_pending_tasks(src_rq, rq, cpu))) {
				src_rq->nr_running -= nr_migrated;
				if (src_rq->nr_running < 2)
					cpumask_clear_cpu(i, &sched_rq_pending_mask);

				spin_release(&src_rq->lock.dep_map, _RET_IP_);
				do_raw_spin_unlock(&src_rq->lock);

				rq->nr_running += nr_migrated;
				if (rq->nr_running > 1)
					cpumask_set_cpu(cpu, &sched_rq_pending_mask);

				update_sched_preempt_mask(rq);
				cpufreq_update_util(rq, 0);

				return 1;
			}

			spin_release(&src_rq->lock.dep_map, _RET_IP_);
			do_raw_spin_unlock(&src_rq->lock);
		}
	} while (++topo_mask < end_mask);

	return 0;
}
#endif

static inline void time_slice_expired(struct task_struct *p, struct rq *rq)
{
	p->time_slice = sysctl_sched_base_slice;

	sched_task_renew(p, rq);

	if (SCHED_FIFO != p->policy && task_on_rq_queued(p))
		requeue_task(p, rq);
}

/*
 * Timeslices below RESCHED_NS are considered as good as expired as there's no
 * point rescheduling when there's so little time left.
 */
static inline void check_curr(struct task_struct *p, struct rq *rq)
{
	if (unlikely(rq->idle == p))
		return;

	update_curr(rq, p);

	if (p->time_slice < RESCHED_NS)
		time_slice_expired(p, rq);
}

static inline struct task_struct *
choose_next_task(struct rq *rq, int cpu)
{
	struct task_struct *next = sched_rq_first_task(rq);

	if (next == rq->idle) {
#ifdef	CONFIG_SMP
		if (!take_other_rq_tasks(rq, cpu)) {
			if (likely(rq->balance_func && rq->online))
				rq->balance_func(rq, cpu);
#endif /* CONFIG_SMP */

			schedstat_inc(rq->sched_goidle);
			/*printk(KERN_INFO "sched: choose_next_task(%d) idle %px\n", cpu, next);*/
			return next;
#ifdef	CONFIG_SMP
		}
		next = sched_rq_first_task(rq);
#endif
	}
#ifdef CONFIG_HIGH_RES_TIMERS
	hrtick_start(rq, next->time_slice);
#endif
	/*printk(KERN_INFO "sched: choose_next_task(%d) next %px\n", cpu, next);*/
	return next;
}

/*
 * Constants for the sched_mode argument of __schedule().
 *
 * The mode argument allows RT enabled kernels to differentiate a
 * preemption from blocking on an 'sleeping' spin/rwlock.
 */
 #define SM_IDLE		(-1)
 #define SM_NONE		0
 #define SM_PREEMPT		1
 #define SM_RTLOCK_WAIT		2

/*
 * schedule() is the main scheduler function.
 *
 * The main means of driving the scheduler and thus entering this function are:
 *
 *   1. Explicit blocking: mutex, semaphore, waitqueue, etc.
 *
 *   2. TIF_NEED_RESCHED flag is checked on interrupt and userspace return
 *      paths. For example, see arch/x86/entry_64.S.
 *
 *      To drive preemption between tasks, the scheduler sets the flag in timer
 *      interrupt handler sched_tick().
 *
 *   3. Wakeups don't really cause entry into schedule(). They add a
 *      task to the run-queue and that's it.
 *
 *      Now, if the new task added to the run-queue preempts the current
 *      task, then the wakeup sets TIF_NEED_RESCHED and schedule() gets
 *      called on the nearest possible occasion:
 *
 *       - If the kernel is preemptible (CONFIG_PREEMPTION=y):
 *
 *         - in syscall or exception context, at the next outmost
 *           preempt_enable(). (this might be as soon as the wake_up()'s
 *           spin_unlock()!)
 *
 *         - in IRQ context, return from interrupt-handler to
 *           preemptible context
 *
 *       - If the kernel is not preemptible (CONFIG_PREEMPTION is not set)
 *         then at the next:
 *
 *          - cond_resched() call
 *          - explicit schedule() call
 *          - return from syscall or exception to user-space
 *          - return from interrupt-handler to user-space
 *
 * WARNING: must be called with preemption disabled!
 */
static void __sched notrace __schedule(int sched_mode)
{
	struct task_struct *prev, *next;
	/*
	 * On PREEMPT_RT kernel, SM_RTLOCK_WAIT is noted
	 * as a preemption by schedule_debug() and RCU.
	 */
	bool preempt = sched_mode > SM_NONE;
	unsigned long *switch_count;
	unsigned long prev_state;
	struct rq *rq;
	int cpu;

	cpu = smp_processor_id();
	rq = cpu_rq(cpu);
	prev = rq->curr;

	schedule_debug(prev, preempt);

	/* by passing sched_feat(HRTICK) checking which Alt schedule FW doesn't support */
	hrtick_clear(rq);

	local_irq_disable();
	rcu_note_context_switch(preempt);

	/*
	 * Make sure that signal_pending_state()->signal_pending() below
	 * can't be reordered with __set_current_state(TASK_INTERRUPTIBLE)
	 * done by the caller to avoid the race with signal_wake_up():
	 *
	 * __set_current_state(@state)		signal_wake_up()
	 * schedule()				  set_tsk_thread_flag(p, TIF_SIGPENDING)
	 *					  wake_up_state(p, state)
	 *   LOCK rq->lock			    LOCK p->pi_state
	 *   smp_mb__after_spinlock()		    smp_mb__after_spinlock()
	 *     if (signal_pending_state())	    if (p->state & @state)
	 *
	 * Also, the membarrier system call requires a full memory barrier
	 * after coming from user-space, before storing to rq->curr; this
	 * barrier matches a full barrier in the proximity of the membarrier
	 * system call exit.
	 */
	raw_spin_lock(&rq->lock);
	smp_mb__after_spinlock();

	update_rq_clock(rq);

	switch_count = &prev->nivcsw;

	/* Task state changes only considers SM_PREEMPT as preemption */
	preempt = sched_mode == SM_PREEMPT;

	/*
	 * We must load prev->state once (task_struct::state is volatile), such
	 * that we form a control dependency vs deactivate_task() below.
	 */
	prev_state = READ_ONCE(prev->__state);
	if (sched_mode == SM_IDLE) {
		if (!rq->nr_running) {
			next = prev;
			goto picked;
		}
	} else if (!preempt && prev_state) {
		if (signal_pending_state(prev_state, prev)) {
			WRITE_ONCE(prev->__state, TASK_RUNNING);
		} else {
			prev->sched_contributes_to_load =
				(prev_state & TASK_UNINTERRUPTIBLE) &&
				!(prev_state & TASK_NOLOAD) &&
				!(prev_state & TASK_FROZEN);

			/*
			 * __schedule()			ttwu()
			 *   prev_state = prev->state;    if (p->on_rq && ...)
			 *   if (prev_state)		    goto out;
			 *     p->on_rq = 0;		  smp_acquire__after_ctrl_dep();
			 *				  p->state = TASK_WAKING
			 *
			 * Where __schedule() and ttwu() have matching control dependencies.
			 *
			 * After this, schedule() must not care about p->state any more.
			 */
			sched_task_deactivate(prev, rq);
			block_task(rq, prev);
		}
		switch_count = &prev->nvcsw;
	}

	check_curr(prev, rq);

	next = choose_next_task(rq, cpu);
picked:
	clear_tsk_need_resched(prev);
	clear_preempt_need_resched();
#ifdef CONFIG_SCHED_DEBUG
	rq->last_seen_need_resched_ns = 0;
#endif

	if (likely(prev != next)) {
		next->last_ran = rq->clock_task;

		/*printk(KERN_INFO "sched: %px -> %px\n", prev, next);*/
		rq->nr_switches++;
		/*
		 * RCU users of rcu_dereference(rq->curr) may not see
		 * changes to task_struct made by pick_next_task().
		 */
		RCU_INIT_POINTER(rq->curr, next);
		/*
		 * The membarrier system call requires each architecture
		 * to have a full memory barrier after updating
		 * rq->curr, before returning to user-space.
		 *
		 * Here are the schemes providing that barrier on the
		 * various architectures:
		 * - mm ? switch_mm() : mmdrop() for x86, s390, sparc, PowerPC,
		 *   RISC-V.  switch_mm() relies on membarrier_arch_switch_mm()
		 *   on PowerPC and on RISC-V.
		 * - finish_lock_switch() for weakly-ordered
		 *   architectures where spin_unlock is a full barrier,
		 * - switch_to() for arm64 (weakly-ordered, spin_unlock
		 *   is a RELEASE barrier),
		 *
		 * The barrier matches a full barrier in the proximity of
		 * the membarrier system call entry.
		 *
		 * On RISC-V, this barrier pairing is also needed for the
		 * SYNC_CORE command when switching between processes, cf.
		 * the inline comments in membarrier_arch_switch_mm().
		 */
		++*switch_count;

		trace_sched_switch(preempt, prev, next, prev_state);

		/* Also unlocks the rq: */
		rq = context_switch(rq, prev, next);

		cpu = cpu_of(rq);
	} else {
		__balance_callbacks(rq);
		raw_spin_unlock_irq(&rq->lock);
	}
}

void __noreturn do_task_dead(void)
{
	/* Causes final put_task_struct in finish_task_switch(): */
	set_special_state(TASK_DEAD);

	/* Tell freezer to ignore us: */
	current->flags |= PF_NOFREEZE;

	__schedule(SM_NONE);
	BUG();

	/* Avoid "noreturn function does return" - but don't continue if BUG() is a NOP: */
	for (;;)
		cpu_relax();
}

static inline void sched_submit_work(struct task_struct *tsk)
{
	static DEFINE_WAIT_OVERRIDE_MAP(sched_map, LD_WAIT_CONFIG);
	unsigned int task_flags;

	/*
	 * Establish LD_WAIT_CONFIG context to ensure none of the code called
	 * will use a blocking primitive -- which would lead to recursion.
	 */
	lock_map_acquire_try(&sched_map);

	task_flags = tsk->flags;
	/*
	 * If a worker goes to sleep, notify and ask workqueue whether it
	 * wants to wake up a task to maintain concurrency.
	 */
	if (task_flags & PF_WQ_WORKER)
		wq_worker_sleeping(tsk);
	else if (task_flags & PF_IO_WORKER)
		io_wq_worker_sleeping(tsk);

	/*
	 * spinlock and rwlock must not flush block requests.  This will
	 * deadlock if the callback attempts to acquire a lock which is
	 * already acquired.
	 */
	SCHED_WARN_ON(current->__state & TASK_RTLOCK_WAIT);

	/*
	 * If we are going to sleep and we have plugged IO queued,
	 * make sure to submit it to avoid deadlocks.
	 */
	blk_flush_plug(tsk->plug, true);

	lock_map_release(&sched_map);
}

static void sched_update_worker(struct task_struct *tsk)
{
	if (tsk->flags & (PF_WQ_WORKER | PF_IO_WORKER | PF_BLOCK_TS)) {
		if (tsk->flags & PF_BLOCK_TS)
			blk_plug_invalidate_ts(tsk);
		if (tsk->flags & PF_WQ_WORKER)
			wq_worker_running(tsk);
		else if (tsk->flags & PF_IO_WORKER)
			io_wq_worker_running(tsk);
	}
}

static __always_inline void __schedule_loop(int sched_mode)
{
	do {
		preempt_disable();
		__schedule(sched_mode);
		sched_preempt_enable_no_resched();
	} while (need_resched());
}

asmlinkage __visible void __sched schedule(void)
{
	struct task_struct *tsk = current;

#ifdef CONFIG_RT_MUTEXES
	lockdep_assert(!tsk->sched_rt_mutex);
#endif

	if (!task_is_running(tsk))
		sched_submit_work(tsk);
	__schedule_loop(SM_NONE);
	sched_update_worker(tsk);
}
EXPORT_SYMBOL(schedule);

/*
 * synchronize_rcu_tasks() makes sure that no task is stuck in preempted
 * state (have scheduled out non-voluntarily) by making sure that all
 * tasks have either left the run queue or have gone into user space.
 * As idle tasks do not do either, they must not ever be preempted
 * (schedule out non-voluntarily).
 *
 * schedule_idle() is similar to schedule_preempt_disable() except that it
 * never enables preemption because it does not call sched_submit_work().
 */
void __sched schedule_idle(void)
{
	/*
	 * As this skips calling sched_submit_work(), which the idle task does
	 * regardless because that function is a NOP when the task is in a
	 * TASK_RUNNING state, make sure this isn't used someplace that the
	 * current task can be in any other state. Note, idle is always in the
	 * TASK_RUNNING state.
	 */
	WARN_ON_ONCE(current->__state);
	do {
		__schedule(SM_IDLE);
	} while (need_resched());
}

#if defined(CONFIG_CONTEXT_TRACKING_USER) && !defined(CONFIG_HAVE_CONTEXT_TRACKING_USER_OFFSTACK)
asmlinkage __visible void __sched schedule_user(void)
{
	/*
	 * If we come here after a random call to set_need_resched(),
	 * or we have been woken up remotely but the IPI has not yet arrived,
	 * we haven't yet exited the RCU idle mode. Do it here manually until
	 * we find a better solution.
	 *
	 * NB: There are buggy callers of this function.  Ideally we
	 * should warn if prev_state != CT_STATE_USER, but that will trigger
	 * too frequently to make sense yet.
	 */
	enum ctx_state prev_state = exception_enter();
	schedule();
	exception_exit(prev_state);
}
#endif

/**
 * schedule_preempt_disabled - called with preemption disabled
 *
 * Returns with preemption disabled. Note: preempt_count must be 1
 */
void __sched schedule_preempt_disabled(void)
{
	sched_preempt_enable_no_resched();
	schedule();
	preempt_disable();
}

#ifdef CONFIG_PREEMPT_RT
void __sched notrace schedule_rtlock(void)
{
	__schedule_loop(SM_RTLOCK_WAIT);
}
NOKPROBE_SYMBOL(schedule_rtlock);
#endif

static void __sched notrace preempt_schedule_common(void)
{
	do {
		/*
		 * Because the function tracer can trace preempt_count_sub()
		 * and it also uses preempt_enable/disable_notrace(), if
		 * NEED_RESCHED is set, the preempt_enable_notrace() called
		 * by the function tracer will call this function again and
		 * cause infinite recursion.
		 *
		 * Preemption must be disabled here before the function
		 * tracer can trace. Break up preempt_disable() into two
		 * calls. One to disable preemption without fear of being
		 * traced. The other to still record the preemption latency,
		 * which can also be traced by the function tracer.
		 */
		preempt_disable_notrace();
		preempt_latency_start(1);
		__schedule(SM_PREEMPT);
		preempt_latency_stop(1);
		preempt_enable_no_resched_notrace();

		/*
		 * Check again in case we missed a preemption opportunity
		 * between schedule and now.
		 */
	} while (need_resched());
}

#ifdef CONFIG_PREEMPTION
/*
 * This is the entry point to schedule() from in-kernel preemption
 * off of preempt_enable.
 */
asmlinkage __visible void __sched notrace preempt_schedule(void)
{
	/*
	 * If there is a non-zero preempt_count or interrupts are disabled,
	 * we do not want to preempt the current task. Just return..
	 */
	if (likely(!preemptible()))
		return;

	preempt_schedule_common();
}
NOKPROBE_SYMBOL(preempt_schedule);
EXPORT_SYMBOL(preempt_schedule);

#ifdef CONFIG_PREEMPT_DYNAMIC
#if defined(CONFIG_HAVE_PREEMPT_DYNAMIC_CALL)
#ifndef preempt_schedule_dynamic_enabled
#define preempt_schedule_dynamic_enabled	preempt_schedule
#define preempt_schedule_dynamic_disabled	NULL
#endif
DEFINE_STATIC_CALL(preempt_schedule, preempt_schedule_dynamic_enabled);
EXPORT_STATIC_CALL_TRAMP(preempt_schedule);
#elif defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)
static DEFINE_STATIC_KEY_TRUE(sk_dynamic_preempt_schedule);
void __sched notrace dynamic_preempt_schedule(void)
{
	if (!static_branch_unlikely(&sk_dynamic_preempt_schedule))
		return;
	preempt_schedule();
}
NOKPROBE_SYMBOL(dynamic_preempt_schedule);
EXPORT_SYMBOL(dynamic_preempt_schedule);
#endif
#endif

/**
 * preempt_schedule_notrace - preempt_schedule called by tracing
 *
 * The tracing infrastructure uses preempt_enable_notrace to prevent
 * recursion and tracing preempt enabling caused by the tracing
 * infrastructure itself. But as tracing can happen in areas coming
 * from userspace or just about to enter userspace, a preempt enable
 * can occur before user_exit() is called. This will cause the scheduler
 * to be called when the system is still in usermode.
 *
 * To prevent this, the preempt_enable_notrace will use this function
 * instead of preempt_schedule() to exit user context if needed before
 * calling the scheduler.
 */
asmlinkage __visible void __sched notrace preempt_schedule_notrace(void)
{
	enum ctx_state prev_ctx;

	if (likely(!preemptible()))
		return;

	do {
		/*
		 * Because the function tracer can trace preempt_count_sub()
		 * and it also uses preempt_enable/disable_notrace(), if
		 * NEED_RESCHED is set, the preempt_enable_notrace() called
		 * by the function tracer will call this function again and
		 * cause infinite recursion.
		 *
		 * Preemption must be disabled here before the function
		 * tracer can trace. Break up preempt_disable() into two
		 * calls. One to disable preemption without fear of being
		 * traced. The other to still record the preemption latency,
		 * which can also be traced by the function tracer.
		 */
		preempt_disable_notrace();
		preempt_latency_start(1);
		/*
		 * Needs preempt disabled in case user_exit() is traced
		 * and the tracer calls preempt_enable_notrace() causing
		 * an infinite recursion.
		 */
		prev_ctx = exception_enter();
		__schedule(SM_PREEMPT);
		exception_exit(prev_ctx);

		preempt_latency_stop(1);
		preempt_enable_no_resched_notrace();
	} while (need_resched());
}
EXPORT_SYMBOL_GPL(preempt_schedule_notrace);

#ifdef CONFIG_PREEMPT_DYNAMIC
#if defined(CONFIG_HAVE_PREEMPT_DYNAMIC_CALL)
#ifndef preempt_schedule_notrace_dynamic_enabled
#define preempt_schedule_notrace_dynamic_enabled	preempt_schedule_notrace
#define preempt_schedule_notrace_dynamic_disabled	NULL
#endif
DEFINE_STATIC_CALL(preempt_schedule_notrace, preempt_schedule_notrace_dynamic_enabled);
EXPORT_STATIC_CALL_TRAMP(preempt_schedule_notrace);
#elif defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)
static DEFINE_STATIC_KEY_TRUE(sk_dynamic_preempt_schedule_notrace);
void __sched notrace dynamic_preempt_schedule_notrace(void)
{
	if (!static_branch_unlikely(&sk_dynamic_preempt_schedule_notrace))
		return;
	preempt_schedule_notrace();
}
NOKPROBE_SYMBOL(dynamic_preempt_schedule_notrace);
EXPORT_SYMBOL(dynamic_preempt_schedule_notrace);
#endif
#endif

#endif /* CONFIG_PREEMPTION */

/*
 * This is the entry point to schedule() from kernel preemption
 * off of IRQ context.
 * Note, that this is called and return with IRQs disabled. This will
 * protect us against recursive calling from IRQ contexts.
 */
asmlinkage __visible void __sched preempt_schedule_irq(void)
{
	enum ctx_state prev_state;

	/* Catch callers which need to be fixed */
	BUG_ON(preempt_count() || !irqs_disabled());

	prev_state = exception_enter();

	do {
		preempt_disable();
		local_irq_enable();
		__schedule(SM_PREEMPT);
		local_irq_disable();
		sched_preempt_enable_no_resched();
	} while (need_resched());

	exception_exit(prev_state);
}

int default_wake_function(wait_queue_entry_t *curr, unsigned mode, int wake_flags,
			  void *key)
{
	WARN_ON_ONCE(IS_ENABLED(CONFIG_SCHED_DEBUG) && wake_flags & ~(WF_SYNC|WF_CURRENT_CPU));
	return try_to_wake_up(curr->private, mode, wake_flags);
}
EXPORT_SYMBOL(default_wake_function);

void check_task_changed(struct task_struct *p, struct rq *rq)
{
	/* Trigger resched if task sched_prio has been modified. */
	if (task_on_rq_queued(p)) {
		update_rq_clock(rq);
		requeue_task(p, rq);
		wakeup_preempt(rq);
	}
}

void __setscheduler_prio(struct task_struct *p, int prio)
{
	p->prio = prio;
}

#ifdef CONFIG_RT_MUTEXES

/*
 * Would be more useful with typeof()/auto_type but they don't mix with
 * bit-fields. Since it's a local thing, use int. Keep the generic sounding
 * name such that if someone were to implement this function we get to compare
 * notes.
 */
#define fetch_and_set(x, v) ({ int _x = (x); (x) = (v); _x; })

void rt_mutex_pre_schedule(void)
{
	lockdep_assert(!fetch_and_set(current->sched_rt_mutex, 1));
	sched_submit_work(current);
}

void rt_mutex_schedule(void)
{
	lockdep_assert(current->sched_rt_mutex);
	__schedule_loop(SM_NONE);
}

void rt_mutex_post_schedule(void)
{
	sched_update_worker(current);
	lockdep_assert(fetch_and_set(current->sched_rt_mutex, 0));
}

/*
 * rt_mutex_setprio - set the current priority of a task
 * @p: task to boost
 * @pi_task: donor task
 *
 * This function changes the 'effective' priority of a task. It does
 * not touch ->normal_prio like __setscheduler().
 *
 * Used by the rt_mutex code to implement priority inheritance
 * logic. Call site only calls if the priority of the task changed.
 */
void rt_mutex_setprio(struct task_struct *p, struct task_struct *pi_task)
{
	int prio;
	struct rq *rq;
	raw_spinlock_t *lock;

	/* XXX used to be waiter->prio, not waiter->task->prio */
	prio = __rt_effective_prio(pi_task, p->normal_prio);

	/*
	 * If nothing changed; bail early.
	 */
	if (p->pi_top_task == pi_task && prio == p->prio)
		return;

	rq = __task_access_lock(p, &lock);
	/*
	 * Set under pi_lock && rq->lock, such that the value can be used under
	 * either lock.
	 *
	 * Note that there is loads of tricky to make this pointer cache work
	 * right. rt_mutex_slowunlock()+rt_mutex_postunlock() work together to
	 * ensure a task is de-boosted (pi_task is set to NULL) before the
	 * task is allowed to run again (and can exit). This ensures the pointer
	 * points to a blocked task -- which guarantees the task is present.
	 */
	p->pi_top_task = pi_task;

	/*
	 * For FIFO/RR we only need to set prio, if that matches we're done.
	 */
	if (prio == p->prio)
		goto out_unlock;

	/*
	 * Idle task boosting is a no-no in general. There is one
	 * exception, when PREEMPT_RT and NOHZ is active:
	 *
	 * The idle task calls get_next_timer_interrupt() and holds
	 * the timer wheel base->lock on the CPU and another CPU wants
	 * to access the timer (probably to cancel it). We can safely
	 * ignore the boosting request, as the idle CPU runs this code
	 * with interrupts disabled and will complete the lock
	 * protected section without being interrupted. So there is no
	 * real need to boost.
	 */
	if (unlikely(p == rq->idle)) {
		WARN_ON(p != rq->curr);
		WARN_ON(p->pi_blocked_on);
		goto out_unlock;
	}

	trace_sched_pi_setprio(p, pi_task);

	__setscheduler_prio(p, prio);

	check_task_changed(p, rq);
out_unlock:
	/* Avoid rq from going away on us: */
	preempt_disable();

	if (task_on_rq_queued(p))
		__balance_callbacks(rq);
	__task_access_unlock(p, lock);

	preempt_enable();
}
#endif

#if !defined(CONFIG_PREEMPTION) || defined(CONFIG_PREEMPT_DYNAMIC)
int __sched __cond_resched(void)
{
	if (should_resched(0)) {
		preempt_schedule_common();
		return 1;
	}
	/*
	 * In preemptible kernels, ->rcu_read_lock_nesting tells the tick
	 * whether the current CPU is in an RCU read-side critical section,
	 * so the tick can report quiescent states even for CPUs looping
	 * in kernel context.  In contrast, in non-preemptible kernels,
	 * RCU readers leave no in-memory hints, which means that CPU-bound
	 * processes executing in kernel context might never report an
	 * RCU quiescent state.  Therefore, the following code causes
	 * cond_resched() to report a quiescent state, but only when RCU
	 * is in urgent need of one.
	 */
#ifndef CONFIG_PREEMPT_RCU
	rcu_all_qs();
#endif
	return 0;
}
EXPORT_SYMBOL(__cond_resched);
#endif

#ifdef CONFIG_PREEMPT_DYNAMIC
#if defined(CONFIG_HAVE_PREEMPT_DYNAMIC_CALL)
#define cond_resched_dynamic_enabled	__cond_resched
#define cond_resched_dynamic_disabled	((void *)&__static_call_return0)
DEFINE_STATIC_CALL_RET0(cond_resched, __cond_resched);
EXPORT_STATIC_CALL_TRAMP(cond_resched);

#define might_resched_dynamic_enabled	__cond_resched
#define might_resched_dynamic_disabled	((void *)&__static_call_return0)
DEFINE_STATIC_CALL_RET0(might_resched, __cond_resched);
EXPORT_STATIC_CALL_TRAMP(might_resched);
#elif defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)
static DEFINE_STATIC_KEY_FALSE(sk_dynamic_cond_resched);
int __sched dynamic_cond_resched(void)
{
	klp_sched_try_switch();
	if (!static_branch_unlikely(&sk_dynamic_cond_resched))
		return 0;
	return __cond_resched();
}
EXPORT_SYMBOL(dynamic_cond_resched);

static DEFINE_STATIC_KEY_FALSE(sk_dynamic_might_resched);
int __sched dynamic_might_resched(void)
{
	if (!static_branch_unlikely(&sk_dynamic_might_resched))
		return 0;
	return __cond_resched();
}
EXPORT_SYMBOL(dynamic_might_resched);
#endif
#endif

/*
 * __cond_resched_lock() - if a reschedule is pending, drop the given lock,
 * call schedule, and on return reacquire the lock.
 *
 * This works OK both with and without CONFIG_PREEMPTION.  We do strange low-level
 * operations here to prevent schedule() from being called twice (once via
 * spin_unlock(), once by hand).
 */
int __cond_resched_lock(spinlock_t *lock)
{
	int resched = should_resched(PREEMPT_LOCK_OFFSET);
	int ret = 0;

	lockdep_assert_held(lock);

	if (spin_needbreak(lock) || resched) {
		spin_unlock(lock);
		if (!_cond_resched())
			cpu_relax();
		ret = 1;
		spin_lock(lock);
	}
	return ret;
}
EXPORT_SYMBOL(__cond_resched_lock);

int __cond_resched_rwlock_read(rwlock_t *lock)
{
	int resched = should_resched(PREEMPT_LOCK_OFFSET);
	int ret = 0;

	lockdep_assert_held_read(lock);

	if (rwlock_needbreak(lock) || resched) {
		read_unlock(lock);
		if (!_cond_resched())
			cpu_relax();
		ret = 1;
		read_lock(lock);
	}
	return ret;
}
EXPORT_SYMBOL(__cond_resched_rwlock_read);

int __cond_resched_rwlock_write(rwlock_t *lock)
{
	int resched = should_resched(PREEMPT_LOCK_OFFSET);
	int ret = 0;

	lockdep_assert_held_write(lock);

	if (rwlock_needbreak(lock) || resched) {
		write_unlock(lock);
		if (!_cond_resched())
			cpu_relax();
		ret = 1;
		write_lock(lock);
	}
	return ret;
}
EXPORT_SYMBOL(__cond_resched_rwlock_write);

#ifdef CONFIG_PREEMPT_DYNAMIC

#ifdef CONFIG_GENERIC_ENTRY
#include <linux/entry-common.h>
#endif

/*
 * SC:cond_resched
 * SC:might_resched
 * SC:preempt_schedule
 * SC:preempt_schedule_notrace
 * SC:irqentry_exit_cond_resched
 *
 *
 * NONE:
 *   cond_resched               <- __cond_resched
 *   might_resched              <- RET0
 *   preempt_schedule           <- NOP
 *   preempt_schedule_notrace   <- NOP
 *   irqentry_exit_cond_resched <- NOP
 *
 * VOLUNTARY:
 *   cond_resched               <- __cond_resched
 *   might_resched              <- __cond_resched
 *   preempt_schedule           <- NOP
 *   preempt_schedule_notrace   <- NOP
 *   irqentry_exit_cond_resched <- NOP
 *
 * FULL:
 *   cond_resched               <- RET0
 *   might_resched              <- RET0
 *   preempt_schedule           <- preempt_schedule
 *   preempt_schedule_notrace   <- preempt_schedule_notrace
 *   irqentry_exit_cond_resched <- irqentry_exit_cond_resched
 */

enum {
	preempt_dynamic_undefined = -1,
	preempt_dynamic_none,
	preempt_dynamic_voluntary,
	preempt_dynamic_full,
};

int preempt_dynamic_mode = preempt_dynamic_undefined;

int sched_dynamic_mode(const char *str)
{
	if (!strcmp(str, "none"))
		return preempt_dynamic_none;

	if (!strcmp(str, "voluntary"))
		return preempt_dynamic_voluntary;

	if (!strcmp(str, "full"))
		return preempt_dynamic_full;

	return -EINVAL;
}

#if defined(CONFIG_HAVE_PREEMPT_DYNAMIC_CALL)
#define preempt_dynamic_enable(f)	static_call_update(f, f##_dynamic_enabled)
#define preempt_dynamic_disable(f)	static_call_update(f, f##_dynamic_disabled)
#elif defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)
#define preempt_dynamic_enable(f)	static_key_enable(&sk_dynamic_##f.key)
#define preempt_dynamic_disable(f)	static_key_disable(&sk_dynamic_##f.key)
#else
#error "Unsupported PREEMPT_DYNAMIC mechanism"
#endif

static DEFINE_MUTEX(sched_dynamic_mutex);
static bool klp_override;

static void __sched_dynamic_update(int mode)
{
	/*
	 * Avoid {NONE,VOLUNTARY} -> FULL transitions from ever ending up in
	 * the ZERO state, which is invalid.
	 */
	if (!klp_override)
		preempt_dynamic_enable(cond_resched);
	preempt_dynamic_enable(cond_resched);
	preempt_dynamic_enable(might_resched);
	preempt_dynamic_enable(preempt_schedule);
	preempt_dynamic_enable(preempt_schedule_notrace);
	preempt_dynamic_enable(irqentry_exit_cond_resched);

	switch (mode) {
	case preempt_dynamic_none:
		if (!klp_override)
			preempt_dynamic_enable(cond_resched);
		preempt_dynamic_disable(might_resched);
		preempt_dynamic_disable(preempt_schedule);
		preempt_dynamic_disable(preempt_schedule_notrace);
		preempt_dynamic_disable(irqentry_exit_cond_resched);
		if (mode != preempt_dynamic_mode)
			pr_info("Dynamic Preempt: none\n");
		break;

	case preempt_dynamic_voluntary:
		if (!klp_override)
			preempt_dynamic_enable(cond_resched);
		preempt_dynamic_enable(might_resched);
		preempt_dynamic_disable(preempt_schedule);
		preempt_dynamic_disable(preempt_schedule_notrace);
		preempt_dynamic_disable(irqentry_exit_cond_resched);
		if (mode != preempt_dynamic_mode)
			pr_info("Dynamic Preempt: voluntary\n");
		break;

	case preempt_dynamic_full:
		if (!klp_override)
			preempt_dynamic_enable(cond_resched);
		preempt_dynamic_disable(might_resched);
		preempt_dynamic_enable(preempt_schedule);
		preempt_dynamic_enable(preempt_schedule_notrace);
		preempt_dynamic_enable(irqentry_exit_cond_resched);
		if (mode != preempt_dynamic_mode)
			pr_info("Dynamic Preempt: full\n");
		break;
	}

	preempt_dynamic_mode = mode;
}

void sched_dynamic_update(int mode)
{
	mutex_lock(&sched_dynamic_mutex);
	__sched_dynamic_update(mode);
	mutex_unlock(&sched_dynamic_mutex);
}

#ifdef CONFIG_HAVE_PREEMPT_DYNAMIC_CALL

static int klp_cond_resched(void)
{
	__klp_sched_try_switch();
	return __cond_resched();
}

void sched_dynamic_klp_enable(void)
{
	mutex_lock(&sched_dynamic_mutex);

	klp_override = true;
	static_call_update(cond_resched, klp_cond_resched);

	mutex_unlock(&sched_dynamic_mutex);
}

void sched_dynamic_klp_disable(void)
{
	mutex_lock(&sched_dynamic_mutex);

	klp_override = false;
	__sched_dynamic_update(preempt_dynamic_mode);

	mutex_unlock(&sched_dynamic_mutex);
}

#endif /* CONFIG_HAVE_PREEMPT_DYNAMIC_CALL */


static int __init setup_preempt_mode(char *str)
{
	int mode = sched_dynamic_mode(str);
	if (mode < 0) {
		pr_warn("Dynamic Preempt: unsupported mode: %s\n", str);
		return 0;
	}

	sched_dynamic_update(mode);
	return 1;
}
__setup("preempt=", setup_preempt_mode);

static void __init preempt_dynamic_init(void)
{
	if (preempt_dynamic_mode == preempt_dynamic_undefined) {
		if (IS_ENABLED(CONFIG_PREEMPT_NONE)) {
			sched_dynamic_update(preempt_dynamic_none);
		} else if (IS_ENABLED(CONFIG_PREEMPT_VOLUNTARY)) {
			sched_dynamic_update(preempt_dynamic_voluntary);
		} else {
			/* Default static call setting, nothing to do */
			WARN_ON_ONCE(!IS_ENABLED(CONFIG_PREEMPT));
			preempt_dynamic_mode = preempt_dynamic_full;
			pr_info("Dynamic Preempt: full\n");
		}
	}
}

#define PREEMPT_MODEL_ACCESSOR(mode) \
	bool preempt_model_##mode(void)						 \
	{									 \
		WARN_ON_ONCE(preempt_dynamic_mode == preempt_dynamic_undefined); \
		return preempt_dynamic_mode == preempt_dynamic_##mode;		 \
	}									 \
	EXPORT_SYMBOL_GPL(preempt_model_##mode)

PREEMPT_MODEL_ACCESSOR(none);
PREEMPT_MODEL_ACCESSOR(voluntary);
PREEMPT_MODEL_ACCESSOR(full);

#else /* !CONFIG_PREEMPT_DYNAMIC: */

static inline void preempt_dynamic_init(void) { }

#endif /* CONFIG_PREEMPT_DYNAMIC */

int io_schedule_prepare(void)
{
	int old_iowait = current->in_iowait;

	current->in_iowait = 1;
	blk_flush_plug(current->plug, true);
	return old_iowait;
}

void io_schedule_finish(int token)
{
	current->in_iowait = token;
}

/*
 * This task is about to go to sleep on IO.  Increment rq->nr_iowait so
 * that process accounting knows that this is a task in IO wait state.
 *
 * But don't do that if it is a deliberate, throttling IO wait (this task
 * has set its backing_dev_info: the queue against which it should throttle)
 */

long __sched io_schedule_timeout(long timeout)
{
	int token;
	long ret;

	token = io_schedule_prepare();
	ret = schedule_timeout(timeout);
	io_schedule_finish(token);

	return ret;
}
EXPORT_SYMBOL(io_schedule_timeout);

void __sched io_schedule(void)
{
	int token;

	token = io_schedule_prepare();
	schedule();
	io_schedule_finish(token);
}
EXPORT_SYMBOL(io_schedule);

void sched_show_task(struct task_struct *p)
{
	unsigned long free;
	int ppid;

	if (!try_get_task_stack(p))
		return;

	pr_info("task:%-15.15s state:%c", p->comm, task_state_to_char(p));

	if (task_is_running(p))
		pr_cont("  running task    ");
	free = stack_not_used(p);
	ppid = 0;
	rcu_read_lock();
	if (pid_alive(p))
		ppid = task_pid_nr(rcu_dereference(p->real_parent));
	rcu_read_unlock();
	pr_cont(" stack:%-5lu pid:%-5d tgid:%-5d ppid:%-6d flags:0x%08lx\n",
		free, task_pid_nr(p), task_tgid_nr(p),
		ppid, read_task_thread_flags(p));

	print_worker_info(KERN_INFO, p);
	print_stop_info(KERN_INFO, p);
	show_stack(p, NULL, KERN_INFO);
	put_task_stack(p);
}
EXPORT_SYMBOL_GPL(sched_show_task);

static inline bool
state_filter_match(unsigned long state_filter, struct task_struct *p)
{
	unsigned int state = READ_ONCE(p->__state);

	/* no filter, everything matches */
	if (!state_filter)
		return true;

	/* filter, but doesn't match */
	if (!(state & state_filter))
		return false;

	/*
	 * When looking for TASK_UNINTERRUPTIBLE skip TASK_IDLE (allows
	 * TASK_KILLABLE).
	 */
	if (state_filter == TASK_UNINTERRUPTIBLE && (state & TASK_NOLOAD))
		return false;

	return true;
}


void show_state_filter(unsigned int state_filter)
{
	struct task_struct *g, *p;

	rcu_read_lock();
	for_each_process_thread(g, p) {
		/*
		 * reset the NMI-timeout, listing all files on a slow
		 * console might take a lot of time:
		 * Also, reset softlockup watchdogs on all CPUs, because
		 * another CPU might be blocked waiting for us to process
		 * an IPI.
		 */
		touch_nmi_watchdog();
		touch_all_softlockup_watchdogs();
		if (state_filter_match(state_filter, p))
			sched_show_task(p);
	}

#ifdef CONFIG_SCHED_DEBUG
	/* TODO: Alt schedule FW should support this
	if (!state_filter)
		sysrq_sched_debug_show();
	*/
#endif
	rcu_read_unlock();
	/*
	 * Only show locks if all tasks are dumped:
	 */
	if (!state_filter)
		debug_show_all_locks();
}

void dump_cpu_task(int cpu)
{
	if (in_hardirq() && cpu == smp_processor_id()) {
		struct pt_regs *regs;

		regs = get_irq_regs();
		if (regs) {
			show_regs(regs);
			return;
		}
	}

	if (trigger_single_cpu_backtrace(cpu))
		return;

	pr_info("Task dump for CPU %d:\n", cpu);
	sched_show_task(cpu_curr(cpu));
}

/**
 * init_idle - set up an idle thread for a given CPU
 * @idle: task in question
 * @cpu: CPU the idle task belongs to
 *
 * NOTE: this function does not set the idle thread's NEED_RESCHED
 * flag, to make booting more robust.
 */
void __init init_idle(struct task_struct *idle, int cpu)
{
#ifdef CONFIG_SMP
	struct affinity_context ac = (struct affinity_context) {
		.new_mask  = cpumask_of(cpu),
		.flags     = 0,
	};
#endif
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	__sched_fork(0, idle);

	raw_spin_lock_irqsave(&idle->pi_lock, flags);
	raw_spin_lock(&rq->lock);

	idle->last_ran = rq->clock_task;
	idle->__state = TASK_RUNNING;
	/*
	 * PF_KTHREAD should already be set at this point; regardless, make it
	 * look like a proper per-CPU kthread.
	 */
	idle->flags |= PF_KTHREAD | PF_NO_SETAFFINITY;
	kthread_set_per_cpu(idle, cpu);

	sched_queue_init_idle(&rq->queue, idle);

#ifdef CONFIG_SMP
	/*
	 * It's possible that init_idle() gets called multiple times on a task,
	 * in that case do_set_cpus_allowed() will not do the right thing.
	 *
	 * And since this is boot we can forgo the serialisation.
	 */
	set_cpus_allowed_common(idle, &ac);
#endif

	/* Silence PROVE_RCU */
	rcu_read_lock();
	__set_task_cpu(idle, cpu);
	rcu_read_unlock();

	rq->idle = idle;
	rcu_assign_pointer(rq->curr, idle);
	idle->on_cpu = 1;

	raw_spin_unlock(&rq->lock);
	raw_spin_unlock_irqrestore(&idle->pi_lock, flags);

	/* Set the preempt count _outside_ the spinlocks! */
	init_idle_preempt_count(idle, cpu);

	ftrace_graph_init_idle_task(idle, cpu);
	vtime_init_idle(idle, cpu);
#ifdef CONFIG_SMP
	sprintf(idle->comm, "%s/%d", INIT_TASK_COMM, cpu);
#endif
}

#ifdef CONFIG_SMP

int cpuset_cpumask_can_shrink(const struct cpumask __maybe_unused *cur,
			      const struct cpumask __maybe_unused *trial)
{
	return 1;
}

int task_can_attach(struct task_struct *p)
{
	int ret = 0;

	/*
	 * Kthreads which disallow setaffinity shouldn't be moved
	 * to a new cpuset; we don't want to change their CPU
	 * affinity and isolating such threads by their set of
	 * allowed nodes is unnecessary.  Thus, cpusets are not
	 * applicable for such threads.  This prevents checking for
	 * success of set_cpus_allowed_ptr() on all attached tasks
	 * before cpus_mask may be changed.
	 */
	if (p->flags & PF_NO_SETAFFINITY)
		ret = -EINVAL;

	return ret;
}

bool sched_smp_initialized __read_mostly;

#ifdef CONFIG_HOTPLUG_CPU
/*
 * Ensures that the idle task is using init_mm right before its CPU goes
 * offline.
 */
void idle_task_exit(void)
{
	struct mm_struct *mm = current->active_mm;

	BUG_ON(current != this_rq()->idle);

	if (mm != &init_mm) {
		switch_mm(mm, &init_mm, current);
		finish_arch_post_lock_switch();
	}

	/* finish_cpu(), as ran on the BP, will clean up the active_mm state */
}

static int __balance_push_cpu_stop(void *arg)
{
	struct task_struct *p = arg;
	struct rq *rq = this_rq();
	struct rq_flags rf;
	int cpu;

	raw_spin_lock_irq(&p->pi_lock);
	rq_lock(rq, &rf);

	update_rq_clock(rq);

	if (task_rq(p) == rq && task_on_rq_queued(p)) {
		cpu = select_fallback_rq(rq->cpu, p);
		rq = __migrate_task(rq, p, cpu);
	}

	rq_unlock(rq, &rf);
	raw_spin_unlock_irq(&p->pi_lock);

	put_task_struct(p);

	return 0;
}

static DEFINE_PER_CPU(struct cpu_stop_work, push_work);

/*
 * This is enabled below SCHED_AP_ACTIVE; when !cpu_active(), but only
 * effective when the hotplug motion is down.
 */
static void balance_push(struct rq *rq)
{
	struct task_struct *push_task = rq->curr;

	lockdep_assert_held(&rq->lock);

	/*
	 * Ensure the thing is persistent until balance_push_set(.on = false);
	 */
	rq->balance_callback = &balance_push_callback;

	/*
	 * Only active while going offline and when invoked on the outgoing
	 * CPU.
	 */
	if (!cpu_dying(rq->cpu) || rq != this_rq())
		return;

	/*
	 * Both the cpu-hotplug and stop task are in this case and are
	 * required to complete the hotplug process.
	 */
	if (kthread_is_per_cpu(push_task) ||
	    is_migration_disabled(push_task)) {

		/*
		 * If this is the idle task on the outgoing CPU try to wake
		 * up the hotplug control thread which might wait for the
		 * last task to vanish. The rcuwait_active() check is
		 * accurate here because the waiter is pinned on this CPU
		 * and can't obviously be running in parallel.
		 *
		 * On RT kernels this also has to check whether there are
		 * pinned and scheduled out tasks on the runqueue. They
		 * need to leave the migrate disabled section first.
		 */
		if (!rq->nr_running && !rq_has_pinned_tasks(rq) &&
		    rcuwait_active(&rq->hotplug_wait)) {
			raw_spin_unlock(&rq->lock);
			rcuwait_wake_up(&rq->hotplug_wait);
			raw_spin_lock(&rq->lock);
		}
		return;
	}

	get_task_struct(push_task);
	/*
	 * Temporarily drop rq->lock such that we can wake-up the stop task.
	 * Both preemption and IRQs are still disabled.
	 */
	preempt_disable();
	raw_spin_unlock(&rq->lock);
	stop_one_cpu_nowait(rq->cpu, __balance_push_cpu_stop, push_task,
			    this_cpu_ptr(&push_work));
	preempt_enable();
	/*
	 * At this point need_resched() is true and we'll take the loop in
	 * schedule(). The next pick is obviously going to be the stop task
	 * which kthread_is_per_cpu() and will push this task away.
	 */
	raw_spin_lock(&rq->lock);
}

static void balance_push_set(int cpu, bool on)
{
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	if (on) {
		WARN_ON_ONCE(rq->balance_callback);
		rq->balance_callback = &balance_push_callback;
	} else if (rq->balance_callback == &balance_push_callback) {
		rq->balance_callback = NULL;
	}
	rq_unlock_irqrestore(rq, &rf);
}

/*
 * Invoked from a CPUs hotplug control thread after the CPU has been marked
 * inactive. All tasks which are not per CPU kernel threads are either
 * pushed off this CPU now via balance_push() or placed on a different CPU
 * during wakeup. Wait until the CPU is quiescent.
 */
static void balance_hotplug_wait(void)
{
	struct rq *rq = this_rq();

	rcuwait_wait_event(&rq->hotplug_wait,
			   rq->nr_running == 1 && !rq_has_pinned_tasks(rq),
			   TASK_UNINTERRUPTIBLE);
}

#else

static void balance_push(struct rq *rq)
{
}

static void balance_push_set(int cpu, bool on)
{
}

static inline void balance_hotplug_wait(void)
{
}
#endif /* CONFIG_HOTPLUG_CPU */

static void set_rq_offline(struct rq *rq)
{
	if (rq->online) {
		update_rq_clock(rq);
		rq->online = false;
	}
}

static void set_rq_online(struct rq *rq)
{
	if (!rq->online)
		rq->online = true;
}

static inline void sched_set_rq_online(struct rq *rq, int cpu)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	set_rq_online(rq);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

static inline void sched_set_rq_offline(struct rq *rq, int cpu)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	set_rq_offline(rq);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

/*
 * used to mark begin/end of suspend/resume:
 */
static int num_cpus_frozen;

/*
 * Update cpusets according to cpu_active mask.  If cpusets are
 * disabled, cpuset_update_active_cpus() becomes a simple wrapper
 * around partition_sched_domains().
 *
 * If we come here as part of a suspend/resume, don't touch cpusets because we
 * want to restore it back to its original state upon resume anyway.
 */
static void cpuset_cpu_active(void)
{
	if (cpuhp_tasks_frozen) {
		/*
		 * num_cpus_frozen tracks how many CPUs are involved in suspend
		 * resume sequence. As long as this is not the last online
		 * operation in the resume sequence, just build a single sched
		 * domain, ignoring cpusets.
		 */
		partition_sched_domains(1, NULL, NULL);
		if (--num_cpus_frozen)
			return;
		/*
		 * This is the last CPU online operation. So fall through and
		 * restore the original sched domains by considering the
		 * cpuset configurations.
		 */
		cpuset_force_rebuild();
	}

	cpuset_update_active_cpus();
}

static int cpuset_cpu_inactive(unsigned int cpu)
{
	if (!cpuhp_tasks_frozen) {
		cpuset_update_active_cpus();
	} else {
		num_cpus_frozen++;
		partition_sched_domains(1, NULL, NULL);
	}
	return 0;
}

static inline void sched_smt_present_inc(int cpu)
{
#ifdef CONFIG_SCHED_SMT
	if (cpumask_weight(cpu_smt_mask(cpu)) == 2) {
		static_branch_inc_cpuslocked(&sched_smt_present);
		cpumask_or(&sched_smt_mask, &sched_smt_mask, cpu_smt_mask(cpu));
	}
#endif
}

static inline void sched_smt_present_dec(int cpu)
{
#ifdef CONFIG_SCHED_SMT
	if (cpumask_weight(cpu_smt_mask(cpu)) == 2) {
		static_branch_dec_cpuslocked(&sched_smt_present);
		if (!static_branch_likely(&sched_smt_present))
			cpumask_clear(sched_pcore_idle_mask);
		cpumask_andnot(&sched_smt_mask, &sched_smt_mask, cpu_smt_mask(cpu));
	}
#endif
}

int sched_cpu_activate(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	/*
	 * Clear the balance_push callback and prepare to schedule
	 * regular tasks.
	 */
	balance_push_set(cpu, false);

	set_cpu_active(cpu, true);

	if (sched_smp_initialized)
		cpuset_cpu_active();

	/*
	 * Put the rq online, if not already. This happens:
	 *
	 * 1) In the early boot process, because we build the real domains
	 *    after all cpus have been brought up.
	 *
	 * 2) At runtime, if cpuset_cpu_active() fails to rebuild the
	 *    domains.
	 */
	sched_set_rq_online(rq, cpu);

	/*
	 * When going up, increment the number of cores with SMT present.
	 */
	sched_smt_present_inc(cpu);

	return 0;
}

int sched_cpu_deactivate(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	int ret;

	set_cpu_active(cpu, false);

	/*
	 * From this point forward, this CPU will refuse to run any task that
	 * is not: migrate_disable() or KTHREAD_IS_PER_CPU, and will actively
	 * push those tasks away until this gets cleared, see
	 * sched_cpu_dying().
	 */
	balance_push_set(cpu, true);

	/*
	 * We've cleared cpu_active_mask, wait for all preempt-disabled and RCU
	 * users of this state to go away such that all new such users will
	 * observe it.
	 *
	 * Specifically, we rely on ttwu to no longer target this CPU, see
	 * ttwu_queue_cond() and is_cpu_allowed().
	 *
	 * Do sync before park smpboot threads to take care the RCU boost case.
	 */
	synchronize_rcu();

	sched_set_rq_offline(rq, cpu);

	/*
	 * When going down, decrement the number of cores with SMT present.
	 */
	sched_smt_present_dec(cpu);

	if (!sched_smp_initialized)
		return 0;

	ret = cpuset_cpu_inactive(cpu);
	if (ret) {
		sched_smt_present_inc(cpu);
		sched_set_rq_online(rq, cpu);
		balance_push_set(cpu, false);
		set_cpu_active(cpu, true);
		return ret;
	}

	return 0;
}

static void sched_rq_cpu_starting(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	rq->calc_load_update = calc_load_update;
}

int sched_cpu_starting(unsigned int cpu)
{
	sched_rq_cpu_starting(cpu);
	sched_tick_start(cpu);
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU

/*
 * Invoked immediately before the stopper thread is invoked to bring the
 * CPU down completely. At this point all per CPU kthreads except the
 * hotplug thread (current) and the stopper thread (inactive) have been
 * either parked or have been unbound from the outgoing CPU. Ensure that
 * any of those which might be on the way out are gone.
 *
 * If after this point a bound task is being woken on this CPU then the
 * responsible hotplug callback has failed to do it's job.
 * sched_cpu_dying() will catch it with the appropriate fireworks.
 */
int sched_cpu_wait_empty(unsigned int cpu)
{
	balance_hotplug_wait();
	return 0;
}

/*
 * Since this CPU is going 'away' for a while, fold any nr_active delta we
 * might have. Called from the CPU stopper task after ensuring that the
 * stopper is the last running task on the CPU, so nr_active count is
 * stable. We need to take the tear-down thread which is calling this into
 * account, so we hand in adjust = 1 to the load calculation.
 *
 * Also see the comment "Global load-average calculations".
 */
static void calc_load_migrate(struct rq *rq)
{
	long delta = calc_load_fold_active(rq, 1);

	if (delta)
		atomic_long_add(delta, &calc_load_tasks);
}

static void dump_rq_tasks(struct rq *rq, const char *loglvl)
{
	struct task_struct *g, *p;
	int cpu = cpu_of(rq);

	lockdep_assert_held(&rq->lock);

	printk("%sCPU%d enqueued tasks (%u total):\n", loglvl, cpu, rq->nr_running);
	for_each_process_thread(g, p) {
		if (task_cpu(p) != cpu)
			continue;

		if (!task_on_rq_queued(p))
			continue;

		printk("%s\tpid: %d, name: %s\n", loglvl, p->pid, p->comm);
	}
}

int sched_cpu_dying(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	/* Handle pending wakeups and then migrate everything off */
	sched_tick_stop(cpu);

	raw_spin_lock_irqsave(&rq->lock, flags);
	if (rq->nr_running != 1 || rq_has_pinned_tasks(rq)) {
		WARN(true, "Dying CPU not properly vacated!");
		dump_rq_tasks(rq, KERN_WARNING);
	}
	raw_spin_unlock_irqrestore(&rq->lock, flags);

	calc_load_migrate(rq);
	hrtick_clear(rq);
	return 0;
}
#endif

#ifdef CONFIG_SMP
static void sched_init_topology_cpumask_early(void)
{
	int cpu;
	cpumask_t *tmp;

	for_each_possible_cpu(cpu) {
		/* init topo masks */
		tmp = per_cpu(sched_cpu_topo_masks, cpu);

		cpumask_copy(tmp, cpu_possible_mask);
		per_cpu(sched_cpu_llc_mask, cpu) = tmp;
		per_cpu(sched_cpu_topo_end_mask, cpu) = ++tmp;
	}
}

#define TOPOLOGY_CPUMASK(name, mask, last)\
	if (cpumask_and(topo, topo, mask)) {					\
		cpumask_copy(topo, mask);					\
		printk(KERN_INFO "sched: cpu#%02d topo: 0x%08lx - "#name,	\
		       cpu, (topo++)->bits[0]);					\
	}									\
	if (!last)								\
		bitmap_complement(cpumask_bits(topo), cpumask_bits(mask),	\
				  nr_cpumask_bits);

static void sched_init_topology_cpumask(void)
{
	int cpu;
	cpumask_t *topo;

	for_each_online_cpu(cpu) {
		topo = per_cpu(sched_cpu_topo_masks, cpu);

		bitmap_complement(cpumask_bits(topo), cpumask_bits(cpumask_of(cpu)),
				  nr_cpumask_bits);
#ifdef CONFIG_SCHED_SMT
		TOPOLOGY_CPUMASK(smt, topology_sibling_cpumask(cpu), false);
#endif
		TOPOLOGY_CPUMASK(cluster, topology_cluster_cpumask(cpu), false);

		per_cpu(sd_llc_id, cpu) = cpumask_first(cpu_coregroup_mask(cpu));
		per_cpu(sched_cpu_llc_mask, cpu) = topo;
		TOPOLOGY_CPUMASK(coregroup, cpu_coregroup_mask(cpu), false);

		TOPOLOGY_CPUMASK(core, topology_core_cpumask(cpu), false);

		TOPOLOGY_CPUMASK(others, cpu_online_mask, true);

		per_cpu(sched_cpu_topo_end_mask, cpu) = topo;
		printk(KERN_INFO "sched: cpu#%02d llc_id = %d, llc_mask idx = %d\n",
		       cpu, per_cpu(sd_llc_id, cpu),
		       (int) (per_cpu(sched_cpu_llc_mask, cpu) -
			      per_cpu(sched_cpu_topo_masks, cpu)));
	}
}
#endif

void __init sched_init_smp(void)
{
	/* Move init over to a non-isolated CPU */
	if (set_cpus_allowed_ptr(current, housekeeping_cpumask(HK_TYPE_DOMAIN)) < 0)
		BUG();
	current->flags &= ~PF_NO_SETAFFINITY;

	sched_init_topology();
	sched_init_topology_cpumask();

	sched_smp_initialized = true;
}

static int __init migration_init(void)
{
	sched_cpu_starting(smp_processor_id());
	return 0;
}
early_initcall(migration_init);

#else
void __init sched_init_smp(void)
{
	cpu_rq(0)->idle->time_slice = sysctl_sched_base_slice;
}
#endif /* CONFIG_SMP */

int in_sched_functions(unsigned long addr)
{
	return in_lock_functions(addr) ||
		(addr >= (unsigned long)__sched_text_start
		&& addr < (unsigned long)__sched_text_end);
}

#ifdef CONFIG_CGROUP_SCHED
/*
 * Default task group.
 * Every task in system belongs to this group at bootup.
 */
struct task_group root_task_group;
LIST_HEAD(task_groups);

/* Cacheline aligned slab cache for task_group */
static struct kmem_cache *task_group_cache __ro_after_init;
#endif /* CONFIG_CGROUP_SCHED */

void __init sched_init(void)
{
	int i;
	struct rq *rq;

	printk(KERN_INFO "sched/alt: "ALT_SCHED_NAME" CPU Scheduler "ALT_SCHED_VERSION\
			 " by Alfred Chen.\n");

	wait_bit_init();

#ifdef CONFIG_SMP
	for (i = 0; i < SCHED_QUEUE_BITS; i++)
		cpumask_copy(sched_preempt_mask + i, cpu_present_mask);
#endif

#ifdef CONFIG_CGROUP_SCHED
	task_group_cache = KMEM_CACHE(task_group, 0);

	list_add(&root_task_group.list, &task_groups);
	INIT_LIST_HEAD(&root_task_group.children);
	INIT_LIST_HEAD(&root_task_group.siblings);
#endif /* CONFIG_CGROUP_SCHED */
	for_each_possible_cpu(i) {
		rq = cpu_rq(i);

		sched_queue_init(&rq->queue);
		rq->prio = IDLE_TASK_SCHED_PRIO;
#ifdef CONFIG_SCHED_PDS
		rq->prio_idx = rq->prio;
#endif

		raw_spin_lock_init(&rq->lock);
		rq->nr_running = rq->nr_uninterruptible = 0;
		rq->calc_load_active = 0;
		rq->calc_load_update = jiffies + LOAD_FREQ;
#ifdef CONFIG_SMP
		rq->online = false;
		rq->cpu = i;

		rq->clear_idle_mask_func = cpumask_clear_cpu;
		rq->set_idle_mask_func = cpumask_set_cpu;
		rq->balance_func = NULL;
		rq->active_balance_arg.active = 0;

#ifdef CONFIG_NO_HZ_COMMON
		INIT_CSD(&rq->nohz_csd, nohz_csd_func, rq);
#endif
		rq->balance_callback = &balance_push_callback;
#ifdef CONFIG_HOTPLUG_CPU
		rcuwait_init(&rq->hotplug_wait);
#endif
#endif /* CONFIG_SMP */
		rq->nr_switches = 0;

		hrtick_rq_init(rq);
		atomic_set(&rq->nr_iowait, 0);

		zalloc_cpumask_var_node(&rq->scratch_mask, GFP_KERNEL, cpu_to_node(i));
	}
#ifdef CONFIG_SMP
	/* Set rq->online for cpu 0 */
	cpu_rq(0)->online = true;
#endif
	/*
	 * The boot idle thread does lazy MMU switching as well:
	 */
	mmgrab(&init_mm);
	enter_lazy_tlb(&init_mm, current);

	/*
	 * The idle task doesn't need the kthread struct to function, but it
	 * is dressed up as a per-CPU kthread and thus needs to play the part
	 * if we want to avoid special-casing it in code that deals with per-CPU
	 * kthreads.
	 */
	WARN_ON(!set_kthread_struct(current));

	/*
	 * Make us the idle thread. Technically, schedule() should not be
	 * called from this thread, however somewhere below it might be,
	 * but because we are the idle thread, we just pick up running again
	 * when this runqueue becomes "idle".
	 */
	init_idle(current, smp_processor_id());

	calc_load_update = jiffies + LOAD_FREQ;

#ifdef CONFIG_SMP
	idle_thread_set_boot_cpu();
	balance_push_set(smp_processor_id(), false);

	sched_init_topology_cpumask_early();
#endif /* SMP */

	preempt_dynamic_init();
}

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP

void __might_sleep(const char *file, int line)
{
	unsigned int state = get_current_state();
	/*
	 * Blocking primitives will set (and therefore destroy) current->state,
	 * since we will exit with TASK_RUNNING make sure we enter with it,
	 * otherwise we will destroy state.
	 */
	WARN_ONCE(state != TASK_RUNNING && current->task_state_change,
			"do not call blocking ops when !TASK_RUNNING; "
			"state=%x set at [<%p>] %pS\n", state,
			(void *)current->task_state_change,
			(void *)current->task_state_change);

	__might_resched(file, line, 0);
}
EXPORT_SYMBOL(__might_sleep);

static void print_preempt_disable_ip(int preempt_offset, unsigned long ip)
{
	if (!IS_ENABLED(CONFIG_DEBUG_PREEMPT))
		return;

	if (preempt_count() == preempt_offset)
		return;

	pr_err("Preemption disabled at:");
	print_ip_sym(KERN_ERR, ip);
}

static inline bool resched_offsets_ok(unsigned int offsets)
{
	unsigned int nested = preempt_count();

	nested += rcu_preempt_depth() << MIGHT_RESCHED_RCU_SHIFT;

	return nested == offsets;
}

void __might_resched(const char *file, int line, unsigned int offsets)
{
	/* Ratelimiting timestamp: */
	static unsigned long prev_jiffy;

	unsigned long preempt_disable_ip;

	/* WARN_ON_ONCE() by default, no rate limit required: */
	rcu_sleep_check();

	if ((resched_offsets_ok(offsets) && !irqs_disabled() &&
	     !is_idle_task(current) && !current->non_block_count) ||
	    system_state == SYSTEM_BOOTING || system_state > SYSTEM_RUNNING ||
	    oops_in_progress)
		return;
	if (time_before(jiffies, prev_jiffy + HZ) && prev_jiffy)
		return;
	prev_jiffy = jiffies;

	/* Save this before calling printk(), since that will clobber it: */
	preempt_disable_ip = get_preempt_disable_ip(current);

	pr_err("BUG: sleeping function called from invalid context at %s:%d\n",
	       file, line);
	pr_err("in_atomic(): %d, irqs_disabled(): %d, non_block: %d, pid: %d, name: %s\n",
	       in_atomic(), irqs_disabled(), current->non_block_count,
	       current->pid, current->comm);
	pr_err("preempt_count: %x, expected: %x\n", preempt_count(),
	       offsets & MIGHT_RESCHED_PREEMPT_MASK);

	if (IS_ENABLED(CONFIG_PREEMPT_RCU)) {
		pr_err("RCU nest depth: %d, expected: %u\n",
		       rcu_preempt_depth(), offsets >> MIGHT_RESCHED_RCU_SHIFT);
	}

	if (task_stack_end_corrupted(current))
		pr_emerg("Thread overran stack, or stack corrupted\n");

	debug_show_held_locks(current);
	if (irqs_disabled())
		print_irqtrace_events(current);

	print_preempt_disable_ip(offsets & MIGHT_RESCHED_PREEMPT_MASK,
				 preempt_disable_ip);

	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}
EXPORT_SYMBOL(__might_resched);

void __cant_sleep(const char *file, int line, int preempt_offset)
{
	static unsigned long prev_jiffy;

	if (irqs_disabled())
		return;

	if (!IS_ENABLED(CONFIG_PREEMPT_COUNT))
		return;

	if (preempt_count() > preempt_offset)
		return;

	if (time_before(jiffies, prev_jiffy + HZ) && prev_jiffy)
		return;
	prev_jiffy = jiffies;

	printk(KERN_ERR "BUG: assuming atomic context at %s:%d\n", file, line);
	printk(KERN_ERR "in_atomic(): %d, irqs_disabled(): %d, pid: %d, name: %s\n",
			in_atomic(), irqs_disabled(),
			current->pid, current->comm);

	debug_show_held_locks(current);
	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}
EXPORT_SYMBOL_GPL(__cant_sleep);

#ifdef CONFIG_SMP
void __cant_migrate(const char *file, int line)
{
	static unsigned long prev_jiffy;

	if (irqs_disabled())
		return;

	if (is_migration_disabled(current))
		return;

	if (!IS_ENABLED(CONFIG_PREEMPT_COUNT))
		return;

	if (preempt_count() > 0)
		return;

	if (time_before(jiffies, prev_jiffy + HZ) && prev_jiffy)
		return;
	prev_jiffy = jiffies;

	pr_err("BUG: assuming non migratable context at %s:%d\n", file, line);
	pr_err("in_atomic(): %d, irqs_disabled(): %d, migration_disabled() %u pid: %d, name: %s\n",
	       in_atomic(), irqs_disabled(), is_migration_disabled(current),
	       current->pid, current->comm);

	debug_show_held_locks(current);
	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}
EXPORT_SYMBOL_GPL(__cant_migrate);
#endif
#endif

#ifdef CONFIG_MAGIC_SYSRQ
void normalize_rt_tasks(void)
{
	struct task_struct *g, *p;
	struct sched_attr attr = {
		.sched_policy = SCHED_NORMAL,
	};

	read_lock(&tasklist_lock);
	for_each_process_thread(g, p) {
		/*
		 * Only normalize user tasks:
		 */
		if (p->flags & PF_KTHREAD)
			continue;

		schedstat_set(p->stats.wait_start,  0);
		schedstat_set(p->stats.sleep_start, 0);
		schedstat_set(p->stats.block_start, 0);

		if (!rt_or_dl_task(p)) {
			/*
			 * Renice negative nice level userspace
			 * tasks back to 0:
			 */
			if (task_nice(p) < 0)
				set_user_nice(p, 0);
			continue;
		}

		__sched_setscheduler(p, &attr, false, false);
	}
	read_unlock(&tasklist_lock);
}
#endif /* CONFIG_MAGIC_SYSRQ */

#if defined(CONFIG_KGDB_KDB)
/*
 * These functions are only useful for KDB.
 *
 * They can only be called when the whole system has been
 * stopped - every CPU needs to be quiescent, and no scheduling
 * activity can take place. Using them for anything else would
 * be a serious bug, and as a result, they aren't even visible
 * under any other configuration.
 */

/**
 * curr_task - return the current task for a given CPU.
 * @cpu: the processor in question.
 *
 * ONLY VALID WHEN THE WHOLE SYSTEM IS STOPPED!
 *
 * Return: The current task for @cpu.
 */
struct task_struct *curr_task(int cpu)
{
	return cpu_curr(cpu);
}

#endif /* defined(CONFIG_KGDB_KDB) */

#ifdef CONFIG_CGROUP_SCHED
static void sched_free_group(struct task_group *tg)
{
	kmem_cache_free(task_group_cache, tg);
}

static void sched_free_group_rcu(struct rcu_head *rhp)
{
	sched_free_group(container_of(rhp, struct task_group, rcu));
}

static void sched_unregister_group(struct task_group *tg)
{
	/*
	 * We have to wait for yet another RCU grace period to expire, as
	 * print_cfs_stats() might run concurrently.
	 */
	call_rcu(&tg->rcu, sched_free_group_rcu);
}

/* allocate runqueue etc for a new task group */
struct task_group *sched_create_group(struct task_group *parent)
{
	struct task_group *tg;

	tg = kmem_cache_alloc(task_group_cache, GFP_KERNEL | __GFP_ZERO);
	if (!tg)
		return ERR_PTR(-ENOMEM);

	return tg;
}

void sched_online_group(struct task_group *tg, struct task_group *parent)
{
}

/* RCU callback to free various structures associated with a task group */
static void sched_unregister_group_rcu(struct rcu_head *rhp)
{
	/* Now it should be safe to free those cfs_rqs: */
	sched_unregister_group(container_of(rhp, struct task_group, rcu));
}

void sched_destroy_group(struct task_group *tg)
{
	/* Wait for possible concurrent references to cfs_rqs complete: */
	call_rcu(&tg->rcu, sched_unregister_group_rcu);
}

void sched_release_group(struct task_group *tg)
{
}

static inline struct task_group *css_tg(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct task_group, css) : NULL;
}

static struct cgroup_subsys_state *
cpu_cgroup_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct task_group *parent = css_tg(parent_css);
	struct task_group *tg;

	if (!parent) {
		/* This is early initialization for the top cgroup */
		return &root_task_group.css;
	}

	tg = sched_create_group(parent);
	if (IS_ERR(tg))
		return ERR_PTR(-ENOMEM);
	return &tg->css;
}

/* Expose task group only after completing cgroup initialization */
static int cpu_cgroup_css_online(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);
	struct task_group *parent = css_tg(css->parent);

	if (parent)
		sched_online_group(tg, parent);
	return 0;
}

static void cpu_cgroup_css_released(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);

	sched_release_group(tg);
}

static void cpu_cgroup_css_free(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);

	/*
	 * Relies on the RCU grace period between css_released() and this.
	 */
	sched_unregister_group(tg);
}

#ifdef CONFIG_RT_GROUP_SCHED
static int cpu_cgroup_can_attach(struct cgroup_taskset *tset)
{
	return 0;
}
#endif

static void cpu_cgroup_attach(struct cgroup_taskset *tset)
{
}

#ifdef CONFIG_GROUP_SCHED_WEIGHT
static int sched_group_set_shares(struct task_group *tg, unsigned long shares)
{
	return 0;
}

static int sched_group_set_idle(struct task_group *tg, long idle)
{
	return 0;
}

static int cpu_shares_write_u64(struct cgroup_subsys_state *css,
				struct cftype *cftype, u64 shareval)
{
	return sched_group_set_shares(css_tg(css), shareval);
}

static u64 cpu_shares_read_u64(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	return 0;
}

static s64 cpu_idle_read_s64(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	return 0;
}

static int cpu_idle_write_s64(struct cgroup_subsys_state *css,
				struct cftype *cft, s64 idle)
{
	return sched_group_set_idle(css_tg(css), idle);
}
#endif

#ifdef CONFIG_CFS_BANDWIDTH
static s64 cpu_cfs_quota_read_s64(struct cgroup_subsys_state *css,
				  struct cftype *cft)
{
	return 0;
}

static int cpu_cfs_quota_write_s64(struct cgroup_subsys_state *css,
				   struct cftype *cftype, s64 cfs_quota_us)
{
	return 0;
}

static u64 cpu_cfs_period_read_u64(struct cgroup_subsys_state *css,
				   struct cftype *cft)
{
	return 0;
}

static int cpu_cfs_period_write_u64(struct cgroup_subsys_state *css,
				    struct cftype *cftype, u64 cfs_period_us)
{
	return 0;
}

static u64 cpu_cfs_burst_read_u64(struct cgroup_subsys_state *css,
				  struct cftype *cft)
{
	return 0;
}

static int cpu_cfs_burst_write_u64(struct cgroup_subsys_state *css,
				   struct cftype *cftype, u64 cfs_burst_us)
{
	return 0;
}

static int cpu_cfs_stat_show(struct seq_file *sf, void *v)
{
	return 0;
}

static int cpu_cfs_local_stat_show(struct seq_file *sf, void *v)
{
	return 0;
}
#endif

#ifdef CONFIG_RT_GROUP_SCHED
static int cpu_rt_runtime_write(struct cgroup_subsys_state *css,
				struct cftype *cft, s64 val)
{
	return 0;
}

static s64 cpu_rt_runtime_read(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	return 0;
}

static int cpu_rt_period_write_uint(struct cgroup_subsys_state *css,
				    struct cftype *cftype, u64 rt_period_us)
{
	return 0;
}

static u64 cpu_rt_period_read_uint(struct cgroup_subsys_state *css,
				   struct cftype *cft)
{
	return 0;
}
#endif

#ifdef CONFIG_UCLAMP_TASK_GROUP
static int cpu_uclamp_min_show(struct seq_file *sf, void *v)
{
	return 0;
}

static int cpu_uclamp_max_show(struct seq_file *sf, void *v)
{
	return 0;
}

static ssize_t cpu_uclamp_min_write(struct kernfs_open_file *of,
				    char *buf, size_t nbytes,
				    loff_t off)
{
	return nbytes;
}

static ssize_t cpu_uclamp_max_write(struct kernfs_open_file *of,
				    char *buf, size_t nbytes,
				    loff_t off)
{
	return nbytes;
}
#endif

static struct cftype cpu_legacy_files[] = {
#ifdef CONFIG_GROUP_SCHED_WEIGHT
	{
		.name = "shares",
		.read_u64 = cpu_shares_read_u64,
		.write_u64 = cpu_shares_write_u64,
	},
	{
		.name = "idle",
		.read_s64 = cpu_idle_read_s64,
		.write_s64 = cpu_idle_write_s64,
	},
#endif
#ifdef CONFIG_CFS_BANDWIDTH
	{
		.name = "cfs_quota_us",
		.read_s64 = cpu_cfs_quota_read_s64,
		.write_s64 = cpu_cfs_quota_write_s64,
	},
	{
		.name = "cfs_period_us",
		.read_u64 = cpu_cfs_period_read_u64,
		.write_u64 = cpu_cfs_period_write_u64,
	},
	{
		.name = "cfs_burst_us",
		.read_u64 = cpu_cfs_burst_read_u64,
		.write_u64 = cpu_cfs_burst_write_u64,
	},
	{
		.name = "stat",
		.seq_show = cpu_cfs_stat_show,
	},
	{
		.name = "stat.local",
		.seq_show = cpu_cfs_local_stat_show,
	},
#endif
#ifdef CONFIG_RT_GROUP_SCHED
	{
		.name = "rt_runtime_us",
		.read_s64 = cpu_rt_runtime_read,
		.write_s64 = cpu_rt_runtime_write,
	},
	{
		.name = "rt_period_us",
		.read_u64 = cpu_rt_period_read_uint,
		.write_u64 = cpu_rt_period_write_uint,
	},
#endif
#ifdef CONFIG_UCLAMP_TASK_GROUP
	{
		.name = "uclamp.min",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cpu_uclamp_min_show,
		.write = cpu_uclamp_min_write,
	},
	{
		.name = "uclamp.max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cpu_uclamp_max_show,
		.write = cpu_uclamp_max_write,
	},
#endif
	{ }	/* Terminate */
};

#ifdef CONFIG_GROUP_SCHED_WEIGHT
static u64 cpu_weight_read_u64(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	return 0;
}

static int cpu_weight_write_u64(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 weight)
{
	return 0;
}

static s64 cpu_weight_nice_read_s64(struct cgroup_subsys_state *css,
				    struct cftype *cft)
{
	return 0;
}

static int cpu_weight_nice_write_s64(struct cgroup_subsys_state *css,
				     struct cftype *cft, s64 nice)
{
	return 0;
}
#endif

#ifdef CONFIG_CFS_BANDWIDTH
static int cpu_max_show(struct seq_file *sf, void *v)
{
	return 0;
}

static ssize_t cpu_max_write(struct kernfs_open_file *of,
			     char *buf, size_t nbytes, loff_t off)
{
	return nbytes;
}
#endif

static struct cftype cpu_files[] = {
#ifdef CONFIG_GROUP_SCHED_WEIGHT
	{
		.name = "weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = cpu_weight_read_u64,
		.write_u64 = cpu_weight_write_u64,
	},
	{
		.name = "weight.nice",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_s64 = cpu_weight_nice_read_s64,
		.write_s64 = cpu_weight_nice_write_s64,
	},
	{
		.name = "idle",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_s64 = cpu_idle_read_s64,
		.write_s64 = cpu_idle_write_s64,
	},
#endif
#ifdef CONFIG_CFS_BANDWIDTH
	{
		.name = "max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cpu_max_show,
		.write = cpu_max_write,
	},
	{
		.name = "max.burst",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = cpu_cfs_burst_read_u64,
		.write_u64 = cpu_cfs_burst_write_u64,
	},
#endif
#ifdef CONFIG_UCLAMP_TASK_GROUP
	{
		.name = "uclamp.min",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cpu_uclamp_min_show,
		.write = cpu_uclamp_min_write,
	},
	{
		.name = "uclamp.max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cpu_uclamp_max_show,
		.write = cpu_uclamp_max_write,
	},
#endif
	{ }	/* terminate */
};

static int cpu_extra_stat_show(struct seq_file *sf,
			       struct cgroup_subsys_state *css)
{
	return 0;
}

static int cpu_local_stat_show(struct seq_file *sf,
			       struct cgroup_subsys_state *css)
{
	return 0;
}

struct cgroup_subsys cpu_cgrp_subsys = {
	.css_alloc	= cpu_cgroup_css_alloc,
	.css_online	= cpu_cgroup_css_online,
	.css_released	= cpu_cgroup_css_released,
	.css_free	= cpu_cgroup_css_free,
	.css_extra_stat_show = cpu_extra_stat_show,
	.css_local_stat_show = cpu_local_stat_show,
#ifdef CONFIG_RT_GROUP_SCHED
	.can_attach	= cpu_cgroup_can_attach,
#endif
	.attach		= cpu_cgroup_attach,
	.legacy_cftypes	= cpu_legacy_files,
	.dfl_cftypes	= cpu_files,
	.early_init	= true,
	.threaded	= true,
};
#endif	/* CONFIG_CGROUP_SCHED */

#undef CREATE_TRACE_POINTS

#ifdef CONFIG_SCHED_MM_CID

#
/*
 * @cid_lock: Guarantee forward-progress of cid allocation.
 *
 * Concurrency ID allocation within a bitmap is mostly lock-free. The cid_lock
 * is only used when contention is detected by the lock-free allocation so
 * forward progress can be guaranteed.
 */
DEFINE_RAW_SPINLOCK(cid_lock);

/*
 * @use_cid_lock: Select cid allocation behavior: lock-free vs spinlock.
 *
 * When @use_cid_lock is 0, the cid allocation is lock-free. When contention is
 * detected, it is set to 1 to ensure that all newly coming allocations are
 * serialized by @cid_lock until the allocation which detected contention
 * completes and sets @use_cid_lock back to 0. This guarantees forward progress
 * of a cid allocation.
 */
int use_cid_lock;

/*
 * mm_cid remote-clear implements a lock-free algorithm to clear per-mm/cpu cid
 * concurrently with respect to the execution of the source runqueue context
 * switch.
 *
 * There is one basic properties we want to guarantee here:
 *
 * (1) Remote-clear should _never_ mark a per-cpu cid UNSET when it is actively
 * used by a task. That would lead to concurrent allocation of the cid and
 * userspace corruption.
 *
 * Provide this guarantee by introducing a Dekker memory ordering to guarantee
 * that a pair of loads observe at least one of a pair of stores, which can be
 * shown as:
 *
 *      X = Y = 0
 *
 *      w[X]=1          w[Y]=1
 *      MB              MB
 *      r[Y]=y          r[X]=x
 *
 * Which guarantees that x==0 && y==0 is impossible. But rather than using
 * values 0 and 1, this algorithm cares about specific state transitions of the
 * runqueue current task (as updated by the scheduler context switch), and the
 * per-mm/cpu cid value.
 *
 * Let's introduce task (Y) which has task->mm == mm and task (N) which has
 * task->mm != mm for the rest of the discussion. There are two scheduler state
 * transitions on context switch we care about:
 *
 * (TSA) Store to rq->curr with transition from (N) to (Y)
 *
 * (TSB) Store to rq->curr with transition from (Y) to (N)
 *
 * On the remote-clear side, there is one transition we care about:
 *
 * (TMA) cmpxchg to *pcpu_cid to set the LAZY flag
 *
 * There is also a transition to UNSET state which can be performed from all
 * sides (scheduler, remote-clear). It is always performed with a cmpxchg which
 * guarantees that only a single thread will succeed:
 *
 * (TMB) cmpxchg to *pcpu_cid to mark UNSET
 *
 * Just to be clear, what we do _not_ want to happen is a transition to UNSET
 * when a thread is actively using the cid (property (1)).
 *
 * Let's looks at the relevant combinations of TSA/TSB, and TMA transitions.
 *
 * Scenario A) (TSA)+(TMA) (from next task perspective)
 *
 * CPU0                                      CPU1
 *
 * Context switch CS-1                       Remote-clear
 *   - store to rq->curr: (N)->(Y) (TSA)     - cmpxchg to *pcpu_id to LAZY (TMA)
 *                                             (implied barrier after cmpxchg)
 *   - switch_mm_cid()
 *     - memory barrier (see switch_mm_cid()
 *       comment explaining how this barrier
 *       is combined with other scheduler
 *       barriers)
 *     - mm_cid_get (next)
 *       - READ_ONCE(*pcpu_cid)              - rcu_dereference(src_rq->curr)
 *
 * This Dekker ensures that either task (Y) is observed by the
 * rcu_dereference() or the LAZY flag is observed by READ_ONCE(), or both are
 * observed.
 *
 * If task (Y) store is observed by rcu_dereference(), it means that there is
 * still an active task on the cpu. Remote-clear will therefore not transition
 * to UNSET, which fulfills property (1).
 *
 * If task (Y) is not observed, but the lazy flag is observed by READ_ONCE(),
 * it will move its state to UNSET, which clears the percpu cid perhaps
 * uselessly (which is not an issue for correctness). Because task (Y) is not
 * observed, CPU1 can move ahead to set the state to UNSET. Because moving
 * state to UNSET is done with a cmpxchg expecting that the old state has the
 * LAZY flag set, only one thread will successfully UNSET.
 *
 * If both states (LAZY flag and task (Y)) are observed, the thread on CPU0
 * will observe the LAZY flag and transition to UNSET (perhaps uselessly), and
 * CPU1 will observe task (Y) and do nothing more, which is fine.
 *
 * What we are effectively preventing with this Dekker is a scenario where
 * neither LAZY flag nor store (Y) are observed, which would fail property (1)
 * because this would UNSET a cid which is actively used.
 */

void sched_mm_cid_migrate_from(struct task_struct *t)
{
	t->migrate_from_cpu = task_cpu(t);
}

static
int __sched_mm_cid_migrate_from_fetch_cid(struct rq *src_rq,
					  struct task_struct *t,
					  struct mm_cid *src_pcpu_cid)
{
	struct mm_struct *mm = t->mm;
	struct task_struct *src_task;
	int src_cid, last_mm_cid;

	if (!mm)
		return -1;

	last_mm_cid = t->last_mm_cid;
	/*
	 * If the migrated task has no last cid, or if the current
	 * task on src rq uses the cid, it means the source cid does not need
	 * to be moved to the destination cpu.
	 */
	if (last_mm_cid == -1)
		return -1;
	src_cid = READ_ONCE(src_pcpu_cid->cid);
	if (!mm_cid_is_valid(src_cid) || last_mm_cid != src_cid)
		return -1;

	/*
	 * If we observe an active task using the mm on this rq, it means we
	 * are not the last task to be migrated from this cpu for this mm, so
	 * there is no need to move src_cid to the destination cpu.
	 */
	guard(rcu)();
	src_task = rcu_dereference(src_rq->curr);
	if (READ_ONCE(src_task->mm_cid_active) && src_task->mm == mm) {
		t->last_mm_cid = -1;
		return -1;
	}

	return src_cid;
}

static
int __sched_mm_cid_migrate_from_try_steal_cid(struct rq *src_rq,
					      struct task_struct *t,
					      struct mm_cid *src_pcpu_cid,
					      int src_cid)
{
	struct task_struct *src_task;
	struct mm_struct *mm = t->mm;
	int lazy_cid;

	if (src_cid == -1)
		return -1;

	/*
	 * Attempt to clear the source cpu cid to move it to the destination
	 * cpu.
	 */
	lazy_cid = mm_cid_set_lazy_put(src_cid);
	if (!try_cmpxchg(&src_pcpu_cid->cid, &src_cid, lazy_cid))
		return -1;

	/*
	 * The implicit barrier after cmpxchg per-mm/cpu cid before loading
	 * rq->curr->mm matches the scheduler barrier in context_switch()
	 * between store to rq->curr and load of prev and next task's
	 * per-mm/cpu cid.
	 *
	 * The implicit barrier after cmpxchg per-mm/cpu cid before loading
	 * rq->curr->mm_cid_active matches the barrier in
	 * sched_mm_cid_exit_signals(), sched_mm_cid_before_execve(), and
	 * sched_mm_cid_after_execve() between store to t->mm_cid_active and
	 * load of per-mm/cpu cid.
	 */

	/*
	 * If we observe an active task using the mm on this rq after setting
	 * the lazy-put flag, this task will be responsible for transitioning
	 * from lazy-put flag set to MM_CID_UNSET.
	 */
	scoped_guard (rcu) {
		src_task = rcu_dereference(src_rq->curr);
		if (READ_ONCE(src_task->mm_cid_active) && src_task->mm == mm) {
			rcu_read_unlock();
			/*
			 * We observed an active task for this mm, there is therefore
			 * no point in moving this cid to the destination cpu.
			 */
			t->last_mm_cid = -1;
			return -1;
		}
	}

	/*
	 * The src_cid is unused, so it can be unset.
	 */
	if (!try_cmpxchg(&src_pcpu_cid->cid, &lazy_cid, MM_CID_UNSET))
		return -1;
	return src_cid;
}

/*
 * Migration to dst cpu. Called with dst_rq lock held.
 * Interrupts are disabled, which keeps the window of cid ownership without the
 * source rq lock held small.
 */
void sched_mm_cid_migrate_to(struct rq *dst_rq, struct task_struct *t)
{
	struct mm_cid *src_pcpu_cid, *dst_pcpu_cid;
	struct mm_struct *mm = t->mm;
	int src_cid, dst_cid, src_cpu;
	struct rq *src_rq;

	lockdep_assert_rq_held(dst_rq);

	if (!mm)
		return;
	src_cpu = t->migrate_from_cpu;
	if (src_cpu == -1) {
		t->last_mm_cid = -1;
		return;
	}
	/*
	 * Move the src cid if the dst cid is unset. This keeps id
	 * allocation closest to 0 in cases where few threads migrate around
	 * many CPUs.
	 *
	 * If destination cid is already set, we may have to just clear
	 * the src cid to ensure compactness in frequent migrations
	 * scenarios.
	 *
	 * It is not useful to clear the src cid when the number of threads is
	 * greater or equal to the number of allowed CPUs, because user-space
	 * can expect that the number of allowed cids can reach the number of
	 * allowed CPUs.
	 */
	dst_pcpu_cid = per_cpu_ptr(mm->pcpu_cid, cpu_of(dst_rq));
	dst_cid = READ_ONCE(dst_pcpu_cid->cid);
	if (!mm_cid_is_unset(dst_cid) &&
	    atomic_read(&mm->mm_users) >= t->nr_cpus_allowed)
		return;
	src_pcpu_cid = per_cpu_ptr(mm->pcpu_cid, src_cpu);
	src_rq = cpu_rq(src_cpu);
	src_cid = __sched_mm_cid_migrate_from_fetch_cid(src_rq, t, src_pcpu_cid);
	if (src_cid == -1)
		return;
	src_cid = __sched_mm_cid_migrate_from_try_steal_cid(src_rq, t, src_pcpu_cid,
							    src_cid);
	if (src_cid == -1)
		return;
	if (!mm_cid_is_unset(dst_cid)) {
		__mm_cid_put(mm, src_cid);
		return;
	}
	/* Move src_cid to dst cpu. */
	mm_cid_snapshot_time(dst_rq, mm);
	WRITE_ONCE(dst_pcpu_cid->cid, src_cid);
}

static void sched_mm_cid_remote_clear(struct mm_struct *mm, struct mm_cid *pcpu_cid,
				      int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct task_struct *t;
	int cid, lazy_cid;

	cid = READ_ONCE(pcpu_cid->cid);
	if (!mm_cid_is_valid(cid))
		return;

	/*
	 * Clear the cpu cid if it is set to keep cid allocation compact.  If
	 * there happens to be other tasks left on the source cpu using this
	 * mm, the next task using this mm will reallocate its cid on context
	 * switch.
	 */
	lazy_cid = mm_cid_set_lazy_put(cid);
	if (!try_cmpxchg(&pcpu_cid->cid, &cid, lazy_cid))
		return;

	/*
	 * The implicit barrier after cmpxchg per-mm/cpu cid before loading
	 * rq->curr->mm matches the scheduler barrier in context_switch()
	 * between store to rq->curr and load of prev and next task's
	 * per-mm/cpu cid.
	 *
	 * The implicit barrier after cmpxchg per-mm/cpu cid before loading
	 * rq->curr->mm_cid_active matches the barrier in
	 * sched_mm_cid_exit_signals(), sched_mm_cid_before_execve(), and
	 * sched_mm_cid_after_execve() between store to t->mm_cid_active and
	 * load of per-mm/cpu cid.
	 */

	/*
	 * If we observe an active task using the mm on this rq after setting
	 * the lazy-put flag, that task will be responsible for transitioning
	 * from lazy-put flag set to MM_CID_UNSET.
	 */
	scoped_guard (rcu) {
		t = rcu_dereference(rq->curr);
		if (READ_ONCE(t->mm_cid_active) && t->mm == mm)
			return;
	}

	/*
	 * The cid is unused, so it can be unset.
	 * Disable interrupts to keep the window of cid ownership without rq
	 * lock small.
	 */
	scoped_guard (irqsave) {
		if (try_cmpxchg(&pcpu_cid->cid, &lazy_cid, MM_CID_UNSET))
			__mm_cid_put(mm, cid);
	}
}

static void sched_mm_cid_remote_clear_old(struct mm_struct *mm, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct mm_cid *pcpu_cid;
	struct task_struct *curr;
	u64 rq_clock;

	/*
	 * rq->clock load is racy on 32-bit but one spurious clear once in a
	 * while is irrelevant.
	 */
	rq_clock = READ_ONCE(rq->clock);
	pcpu_cid = per_cpu_ptr(mm->pcpu_cid, cpu);

	/*
	 * In order to take care of infrequently scheduled tasks, bump the time
	 * snapshot associated with this cid if an active task using the mm is
	 * observed on this rq.
	 */
	scoped_guard (rcu) {
		curr = rcu_dereference(rq->curr);
		if (READ_ONCE(curr->mm_cid_active) && curr->mm == mm) {
			WRITE_ONCE(pcpu_cid->time, rq_clock);
			return;
		}
	}

	if (rq_clock < pcpu_cid->time + SCHED_MM_CID_PERIOD_NS)
		return;
	sched_mm_cid_remote_clear(mm, pcpu_cid, cpu);
}

static void sched_mm_cid_remote_clear_weight(struct mm_struct *mm, int cpu,
					     int weight)
{
	struct mm_cid *pcpu_cid;
	int cid;

	pcpu_cid = per_cpu_ptr(mm->pcpu_cid, cpu);
	cid = READ_ONCE(pcpu_cid->cid);
	if (!mm_cid_is_valid(cid) || cid < weight)
		return;
	sched_mm_cid_remote_clear(mm, pcpu_cid, cpu);
}

static void task_mm_cid_work(struct callback_head *work)
{
	unsigned long now = jiffies, old_scan, next_scan;
	struct task_struct *t = current;
	struct cpumask *cidmask;
	struct mm_struct *mm;
	int weight, cpu;

	SCHED_WARN_ON(t != container_of(work, struct task_struct, cid_work));

	work->next = work;	/* Prevent double-add */
	if (t->flags & PF_EXITING)
		return;
	mm = t->mm;
	if (!mm)
		return;
	old_scan = READ_ONCE(mm->mm_cid_next_scan);
	next_scan = now + msecs_to_jiffies(MM_CID_SCAN_DELAY);
	if (!old_scan) {
		unsigned long res;

		res = cmpxchg(&mm->mm_cid_next_scan, old_scan, next_scan);
		if (res != old_scan)
			old_scan = res;
		else
			old_scan = next_scan;
	}
	if (time_before(now, old_scan))
		return;
	if (!try_cmpxchg(&mm->mm_cid_next_scan, &old_scan, next_scan))
		return;
	cidmask = mm_cidmask(mm);
	/* Clear cids that were not recently used. */
	for_each_possible_cpu(cpu)
		sched_mm_cid_remote_clear_old(mm, cpu);
	weight = cpumask_weight(cidmask);
	/*
	 * Clear cids that are greater or equal to the cidmask weight to
	 * recompact it.
	 */
	for_each_possible_cpu(cpu)
		sched_mm_cid_remote_clear_weight(mm, cpu, weight);
}

void init_sched_mm_cid(struct task_struct *t)
{
	struct mm_struct *mm = t->mm;
	int mm_users = 0;

	if (mm) {
		mm_users = atomic_read(&mm->mm_users);
		if (mm_users == 1)
			mm->mm_cid_next_scan = jiffies + msecs_to_jiffies(MM_CID_SCAN_DELAY);
	}
	t->cid_work.next = &t->cid_work;	/* Protect against double add */
	init_task_work(&t->cid_work, task_mm_cid_work);
}

void task_tick_mm_cid(struct rq *rq, struct task_struct *curr)
{
	struct callback_head *work = &curr->cid_work;
	unsigned long now = jiffies;

	if (!curr->mm || (curr->flags & (PF_EXITING | PF_KTHREAD)) ||
	    work->next != work)
		return;
	if (time_before(now, READ_ONCE(curr->mm->mm_cid_next_scan)))
		return;

	/* No page allocation under rq lock */
	task_work_add(curr, work, TWA_RESUME | TWAF_NO_ALLOC);
}

void sched_mm_cid_exit_signals(struct task_struct *t)
{
	struct mm_struct *mm = t->mm;
	struct rq *rq;

	if (!mm)
		return;

	preempt_disable();
	rq = this_rq();
	guard(rq_lock_irqsave)(rq);
	preempt_enable_no_resched();	/* holding spinlock */
	WRITE_ONCE(t->mm_cid_active, 0);
	/*
	 * Store t->mm_cid_active before loading per-mm/cpu cid.
	 * Matches barrier in sched_mm_cid_remote_clear_old().
	 */
	smp_mb();
	mm_cid_put(mm);
	t->last_mm_cid = t->mm_cid = -1;
}

void sched_mm_cid_before_execve(struct task_struct *t)
{
	struct mm_struct *mm = t->mm;
	struct rq *rq;

	if (!mm)
		return;

	preempt_disable();
	rq = this_rq();
	guard(rq_lock_irqsave)(rq);
	preempt_enable_no_resched();	/* holding spinlock */
	WRITE_ONCE(t->mm_cid_active, 0);
	/*
	 * Store t->mm_cid_active before loading per-mm/cpu cid.
	 * Matches barrier in sched_mm_cid_remote_clear_old().
	 */
	smp_mb();
	mm_cid_put(mm);
	t->last_mm_cid = t->mm_cid = -1;
}

void sched_mm_cid_after_execve(struct task_struct *t)
{
	struct mm_struct *mm = t->mm;
	struct rq *rq;

	if (!mm)
		return;

	preempt_disable();
	rq = this_rq();
	scoped_guard (rq_lock_irqsave, rq) {
		preempt_enable_no_resched();	/* holding spinlock */
		WRITE_ONCE(t->mm_cid_active, 1);
		/*
		 * Store t->mm_cid_active before loading per-mm/cpu cid.
		 * Matches barrier in sched_mm_cid_remote_clear_old().
		 */
		smp_mb();
		t->last_mm_cid = t->mm_cid = mm_cid_get(rq, mm);
	}
	rseq_set_notify_resume(t);
}

void sched_mm_cid_fork(struct task_struct *t)
{
	WARN_ON_ONCE(!t->mm || t->mm_cid != -1);
	t->mm_cid_active = 1;
}
#endif
