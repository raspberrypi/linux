/*
 *	linux/kernel/softirq.c
 *
 *	Copyright (C) 1992 Linus Torvalds
 *
 *	Distribute under GPLv2.
 *
 *	Rewritten. Old one was good in 2.2, but in 2.3 it was immoral. --ANK (990903)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/export.h>
#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>
#include <linux/delay.h>
#include <linux/ftrace.h>
#include <linux/smp.h>
#include <linux/smpboot.h>
#include <linux/tick.h>
#include <linux/locallock.h>
#include <linux/irq.h>
#include <linux/sched/types.h>

#define CREATE_TRACE_POINTS
#include <trace/events/irq.h>

/*
   - No shared variables, all the data are CPU local.
   - If a softirq needs serialization, let it serialize itself
     by its own spinlocks.
   - Even if softirq is serialized, only local cpu is marked for
     execution. Hence, we get something sort of weak cpu binding.
     Though it is still not clear, will it result in better locality
     or will not.

   Examples:
   - NET RX softirq. It is multithreaded and does not require
     any global serialization.
   - NET TX softirq. It kicks software netdevice queues, hence
     it is logically serialized per device, but this serialization
     is invisible to common code.
   - Tasklets: serialized wrt itself.
 */

#ifndef __ARCH_IRQ_STAT
irq_cpustat_t irq_stat[NR_CPUS] ____cacheline_aligned;
EXPORT_SYMBOL(irq_stat);
#endif

static struct softirq_action softirq_vec[NR_SOFTIRQS] __cacheline_aligned_in_smp;

DEFINE_PER_CPU(struct task_struct *, ksoftirqd);
#ifdef CONFIG_PREEMPT_RT_FULL
#define TIMER_SOFTIRQS ((1 << TIMER_SOFTIRQ) | (1 << HRTIMER_SOFTIRQ))
DEFINE_PER_CPU(struct task_struct *, ktimer_softirqd);
#endif

const char * const softirq_to_name[NR_SOFTIRQS] = {
	"HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "IRQ_POLL",
	"TASKLET", "SCHED", "HRTIMER", "RCU"
};

#ifdef CONFIG_NO_HZ_COMMON
# ifdef CONFIG_PREEMPT_RT_FULL

struct softirq_runner {
	struct task_struct *runner[NR_SOFTIRQS];
};

static DEFINE_PER_CPU(struct softirq_runner, softirq_runners);

static inline void softirq_set_runner(unsigned int sirq)
{
	struct softirq_runner *sr = this_cpu_ptr(&softirq_runners);

	sr->runner[sirq] = current;
}

static inline void softirq_clr_runner(unsigned int sirq)
{
	struct softirq_runner *sr = this_cpu_ptr(&softirq_runners);

	sr->runner[sirq] = NULL;
}

/*
 * On preempt-rt a softirq running context might be blocked on a
 * lock. There might be no other runnable task on this CPU because the
 * lock owner runs on some other CPU. So we have to go into idle with
 * the pending bit set. Therefor we need to check this otherwise we
 * warn about false positives which confuses users and defeats the
 * whole purpose of this test.
 *
 * This code is called with interrupts disabled.
 */
void softirq_check_pending_idle(void)
{
	static int rate_limit;
	struct softirq_runner *sr = this_cpu_ptr(&softirq_runners);
	u32 warnpending;
	int i;

	if (rate_limit >= 10)
		return;

	warnpending = local_softirq_pending() & SOFTIRQ_STOP_IDLE_MASK;
	for (i = 0; i < NR_SOFTIRQS; i++) {
		struct task_struct *tsk = sr->runner[i];

		/*
		 * The wakeup code in rtmutex.c wakes up the task
		 * _before_ it sets pi_blocked_on to NULL under
		 * tsk->pi_lock. So we need to check for both: state
		 * and pi_blocked_on.
		 */
		if (tsk) {
			raw_spin_lock(&tsk->pi_lock);
			if (tsk->pi_blocked_on || tsk->state == TASK_RUNNING) {
				/* Clear all bits pending in that task */
				warnpending &= ~(tsk->softirqs_raised);
				warnpending &= ~(1 << i);
			}
			raw_spin_unlock(&tsk->pi_lock);
		}
	}

	if (warnpending) {
		printk(KERN_ERR "NOHZ: local_softirq_pending %02x\n",
		       warnpending);
		rate_limit++;
	}
}
# else
/*
 * On !PREEMPT_RT we just printk rate limited:
 */
void softirq_check_pending_idle(void)
{
	static int rate_limit;

	if (rate_limit < 10 && !in_softirq() &&
			(local_softirq_pending() & SOFTIRQ_STOP_IDLE_MASK)) {
		printk(KERN_ERR "NOHZ: local_softirq_pending %02x\n",
		       local_softirq_pending());
		rate_limit++;
	}
}
# endif

