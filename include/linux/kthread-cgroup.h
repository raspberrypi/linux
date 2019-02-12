/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KTHREAD_CGROUP_H
#define _LINUX_KTHREAD_CGROUP_H
#include <linux/kthread.h>
#include <linux/cgroup.h>

#ifdef CONFIG_BLK_CGROUP
void kthread_associate_blkcg(struct cgroup_subsys_state *css);
struct cgroup_subsys_state *kthread_blkcg(void);
#else
static inline void kthread_associate_blkcg(struct cgroup_subsys_state *css) { }
static inline struct cgroup_subsys_state *kthread_blkcg(void)
{
	return NULL;
}
#endif
#endif
