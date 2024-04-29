// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Vincent Donnefort <vdonnefort@google.com>
 */

#include <nvhe/alloc.h>
#include <nvhe/clock.h>
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>
#include <nvhe/trace/trace.h>

#include <asm/percpu.h>
#include <asm/kvm_mmu.h>
#include <asm/local.h>

#define HYP_RB_PAGE_HEAD		1UL
#define HYP_RB_PAGE_UPDATE		2UL
#define HYP_RB_FLAG_MASK		3UL

struct hyp_buffer_page {
	struct list_head	list;
	struct buffer_data_page	*page;
	unsigned long		write;
	unsigned long		entries;
	u32			id;
};

struct hyp_rb_per_cpu {
	struct ring_buffer_meta	*meta;
	struct hyp_buffer_page	*tail_page;
	struct hyp_buffer_page	*reader_page;
	struct hyp_buffer_page	*head_page;
	struct hyp_buffer_page	*bpages;
	unsigned long		nr_pages;
	unsigned long		last_overrun;
	u64			write_stamp;
	atomic_t		status;
};

#define HYP_RB_UNAVAILABLE	0
#define HYP_RB_READY		1
#define HYP_RB_WRITING		2

DEFINE_PER_CPU(struct hyp_rb_per_cpu, trace_rb);
DEFINE_HYP_SPINLOCK(trace_rb_lock);

static bool rb_set_flag(struct hyp_buffer_page *bpage, int new_flag)
{
	unsigned long ret, val = (unsigned long)bpage->list.next;

	ret = cmpxchg((unsigned long *)&bpage->list.next,
		      val, (val & ~HYP_RB_FLAG_MASK) | new_flag);

	return ret == val;
}

static struct hyp_buffer_page *rb_hyp_buffer_page(struct list_head *list)
{
	unsigned long ptr = (unsigned long)list & ~HYP_RB_FLAG_MASK;

	return container_of((struct list_head *)ptr, struct hyp_buffer_page, list);
}

static struct hyp_buffer_page *rb_next_page(struct hyp_buffer_page *bpage)
{
	return rb_hyp_buffer_page(bpage->list.next);
}

static bool rb_is_head_page(struct hyp_buffer_page *bpage)
{
	return (unsigned long)bpage->list.prev->next & HYP_RB_PAGE_HEAD;
}

static struct hyp_buffer_page *rb_set_head_page(struct hyp_rb_per_cpu *cpu_buffer)
{
	struct hyp_buffer_page *bpage, *prev_head;
	int cnt = 0;
again:
	bpage = prev_head = cpu_buffer->head_page;
	do {
		if (rb_is_head_page(bpage)) {
			cpu_buffer->head_page = bpage;
			return bpage;
		}

		bpage = rb_next_page(bpage);
	} while (bpage != prev_head);

	/* We might have race with the writer let's try again */
	if (++cnt < 3)
		goto again;

	return NULL;
}

static int rb_swap_reader_page(struct hyp_rb_per_cpu *cpu_buffer)
{
	unsigned long *old_head_link, old_link_val, new_link_val, overrun;
	struct hyp_buffer_page *head, *reader = cpu_buffer->reader_page;
spin:
	/* Update the cpu_buffer->header_page according to HYP_RB_PAGE_HEAD */
	head = rb_set_head_page(cpu_buffer);
	if (!head)
		return -ENODEV;

	/* Connect the reader page around the header page */
	reader->list.next = head->list.next;
	reader->list.prev = head->list.prev;

	/* The reader page points to the new header page */
	rb_set_flag(reader, HYP_RB_PAGE_HEAD);

	/*
	 * Paired with the cmpxchg in rb_move_tail(). Order the read of the head
	 * page and overrun.
	 */
	smp_mb();
	overrun = READ_ONCE(cpu_buffer->meta->overrun);

	/* Try to swap the prev head link to the reader page */
	old_head_link = (unsigned long *)&reader->list.prev->next;
	old_link_val = (*old_head_link & ~HYP_RB_FLAG_MASK) | HYP_RB_PAGE_HEAD;
	new_link_val = (unsigned long)&reader->list;
	if (cmpxchg(old_head_link, old_link_val, new_link_val)
		      != old_link_val)
		goto spin;

	cpu_buffer->head_page = rb_hyp_buffer_page(reader->list.next);
	cpu_buffer->head_page->list.prev = &reader->list;
	cpu_buffer->reader_page = head;
	cpu_buffer->meta->reader_page.lost_events = overrun - cpu_buffer->last_overrun;
	cpu_buffer->meta->reader_page.id = cpu_buffer->reader_page->id;
	cpu_buffer->last_overrun = overrun;

	return 0;
}