#else /* !CONFIG_NO_HZ_COMMON */
static inline void softirq_set_runner(unsigned int sirq) { }
static inline void softirq_clr_runner(unsigned int sirq) { }
#endif

/*
 * we cannot loop indefinitely here to avoid userspace starvation,
 * but we also don't want to introduce a worst case 1/HZ latency
 * to the pending events, so lets the scheduler to balance
 * the softirq load for us.
 */
static void wakeup_softirqd(void)
{
	/* Interrupts are disabled: no need to stop preemption */
	struct task_struct *tsk = __this_cpu_read(ksoftirqd);

	if (tsk && tsk->state != TASK_RUNNING)
		wake_up_process(tsk);
}

#ifdef CONFIG_PREEMPT_RT_FULL
static void wakeup_timer_softirqd(void)
{
	/* Interrupts are disabled: no need to stop preemption */
	struct task_struct *tsk = __this_cpu_read(ktimer_softirqd);

	if (tsk && tsk->state != TASK_RUNNING)
		wake_up_process(tsk);
}
#endif

static void handle_softirq(unsigned int vec_nr)
{
	struct softirq_action *h = softirq_vec + vec_nr;
	int prev_count;

	prev_count = preempt_count();

	kstat_incr_softirqs_this_cpu(vec_nr);

	trace_softirq_entry(vec_nr);
	h->action(h);
	trace_softirq_exit(vec_nr);
	if (unlikely(prev_count != preempt_count())) {
		pr_err("huh, entered softirq %u %s %p with preempt_count %08x, exited with %08x?\n",
		       vec_nr, softirq_to_name[vec_nr], h->action,
		       prev_count, preempt_count());
		preempt_count_set(prev_count);
	}
}

#ifndef CONFIG_PREEMPT_RT_FULL
/*
 * If ksoftirqd is scheduled, we do not want to process pending softirqs
 * right now. Let ksoftirqd handle this at its own rate, to get fairness,
 * unless we're doing some of the synchronous softirqs.
 */
#define SOFTIRQ_NOW_MASK ((1 << HI_SOFTIRQ) | (1 << TASKLET_SOFTIRQ))
static bool ksoftirqd_running(unsigned long pending)
{
	struct task_struct *tsk = __this_cpu_read(ksoftirqd);

	if (pending & SOFTIRQ_NOW_MASK)
		return false;
	return tsk && (tsk->state == TASK_RUNNING);
}

static inline int ksoftirqd_softirq_pending(void)
{
	return local_softirq_pending();
}

static void handle_pending_softirqs(u32 pending)
{
	struct softirq_action *h = softirq_vec;
	int softirq_bit;

	local_irq_enable();

	h = softirq_vec;

	while ((softirq_bit = ffs(pending))) {
		unsigned int vec_nr;

		h += softirq_bit - 1;
		vec_nr = h - softirq_vec;
		handle_softirq(vec_nr);

		h++;
		pending >>= softirq_bit;
	}

	rcu_bh_qs();
	local_irq_disable();
}

static void run_ksoftirqd(unsigned int cpu)
{
	local_irq_disable();
	if (ksoftirqd_softirq_pending()) {
		__do_softirq();
		local_irq_enable();
		cond_resched_rcu_qs();
		return;
	}
	local_irq_enable();
}

/*
 * preempt_count and SOFTIRQ_OFFSET usage:
 * - preempt_count is changed by SOFTIRQ_OFFSET on entering or leaving
 *   softirq processing.
 * - preempt_count is changed by SOFTIRQ_DISABLE_OFFSET (= 2 * SOFTIRQ_OFFSET)
 *   on local_bh_disable or local_bh_enable.
 * This lets us distinguish between whether we are currently processing
 * softirq and whether we just have bh disabled.
 */

/*
 * This one is for softirq.c-internal use,
 * where hardirqs are disabled legitimately:
 */
#ifdef CONFIG_TRACE_IRQFLAGS
void __local_bh_disable_ip(unsigned long ip, unsigned int cnt)
{
	unsigned long flags;

	WARN_ON_ONCE(in_irq());

	raw_local_irq_save(flags);
	/*
	 * The preempt tracer hooks into preempt_count_add and will break
	 * lockdep because it calls back into lockdep after SOFTIRQ_OFFSET
	 * is set and before current->softirq_enabled is cleared.
	 * We must manually increment preempt_count here and manually
	 * call the trace_preempt_off later.
	 */
	__preempt_count_add(cnt);
	/*
	 * Were softirqs turned off above:
	 */
	if (softirq_count() == (cnt & SOFTIRQ_MASK))
		trace_softirqs_off(ip);
	raw_local_irq_restore(flags);

	if (preempt_count() == cnt) {
#ifdef CONFIG_DEBUG_PREEMPT
		current->preempt_disable_ip = get_lock_parent_ip();
#endif
		trace_preempt_off(CALLER_ADDR0, get_lock_parent_ip());
	}
}
EXPORT_SYMBOL(__local_bh_disable_ip);
#endif /* CONFIG_TRACE_IRQFLAGS */

