// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google, Inc.
 */

#include <linux/cred.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/timekeeping.h>
#include <linux/tracepoint.h>
#include <linux/workqueue.h>

#include <trace/events/oom.h>

#define MEMHEALTH_DIRECTORY "memhealth"
#define OOM_VICTIM_LIST_ENTRY "oom_victim_list"

static wait_queue_head_t memhealth_wq;
static struct proc_dir_entry *proc_memhealth_dir;

/* List of oom victims */
static struct list_head oom_victim_list;
static size_t oom_victim_count;
static size_t oom_victim_removed_count;
/*
 * Lock protecting oom_victim_list, oom_victim_count and
 * oom_victim_removed_count
 */
static DEFINE_MUTEX(memhealth_mutex);
/* List of new oom victims not yet added into oom_victim_list */
static struct list_head new_oom_victims_list;
static size_t new_oom_victims_count;
/* Lock protecting new_oom_victims_list and  new_oom_victims_count */
static DEFINE_SPINLOCK(memhealth_spin_lock);

struct oom_victim {
	pid_t pid;
	uid_t uid;
	char process_name[TASK_COMM_LEN];
	ktime_t timestamp;
	short oom_score_adj;
	struct list_head list;
};

#define OOM_VICTIM_LIST_MAX_SIZE ((PAGE_SIZE/sizeof(struct oom_victim)))

static void oom_list_move_victims(struct work_struct *work)
{
	struct oom_victim *head;
	size_t total_new_nodes;

	mutex_lock(&memhealth_mutex);
	spin_lock(&memhealth_spin_lock);

	if (list_empty(&new_oom_victims_list)) {
		spin_unlock(&memhealth_spin_lock);
		mutex_unlock(&memhealth_mutex);
		return;
	}

	total_new_nodes = new_oom_victims_count;
	list_splice_tail_init(&new_oom_victims_list, &oom_victim_list);
	new_oom_victims_count = 0;

	spin_unlock(&memhealth_spin_lock);

	oom_victim_count += total_new_nodes;
	while (oom_victim_count - oom_victim_removed_count >=
			OOM_VICTIM_LIST_MAX_SIZE) {
		head = list_first_entry(&oom_victim_list, struct oom_victim, list);

		list_del(&head->list);

		kfree(head);
		oom_victim_removed_count++;
	}
	mutex_unlock(&memhealth_mutex);
}
static DECLARE_WORK(memhealth_oom_work, oom_list_move_victims);

static int add_oom_victim_to_list(pid_t pid, ktime_t timestamp)
{
	struct oom_victim *new_node;
	struct task_struct *task;
	const struct cred *cred;
	struct pid *pid_struct;
	int ret = -EINVAL;

	/*
	 * Function could be called while a spinlock is being held, we want to
	 * prevent blocking while allocating for the new node
	 */
	new_node = kmalloc(sizeof(*new_node), GFP_ATOMIC);
	if (!new_node) {
		pr_err("memhealth failed to create new oom node for pid %d\n", pid);
		ret = -ENOMEM;
		goto err_create_oom_node;
	}

	pid_struct = find_get_pid(pid);
	if (!pid_struct) {
		pr_err("memhealth failed to find pid %d\n", pid);
		goto err_get_pid;
	}

	task = get_pid_task(pid_struct, PIDTYPE_PID);
	put_pid(pid_struct);
	if (!task) {
		pr_err("memhealth failed to find task with pid %d\n", pid);
		goto err_get_task;
	}

	cred = get_task_cred(task);
	if (!cred) {
		pr_err("memhealth failed to find credentials\n");
		goto err_get_cred;
	}

	if (!task->signal) {
		pr_err("memhealth failed to find signal in task\n");
		goto err_no_task_signal;
	}
	new_node->oom_score_adj = task->signal->oom_score_adj;

	ret = strscpy_pad(new_node->process_name,
		task->comm, TASK_COMM_LEN);

	if (ret < 0) {
		pr_err("memhealth failed to copy process name to new oom victim node\n");
		goto err_write_process_name;
	}
	put_task_struct(task);

	new_node->pid = pid;
	new_node->timestamp = timestamp;
	new_node->uid = cred->uid.val;

	put_cred(cred);

	spin_lock(&memhealth_spin_lock);
	/*
	 * We add to `new_oom_victims_list` so the file read does not
	 * block the caller of mark victim
	 */
	list_add_tail(&new_node->list, &new_oom_victims_list);
	new_oom_victims_count++;
	spin_unlock(&memhealth_spin_lock);

	schedule_work(&memhealth_oom_work);
	return 0;

err_write_process_name:
err_no_task_signal:
	put_cred(cred);
err_get_cred:
	put_task_struct(task);
err_get_task:
err_get_pid:
	kfree(new_node);
err_create_oom_node:
	return ret;
}

