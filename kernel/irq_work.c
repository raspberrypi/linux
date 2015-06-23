/*
 * Copyright (C) 2010 Red Hat, Inc., Peter Zijlstra
 *
 * Provides a framework for enqueueing and running callbacks from hardirq
 * context. The enqueueing is NMI-safe.
 */

#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/irq_work.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <asm/processor.h>


static DEFINE_PER_CPU(struct llist_head, raised_list);
static DEFINE_PER_CPU(struct llist_head, lazy_list);

/*
 * Claim the entry so that no one else will poke at it.
 */
static bool irq_work_claim(struct irq_work *work)
{
	unsigned long flags, oflags, nflags;

	/*
	 * Start with our best wish as a premise but only trust any
	 * flag value after cmpxchg() result.
	 */
	flags = work->flags & ~IRQ_WORK_PENDING;
	for (;;) {
		nflags = flags | IRQ_WORK_CLAIMED;
		oflags = cmpxchg(&work->flags, flags, nflags);
		if (oflags == flags)
			break;
		if (oflags & IRQ_WORK_PENDING)
			return false;
		flags = oflags;
		cpu_relax();
	}

	return true;
}

void __weak arch_irq_work_raise(void)
{
	/*
	 * Lame architectures will get the timer tick callback
	 */
}

/* Enqueue on current CPU, work must already be claimed and preempt disabled */
static void __irq_work_queue_local(struct irq_work *work, struct llist_head *list)
{
	bool empty;

	empty = llist_add(&work->llnode, list);

	if (empty &&
	    (!(work->flags & IRQ_WORK_LAZY) ||
	     tick_nohz_tick_stopped()))
		arch_irq_work_raise();
}

/* Enqueue the irq work @work on the current CPU */
bool irq_work_queue(struct irq_work *work)
{
	struct llist_head *list;

	/* Only queue if not already pending */
	if (!irq_work_claim(work))
		return false;

	/* Queue the entry and raise the IPI if needed. */
	preempt_disable();
	if (IS_ENABLED(CONFIG_PREEMPT_RT_FULL) && !(work->flags & IRQ_WORK_HARD_IRQ))
		list = this_cpu_ptr(&lazy_list);
	else
		list = this_cpu_ptr(&raised_list);

	__irq_work_queue_local(work, list);
	preempt_enable();

	return true;
}
EXPORT_SYMBOL_GPL(irq_work_queue);

/*
 * Enqueue the irq_work @work on @cpu unless it's already pending
 * somewhere.
 *
 * Can be re-enqueued while the callback is still in progress.
 */
bool irq_work_queue_on(struct irq_work *work, int cpu)
{
#ifndef CONFIG_SMP
	return irq_work_queue(work);

#else /* CONFIG_SMP: */
	struct llist_head *list;
	bool lazy_work, realtime = IS_ENABLED(CONFIG_PREEMPT_RT_FULL);

	/* All work should have been flushed before going offline */
	WARN_ON_ONCE(cpu_is_offline(cpu));

	/* Only queue if not already pending */
	if (!irq_work_claim(work))
		return false;

	preempt_disable();

	lazy_work = work->flags & IRQ_WORK_LAZY;

	if (lazy_work || (realtime && !(work->flags & IRQ_WORK_HARD_IRQ)))
		list = &per_cpu(lazy_list, cpu);
	else
		list = &per_cpu(raised_list, cpu);

	if (cpu != smp_processor_id()) {
		/* Arch remote IPI send/receive backend aren't NMI safe */
		WARN_ON_ONCE(in_nmi());
		if (llist_add(&work->llnode, list))
			arch_send_call_function_single_ipi(cpu);
	} else {
		__irq_work_queue_local(work, list);
	}
	preempt_enable();

	return true;
#endif /* CONFIG_SMP */
}


bool irq_work_needs_cpu(void)
{
	struct llist_head *raised, *lazy;

	raised = this_cpu_ptr(&raised_list);
	lazy = this_cpu_ptr(&lazy_list);

	if (llist_empty(raised) && llist_empty(lazy))
		return false;

	/* All work should have been flushed before going offline */
	WARN_ON_ONCE(cpu_is_offline(smp_processor_id()));

	return true;
}

static void irq_work_run_list(struct llist_head *list)
{
	struct irq_work *work, *tmp;
	struct llist_node *llnode;
	unsigned long flags;

#ifndef CONFIG_PREEMPT_RT_FULL
	/*
	 * nort: On RT IRQ-work may run in SOFTIRQ context.
	 */
	BUG_ON(!irqs_disabled());
#endif
	if (llist_empty(list))
		return;

	llnode = llist_del_all(list);
	llist_for_each_entry_safe(work, tmp, llnode, llnode) {
		/*
		 * Clear the PENDING bit, after this point the @work
		 * can be re-used.
		 * Make it immediately visible so that other CPUs trying
		 * to claim that work don't rely on us to handle their data
		 * while we are in the middle of the func.
		 */
		flags = work->flags & ~IRQ_WORK_PENDING;
		xchg(&work->flags, flags);

		work->func(work);
		/*
		 * Clear the BUSY bit and return to the free state if
		 * no-one else claimed it meanwhile.
		 */
		(void)cmpxchg(&work->flags, flags, flags & ~IRQ_WORK_BUSY);
	}
}

/*
 * hotplug calls this through:
 *  hotplug_cfd() -> flush_smp_call_function_queue()
 */
void irq_work_run(void)
{
	irq_work_run_list(this_cpu_ptr(&raised_list));
	if (IS_ENABLED(CONFIG_PREEMPT_RT_FULL)) {
		/*
		 * NOTE: we raise softirq via IPI for safety,
		 * and execute in irq_work_tick() to move the
		 * overhead from hard to soft irq context.
		 */
		if (!llist_empty(this_cpu_ptr(&lazy_list)))
			raise_softirq(TIMER_SOFTIRQ);
	} else
		irq_work_run_list(this_cpu_ptr(&lazy_list));
}
EXPORT_SYMBOL_GPL(irq_work_run);

void irq_work_tick(void)
{
	struct llist_head *raised = this_cpu_ptr(&raised_list);

	if (!llist_empty(raised) && !arch_irq_work_has_interrupt())
		irq_work_run_list(raised);

	if (!IS_ENABLED(CONFIG_PREEMPT_RT_FULL))
		irq_work_run_list(this_cpu_ptr(&lazy_list));
}

#if defined(CONFIG_IRQ_WORK) && defined(CONFIG_PREEMPT_RT_FULL)
void irq_work_tick_soft(void)
{
	irq_work_run_list(this_cpu_ptr(&lazy_list));
}
#endif

/*
 * Synchronize against the irq_work @entry, ensures the entry is not
 * currently in use.
 */
void irq_work_sync(struct irq_work *work)
{
	lockdep_assert_irqs_enabled();

	while (work->flags & IRQ_WORK_BUSY)
		cpu_relax();
}
EXPORT_SYMBOL_GPL(irq_work_sync);