static struct hyp_buffer_page *
rb_move_tail(struct hyp_rb_per_cpu *cpu_buffer)
{
	struct hyp_buffer_page *tail_page, *new_tail, *new_head;

	tail_page = cpu_buffer->tail_page;
	new_tail = rb_next_page(tail_page);
again:
	/*
	 * We caught the reader ... Let's try to move the head page.
	 * The writer can only rely on ->next links to check if this is head.
	 */
	if ((unsigned long)tail_page->list.next & HYP_RB_PAGE_HEAD) {
		/* The reader moved the head in between */
		if (!rb_set_flag(tail_page, HYP_RB_PAGE_UPDATE))
			goto again;

		WRITE_ONCE(cpu_buffer->meta->overrun,
			   cpu_buffer->meta->overrun + new_tail->entries);
		WRITE_ONCE(cpu_buffer->meta->pages_lost,
			   cpu_buffer->meta->pages_lost + 1);

		/* Move the head */
		rb_set_flag(new_tail, HYP_RB_PAGE_HEAD);

		/* The new head is in place, reset the update flag */
		rb_set_flag(tail_page, 0);

		new_head = rb_next_page(new_tail);
	}

	local_set(&new_tail->page->commit, 0);

	new_tail->write = 0;
	new_tail->entries = 0;

	WRITE_ONCE(cpu_buffer->meta->pages_touched,
		   cpu_buffer->meta->pages_touched + 1);
	cpu_buffer->tail_page = new_tail;

	return new_tail;
}

unsigned long rb_event_size(unsigned long length)
{
	struct ring_buffer_event *event;

	return length + RB_EVNT_HDR_SIZE + sizeof(event->array[0]);
}

static struct ring_buffer_event *
rb_add_ts_extend(struct ring_buffer_event *event, u64 delta)
{
	event->type_len = RINGBUF_TYPE_TIME_EXTEND;
	event->time_delta = delta & TS_MASK;
	event->array[0] = delta >> TS_SHIFT;

	return (struct ring_buffer_event *)((unsigned long)event + 8);
}

static struct ring_buffer_event *
rb_reserve_next(struct hyp_rb_per_cpu *cpu_buffer, unsigned long length)
{
	unsigned long ts_ext_size = 0, event_size = rb_event_size(length);
	struct hyp_buffer_page *tail_page = cpu_buffer->tail_page;
	struct ring_buffer_event *event;
	unsigned long write, prev_write;
	u64 ts, time_delta;

	ts = trace_clock();

	time_delta = ts - cpu_buffer->write_stamp;

	if (test_time_stamp(time_delta))
		ts_ext_size = 8;

	prev_write = tail_page->write;
	write = prev_write + event_size + ts_ext_size;

	if (unlikely(write > BUF_PAGE_SIZE))
		tail_page = rb_move_tail(cpu_buffer);

	if (!tail_page->entries) {
		tail_page->page->time_stamp = ts;
		time_delta = 0;
		ts_ext_size = 0;
		write = event_size;
		prev_write = 0;
	}

	tail_page->write = write;
	tail_page->entries++;

	cpu_buffer->write_stamp = ts;

	event = (struct ring_buffer_event *)(tail_page->page->data +
					     prev_write);
	if (ts_ext_size) {
		event = rb_add_ts_extend(event, time_delta);
		time_delta = 0;
	}

	event->type_len = 0;
	event->time_delta = time_delta;
	event->array[0] = event_size - RB_EVNT_HDR_SIZE;

	return event;
}

void *tracing_reserve_entry(unsigned long length)
{
	struct hyp_rb_per_cpu *cpu_buffer = this_cpu_ptr(&trace_rb);
	struct ring_buffer_event *rb_event;

	if (atomic_cmpxchg(&cpu_buffer->status, HYP_RB_READY, HYP_RB_WRITING)
	    == HYP_RB_UNAVAILABLE)
		return NULL;

	rb_event = rb_reserve_next(cpu_buffer, length);

	return &rb_event->array[1];
}

