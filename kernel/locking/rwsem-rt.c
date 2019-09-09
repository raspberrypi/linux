/*
 */
#include <linux/blkdev.h>
#include <linux/rwsem.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/export.h>

#include "rtmutex_common.h"

/*
 * RT-specific reader/writer semaphores
 *
 * down_write()
 *  1) Lock sem->rtmutex
 *  2) Remove the reader BIAS to force readers into the slow path
 *  3) Wait until all readers have left the critical region
 *  4) Mark it write locked
 *
 * up_write()
 *  1) Remove the write locked marker
 *  2) Set the reader BIAS so readers can use the fast path again
 *  3) Unlock sem->rtmutex to release blocked readers
 *
 * down_read()
 *  1) Try fast path acquisition (reader BIAS is set)
 *  2) Take sem->rtmutex.wait_lock which protects the writelocked flag
 *  3) If !writelocked, acquire it for read
 *  4) If writelocked, block on sem->rtmutex
 *  5) unlock sem->rtmutex, goto 1)
 *
 * up_read()
 *  1) Try fast path release (reader count != 1)
 *  2) Wake the writer waiting in down_write()#3
 *
 * down_read()#3 has the consequence, that rw semaphores on RT are not writer
 * fair, but writers, which should be avoided in RT tasks (think mmap_sem),
 * are subject to the rtmutex priority/DL inheritance mechanism.
 *
 * It's possible to make the rw semaphores writer fair by keeping a list of
 * active readers. A blocked writer would force all newly incoming readers to
 * block on the rtmutex, but the rtmutex would have to be proxy locked for one
 * reader after the other. We can't use multi-reader inheritance because there
 * is no way to support that with SCHED_DEADLINE. Implementing the one by one
 * reader boosting/handover mechanism is a major surgery for a very dubious
 * value.
 *
 * The risk of writer starvation is there, but the pathological use cases
 * which trigger it are not necessarily the typical RT workloads.
 */

void __rwsem_init(struct rw_semaphore *sem, const char *name,
		  struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	/*
	 * Make sure we are not reinitializing a held semaphore:
	 */
	debug_check_no_locks_freed((void *)sem, sizeof(*sem));
	lockdep_init_map(&sem->dep_map, name, key, 0);
#endif
	atomic_set(&sem->readers, READER_BIAS);
}
EXPORT_SYMBOL(__rwsem_init);

int __down_read_trylock(struct rw_semaphore *sem)
{
	int r, old;

	/*
	 * Increment reader count, if sem->readers < 0, i.e. READER_BIAS is
	 * set.
	 */
	for (r = atomic_read(&sem->readers); r < 0;) {
		old = atomic_cmpxchg(&sem->readers, r, r + 1);
		if (likely(old == r))
			return 1;
		r = old;
	}
	return 0;
}

void __sched __down_read(struct rw_semaphore *sem)
{
	struct rt_mutex *m = &sem->rtmutex;
	struct rt_mutex_waiter waiter;

	if (__down_read_trylock(sem))
		return;
	/*
	 * If rt_mutex blocks, the function sched_submit_work will not call
	 * blk_schedule_flush_plug (because tsk_is_pi_blocked would be true).
	 * We must call blk_schedule_flush_plug here, if we don't call it,
	 * a deadlock in I/O may happen.
	 */
	if (unlikely(blk_needs_flush_plug(current)))
		blk_schedule_flush_plug(current);

	might_sleep();
	raw_spin_lock_irq(&m->wait_lock);
	/*
	 * Allow readers as long as the writer has not completely
	 * acquired the semaphore for write.
	 */
	if (atomic_read(&sem->readers) != WRITER_BIAS) {
		atomic_inc(&sem->readers);
		raw_spin_unlock_irq(&m->wait_lock);
		return;
	}

	/*
	 * Call into the slow lock path with the rtmutex->wait_lock
	 * held, so this can't result in the following race:
	 *
	 * Reader1		Reader2		Writer
	 *			down_read()
	 *					down_write()
	 *					rtmutex_lock(m)
	 *					swait()
	 * down_read()
	 * unlock(m->wait_lock)
	 *			up_read()
	 *			swake()
	 *					lock(m->wait_lock)
	 *					sem->writelocked=true
	 *					unlock(m->wait_lock)
	 *
	 *					up_write()
	 *					sem->writelocked=false
	 *					rtmutex_unlock(m)
	 *			down_read()
	 *					down_write()
	 *					rtmutex_lock(m)
	 *					swait()
	 * rtmutex_lock(m)
	 *
	 * That would put Reader1 behind the writer waiting on
	 * Reader2 to call up_read() which might be unbound.
	 */
	rt_mutex_init_waiter(&waiter, false);
	rt_mutex_slowlock_locked(m, TASK_UNINTERRUPTIBLE, NULL,
				 RT_MUTEX_MIN_CHAINWALK, NULL,
				 &waiter);
	/*
	 * The slowlock() above is guaranteed to return with the rtmutex is
	 * now held, so there can't be a writer active. Increment the reader
	 * count and immediately drop the rtmutex again.
	 */
	atomic_inc(&sem->readers);
	raw_spin_unlock_irq(&m->wait_lock);
	__rt_mutex_unlock(m);

	debug_rt_mutex_free_waiter(&waiter);
}

