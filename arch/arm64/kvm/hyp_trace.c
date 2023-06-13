// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 */

#include <linux/arm-smccc.h>
#include <linux/list.h>
#include <linux/percpu-defs.h>
#include <linux/tracefs.h>

#include <asm/kvm_host.h>
#include <asm/kvm_hyptrace.h>

#include "hyp_constants.h"
#include "hyp_trace.h"

#define RB_POLL_MS 100

#define TRACEFS_DIR "hyp"
#define TRACEFS_MODE_WRITE 0640
#define TRACEFS_MODE_READ 0440

static struct hyp_trace_buffer {
	struct hyp_trace_desc		*desc;
	struct ring_buffer_writer	writer;
	struct trace_buffer		*trace_buffer;
	size_t				desc_size;
	bool				tracing_on;
	int				nr_readers;
	struct mutex			lock;
} hyp_trace_buffer = {
	.lock			= __MUTEX_INITIALIZER(hyp_trace_buffer.lock),
};

static size_t hyp_trace_buffer_size = 7 << 10;

/* Number of pages the ring-buffer requires to accommodate for size */
#define NR_PAGES(size) \
	((PAGE_ALIGN(size) >> PAGE_SHIFT) + 1)

static inline bool hyp_trace_buffer_loaded(struct hyp_trace_buffer *hyp_buffer)
{
	return !!hyp_buffer->trace_buffer;
}

static int
bpage_backing_alloc(struct hyp_buffer_pages_backing *bpage_backing, size_t size)
{
	size_t backing_size;
	void *start;

	backing_size = PAGE_ALIGN(STRUCT_HYP_BUFFER_PAGE_SIZE * NR_PAGES(size));

	start = alloc_pages_exact(backing_size, GFP_KERNEL_ACCOUNT);
	if (!start)
		return -ENOMEM;

	bpage_backing->start = (unsigned long)start;
	bpage_backing->size = backing_size;

	return 0;
}

static void
bpage_backing_free(struct hyp_buffer_pages_backing *bpage_backing)
{
	free_pages_exact((void *)bpage_backing->start, bpage_backing->size);
}

/*
 * Configure the hyp tracing clock. So far, only one is supported: "boot". This
 * clock doesn't stop during suspend making it a good candidate. The downside is
 * if this clock is corrected by NTP while tracing, the hyp clock will slightly
 * drift compared to the host version.
 */
static void hyp_clock_setup(struct hyp_trace_desc *desc)
{
	struct kvm_nvhe_clock_data *clock_data = &desc->clock_data;
	struct system_time_snapshot snap;

	ktime_get_snapshot(&snap);

	clock_data->epoch_cyc = snap.cycles;
	clock_data->epoch_ns = snap.boot;
	clock_data->mult = snap.mono_mult;
	clock_data->shift = snap.mono_shift;
}

static int __swap_reader(int cpu)
{
	return kvm_call_hyp_nvhe(__pkvm_swap_reader_tracing, cpu);
}

static void hyp_trace_free_pages(struct hyp_trace_desc *desc)
{
	struct rb_page_desc *rb_desc;
	int cpu, id;

	for_each_rb_page_desc(rb_desc, cpu, &desc->page_desc) {
		free_page(rb_desc->meta_va);
		for (id = 0; id < rb_desc->nr_page_va; id++)
			free_page(rb_desc->page_va[id]);
	}
}

static int hyp_trace_alloc_pages(struct hyp_trace_desc *desc, size_t size)
{
	int err = 0, cpu, id, nr_pages = NR_PAGES(size);
	struct trace_page_desc *trace_desc;
	struct rb_page_desc *rb_desc;

	trace_desc = &desc->page_desc;
	trace_desc->nr_cpus = 0;

	rb_desc = (struct rb_page_desc *)&trace_desc->__data[0];

	for_each_possible_cpu(cpu) {
		rb_desc->cpu = cpu;
		rb_desc->nr_page_va = 0;
		rb_desc->meta_va = (unsigned long)page_to_virt(alloc_page(GFP_KERNEL));
		if (!rb_desc->meta_va) {
			err = -ENOMEM;
			break;
		}
		for (id = 0; id < nr_pages; id++) {
			rb_desc->page_va[id] = (unsigned long)page_to_virt(alloc_page(GFP_KERNEL));
			if (!rb_desc->page_va[id]) {
				err = -ENOMEM;
				break;
			}
			rb_desc->nr_page_va++;
		}
		trace_desc->nr_cpus++;
		rb_desc = __next_rb_page_desc(rb_desc);
	}

	if (err) {
		hyp_trace_free_pages(desc);
		return err;
	}

	return 0;
}