static void __local_bh_enable(unsigned int cnt)
{
	WARN_ON_ONCE(!irqs_disabled());

	if (softirq_count() == (cnt & SOFTIRQ_MASK))
		trace_softirqs_on(_RET_IP_);
	preempt_count_sub(cnt);
}

/*
 * Special-case - softirqs can safely be enabled in
 * cond_resched_softirq(), or by __do_softirq(),
 * without processing still-pending softirqs:
 */
void _local_bh_enable(void)
{
	WARN_ON_ONCE(in_irq());
	__local_bh_enable(SOFTIRQ_DISABLE_OFFSET);
}
EXPORT_SYMBOL(_local_bh_enable);

void __local_bh_enable_ip(unsigned long ip, unsigned int cnt)
{
	WARN_ON_ONCE(in_irq() || irqs_disabled());
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_disable();
#endif
	/*
	 * Are softirqs going to be turned on now:
	 */
	if (softirq_count() == SOFTIRQ_DISABLE_OFFSET)
		trace_softirqs_on(ip);
	/*
	 * Keep preemption disabled until we are done with
	 * softirq processing:
	 */
	preempt_count_sub(cnt - 1);

	if (unlikely(!in_interrupt() && local_softirq_pending())) {
		/*
		 * Run softirq if any pending. And do it in its own stack
		 * as we may be calling this deep in a task call stack already.
		 */
		do_softirq();
	}

	preempt_count_dec();
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_enable();
#endif
	preempt_check_resched();
}
EXPORT_SYMBOL(__local_bh_enable_ip);

/*
 * We restart softirq processing for at most MAX_SOFTIRQ_RESTART times,
 * but break the loop if need_resched() is set or after 2 ms.
 * The MAX_SOFTIRQ_TIME provides a nice upper bound in most cases, but in
 * certain cases, such as stop_machine(), jiffies may cease to
 * increment and so we need the MAX_SOFTIRQ_RESTART limit as
 * well to make sure we eventually return from this method.
 *
 * These limits have been established via experimentation.
 * The two things to balance is latency against fairness -
 * we want to handle softirqs as soon as possible, but they
 * should not be able to lock up the box.
 */
#define MAX_SOFTIRQ_TIME  msecs_to_jiffies(2)
#define MAX_SOFTIRQ_RESTART 10

#ifdef CONFIG_TRACE_IRQFLAGS
/*
 * When we run softirqs from irq_exit() and thus on the hardirq stack we need
 * to keep the lockdep irq context tracking as tight as possible in order to
 * not miss-qualify lock contexts and miss possible deadlocks.
 */

static inline bool lockdep_softirq_start(void)
{
	bool in_hardirq = false;

	if (trace_hardirq_context(current)) {
		in_hardirq = true;
		trace_hardirq_exit();
	}

	lockdep_softirq_enter();

	return in_hardirq;
}

static inline void lockdep_softirq_end(bool in_hardirq)
{
	lockdep_softirq_exit();

	if (in_hardirq)
		trace_hardirq_enter();
}
#else
static inline bool lockdep_softirq_start(void) { return false; }
static inline void lockdep_softirq_end(bool in_hardirq) { }
#endif

asmlinkage __visible void __softirq_entry __do_softirq(void)
{
	unsigned long end = jiffies + MAX_SOFTIRQ_TIME;
	unsigned long old_flags = current->flags;
	int max_restart = MAX_SOFTIRQ_RESTART;
	bool in_hardirq;
	__u32 pending;

	/*
	 * Mask out PF_MEMALLOC s current task context is borrowed for the
	 * softirq. A softirq handled such as network RX might set PF_MEMALLOC
	 * again if the socket is related to swap
	 */
	current->flags &= ~PF_MEMALLOC;

	pending = local_softirq_pending();
	account_irq_enter_time(current);

	__local_bh_disable_ip(_RET_IP_, SOFTIRQ_OFFSET);
	in_hardirq = lockdep_softirq_start();

restart:
	/* Reset the pending bitmask before enabling irqs */
	set_softirq_pending(0);

	handle_pending_softirqs(pending);

	pending = local_softirq_pending();
	if (pending) {
		if (time_before(jiffies, end) && !need_resched() &&
		    --max_restart)
			goto restart;

		wakeup_softirqd();
	}

	lockdep_softirq_end(in_hardirq);
	account_irq_exit_time(current);
	__local_bh_enable(SOFTIRQ_OFFSET);
	WARN_ON_ONCE(in_interrupt());
	current_restore_flags(old_flags, PF_MEMALLOC);
}

