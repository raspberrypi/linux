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

static struct hyp_event *find_hyp_event(const char *name)
{
	struct hyp_event *event = __hyp_events_start;

	for (; (unsigned long)event < (unsigned long)__hyp_events_end; event++) {
		if (!strncmp(name, event->name, HYP_EVENT_NAME_MAX))
			return event;
	}

	return NULL;
}

static int enable_hyp_event(struct hyp_event *event, bool enable)
{
	unsigned short id = event->id;
	int ret;

	if (enable == *event->enabled)
		return 0;

	ret = kvm_call_hyp_nvhe(__pkvm_enable_event, id, enable);
	if (ret)
		return ret;

	*event->enabled = enable;

	return 0;
}

static ssize_t
hyp_event_write(struct file *filp, const char __user *ubuf, size_t cnt, loff_t *ppos)
{
	struct seq_file *seq_file = (struct seq_file *)filp->private_data;
	struct hyp_event *evt = (struct hyp_event *)seq_file->private;
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

	ret = enable_hyp_event(evt, enabling);
	if (ret)
		return ret;

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

static int hyp_event_format_show(struct seq_file *m, void *v)
{
	struct hyp_event *evt = (struct hyp_event *)m->private;
	struct trace_event_fields *field;
	unsigned int offset = sizeof(struct hyp_entry_hdr);

	seq_printf(m, "name: %s\n", evt->name);
	seq_printf(m, "ID: %d\n", evt->id);
	seq_puts(m, "format:\n\tfield:unsigned short common_type;\toffset:0;\tsize:2;\tsigned:0;\n");
	seq_puts(m, "\n");

	field = &evt->fields[0];
	while (field->name) {
		seq_printf(m, "\tfield:%s %s;\toffset:%u;\tsize:%u;\tsigned:%d;\n",
			  field->type, field->name, offset, field->size,
			  !!field->is_signed);
		offset += field->size;
		field++;
	}

	if (field != &evt->fields[0])
		seq_puts(m, "\n");

	seq_printf(m, "print fmt: %s\n", evt->print_fmt);

	return 0;
}

static int hyp_event_format_open(struct inode *inode, struct file *file)
{
	return single_open(file, hyp_event_format_show, inode->i_private);
}

static const struct file_operations hyp_event_format_fops = {
	.open = hyp_event_format_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t hyp_header_page_read(struct file *filp, char __user *ubuf,
				   size_t cnt, loff_t *ppos)
{
	struct trace_seq *s;
	ssize_t r;

	s = kmalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	trace_seq_init(s);
	ring_buffer_print_page_header(s);
	r = simple_read_from_buffer(ubuf, cnt, ppos, s->buffer,
				    trace_seq_used(s));
	kfree(s);

	return r;
}

static const struct file_operations hyp_header_page_fops = {
	.read = hyp_header_page_read,
	.llseek = default_llseek,
};

static char early_events[COMMAND_LINE_SIZE];

static __init int setup_hyp_event_early(char *str)
{
	strscpy(early_events, str, COMMAND_LINE_SIZE);

	return 1;
}
__setup("hyp_event=", setup_hyp_event_early);

bool hyp_trace_init_event_early(void)
{
	char *token, *buf = early_events;
	bool enabled = false;

	while (true) {
		token = strsep(&buf, ",");

		if (!token)
			break;

		if (*token) {
			struct hyp_event *event;
			int ret;

			event = find_hyp_event(token);
			if (event) {
				ret = enable_hyp_event(event, true);
				if (ret)
					pr_warn("Couldn't enable hyp event %s:%d\n",
						token, ret);
				else
					enabled = true;
			} else {
				pr_warn("Couldn't find hyp event %s\n", token);
			}
		}

		if (buf)
			*(buf - 1) = ',';
	}

	return enabled;
}

void hyp_trace_init_event_tracefs(struct dentry *parent)
{
	struct hyp_event *event = __hyp_events_start;

	parent = tracefs_create_dir("events", parent);
	if (!parent) {
		pr_err("Failed to create tracefs folder for hyp events\n");
		return;
	}

	tracefs_create_file("header_page", 0400, parent, NULL,
			    &hyp_header_page_fops);

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
		tracefs_create_file("format", 0400, event_dir, (void *)event,
				    &hyp_event_format_fops);
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
