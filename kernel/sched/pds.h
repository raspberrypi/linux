#ifndef _KERNEL_SCHED_PDS_H
#define _KERNEL_SCHED_PDS_H

#define ALT_SCHED_NAME "PDS"

static const u64 RT_MASK = ((1ULL << MIN_SCHED_NORMAL_PRIO) - 1);

#define SCHED_NORMAL_PRIO_NUM	(32)
#define SCHED_EDGE_DELTA	(SCHED_NORMAL_PRIO_NUM - NICE_WIDTH / 2)

/* PDS assume SCHED_NORMAL_PRIO_NUM is power of 2 */
#define SCHED_NORMAL_PRIO_MOD(x)	((x) & (SCHED_NORMAL_PRIO_NUM - 1))

/* default time slice 4ms -> shift 22, 2 time slice slots -> shift 23 */
static __read_mostly int sched_timeslice_shift = 23;

/*
 * Common interfaces
 */
static inline int
task_sched_prio_normal(const struct task_struct *p, const struct rq *rq)
{
	u64 sched_dl = max(p->deadline, rq->time_edge);

#ifdef ALT_SCHED_DEBUG
	if (WARN_ONCE(sched_dl - rq->time_edge > NORMAL_PRIO_NUM - 1,
		      "pds: task_sched_prio_normal() delta %lld\n", sched_dl - rq->time_edge))
		return SCHED_NORMAL_PRIO_NUM - 1;
#endif

	return sched_dl - rq->time_edge;
}

static inline int task_sched_prio(const struct task_struct *p)
{
	return (p->prio < MIN_NORMAL_PRIO) ? (p->prio >> 2) :
		MIN_SCHED_NORMAL_PRIO + task_sched_prio_normal(p, task_rq(p));
}

#define TASK_SCHED_PRIO_IDX(p, rq, idx, prio)							\
	if (p->prio < MIN_NORMAL_PRIO) {							\
		prio = p->prio >> 2;								\
		idx = prio;									\
	} else {										\
		u64 sched_dl = max(p->deadline, rq->time_edge);					\
		prio = MIN_SCHED_NORMAL_PRIO + sched_dl - rq->time_edge;			\
		idx = MIN_SCHED_NORMAL_PRIO + SCHED_NORMAL_PRIO_MOD(sched_dl);			\
	}

static inline int sched_prio2idx(int sched_prio, struct rq *rq)
{
	return (IDLE_TASK_SCHED_PRIO == sched_prio || sched_prio < MIN_SCHED_NORMAL_PRIO) ?
		sched_prio :
		MIN_SCHED_NORMAL_PRIO + SCHED_NORMAL_PRIO_MOD(sched_prio + rq->time_edge);
}

static inline int sched_idx2prio(int sched_idx, struct rq *rq)
{
	return (sched_idx < MIN_SCHED_NORMAL_PRIO) ?
		sched_idx :
		MIN_SCHED_NORMAL_PRIO + SCHED_NORMAL_PRIO_MOD(sched_idx - rq->time_edge);
}

static inline int sched_rq_prio_idx(struct rq *rq)
{
	return rq->prio_idx;
}

static inline int task_running_nice(struct task_struct *p)
{
	return (p->prio > DEFAULT_PRIO);
}

static inline void sched_update_rq_clock(struct rq *rq)
{
	struct list_head head;
	u64 old = rq->time_edge;
	u64 now = rq->clock >> sched_timeslice_shift;
	u64 prio, delta;
	DECLARE_BITMAP(normal, SCHED_QUEUE_BITS);

	if (now == old)
		return;

	rq->time_edge = now;
	delta = min_t(u64, SCHED_NORMAL_PRIO_NUM, now - old);
	INIT_LIST_HEAD(&head);

	prio = MIN_SCHED_NORMAL_PRIO;
	for_each_set_bit_from(prio, rq->queue.bitmap, MIN_SCHED_NORMAL_PRIO + delta)
		list_splice_tail_init(rq->queue.heads + MIN_SCHED_NORMAL_PRIO +
				      SCHED_NORMAL_PRIO_MOD(prio + old), &head);

	bitmap_shift_right(normal, rq->queue.bitmap, delta, SCHED_QUEUE_BITS);
	if (!list_empty(&head)) {
		u64 idx = MIN_SCHED_NORMAL_PRIO + SCHED_NORMAL_PRIO_MOD(now);

		__list_splice(&head, rq->queue.heads + idx, rq->queue.heads[idx].next);
		set_bit(MIN_SCHED_NORMAL_PRIO, normal);
	}
	bitmap_replace(rq->queue.bitmap, normal, rq->queue.bitmap,
		       (const unsigned long *)&RT_MASK, SCHED_QUEUE_BITS);

	if (rq->prio < MIN_SCHED_NORMAL_PRIO || IDLE_TASK_SCHED_PRIO == rq->prio)
		return;

	rq->prio = max_t(u64, MIN_SCHED_NORMAL_PRIO, rq->prio - delta);
	rq->prio_idx = sched_prio2idx(rq->prio, rq);
}

static inline void sched_task_renew(struct task_struct *p, const struct rq *rq)
{
	if (p->prio >= MIN_NORMAL_PRIO)
		p->deadline = rq->time_edge + SCHED_EDGE_DELTA +
			      (p->static_prio - (MAX_PRIO - NICE_WIDTH)) / 2;
}

static inline void sched_task_sanity_check(struct task_struct *p, struct rq *rq)
{
	u64 max_dl = rq->time_edge + SCHED_EDGE_DELTA + NICE_WIDTH / 2 - 1;
	if (unlikely(p->deadline > max_dl))
		p->deadline = max_dl;
}

static inline void sched_task_fork(struct task_struct *p, struct rq *rq)
{
	sched_task_renew(p, rq);
}

static inline void do_sched_yield_type_1(struct task_struct *p, struct rq *rq)
{
	p->time_slice = sysctl_sched_base_slice;
	sched_task_renew(p, rq);
}

static inline void sched_task_ttwu(struct task_struct *p) {}
static inline void sched_task_deactivate(struct task_struct *p, struct rq *rq) {}

#endif /* _KERNEL_SCHED_PDS_H */