asmlinkage __visible void do_softirq(void)
{
	__u32 pending;
	unsigned long flags;

	if (in_interrupt())
		return;

	local_irq_save(flags);

	pending = local_softirq_pending();

	if (pending && !ksoftirqd_running(pending))
		do_softirq_own_stack();

	local_irq_restore(flags);
}

/*
 * This function must run with irqs disabled!
 */
void raise_softirq_irqoff(unsigned int nr)
{
	__raise_softirq_irqoff(nr);

	/*
	 * If we're in an interrupt or softirq, we're done
	 * (this also catches softirq-disabled code). We will
	 * actually run the softirq once we return from
	 * the irq or softirq.
	 *
	 * Otherwise we wake up ksoftirqd to make sure we
	 * schedule the softirq soon.
	 */
	if (!in_interrupt())
		wakeup_softirqd();
}

void __raise_softirq_irqoff(unsigned int nr)
{
	trace_softirq_raise(nr);
	or_softirq_pending(1UL << nr);
}

static inline void local_bh_disable_nort(void) { local_bh_disable(); }
static inline void _local_bh_enable_nort(void) { _local_bh_enable(); }
static void ksoftirqd_set_sched_params(unsigned int cpu) { }

#else /* !PREEMPT_RT_FULL */

/*
 * On RT we serialize softirq execution with a cpu local lock per softirq
 */
static DEFINE_PER_CPU(struct local_irq_lock [NR_SOFTIRQS], local_softirq_locks);

void __init softirq_early_init(void)
{
	int i;

	for (i = 0; i < NR_SOFTIRQS; i++)
		local_irq_lock_init(local_softirq_locks[i]);
}

static void lock_softirq(int which)
{
	local_lock(local_softirq_locks[which]);
}

static void unlock_softirq(int which)
{
	local_unlock(local_softirq_locks[which]);
}

static void do_single_softirq(int which)
{
	unsigned long old_flags = current->flags;

	current->flags &= ~PF_MEMALLOC;
	vtime_account_irq_enter(current);
	current->flags |= PF_IN_SOFTIRQ;
	lockdep_softirq_enter();
	local_irq_enable();
	handle_softirq(which);
	local_irq_disable();
	lockdep_softirq_exit();
	current->flags &= ~PF_IN_SOFTIRQ;
	vtime_account_irq_enter(current);
	current_restore_flags(old_flags, PF_MEMALLOC);
}

/*
 * Called with interrupts disabled. Process softirqs which were raised
 * in current context (or on behalf of ksoftirqd).
 */
static void do_current_softirqs(void)
{
	while (current->softirqs_raised) {
		int i = __ffs(current->softirqs_raised);
		unsigned int pending, mask = (1U << i);

		current->softirqs_raised &= ~mask;
		local_irq_enable();

		/*
		 * If the lock is contended, we boost the owner to
		 * process the softirq or leave the critical section
		 * now.
		 */
		lock_softirq(i);
		local_irq_disable();
		softirq_set_runner(i);
		/*
		 * Check with the local_softirq_pending() bits,
		 * whether we need to process this still or if someone
		 * else took care of it.
		 */
		pending = local_softirq_pending();
		if (pending & mask) {
			set_softirq_pending(pending & ~mask);
			do_single_softirq(i);
		}
		softirq_clr_runner(i);
		WARN_ON(current->softirq_nestcnt != 1);
		local_irq_enable();
		unlock_softirq(i);
		local_irq_disable();
	}
}

void __local_bh_disable(void)
{
	if (++current->softirq_nestcnt == 1)
		migrate_disable();
}
EXPORT_SYMBOL(__local_bh_disable);

void __local_bh_enable(void)
{
	if (WARN_ON(current->softirq_nestcnt == 0))
		return;

	local_irq_disable();
	if (current->softirq_nestcnt == 1 && current->softirqs_raised)
		do_current_softirqs();
	local_irq_enable();

	if (--current->softirq_nestcnt == 0)
		migrate_enable();
}
EXPORT_SYMBOL(__local_bh_enable);

void _local_bh_enable(void)
{
	if (WARN_ON(current->softirq_nestcnt == 0))
		return;
	if (--current->softirq_nestcnt == 0)
		migrate_enable();
}
EXPORT_SYMBOL(_local_bh_enable);