static int __load_page(unsigned long va)
{
	return kvm_call_hyp_nvhe(__pkvm_host_share_hyp, virt_to_pfn((void *)va), 1);
}

static void __teardown_page(unsigned long va)
{
	WARN_ON(kvm_call_hyp_nvhe(__pkvm_host_unshare_hyp, virt_to_pfn((void *)va), 1));
}

static void hyp_trace_teardown_pages(struct hyp_trace_desc *desc,
				     int last_cpu)
{
	struct rb_page_desc *rb_desc;
	int cpu, id;

	for_each_rb_page_desc(rb_desc, cpu, &desc->page_desc) {
		if (cpu > last_cpu)
			break;
		__teardown_page(rb_desc->meta_va);
		for (id = 0; id < rb_desc->nr_page_va; id++)
			__teardown_page(rb_desc->page_va[id]);
	}
}

static int hyp_trace_load_pages(struct hyp_trace_desc *desc)
{
	int last_loaded_cpu = 0, cpu, id, err = -EINVAL;
	struct rb_page_desc *rb_desc;

	for_each_rb_page_desc(rb_desc, cpu, &desc->page_desc) {
		err = __load_page(rb_desc->meta_va);
		if (err)
			break;

		for (id = 0; id < rb_desc->nr_page_va; id++) {
			err = __load_page(rb_desc->page_va[id]);
			if (err)
				break;
		}

		if (!err)
			continue;

		for (id--; id >= 0; id--)
			__teardown_page(rb_desc->page_va[id]);

		last_loaded_cpu = cpu - 1;

		break;
	}

	if (!err)
		return 0;

	hyp_trace_teardown_pages(desc, last_loaded_cpu);

	return err;
}