void __up_read(struct rw_semaphore *sem)
{
	struct rt_mutex *m = &sem->rtmutex;
	struct task_struct *tsk;

	/*
	 * sem->readers can only hit 0 when a writer is waiting for the
	 * active readers to leave the critical region.
	 */
	if (!atomic_dec_and_test(&sem->readers))
		return;

	might_sleep();
	raw_spin_lock_irq(&m->wait_lock);
	/*
	 * Wake the writer, i.e. the rtmutex owner. It might release the
	 * rtmutex concurrently in the fast path (due to a signal), but to
	 * clean up the rwsem it needs to acquire m->wait_lock. The worst
	 * case which can happen is a spurious wakeup.
	 */
	tsk = rt_mutex_owner(m);
	if (tsk)
		wake_up_process(tsk);

	raw_spin_unlock_irq(&m->wait_lock);
}

static void __up_write_unlock(struct rw_semaphore *sem, int bias,
			      unsigned long flags)
{
	struct rt_mutex *m = &sem->rtmutex;

	atomic_add(READER_BIAS - bias, &sem->readers);
	raw_spin_unlock_irqrestore(&m->wait_lock, flags);
	__rt_mutex_unlock(m);
}

static int __sched __down_write_common(struct rw_semaphore *sem, int state)
{
	struct rt_mutex *m = &sem->rtmutex;
	unsigned long flags;

	/* Take the rtmutex as a first step */
	if (__rt_mutex_lock_state(m, state))
		return -EINTR;

	/* Force readers into slow path */
	atomic_sub(READER_BIAS, &sem->readers);
	might_sleep();

	set_current_state(state);
	for (;;) {
		raw_spin_lock_irqsave(&m->wait_lock, flags);
		/* Have all readers left the critical region? */
		if (!atomic_read(&sem->readers)) {
			atomic_set(&sem->readers, WRITER_BIAS);
			__set_current_state(TASK_RUNNING);
			raw_spin_unlock_irqrestore(&m->wait_lock, flags);
			return 0;
		}

		if (signal_pending_state(state, current)) {
			__set_current_state(TASK_RUNNING);
			__up_write_unlock(sem, 0, flags);
			return -EINTR;
		}
		raw_spin_unlock_irqrestore(&m->wait_lock, flags);

		if (atomic_read(&sem->readers) != 0) {
			schedule();
			set_current_state(state);
		}
	}
}

void __sched __down_write(struct rw_semaphore *sem)
{
	__down_write_common(sem, TASK_UNINTERRUPTIBLE);
}

int __sched __down_write_killable(struct rw_semaphore *sem)
{
	return __down_write_common(sem, TASK_KILLABLE);
}

int __down_write_trylock(struct rw_semaphore *sem)
{
	struct rt_mutex *m = &sem->rtmutex;
	unsigned long flags;

	if (!__rt_mutex_trylock(m))
		return 0;

	atomic_sub(READER_BIAS, &sem->readers);

	raw_spin_lock_irqsave(&m->wait_lock, flags);
	if (!atomic_read(&sem->readers)) {
		atomic_set(&sem->readers, WRITER_BIAS);
		raw_spin_unlock_irqrestore(&m->wait_lock, flags);
		return 1;
	}
	__up_write_unlock(sem, 0, flags);
	return 0;
}

void __up_write(struct rw_semaphore *sem)
{
	struct rt_mutex *m = &sem->rtmutex;
	unsigned long flags;

	raw_spin_lock_irqsave(&m->wait_lock, flags);
	__up_write_unlock(sem, WRITER_BIAS, flags);
}

void __downgrade_write(struct rw_semaphore *sem)
{
	struct rt_mutex *m = &sem->rtmutex;
	unsigned long flags;

	raw_spin_lock_irqsave(&m->wait_lock, flags);
	/* Release it and account current as reader */
	__up_write_unlock(sem, WRITER_BIAS - 1, flags);
}