int in_serving_softirq(void)
{
	return current->flags & PF_IN_SOFTIRQ;
}
EXPORT_SYMBOL(in_serving_softirq);

/* Called with preemption disabled */
static void run_ksoftirqd(unsigned int cpu)
{
	local_irq_disable();
	current->softirq_nestcnt++;

	do_current_softirqs();
	current->softirq_nestcnt--;
	local_irq_enable();
	cond_resched_rcu_qs();
}

/*
 * Called from netif_rx_ni(). Preemption enabled, but migration
 * disabled. So the cpu can't go away under us.
 */
void thread_do_softirq(void)
{
	if (!in_serving_softirq() && current->softirqs_raised) {
		current->softirq_nestcnt++;
		do_current_softirqs();
		current->softirq_nestcnt--;
	}
}

static void do_raise_softirq_irqoff(unsigned int nr)
{
	unsigned int mask;

	mask = 1UL << nr;

	trace_softirq_raise(nr);
	or_softirq_pending(mask);

	/*
	 * If we are not in a hard interrupt and inside a bh disabled
	 * region, we simply raise the flag on current. local_bh_enable()
	 * will make sure that the softirq is executed. Otherwise we
	 * delegate it to ksoftirqd.
	 */
	if (!in_irq() && current->softirq_nestcnt)
		current->softirqs_raised |= mask;
	else if (!__this_cpu_read(ksoftirqd) || !__this_cpu_read(ktimer_softirqd))
		return;

	if (mask & TIMER_SOFTIRQS)
		__this_cpu_read(ktimer_softirqd)->softirqs_raised |= mask;
	else
		__this_cpu_read(ksoftirqd)->softirqs_raised |= mask;
}

static void wakeup_proper_softirq(unsigned int nr)
{
	if ((1UL << nr) & TIMER_SOFTIRQS)
		wakeup_timer_softirqd();
	else
		wakeup_softirqd();
}

void __raise_softirq_irqoff(unsigned int nr)
{
	do_raise_softirq_irqoff(nr);
	if (!in_irq() && !current->softirq_nestcnt)
		wakeup_proper_softirq(nr);
}

/*
 * Same as __raise_softirq_irqoff() but will process them in ksoftirqd
 */
void __raise_softirq_irqoff_ksoft(unsigned int nr)
{
	unsigned int mask;

	if (WARN_ON_ONCE(!__this_cpu_read(ksoftirqd) ||
			 !__this_cpu_read(ktimer_softirqd)))
		return;
	mask = 1UL << nr;

	trace_softirq_raise(nr);
	or_softirq_pending(mask);
	if (mask & TIMER_SOFTIRQS)
		__this_cpu_read(ktimer_softirqd)->softirqs_raised |= mask;
	else
		__this_cpu_read(ksoftirqd)->softirqs_raised |= mask;
	wakeup_proper_softirq(nr);
}

/*
 * This function must run with irqs disabled!
 */
void raise_softirq_irqoff(unsigned int nr)
{
	do_raise_softirq_irqoff(nr);

	/*
	 * If we're in an hard interrupt we let irq return code deal
	 * with the wakeup of ksoftirqd.
	 */
	if (in_irq())
		return;
	/*
	 * If we are in thread context but outside of a bh disabled
	 * region, we need to wake ksoftirqd as well.
	 *
	 * CHECKME: Some of the places which do that could be wrapped
	 * into local_bh_disable/enable pairs. Though it's unclear
	 * whether this is worth the effort. To find those places just
	 * raise a WARN() if the condition is met.
	 */
	if (!current->softirq_nestcnt)
		wakeup_proper_softirq(nr);
}

static inline int ksoftirqd_softirq_pending(void)
{
	return current->softirqs_raised;
}

static inline void local_bh_disable_nort(void) { }
static inline void _local_bh_enable_nort(void) { }

static inline void ksoftirqd_set_sched_params(unsigned int cpu)
{
	/* Take over all but timer pending softirqs when starting */
	local_irq_disable();
	current->softirqs_raised = local_softirq_pending() & ~TIMER_SOFTIRQS;
	local_irq_enable();
}

static inline void ktimer_softirqd_set_sched_params(unsigned int cpu)
{
	struct sched_param param = { .sched_priority = 1 };

	sched_setscheduler(current, SCHED_FIFO, &param);

	/* Take over timer pending softirqs when starting */
	local_irq_disable();
	current->softirqs_raised = local_softirq_pending() & TIMER_SOFTIRQS;
	local_irq_enable();
}

