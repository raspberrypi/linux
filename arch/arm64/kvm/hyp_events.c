// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 */

#include <linux/tracefs.h>

#include <asm/kvm_host.h>
#include <asm/kvm_define_hypevents.h>
#include <asm/setup.h>

extern struct hyp_event __hyp_events_start[];
extern struct hyp_event __hyp_events_end[];

/* hyp_event section used by the hypervisor */
extern struct hyp_event_id __hyp_event_ids_start[];
extern struct hyp_event_id __hyp_event_ids_end[];

static ssize_t
hyp_event_write(struct file *filp, const char __user *ubuf, size_t cnt, loff_t *ppos)
{
	struct seq_file *seq_file = (struct seq_file *)filp->private_data;
	struct hyp_event *evt = (struct hyp_event *)seq_file->private;
	unsigned short id = evt->id;
	bool enabling;
	int ret;
	char c;

	if (!cnt || cnt > 2)
		return -EINVAL;

	if (get_user(c, ubuf))
		return -EFAULT;

	switch (c) {
	case '1':
		enabling = true;
		break;
	case '0':
		enabling = false;
		break;
	default:
		return -EINVAL;
	}

	if (enabling != *evt->enabled) {
		ret = kvm_call_hyp_nvhe(__pkvm_enable_event, id, enabling);
		if (ret)
			return ret;
	}

	*evt->enabled = enabling;

	return cnt;
}

static int hyp_event_show(struct seq_file *m, void *v)
{
	struct hyp_event *evt = (struct hyp_event *)m->private;

	seq_printf(m, "%d\n", *evt->enabled);

	return 0;
}

static int hyp_event_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, hyp_event_show, inode->i_private);
}

static const struct file_operations hyp_event_fops = {
	.open		= hyp_event_open,
	.write		= hyp_event_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int hyp_event_id_show(struct seq_file *m, void *v)
{
	struct hyp_event *evt = (struct hyp_event *)m->private;

	seq_printf(m, "%d\n", evt->id);

	return 0;
}

static int hyp_event_id_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, hyp_event_id_show, inode->i_private);
}

static const struct file_operations hyp_event_id_fops = {
	.open = hyp_event_id_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

void hyp_trace_init_event_tracefs(struct dentry *parent)
{
	struct hyp_event *event = __hyp_events_start;

	parent = tracefs_create_dir("events", parent);
	if (!parent) {
		pr_err("Failed to create tracefs folder for hyp events\n");
		return;
	}

	parent = tracefs_create_dir("hyp", parent);
	if (!parent) {
		pr_err("Failed to create tracefs folder for hyp events\n");
		return;
	}

	for (; (unsigned long)event < (unsigned long)__hyp_events_end; event++) {
		struct dentry *event_dir = tracefs_create_dir(event->name, parent);
		if (!event_dir) {
			pr_err("Failed to create events/hyp/%s\n", event->name);
			continue;
		}

		tracefs_create_file("enable", 0700, event_dir, (void *)event,
				    &hyp_event_fops);
		tracefs_create_file("id", 0400, event_dir, (void *)event,
				    &hyp_event_id_fops);
	}
}

struct hyp_event *hyp_trace_find_event(int id)
{
	struct hyp_event *event = __hyp_events_start + id;

	if ((unsigned long)event > (unsigned long)__hyp_events_end)
		return NULL;

	return event;
}

/*
 * Register hyp events and write their id into the hyp section _hyp_event_ids.
 */
int hyp_trace_init_events(void)
{
	struct hyp_event_id *hyp_event_id = __hyp_event_ids_start;
	struct hyp_event *event = __hyp_events_start;
	int id = 0;

	for (; (unsigned long)event < (unsigned long)__hyp_events_end;
		event++, hyp_event_id++, id++) {

		/*
		 * Both the host and the hypervisor relies on the same hyp event
		 * declarations from kvm_hypevents.h. We have then a 1:1
		 * mapping.
		 */
		event->id = hyp_event_id->id = id;
	}

	return 0;
}