static void mark_victim_probe(void *data, pid_t pid)
{
	ktime_t timestamp;

	timestamp = ktime_get();
	if (add_oom_victim_to_list(pid, timestamp) < 0) {
		pr_err("memhealth failed to add pid(%d) as new OOM killer victim\n", pid);
		return;
	}
	wake_up_interruptible(&memhealth_wq);
}

static void *oom_victim_list_seq_start(struct seq_file *s, loff_t *pos)
{
	mutex_lock(&memhealth_mutex);
	return seq_list_start(&oom_victim_list, *pos);
}

static void *oom_victim_list_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	return seq_list_next(v, &oom_victim_list, pos);
}

static void oom_victim_list_seq_stop(struct seq_file *s, void *v)
{
	mutex_unlock(&memhealth_mutex);
}

static int oom_victim_list_seq_show(struct seq_file *s, void *v)
{
	struct oom_victim *entry = list_entry(v, struct oom_victim, list);

	seq_printf(s, "%llu %d %d %i %s\n",
		ktime_to_ms(entry->timestamp), entry->pid,
		entry->uid, entry->oom_score_adj, entry->process_name
	);

	return 0;
}

static const struct seq_operations oom_victim_list_seq_ops = {
	.start = oom_victim_list_seq_start,
	.next = oom_victim_list_seq_next,
	.stop = oom_victim_list_seq_stop,
	.show = oom_victim_list_seq_show,
};

static int oom_victim_list_seq_open(struct inode *inode, struct file *file)
{
	struct seq_file *seq;
	int seq_open_result;

	if (!capable(CAP_SYS_PTRACE))
		return -EPERM;

	seq_open_result = seq_open(file, &oom_victim_list_seq_ops);
	if (seq_open_result) {
		pr_err("memhealth failed opening OOM sequential file\n");
		return seq_open_result;
	}

	seq = file->private_data;
	seq->poll_event = 0;
	return seq_open_result;
}

static __poll_t oom_victim_list_poll(struct file *filp, poll_table *wait)
{
	struct seq_file *seq = filp->private_data;
	__poll_t mask = DEFAULT_POLLMASK;

	poll_wait(filp, &memhealth_wq, wait);

	mutex_lock(&memhealth_mutex);
	if (seq->poll_event < oom_victim_count) {
		seq->poll_event = oom_victim_count;
		mask |= EPOLLPRI;
	}
	mutex_unlock(&memhealth_mutex);

	return mask;
}

static const struct proc_ops oom_victims_list_proc_ops = {
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = seq_release,
	.proc_open = oom_victim_list_seq_open,
	.proc_poll = oom_victim_list_poll,
};

static int __init memhealthmod_start(void)
{
	struct proc_dir_entry *entry;
	int ret = -ENOMEM;

	proc_memhealth_dir = proc_mkdir(MEMHEALTH_DIRECTORY, NULL);
	if (!proc_memhealth_dir) {
		pr_err("memhealth failed to create directory (%s)\n", MEMHEALTH_DIRECTORY);
		goto err_create_memhealth_dir;
	}

	entry = proc_create(OOM_VICTIM_LIST_ENTRY, 0444,
		proc_memhealth_dir, &oom_victims_list_proc_ops);
	if (!entry) {
		pr_err("memhealth failed to create proc entry: %s\n",
			OOM_VICTIM_LIST_ENTRY);
		goto err_create_oom_entry;
	}

	INIT_LIST_HEAD(&oom_victim_list);
	INIT_LIST_HEAD(&new_oom_victims_list);
	init_waitqueue_head(&memhealth_wq);
	oom_victim_count = 0;
	oom_victim_removed_count = 0;
	new_oom_victims_count = 0;

	ret = register_trace_mark_victim(mark_victim_probe, NULL);
	if (ret) {
		pr_err("memhealth failed to hook a probe to the mark_victim tracepoint\n");
		goto err_register_victim;
	}

	return 0;

err_register_victim:
	remove_proc_entry(OOM_VICTIM_LIST_ENTRY, proc_memhealth_dir);
err_create_oom_entry:
	remove_proc_entry(MEMHEALTH_DIRECTORY, NULL);
err_create_memhealth_dir:
	return ret;
}

static void __exit memhealthmod_end(void)
{
	struct oom_victim *entry, *tmp;

	if (unregister_trace_mark_victim(mark_victim_probe, NULL))
		pr_warn("memhealth failed to unhook a probe from the mark_victim tracepoint\n");

	list_for_each_entry_safe(entry, tmp, &oom_victim_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}

	remove_proc_entry(OOM_VICTIM_LIST_ENTRY, proc_memhealth_dir);
	remove_proc_entry(MEMHEALTH_DIRECTORY, NULL);
}

module_init(memhealthmod_start);
module_exit(memhealthmod_end);

MODULE_LICENSE("GPL");