static int hyp_trace_buffer_load(struct hyp_trace_buffer *hyp_buffer, size_t size)
{
	int ret, nr_pages = NR_PAGES(size);
	struct rb_page_desc *rbdesc;
	struct hyp_trace_desc *desc;
	size_t desc_size;

	if (hyp_trace_buffer_loaded(hyp_buffer))
		return 0;

	desc_size = size_add(offsetof(struct hyp_trace_desc, page_desc),
			     offsetof(struct trace_page_desc, __data));
	desc_size = size_add(desc_size,
			     size_mul(num_possible_cpus(),
				      struct_size(rbdesc, page_va, nr_pages)));
	if (desc_size == SIZE_MAX)
		return -E2BIG;

	/*
	 * The hypervisor will unmap the descriptor from the host to protect the
	 * reading. Page granularity for the allocation ensures no other
	 * useful data will be unmapped.
	 */
	desc_size = PAGE_ALIGN(desc_size);

	desc = (struct hyp_trace_desc *)alloc_pages_exact(desc_size, GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	ret = hyp_trace_alloc_pages(desc, size);
	if (ret)
		goto err_free_desc;

	ret = bpage_backing_alloc(&desc->backing, size);
	if (ret)
		goto err_free_pages;

	ret = hyp_trace_load_pages(desc);
	if (ret)
		goto err_free_backing;

	hyp_clock_setup(desc);

	ret = kvm_call_hyp_nvhe(__pkvm_load_tracing, (unsigned long)desc, desc_size);
	if (ret)
		goto err_teardown_pages;

	hyp_buffer->writer.pdesc = &desc->page_desc;
	hyp_buffer->writer.get_reader_page = __swap_reader;
	hyp_buffer->trace_buffer = ring_buffer_reader(&hyp_buffer->writer);
	if (!hyp_buffer->trace_buffer) {
		ret = -ENOMEM;
		goto err_teardown_tracing;
	}

	hyp_buffer->desc = desc;
	hyp_buffer->desc_size = desc_size;

	return 0;

err_teardown_tracing:
	kvm_call_hyp_nvhe(__pkvm_teardown_tracing);
err_teardown_pages:
	hyp_trace_teardown_pages(desc, INT_MAX);
err_free_backing:
	bpage_backing_free(&desc->backing);
err_free_pages:
	hyp_trace_free_pages(desc);
err_free_desc:
	free_pages_exact(desc, desc_size);

	return ret;
}

static void hyp_trace_buffer_teardown(struct hyp_trace_buffer *hyp_buffer)
{
	struct hyp_trace_desc *desc = hyp_buffer->desc;
	size_t desc_size = hyp_buffer->desc_size;

	if (kvm_call_hyp_nvhe(__pkvm_teardown_tracing))
		return;
	ring_buffer_free(hyp_buffer->trace_buffer);
	hyp_trace_teardown_pages(desc, INT_MAX);
	bpage_backing_free(&desc->backing);
	hyp_trace_free_pages(desc);
	free_pages_exact(desc, desc_size);
	hyp_buffer->trace_buffer = NULL;
}

static int hyp_tracing_teardown(void)
{
	struct hyp_trace_buffer *hyp_buffer = &hyp_trace_buffer;
	int ret = 0;

	mutex_lock(&hyp_buffer->lock);
	if (!hyp_trace_buffer_loaded(hyp_buffer))
		goto out;

	if (hyp_buffer->tracing_on || hyp_buffer->nr_readers) {
		ret = -EBUSY;
		goto out;
	}

	hyp_trace_buffer_teardown(hyp_buffer);
out:
	mutex_unlock(&hyp_buffer->lock);

	return ret;
}

static int hyp_tracing_start(void)
{
	struct hyp_trace_buffer *hyp_buffer = &hyp_trace_buffer;
	int ret;

	mutex_lock(&hyp_buffer->lock);

	ret = hyp_trace_buffer_load(hyp_buffer, hyp_trace_buffer_size);
	if (ret)
		goto out;

	ret = kvm_call_hyp_nvhe(__pkvm_enable_tracing, true);
	if (!ret)
		hyp_buffer->tracing_on = true;
out:
	mutex_unlock(&hyp_buffer->lock);

	return ret;
}

static void hyp_tracing_stop(void)
{
	struct hyp_trace_buffer *hyp_buffer = &hyp_trace_buffer;
	int ret;

	mutex_lock(&hyp_buffer->lock);
	if (!hyp_trace_buffer_loaded(hyp_buffer))
		goto end;

	ret = kvm_call_hyp_nvhe(__pkvm_enable_tracing, false);
	if (!ret) {
		/*
		 * There are no way to flush the remaining data on reader
		 * release. So instead, do it when tracing stops.
		 */
		ring_buffer_poll_writer(hyp_buffer->trace_buffer,
					RING_BUFFER_ALL_CPUS);
		hyp_buffer->tracing_on = false;
	}
end:
	mutex_unlock(&hyp_buffer->lock);
}

static ssize_t
hyp_tracing_on(struct file *filp, const char __user *ubuf, size_t cnt, loff_t *ppos)
{
	int err = 0;
	char c;

	if (!cnt || cnt > 2)
		return -EINVAL;

	if (get_user(c, ubuf))
		return -EFAULT;

	switch (c) {
	case '1':
		err = hyp_tracing_start();
		break;
	case '0':
		hyp_tracing_stop();
		break;
	default:
		err = -EINVAL;
	}

	return err ? err : cnt;
}

static ssize_t hyp_tracing_on_read(struct file *filp, char __user *ubuf,
				   size_t cnt, loff_t *ppos)
{
	char buf[3];
	int r;

	mutex_lock(&hyp_trace_buffer.lock);
	r = sprintf(buf, "%d\n", hyp_trace_buffer.tracing_on);
	mutex_unlock(&hyp_trace_buffer.lock);

	return simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
}

static const struct file_operations hyp_tracing_on_fops = {
	.write	= hyp_tracing_on,
	.read	= hyp_tracing_on_read,
};

static ssize_t hyp_buffer_size(struct file *filp, const char __user *ubuf,
			       size_t cnt, loff_t *ppos)
{
	unsigned long val;
	int ret;

	ret = kstrtoul_from_user(ubuf, cnt, 10, &val);
	if (ret)
		return ret;

	if (!val)
		return -EINVAL;

	mutex_lock(&hyp_trace_buffer.lock);
	hyp_trace_buffer_size = val << 10; /* KB to B */
	mutex_unlock(&hyp_trace_buffer.lock);

	return cnt;
}

static ssize_t hyp_buffer_size_read(struct file *filp, char __user *ubuf,
				    size_t cnt, loff_t *ppos)
{
	char buf[64];
	int r;

	mutex_lock(&hyp_trace_buffer.lock);
	r = sprintf(buf, "%lu\n", hyp_trace_buffer_size >> 10);
	mutex_unlock(&hyp_trace_buffer.lock);

	return simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
}

static const struct file_operations hyp_buffer_size_fops = {
	.write	= hyp_buffer_size,
	.read	= hyp_buffer_size_read,
};

static void ht_print_trace_time(struct ht_iterator *iter)
{
	unsigned long usecs_rem;
	u64 ts_ns = iter->ts;

	do_div(ts_ns, 1000);
	usecs_rem = do_div(ts_ns, USEC_PER_SEC);

	trace_seq_printf(&iter->seq, "%5lu.%06lu: ",
			 (unsigned long)ts_ns, usecs_rem);
}

static void ht_print_trace_cpu(struct ht_iterator *iter)
{
	trace_seq_printf(&iter->seq, "[%03d]\t", iter->ent_cpu);
}

static int ht_print_trace_fmt(struct ht_iterator *iter)
{
	if (iter->lost_events)
		trace_seq_printf(&iter->seq, "CPU:%d [LOST %lu EVENTS]\n",
				 iter->ent_cpu, iter->lost_events);

	ht_print_trace_cpu(iter);
	ht_print_trace_time(iter);

	return trace_seq_has_overflowed(&iter->seq) ? -EOVERFLOW : 0;
};

static struct ring_buffer_event *__ht_next_pipe_event(struct ht_iterator *iter)
{
	struct ring_buffer_event *evt = NULL;
	int cpu = iter->cpu;

	if (cpu != RING_BUFFER_ALL_CPUS) {
		if (ring_buffer_empty_cpu(iter->trace_buffer, cpu))
			return NULL;

		iter->ent_cpu = cpu;

		return ring_buffer_peek(iter->trace_buffer, cpu, &iter->ts,
					&iter->lost_events);
	}

	iter->ts = LLONG_MAX;
	for_each_possible_cpu(cpu) {
		struct ring_buffer_event *_evt;
		unsigned long lost_events;
		u64 ts;

		if (ring_buffer_empty_cpu(iter->trace_buffer, cpu))
			continue;

		_evt = ring_buffer_peek(iter->trace_buffer, cpu, &ts,
					&lost_events);
		if (!_evt)
			continue;

		if (ts >= iter->ts)
			continue;

		iter->ts = ts;
		iter->ent_cpu = cpu;
		iter->lost_events = lost_events;
		evt = _evt;
	}

	return evt;
}

static void *ht_next_pipe_event(struct ht_iterator *iter)
{
	struct ring_buffer_event *event;

	event = __ht_next_pipe_event(iter);
	if (!event)
		return NULL;

	iter->ent = (struct hyp_entry_hdr *)&event->array[1];
	iter->ent_size = event->array[0];

	return iter;
}

static ssize_t
hyp_trace_pipe_read(struct file *file, char __user *ubuf,
		    size_t cnt, loff_t *ppos)
{
	struct ht_iterator *iter = (struct ht_iterator *)file->private_data;
	int ret;

	trace_seq_init(&iter->seq);
again:
	ret = ring_buffer_wait(iter->trace_buffer, iter->cpu, 0);
	if (ret < 0)
		return ret;

	while (ht_next_pipe_event(iter)) {
		int prev_len = iter->seq.seq.len;

		if (ht_print_trace_fmt(iter)) {
			iter->seq.seq.len = prev_len;
			break;
		}

		ring_buffer_consume(iter->trace_buffer, iter->ent_cpu, NULL,
				    NULL);
	}

	ret = trace_seq_to_user(&iter->seq, ubuf, cnt);
	if (ret == -EBUSY)
		goto again;

	return ret;
}

static void __poll_writer(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct ht_iterator *iter;

	iter = container_of(dwork, struct ht_iterator, poll_work);

	ring_buffer_poll_writer(iter->trace_buffer, iter->cpu);

	schedule_delayed_work((struct delayed_work *)work,
			      msecs_to_jiffies(RB_POLL_MS));
}

static int hyp_trace_pipe_open(struct inode *inode, struct file *file)
{
	struct hyp_trace_buffer *hyp_buffer = &hyp_trace_buffer;
	int cpu = (s64)inode->i_private;
	struct ht_iterator *iter = NULL;
	bool need_loading = false;
	int ret;

	mutex_lock(&hyp_buffer->lock);

	need_loading = !hyp_trace_buffer_loaded(hyp_buffer);
	if (need_loading) {
		ret = hyp_trace_buffer_load(hyp_buffer, hyp_trace_buffer_size);
		if (ret) {
			need_loading = false;
			goto unlock;
		}
	}

	iter = kzalloc(sizeof(*iter), GFP_KERNEL);
	if (!iter) {
		ret = -ENOMEM;
		goto unlock;
	}
	iter->trace_buffer = hyp_buffer->trace_buffer;
	iter->cpu = cpu;
	file->private_data = iter;

	ret = ring_buffer_poll_writer(hyp_buffer->trace_buffer, cpu);
	if (ret)
		goto unlock;

	INIT_DELAYED_WORK(&iter->poll_work, __poll_writer);
	schedule_delayed_work(&iter->poll_work, msecs_to_jiffies(RB_POLL_MS));

	hyp_buffer->nr_readers++;
unlock:
	if (ret) {
		if (need_loading)
			hyp_trace_buffer_teardown(hyp_buffer);
		kfree(iter);
	}

	mutex_unlock(&hyp_buffer->lock);

	return ret;
}

static int hyp_trace_pipe_release(struct inode *inode, struct file *file)
{
	struct hyp_trace_buffer *hyp_buffer = &hyp_trace_buffer;
	struct ht_iterator *iter = file->private_data;

	cancel_delayed_work_sync(&iter->poll_work);

	mutex_lock(&hyp_buffer->lock);
	WARN_ON(--hyp_buffer->nr_readers < 0);
	mutex_unlock(&hyp_buffer->lock);

	kfree(iter);

	return 0;
}

static const struct file_operations hyp_trace_pipe_fops = {
	.open           = hyp_trace_pipe_open,
	.read           = hyp_trace_pipe_read,
	.release        = hyp_trace_pipe_release,
	.llseek         = no_llseek,
};

static int hyp_trace_open(struct inode *inode, struct file *file)
{
	return file->f_mode & FMODE_WRITE ? hyp_tracing_teardown() : 0;
}

static ssize_t hyp_trace_read(struct file *filp, char __user *ubuf,
			      size_t cnt, loff_t *ppos)
{
	char buf[] = "** Reading trace not yet supported **\n";

	return simple_read_from_buffer(ubuf, cnt, ppos, buf, strlen(buf));
}

static ssize_t hyp_trace_write(struct file *filp, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	return count;
}

static const struct file_operations hyp_trace_fops = {
	.open           = hyp_trace_open,
	.read           = hyp_trace_read,
	.write          = hyp_trace_write,
	.release        = NULL,
};

int hyp_trace_init_tracefs(void)
{
	struct dentry *root, *per_cpu_root;
	char per_cpu_name[16];
	long cpu;

	if (!is_protected_kvm_enabled())
		return 0;

	root = tracefs_create_dir(TRACEFS_DIR, NULL);
	if (!root) {
		pr_err("Failed to create tracefs "TRACEFS_DIR"/\n");
		return -ENODEV;
	}

	tracefs_create_file("tracing_on", TRACEFS_MODE_WRITE, root, NULL,
			    &hyp_tracing_on_fops);

	tracefs_create_file("buffer_size_kb", TRACEFS_MODE_WRITE, root, NULL,
			    &hyp_buffer_size_fops);

	tracefs_create_file("trace", TRACEFS_MODE_WRITE, root,
			    (void *)RING_BUFFER_ALL_CPUS, &hyp_trace_fops);

	tracefs_create_file("trace_pipe", TRACEFS_MODE_WRITE, root,
			    (void *)RING_BUFFER_ALL_CPUS, &hyp_trace_pipe_fops);

	per_cpu_root = tracefs_create_dir("per_cpu", root);
	if (!per_cpu_root) {
		pr_err("Failed to create tracefs folder "TRACEFS_DIR"/per_cpu/\n");
		return -ENODEV;
	}

	for_each_possible_cpu(cpu) {
		struct dentry *per_cpu_dir;

		snprintf(per_cpu_name, sizeof(per_cpu_name), "cpu%ld", cpu);
		per_cpu_dir = tracefs_create_dir(per_cpu_name, per_cpu_root);
		if (!per_cpu_dir) {
			pr_warn("Failed to create tracefs "TRACEFS_DIR"/per_cpu/cpu%ld\n",
				cpu);
			continue;
		}
		tracefs_create_file("trace_pipe", TRACEFS_MODE_READ, per_cpu_dir,
				    (void *)cpu, &hyp_trace_pipe_fops);
	}

	return 0;
}
