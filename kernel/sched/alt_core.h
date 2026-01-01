#ifndef _KERNEL_SCHED_ALT_CORE_H
#define _KERNEL_SCHED_ALT_CORE_H

/*
 * Compile time debug macro
 * #define ALT_SCHED_DEBUG
 */

/*
 * Task related inlined functions
 */
static inline bool is_migration_disabled(struct task_struct *p)
{
#ifdef CONFIG_SMP
	return p->migration_disabled;
#else
	return false;
#endif
}

/* rt_prio(prio) defined in include/linux/sched/rt.h */
#define rt_task(p)		rt_prio((p)->prio)
#define rt_policy(policy)	((policy) == SCHED_FIFO || (policy) == SCHED_RR)
#define task_has_rt_policy(p)	(rt_policy((p)->policy))

struct affinity_context {
	const struct cpumask	*new_mask;
	struct cpumask		*user_mask;
	unsigned int		flags;
};

/* CONFIG_SCHED_CLASS_EXT is not supported */
#define scx_switched_all()	false

#define SCA_CHECK		0x01
#define SCA_MIGRATE_DISABLE	0x02
#define SCA_MIGRATE_ENABLE	0x04
#define SCA_USER		0x08

#ifdef CONFIG_SMP

extern int __set_cpus_allowed_ptr(struct task_struct *p, struct affinity_context *ctx);

static inline cpumask_t *alloc_user_cpus_ptr(int node)
{
	/*
	 * See do_set_cpus_allowed() above for the rcu_head usage.
	 */
	int size = max_t(int, cpumask_size(), sizeof(struct rcu_head));

	return kmalloc_node(size, GFP_KERNEL, node);
}

#else /* !CONFIG_SMP: */

static inline int __set_cpus_allowed_ptr(struct task_struct *p,
					 struct affinity_context *ctx)
{
	return set_cpus_allowed_ptr(p, ctx->new_mask);
}

static inline cpumask_t *alloc_user_cpus_ptr(int node)
{
	return NULL;
}

#endif /* !CONFIG_SMP */

#ifdef CONFIG_RT_MUTEXES

static inline int __rt_effective_prio(struct task_struct *pi_task, int prio)
{
	if (pi_task)
		prio = min(prio, pi_task->prio);

	return prio;
}

static inline int rt_effective_prio(struct task_struct *p, int prio)
{
	struct task_struct *pi_task = rt_mutex_get_top_task(p);

	return __rt_effective_prio(pi_task, prio);
}

#else /* !CONFIG_RT_MUTEXES: */

static inline int rt_effective_prio(struct task_struct *p, int prio)
{
	return prio;
}

#endif /* !CONFIG_RT_MUTEXES */

extern int __sched_setscheduler(struct task_struct *p, const struct sched_attr *attr, bool user, bool pi);
extern int __sched_setaffinity(struct task_struct *p, struct affinity_context *ctx);
extern void __setscheduler_prio(struct task_struct *p, int prio);

/*
 * Context API
 */
static inline struct rq *__task_access_lock(struct task_struct *p, raw_spinlock_t **plock)
{
	struct rq *rq;
	for (;;) {
		rq = task_rq(p);
		if (p->on_cpu || task_on_rq_queued(p)) {
			raw_spin_lock(&rq->lock);
			if (likely((p->on_cpu || task_on_rq_queued(p)) && rq == task_rq(p))) {
				*plock = &rq->lock;
				return rq;
			}
			raw_spin_unlock(&rq->lock);
		} else if (task_on_rq_migrating(p)) {
			do {
				cpu_relax();
			} while (unlikely(task_on_rq_migrating(p)));
		} else {
			*plock = NULL;
			return rq;
		}
	}
}

static inline void __task_access_unlock(struct task_struct *p, raw_spinlock_t *lock)
{
	if (NULL != lock)
		raw_spin_unlock(lock);
}

void check_task_changed(struct task_struct *p, struct rq *rq);

/*
 * RQ related inlined functions
 */

/*
 * This routine assume that the idle task always in queue
 */
static inline struct task_struct *sched_rq_first_task(struct rq *rq)
{
	const struct list_head *head = &rq->queue.heads[sched_rq_prio_idx(rq)];

	return list_first_entry(head, struct task_struct, sq_node);
}

static inline struct task_struct * sched_rq_next_task(struct task_struct *p, struct rq *rq)
{
	struct list_head *next = p->sq_node.next;

	if (&rq->queue.heads[0] <= next && next < &rq->queue.heads[SCHED_LEVELS]) {
		struct list_head *head;
		unsigned long idx = next - &rq->queue.heads[0];

		idx = find_next_bit(rq->queue.bitmap, SCHED_QUEUE_BITS,
				    sched_idx2prio(idx, rq) + 1);
		head = &rq->queue.heads[sched_prio2idx(idx, rq)];

		return list_first_entry(head, struct task_struct, sq_node);
	}

	return list_next_entry(p, sq_node);
}

extern void requeue_task(struct task_struct *p, struct rq *rq);

#ifdef ALT_SCHED_DEBUG
extern void alt_sched_debug(void);
#else
static inline void alt_sched_debug(void) {}
#endif

extern int sched_yield_type;

#ifdef CONFIG_SMP
extern cpumask_t sched_rq_pending_mask ____cacheline_aligned_in_smp;

DECLARE_STATIC_KEY_FALSE(sched_smt_present);
DECLARE_PER_CPU_ALIGNED(cpumask_t *, sched_cpu_llc_mask);

extern cpumask_t sched_smt_mask ____cacheline_aligned_in_smp;

extern cpumask_t *const sched_idle_mask;
extern cpumask_t *const sched_sg_idle_mask;
extern cpumask_t *const sched_pcore_idle_mask;
extern cpumask_t *const sched_ecore_idle_mask;

extern struct rq *move_queued_task(struct rq *rq, struct task_struct *p, int new_cpu);

typedef bool (*idle_select_func_t)(struct cpumask *dstp, const struct cpumask *src1p,
				   const struct cpumask *src2p);

extern idle_select_func_t idle_select_func;
#endif

/* balance callback */
#ifdef CONFIG_SMP
extern struct balance_callback *splice_balance_callbacks(struct rq *rq);
extern void balance_callbacks(struct rq *rq, struct balance_callback *head);
#else

static inline struct balance_callback *splice_balance_callbacks(struct rq *rq)
{
	return NULL;
}

static inline void balance_callbacks(struct rq *rq, struct balance_callback *head)
{
}

#endif

#endif /* _KERNEL_SCHED_ALT_CORE_H */
