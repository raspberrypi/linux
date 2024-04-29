// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 */

#include <linux/tracefs.h>
#include <linux/rcupdate.h>

#include <asm/kvm_host.h>
#include <asm/setup.h>

static const char *hyp_printk_fmt_from_id(u8 fmt_id);

#include <asm/kvm_define_hypevents.h>

extern char __hyp_printk_fmts_start[];
extern char __hyp_printk_fmts_end[];

#define nr_printk_fmts() ((__hyp_printk_fmts_end - __hyp_printk_fmts_start) / \
				sizeof(struct hyp_printk_fmt))

static const char *hyp_printk_fmt_from_id(u8 fmt_id)
{
	if (fmt_id >= nr_printk_fmts())
		return "Unknown Format";

	return (const char *)(__hyp_printk_fmts_start +
			      (fmt_id * sizeof(struct hyp_printk_fmt)));
}

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

static struct dentry *event_tracefs;
static unsigned int last_event_id;

struct hyp_event_table {
	struct hyp_event	*start;
	unsigned long		nr_events;
};
static struct hyp_event_mod_tables {
	struct hyp_event_table *tables;
	unsigned long		nr_tables;
} mod_event_tables;

#define nr_events(__start, __stop) \
	(((unsigned long)__stop - (unsigned long)__start) / sizeof(*__start))

struct hyp_event *hyp_trace_find_event(int id)
{
	struct hyp_event *event = __hyp_events_start + id;

	if ((unsigned long)event >= (unsigned long)__hyp_events_end) {
		struct hyp_event_table *table;

		event = NULL;
		id -= nr_events(__hyp_events_start, __hyp_events_end);

		rcu_read_lock();
		table = rcu_dereference(mod_event_tables.tables);

		for (int i = 0; i < mod_event_tables.nr_tables; i++) {
			if (table->nr_events < id) {
				id -= table->nr_events;
				table++;
				continue;
			}

			event = table->start + id;
			break;
		}
		rcu_read_unlock();
	}

	return event;
}

static void hyp_event_table_init_tracefs(struct hyp_event *event, int nr_events)
{
	struct dentry *event_dir;
	int i;

	if (!event_tracefs)
		return;

	for (i = 0; i < nr_events; event++, i++) {
		event_dir = tracefs_create_dir(event->name, event_tracefs);
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

/*
 * Register hyp events and write their id into the hyp section _hyp_event_ids.
 */
static int hyp_event_table_init(struct hyp_event *event,
				struct hyp_event_id *event_id, int nr_events)
{
	while (nr_events--) {
		/*
		 * Both the host and the hypervisor rely on the same hyp event
		 * declarations from kvm_hypevents.h. We have then a 1:1
		 * mapping.
		 */
		event->id = event_id->id = last_event_id++;

		event++;
		event_id++;
	}

	return 0;
}

void hyp_trace_init_event_tracefs(struct dentry *parent)
{
	int nr_events = nr_events(__hyp_events_start, __hyp_events_end);

	parent = tracefs_create_dir("events", parent);
	if (!parent) {
		pr_err("Failed to create tracefs folder for hyp events\n");
		return;
	}

	tracefs_create_file("header_page", 0400, parent, NULL,
			    &hyp_header_page_fops);

	event_tracefs = tracefs_create_dir("hyp", parent);
	if (!event_tracefs) {
		pr_err("Failed to create tracefs folder for hyp events\n");
		return;
	}

	hyp_event_table_init_tracefs(__hyp_events_start, nr_events);
}

int hyp_trace_init_events(void)
{
	int nr_events = nr_events(__hyp_events_start, __hyp_events_end);
	int nr_event_ids = nr_events(__hyp_event_ids_start, __hyp_event_ids_end);

	/* __hyp_printk event only supports U8_MAX different formats */
	WARN_ON(nr_printk_fmts() > U8_MAX);

	if (WARN_ON(nr_events != nr_event_ids))
		return -EINVAL;

	return hyp_event_table_init(__hyp_events_start, __hyp_event_ids_start,
				    nr_events);
}

int hyp_trace_init_mod_events(struct hyp_event *event,
			      struct hyp_event_id *event_id, int nr_events)
{
	struct hyp_event_table *tables;
	int ret, i;

	ret = hyp_event_table_init(event, event_id, nr_events);
	if (ret)
		return ret;

	tables = kmalloc_array(mod_event_tables.nr_tables + 1,
			       sizeof(*tables), GFP_KERNEL);
	if (!tables)
		return -ENOMEM;

	for (i = 0; i < mod_event_tables.nr_tables; i++) {
		tables[i].start = mod_event_tables.tables[i].start;
		tables[i].nr_events = mod_event_tables.tables[i].nr_events;
	}
	tables[i].start = event;
	tables[i].nr_events = nr_events;

	tables = rcu_replace_pointer(mod_event_tables.tables, tables, true);
	synchronize_rcu();
	mod_event_tables.nr_tables++;
	kfree(tables);

	hyp_event_table_init_tracefs(event, nr_events);

	return 0;
}