static inline void ktimer_softirqd_clr_sched_params(unsigned int cpu,
						    bool online)
{
	struct sched_param param = { .sched_priority = 0 };

	sched_setscheduler(current, SCHED_NORMAL, &param);
}

static int ktimer_softirqd_should_run(unsigned int cpu)
{
	return current->softirqs_raised;
}

#endif /* PREEMPT_RT_FULL */
/*
 * Enter an interrupt context.
 */
void irq_enter(void)
{
	rcu_irq_enter();
	if (is_idle_task(current) && !in_interrupt()) {
		/*
		 * Prevent raise_softirq from needlessly waking up ksoftirqd
		 * here, as softirq will be serviced on return from interrupt.
		 */
		local_bh_disable_nort();
		tick_irq_enter();
		_local_bh_enable_nort();
	}

	__irq_enter();
}

static inline void invoke_softirq(void)
{
#ifndef CONFIG_PREEMPT_RT_FULL
	if (ksoftirqd_running(local_softirq_pending()))
		return;

	if (!force_irqthreads) {
#ifdef CONFIG_HAVE_IRQ_EXIT_ON_IRQ_STACK
		/*
		 * We can safely execute softirq on the current stack if
		 * it is the irq stack, because it should be near empty
		 * at this stage.
		 */
		__do_softirq();
#else
		/*
		 * Otherwise, irq_exit() is called on the task stack that can
		 * be potentially deep already. So call softirq in its own stack
		 * to prevent from any overrun.
		 */
		do_softirq_own_stack();
#endif
	} else {
		wakeup_softirqd();
	}
#else /* PREEMPT_RT_FULL */
	unsigned long flags;

	local_irq_save(flags);
	if (__this_cpu_read(ksoftirqd) &&
			__this_cpu_read(ksoftirqd)->softirqs_raised)
		wakeup_softirqd();
	if (__this_cpu_read(ktimer_softirqd) &&
			__this_cpu_read(ktimer_softirqd)->softirqs_raised)
		wakeup_timer_softirqd();
	local_irq_restore(flags);
#endif
}

static inline void tick_irq_exit(void)
{
#ifdef CONFIG_NO_HZ_COMMON
	int cpu = smp_processor_id();

	/* Make sure that timer wheel updates are propagated */
	if ((idle_cpu(cpu) || tick_nohz_full_cpu(cpu)) &&
	    !need_resched() && !local_softirq_pending()) {
		if (!in_irq())
			tick_nohz_irq_exit();
	}
#endif
}

/*
 * Exit an interrupt context. Process softirqs if needed and possible:
 */
void irq_exit(void)
{
#ifndef __ARCH_IRQ_EXIT_IRQS_DISABLED
	local_irq_disable();
#else
	WARN_ON_ONCE(!irqs_disabled());
#endif

	account_irq_exit_time(current);
	preempt_count_sub(HARDIRQ_OFFSET);
	if (!in_interrupt() && local_softirq_pending())
		invoke_softirq();

	tick_irq_exit();
	rcu_irq_exit();
	trace_hardirq_exit(); /* must be last! */
}

void raise_softirq(unsigned int nr)
{
	unsigned long flags;

	local_irq_save(flags);
	raise_softirq_irqoff(nr);
	local_irq_restore(flags);
}

void open_softirq(int nr, void (*action)(struct softirq_action *))
{
	softirq_vec[nr].action = action;
}

/*
 * Tasklets
 */
struct tasklet_head {
	struct tasklet_struct *head;
	struct tasklet_struct **tail;
};

static DEFINE_PER_CPU(struct tasklet_head, tasklet_vec);
static DEFINE_PER_CPU(struct tasklet_head, tasklet_hi_vec);

static void inline
__tasklet_common_schedule(struct tasklet_struct *t, struct tasklet_head *head, unsigned int nr)
{
	if (tasklet_trylock(t)) {
again:
		/* We may have been preempted before tasklet_trylock
		 * and __tasklet_action may have already run.
		 * So double check the sched bit while the takslet
		 * is locked before adding it to the list.
		 */
		if (test_bit(TASKLET_STATE_SCHED, &t->state)) {
			t->next = NULL;
			*head->tail = t;
			head->tail = &(t->next);
			raise_softirq_irqoff(nr);
			tasklet_unlock(t);
		} else {
			/* This is subtle. If we hit the corner case above
			 * It is possible that we get preempted right here,
			 * and another task has successfully called
			 * tasklet_schedule(), then this function, and
			 * failed on the trylock. Thus we must be sure
			 * before releasing the tasklet lock, that the
			 * SCHED_BIT is clear. Otherwise the tasklet
			 * may get its SCHED_BIT set, but not added to the
			 * list
			 */
			if (!tasklet_tryunlock(t))
				goto again;
		}
	}
}