void tracing_commit_entry(void)
{
	struct hyp_rb_per_cpu *cpu_buffer = this_cpu_ptr(&trace_rb);

	local_set(&cpu_buffer->tail_page->page->commit,
		  cpu_buffer->tail_page->write);
	WRITE_ONCE(cpu_buffer->meta->entries,
		   cpu_buffer->meta->entries + 1);

	/* Paired with rb_cpu_disable_writing() */
	atomic_set_release(&cpu_buffer->status, HYP_RB_READY);
}

static int rb_page_init(struct hyp_buffer_page *bpage, unsigned long hva)
{
	void *hyp_va = (void *)kern_hyp_va(hva);
	int ret;

	ret = hyp_pin_shared_mem(hyp_va, hyp_va + PAGE_SIZE);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&bpage->list);
	bpage->page = (struct buffer_data_page *)hyp_va;

	local_set(&bpage->page->commit, 0);

	return 0;
}

static bool rb_cpu_loaded(struct hyp_rb_per_cpu *cpu_buffer)
{
	return !!cpu_buffer->bpages;
}

static void rb_cpu_disable_writing(struct hyp_rb_per_cpu *cpu_buffer)
{
	int prev_status;

	/* Wait for release of the buffer */
	do {
		prev_status = atomic_cmpxchg_acquire(&cpu_buffer->status,
						     HYP_RB_READY,
						     HYP_RB_UNAVAILABLE);
	} while (prev_status == HYP_RB_WRITING);
}

static int rb_cpu_enable_writing(struct hyp_rb_per_cpu *cpu_buffer)
{
	if (!rb_cpu_loaded(cpu_buffer))
		return -ENODEV;

	atomic_cmpxchg(&cpu_buffer->status, HYP_RB_UNAVAILABLE, HYP_RB_READY);

	return 0;
}

static void rb_cpu_teardown(struct hyp_rb_per_cpu *cpu_buffer)
{
	int i;

	if (!rb_cpu_loaded(cpu_buffer))
		return;

	rb_cpu_disable_writing(cpu_buffer);

	hyp_unpin_shared_mem((void *)cpu_buffer->meta,
			     (void *)(cpu_buffer->meta) + PAGE_SIZE);

	for (i = 0; i < cpu_buffer->nr_pages; i++) {
		struct hyp_buffer_page *bpage = &cpu_buffer->bpages[i];

		if (!bpage->page)
			continue;

		hyp_unpin_shared_mem((void *)bpage->page,
				     (void *)bpage->page + PAGE_SIZE);
	}

	hyp_free(cpu_buffer->bpages);
	cpu_buffer->bpages = 0;
}

static bool rb_cpu_fits_desc(struct rb_page_desc *pdesc,
			     unsigned long desc_end)
{
	unsigned long *end;

	/* Check we can at least read nr_pages */
	if ((unsigned long)&pdesc->nr_page_va >= desc_end)
		return false;

	end = &pdesc->page_va[pdesc->nr_page_va];

	return (unsigned long)end <= desc_end;
}

static int rb_cpu_init(struct rb_page_desc *pdesc, struct hyp_rb_per_cpu *cpu_buffer)
{
	struct hyp_buffer_page *bpage;
	int i, ret;

	/* At least 1 reader page and one head */
	if (pdesc->nr_page_va < 2)
		return -EINVAL;

	if (rb_cpu_loaded(cpu_buffer))
		return -EBUSY;

	bpage = hyp_alloc(sizeof(*bpage) * pdesc->nr_page_va);
	if (!bpage)
		return hyp_alloc_errno();
	cpu_buffer->bpages = bpage;

	cpu_buffer->meta = (struct ring_buffer_meta *)kern_hyp_va(pdesc->meta_va);
	ret = hyp_pin_shared_mem((void *)cpu_buffer->meta,
				 ((void *)cpu_buffer->meta) + PAGE_SIZE);
	if (ret) {
		hyp_free(cpu_buffer->bpages);
		return ret;
	}

	memset(cpu_buffer->meta, 0, sizeof(*cpu_buffer->meta));
	cpu_buffer->meta->meta_page_size = PAGE_SIZE;
	cpu_buffer->meta->nr_data_pages = cpu_buffer->nr_pages;

	/* The reader page is not part of the ring initially */
	ret = rb_page_init(bpage, pdesc->page_va[0]);
	if (ret)
		goto err;

	cpu_buffer->nr_pages = 1;

	cpu_buffer->reader_page = bpage;
	cpu_buffer->tail_page = bpage + 1;
	cpu_buffer->head_page = bpage + 1;

	for (i = 1; i < pdesc->nr_page_va; i++) {
		ret = rb_page_init(++bpage, pdesc->page_va[i]);
		if (ret)
			goto err;

		bpage->list.next = &(bpage + 1)->list;
		bpage->list.prev = &(bpage - 1)->list;
		bpage->id = i;

		cpu_buffer->nr_pages = i + 1;
	}

	/* Close the ring */
	bpage->list.next = &cpu_buffer->tail_page->list;
	cpu_buffer->tail_page->list.prev = &bpage->list;

	/* The last init'ed page points to the head page */
	rb_set_flag(bpage, HYP_RB_PAGE_HEAD);

	cpu_buffer->last_overrun = 0;

	return 0;
err:
	rb_cpu_teardown(cpu_buffer);

	return ret;
}

