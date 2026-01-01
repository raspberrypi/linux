#ifndef _KERNEL_SCHED_BMQ_H
#define _KERNEL_SCHED_BMQ_H

#define ALT_SCHED_NAME "BMQ"

/*
 * BMQ only routines
 */
static inline void boost_task(struct task_struct *p, int n)
{
	int limit;

	switch (p->policy) {
	case SCHED_NORMAL:
		limit = -MAX_PRIORITY_ADJ;
		break;
	case SCHED_BATCH:
		limit = 0;
		break;
	default:
		return;
	}

	p->boost_prio = max(limit, p->boost_prio - n);
}

static inline void deboost_task(struct task_struct *p)
{
	if (p->boost_prio < MAX_PRIORITY_ADJ)
		p->boost_prio++;
}

/*
 * Common interfaces
 */
static inline void sched_timeslice_imp(const int timeslice_ms) {}

/* This API is used in task_prio(), return value readed by human users */
static inline int
task_sched_prio_normal(const struct task_struct *p, const struct rq *rq)
{
	return p->prio + p->boost_prio - MIN_NORMAL_PRIO;
}

static inline int task_sched_prio(const struct task_struct *p)
{
	return (p->prio < MIN_NORMAL_PRIO)? (p->prio >> 2) :
		MIN_SCHED_NORMAL_PRIO + (p->prio + p->boost_prio - MIN_NORMAL_PRIO) / 2;
}

#define TASK_SCHED_PRIO_IDX(p, rq, idx, prio)	\
	prio = task_sched_prio(p);		\
	idx = prio;

static inline int sched_prio2idx(int prio, struct rq *rq)
{
	return prio;
}

static inline int sched_idx2prio(int idx, struct rq *rq)
{
	return idx;
}

static inline int sched_rq_prio_idx(struct rq *rq)
{
	return rq->prio;
}

static inline int task_running_nice(struct task_struct *p)
{
	return (p->prio + p->boost_prio > DEFAULT_PRIO);
}

static inline void sched_update_rq_clock(struct rq *rq) {}

static inline void sched_task_renew(struct task_struct *p, const struct rq *rq)
{
	deboost_task(p);
}

static inline void sched_task_sanity_check(struct task_struct *p, struct rq *rq) {}
static inline void sched_task_fork(struct task_struct *p, struct rq *rq) {}

static inline void do_sched_yield_type_1(struct task_struct *p, struct rq *rq)
{
	p->boost_prio = MAX_PRIORITY_ADJ;
}

static inline void sched_task_ttwu(struct task_struct *p)
{
	s64 delta = this_rq()->clock_task > p->last_ran;

	if (likely(delta > 0))
		boost_task(p, delta  >> 22);
}

static inline void sched_task_deactivate(struct task_struct *p, struct rq *rq)
{
	boost_task(p, 1);
}

#endif /* _KERNEL_SCHED_BMQ_H */