void __tasklet_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	local_irq_save(flags);
	__tasklet_common_schedule(t, this_cpu_ptr(&tasklet_vec), TASKLET_SOFTIRQ);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(__tasklet_schedule);

void __tasklet_hi_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	local_irq_save(flags);
	__tasklet_common_schedule(t, this_cpu_ptr(&tasklet_hi_vec), HI_SOFTIRQ);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(__tasklet_hi_schedule);

void tasklet_enable(struct tasklet_struct *t)
{
	if (!atomic_dec_and_test(&t->count))
		return;
	if (test_and_clear_bit(TASKLET_STATE_PENDING, &t->state))
		tasklet_schedule(t);
}
EXPORT_SYMBOL(tasklet_enable);

static void __tasklet_action(struct softirq_action *a,
			     struct tasklet_struct *list)
{
	int loops = 1000000;

	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		/*
		 * Should always succeed - after a tasklist got on the
		 * list (after getting the SCHED bit set from 0 to 1),
		 * nothing but the tasklet softirq it got queued to can
		 * lock it:
		 */
		if (!tasklet_trylock(t)) {
			WARN_ON(1);
			continue;
		}

		t->next = NULL;

		/*
		 * If we cannot handle the tasklet because it's disabled,
		 * mark it as pending. tasklet_enable() will later
		 * re-schedule the tasklet.
		 */
		if (unlikely(atomic_read(&t->count))) {
out_disabled:
			/* implicit unlock: */
			wmb();
			t->state = TASKLET_STATEF_PENDING;
			continue;
		}

		/*
		 * After this point on the tasklet might be rescheduled
		 * on another CPU, but it can only be added to another
		 * CPU's tasklet list if we unlock the tasklet (which we
		 * dont do yet).
		 */
		if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
			WARN_ON(1);

again:
		t->func(t->data);

		/*
		 * Try to unlock the tasklet. We must use cmpxchg, because
		 * another CPU might have scheduled or disabled the tasklet.
		 * We only allow the STATE_RUN -> 0 transition here.
		 */
		while (!tasklet_tryunlock(t)) {
			/*
			 * If it got disabled meanwhile, bail out:
			 */
			if (atomic_read(&t->count))
				goto out_disabled;
			/*
			 * If it got scheduled meanwhile, re-execute
			 * the tasklet function:
			 */
			if (test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
				goto again;
			if (!--loops) {
				printk("hm, tasklet state: %08lx\n", t->state);
				WARN_ON(1);
				tasklet_unlock(t);
				break;
			}
		}
	}
}

static __latent_entropy void tasklet_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();
	list = __this_cpu_read(tasklet_vec.head);
	__this_cpu_write(tasklet_vec.head, NULL);
	__this_cpu_write(tasklet_vec.tail, this_cpu_ptr(&tasklet_vec.head));
	local_irq_enable();

	__tasklet_action(a, list);
}

static __latent_entropy void tasklet_hi_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();
	list = __this_cpu_read(tasklet_hi_vec.head);
	__this_cpu_write(tasklet_hi_vec.head, NULL);
	__this_cpu_write(tasklet_hi_vec.tail, this_cpu_ptr(&tasklet_hi_vec.head));
	local_irq_enable();

	__tasklet_action(a, list);
}

void tasklet_init(struct tasklet_struct *t,
		  void (*func)(unsigned long), unsigned long data)
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->func = func;
	t->data = data;
}
EXPORT_SYMBOL(tasklet_init);

void tasklet_kill(struct tasklet_struct *t)
{
	if (in_interrupt())
		pr_notice("Attempt to kill tasklet from interrupt\n");

	while (test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) {
		do {
			msleep(1);
		} while (test_bit(TASKLET_STATE_SCHED, &t->state));
	}
	tasklet_unlock_wait(t);
	clear_bit(TASKLET_STATE_SCHED, &t->state);
}
EXPORT_SYMBOL(tasklet_kill);

void __init softirq_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		per_cpu(tasklet_vec, cpu).tail =
			&per_cpu(tasklet_vec, cpu).head;
		per_cpu(tasklet_hi_vec, cpu).tail =
			&per_cpu(tasklet_hi_vec, cpu).head;
	}

	open_softirq(TASKLET_SOFTIRQ, tasklet_action);
	open_softirq(HI_SOFTIRQ, tasklet_hi_action);
}

#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RT_FULL)
void tasklet_unlock_wait(struct tasklet_struct *t)
{
	while (test_bit(TASKLET_STATE_RUN, &(t)->state)) {
		/*
		 * Hack for now to avoid this busy-loop:
		 */
#ifdef CONFIG_PREEMPT_RT_FULL
		msleep(1);
#else
		barrier();
#endif
	}
}
EXPORT_SYMBOL(tasklet_unlock_wait);
#endif