int __pkvm_swap_reader_tracing(int cpu)
{
	struct hyp_rb_per_cpu *cpu_buffer = per_cpu_ptr(&trace_rb, cpu);
	int ret = 0;

	hyp_spin_lock(&trace_rb_lock);

	if (cpu >= hyp_nr_cpus) {
		ret = -EINVAL;
		goto err;
	}

	cpu_buffer = per_cpu_ptr(&trace_rb, cpu);
	if (!rb_cpu_loaded(cpu_buffer))
		ret = -ENODEV;
	else
		ret = rb_swap_reader_page(cpu_buffer);
err:
	hyp_spin_unlock(&trace_rb_lock);

	return ret;
}

static void __pkvm_teardown_tracing_locked(void)
{
	int cpu;

	hyp_assert_lock_held(&trace_rb_lock);

	for (cpu = 0; cpu < hyp_nr_cpus; cpu++) {
		struct hyp_rb_per_cpu *cpu_buffer = per_cpu_ptr(&trace_rb, cpu);

		rb_cpu_teardown(cpu_buffer);
	}
}

void __pkvm_teardown_tracing(void)
{
	hyp_spin_lock(&trace_rb_lock);
	__pkvm_teardown_tracing_locked();
	hyp_spin_unlock(&trace_rb_lock);
}

int __pkvm_load_tracing(unsigned long desc_hva, size_t desc_size)
{
	struct hyp_trace_desc *desc = (struct hyp_trace_desc *)kern_hyp_va(desc_hva);
	struct trace_page_desc *trace_pdesc = &desc->page_desc;
	struct rb_page_desc *pdesc;
	int ret, cpu;

	if (!desc_size || !PAGE_ALIGNED(desc_hva) || !PAGE_ALIGNED(desc_size))
		return -EINVAL;

	ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn((void *)desc),
				     desc_size >> PAGE_SHIFT);
	if (ret)
		return ret;

	hyp_spin_lock(&trace_rb_lock);

	trace_clock_update(&desc->clock_data);

	for_each_rb_page_desc(pdesc, cpu, trace_pdesc) {
		struct hyp_rb_per_cpu *cpu_buffer;
		int cpu;

		ret = -EINVAL;
		if (!rb_cpu_fits_desc(pdesc, desc_hva + desc_size))
			break;

		cpu = pdesc->cpu;
		if (cpu >= hyp_nr_cpus)
			break;

		cpu_buffer = per_cpu_ptr(&trace_rb, cpu);

		ret = rb_cpu_init(pdesc, cpu_buffer);
		if (ret)
			break;
	}
	if (ret)
		__pkvm_teardown_tracing_locked();

	hyp_spin_unlock(&trace_rb_lock);

	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn((void *)desc),
				       desc_size >> PAGE_SHIFT));
	return ret;
}

int __pkvm_enable_tracing(bool enable)
{
	int cpu, ret = enable ? -EINVAL : 0;

	hyp_spin_lock(&trace_rb_lock);
	for (cpu = 0; cpu < hyp_nr_cpus; cpu++) {
		struct hyp_rb_per_cpu *cpu_buffer = per_cpu_ptr(&trace_rb, cpu);

		if (enable) {
			if (!rb_cpu_enable_writing(cpu_buffer))
				ret = 0;
		} else {
			rb_cpu_disable_writing(cpu_buffer);
		}

	}
	hyp_spin_unlock(&trace_rb_lock);

	return ret;
}
