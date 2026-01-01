/*
 * kernel/sched/alt_debug.c
 *
 * Print the alt scheduler debugging details
 *
 * Author: Alfred Chen
 * Date  : 2020
 */
#include "sched.h"
#include "linux/sched/debug.h"

/*
 * This allows printing both to /proc/sched_debug and
 * to the console
 */
#define SEQ_printf(m, x...)			\
 do {						\
	if (m)					\
		seq_printf(m, x);		\
	else					\
		pr_cont(x);			\
 } while (0)

void proc_sched_show_task(struct task_struct *p, struct pid_namespace *ns,
			  struct seq_file *m)
{
	SEQ_printf(m, "%s (%d, #threads: %d)\n", p->comm, task_pid_nr_ns(p, ns),
						get_nr_threads(p));
}

void proc_sched_set_task(struct task_struct *p)
{}