static int ksoftirqd_should_run(unsigned int cpu)
{
	return ksoftirqd_softirq_pending();
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * tasklet_kill_immediate is called to remove a tasklet which can already be
 * scheduled for execution on @cpu.
 *
 * Unlike tasklet_kill, this function removes the tasklet
 * _immediately_, even if the tasklet is in TASKLET_STATE_SCHED state.
 *
 * When this function is called, @cpu must be in the CPU_DEAD state.
 */
void tasklet_kill_immediate(struct tasklet_struct *t, unsigned int cpu)
{
	struct tasklet_struct **i;

	BUG_ON(cpu_online(cpu));
	BUG_ON(test_bit(TASKLET_STATE_RUN, &t->state));

	if (!test_bit(TASKLET_STATE_SCHED, &t->state))
		return;

	/* CPU is dead, so no lock needed. */
	for (i = &per_cpu(tasklet_vec, cpu).head; *i; i = &(*i)->next) {
		if (*i == t) {
			*i = t->next;
			/* If this was the tail element, move the tail ptr */
			if (*i == NULL)
				per_cpu(tasklet_vec, cpu).tail = i;
			return;
		}
	}
	BUG();
}

static int takeover_tasklets(unsigned int cpu)
{
	/* CPU is dead, so no lock needed. */
	local_irq_disable();

	/* Find end, append list for that CPU. */
	if (&per_cpu(tasklet_vec, cpu).head != per_cpu(tasklet_vec, cpu).tail) {
		*__this_cpu_read(tasklet_vec.tail) = per_cpu(tasklet_vec, cpu).head;
		this_cpu_write(tasklet_vec.tail, per_cpu(tasklet_vec, cpu).tail);
		per_cpu(tasklet_vec, cpu).head = NULL;
		per_cpu(tasklet_vec, cpu).tail = &per_cpu(tasklet_vec, cpu).head;
	}
	raise_softirq_irqoff(TASKLET_SOFTIRQ);

	if (&per_cpu(tasklet_hi_vec, cpu).head != per_cpu(tasklet_hi_vec, cpu).tail) {
		*__this_cpu_read(tasklet_hi_vec.tail) = per_cpu(tasklet_hi_vec, cpu).head;
		__this_cpu_write(tasklet_hi_vec.tail, per_cpu(tasklet_hi_vec, cpu).tail);
		per_cpu(tasklet_hi_vec, cpu).head = NULL;
		per_cpu(tasklet_hi_vec, cpu).tail = &per_cpu(tasklet_hi_vec, cpu).head;
	}
	raise_softirq_irqoff(HI_SOFTIRQ);

	local_irq_enable();
	return 0;
}
#else
#define takeover_tasklets	NULL
#endif /* CONFIG_HOTPLUG_CPU */

static struct smp_hotplug_thread softirq_threads = {
	.store			= &ksoftirqd,
	.setup			= ksoftirqd_set_sched_params,
	.thread_should_run	= ksoftirqd_should_run,
	.thread_fn		= run_ksoftirqd,
	.thread_comm		= "ksoftirqd/%u",
};

#ifdef CONFIG_PREEMPT_RT_FULL
static struct smp_hotplug_thread softirq_timer_threads = {
	.store			= &ktimer_softirqd,
	.setup			= ktimer_softirqd_set_sched_params,
	.cleanup		= ktimer_softirqd_clr_sched_params,
	.thread_should_run	= ktimer_softirqd_should_run,
	.thread_fn		= run_ksoftirqd,
	.thread_comm		= "ktimersoftd/%u",
};
#endif

static __init int spawn_ksoftirqd(void)
{
	cpuhp_setup_state_nocalls(CPUHP_SOFTIRQ_DEAD, "softirq:dead", NULL,
				  takeover_tasklets);
	BUG_ON(smpboot_register_percpu_thread(&softirq_threads));
#ifdef CONFIG_PREEMPT_RT_FULL
	BUG_ON(smpboot_register_percpu_thread(&softirq_timer_threads));
#endif
	return 0;
}
early_initcall(spawn_ksoftirqd);

/*
 * [ These __weak aliases are kept in a separate compilation unit, so that
 *   GCC does not inline them incorrectly. ]
 */

int __init __weak early_irq_init(void)
{
	return 0;
}

int __init __weak arch_probe_nr_irqs(void)
{
	return NR_IRQS_LEGACY;
}

int __init __weak arch_early_irq_init(void)
{
	return 0;
}

unsigned int __weak arch_dynirq_lower_bound(unsigned int from)
{
	return from;
}
