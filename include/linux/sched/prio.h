/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_PRIO_H
#define _LINUX_SCHED_PRIO_H

#define MAX_NICE	19
#define MIN_NICE	-20
#define NICE_WIDTH	(MAX_NICE - MIN_NICE + 1)

/*
 * Priority of a process goes from 0..MAX_PRIO-1, valid RT
 * priority is 0..MAX_RT_PRIO-1, and SCHED_NORMAL/SCHED_BATCH
 * tasks are in the range MAX_RT_PRIO..MAX_PRIO-1. Priority
 * values are inverted: lower p->prio value means higher priority.
 */

#define MAX_RT_PRIO		100
#define MAX_DL_PRIO		0

#define MAX_PRIO		(MAX_RT_PRIO + NICE_WIDTH)
#define DEFAULT_PRIO		(MAX_RT_PRIO + NICE_WIDTH / 2)

#ifdef CONFIG_SCHED_ALT

/* Undefine MAX_PRIO and DEFAULT_PRIO */
#undef MAX_PRIO
#undef DEFAULT_PRIO

/* +/- priority levels from the base priority */
#ifdef CONFIG_SCHED_BMQ
#define MAX_PRIORITY_ADJ	(12)
#endif

#ifdef CONFIG_SCHED_PDS
#define MAX_PRIORITY_ADJ	(0)
#endif

#define MIN_NORMAL_PRIO		(128)
#define NORMAL_PRIO_NUM		(64)
#define MAX_PRIO		(MIN_NORMAL_PRIO + NORMAL_PRIO_NUM)
#define DEFAULT_PRIO		(MAX_PRIO - MAX_PRIORITY_ADJ - NICE_WIDTH / 2)

#endif /* CONFIG_SCHED_ALT */

/*
 * Convert user-nice values [ -20 ... 0 ... 19 ]
 * to static priority [ MAX_RT_PRIO..MAX_PRIO-1 ],
 * and back.
 */
#define NICE_TO_PRIO(nice)	((nice) + DEFAULT_PRIO)
#define PRIO_TO_NICE(prio)	((prio) - DEFAULT_PRIO)

/*
 * Convert nice value [19,-20] to rlimit style value [1,40].
 */
static inline long nice_to_rlimit(long nice)
{
	return (MAX_NICE - nice + 1);
}

/*
 * Convert rlimit style value [1,40] to nice value [-20, 19].
 */
static inline long rlimit_to_nice(long prio)
{
	return (MAX_NICE - prio + 1);
}

#endif /* _LINUX_SCHED_PRIO_H */
