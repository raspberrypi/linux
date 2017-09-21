/*
 */
#include <linux/sched/debug.h>
#include <linux/export.h>

#include "rtmutex_common.h"
#include <linux/rwlock_types_rt.h>

/*
 * RT-specific reader/writer locks
 *
 * write_lock()
 *  1) Lock lock->rtmutex
 *  2) Remove the reader BIAS to force readers into the slow path
 *  3) Wait until all readers have left the critical region
 *  4) Mark it write locked
 *
 * write_unlock()
 *  1) Remove the write locked marker
 *  2) Set the reader BIAS so readers can use the fast path again
 *  3) Unlock lock->rtmutex to release blocked readers
 *
 * read_lock()
 *  1) Try fast path acquisition (reader BIAS is set)
 *  2) Take lock->rtmutex.wait_lock which protects the writelocked flag
 *  3) If !writelocked, acquire it for read
 *  4) If writelocked, block on lock->rtmutex
 *  5) unlock lock->rtmutex, goto 1)
 *
 * read_unlock()
 *  1) Try fast path release (reader count != 1)
 *  2) Wake the writer waiting in write_lock()#3
 *
 * read_lock()#3 has the consequence, that rw locks on RT are not writer
 * fair, but writers, which should be avoided in RT tasks (think tasklist
 * lock), are subject to the rtmutex priority/DL inheritance mechanism.
 *
 * It's possible to make the rw locks writer fair by keeping a list of
 * active readers. A blocked writer would force all newly incoming readers
 * to block on the rtmutex, but the rtmutex would have to be proxy locked
 * for one reader after the other. We can't use multi-reader inheritance
 * because there is no way to support that with
 * SCHED_DEADLINE. Implementing the one by one reader boosting/handover
 * mechanism is a major surgery for a very dubious value.
 *
 * The risk of writer starvation is there, but the pathological use cases
 * which trigger it are not necessarily the typical RT workloads.
 */

void __rwlock_biased_rt_init(struct rt_rw_lock *lock, const char *name,
			     struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	/*
	 * Make sure we are not reinitializing a held semaphore:
	 */
	debug_check_no_locks_freed((void *)lock, sizeof(*lock));
	lockdep_init_map(&lock->dep_map, name, key, 0);
#endif
	atomic_set(&lock->readers, READER_BIAS);
	rt_mutex_init(&lock->rtmutex);
	lock->rtmutex.save_state = 1;
}

int __read_rt_trylock(struct rt_rw_lock *lock)
{
	int r, old;

	/*
	 * Increment reader count, if lock->readers < 0, i.e. READER_BIAS is
	 * set.
	 */
	for (r = atomic_read(&lock->readers); r < 0;) {
		old = atomic_cmpxchg(&lock->readers, r, r + 1);
		if (likely(old == r))
			return 1;
		r = old;
	}
	return 0;
}

void __sched __read_rt_lock(struct rt_rw_lock *lock)
{
	struct rt_mutex *m = &lock->rtmutex;
	struct rt_mutex_waiter waiter;
	unsigned long flags;

	if (__read_rt_trylock(lock))
		return;

	raw_spin_lock_irqsave(&m->wait_lock, flags);
	/*
	 * Allow readers as long as the writer has not completely
	 * acquired the semaphore for write.
	 */
	if (atomic_read(&lock->readers) != WRITER_BIAS) {
		atomic_inc(&lock->readers);
		raw_spin_unlock_irqrestore(&m->wait_lock, flags);
		return;
	}

	/*
	 * Call into the slow lock path with the rtmutex->wait_lock
	 * held, so this can't result in the following race:
	 *
	 * Reader1		Reader2		Writer
	 *			read_lock()
	 *					write_lock()
	 *					rtmutex_lock(m)
	 *					swait()
	 * read_lock()
	 * unlock(m->wait_lock)
	 *			read_unlock()
	 *			swake()
	 *					lock(m->wait_lock)
	 *					lock->writelocked=true
	 *					unlock(m->wait_lock)
	 *
	 *					write_unlock()
	 *					lock->writelocked=false
	 *					rtmutex_unlock(m)
	 *			read_lock()
	 *					write_lock()
	 *					rtmutex_lock(m)
	 *					swait()
	 * rtmutex_lock(m)
	 *
	 * That would put Reader1 behind the writer waiting on
	 * Reader2 to call read_unlock() which might be unbound.
	 */
	rt_mutex_init_waiter(&waiter, true);
	rt_spin_lock_slowlock_locked(m, &waiter, flags);
	/*
	 * The slowlock() above is guaranteed to return with the rtmutex is
	 * now held, so there can't be a writer active. Increment the reader
	 * count and immediately drop the rtmutex again.
	 */
	atomic_inc(&lock->readers);
	raw_spin_unlock_irqrestore(&m->wait_lock, flags);
	rt_spin_lock_slowunlock(m);

	debug_rt_mutex_free_waiter(&waiter);
}

void __read_rt_unlock(struct rt_rw_lock *lock)
{
	struct rt_mutex *m = &lock->rtmutex;
	struct task_struct *tsk;

	/*
	 * sem->readers can only hit 0 when a writer is waiting for the
	 * active readers to leave the critical region.
	 */
	if (!atomic_dec_and_test(&lock->readers))
		return;

	raw_spin_lock_irq(&m->wait_lock);
	/*
	 * Wake the writer, i.e. the rtmutex owner. It might release the
	 * rtmutex concurrently in the fast path, but to clean up the rw
	 * lock it needs to acquire m->wait_lock. The worst case which can
	 * happen is a spurious wakeup.
	 */
	tsk = rt_mutex_owner(m);
	if (tsk)
		wake_up_process(tsk);

	raw_spin_unlock_irq(&m->wait_lock);
}

static void __write_unlock_common(struct rt_rw_lock *lock, int bias,
				  unsigned long flags)
{
	struct rt_mutex *m = &lock->rtmutex;

	atomic_add(READER_BIAS - bias, &lock->readers);
	raw_spin_unlock_irqrestore(&m->wait_lock, flags);
	rt_spin_lock_slowunlock(m);
}

void __sched __write_rt_lock(struct rt_rw_lock *lock)
{
	struct rt_mutex *m = &lock->rtmutex;
	struct task_struct *self = current;
	unsigned long flags;

	/* Take the rtmutex as a first step */
	__rt_spin_lock(m);

	/* Force readers into slow path */
	atomic_sub(READER_BIAS, &lock->readers);

	raw_spin_lock_irqsave(&m->wait_lock, flags);

	raw_spin_lock(&self->pi_lock);
	self->saved_state = self->state;
	__set_current_state_no_track(TASK_UNINTERRUPTIBLE);
	raw_spin_unlock(&self->pi_lock);

	for (;;) {
		/* Have all readers left the critical region? */
		if (!atomic_read(&lock->readers)) {
			atomic_set(&lock->readers, WRITER_BIAS);
			raw_spin_lock(&self->pi_lock);
			__set_current_state_no_track(self->saved_state);
			self->saved_state = TASK_RUNNING;
			raw_spin_unlock(&self->pi_lock);
			raw_spin_unlock_irqrestore(&m->wait_lock, flags);
			return;
		}

		raw_spin_unlock_irqrestore(&m->wait_lock, flags);

		if (atomic_read(&lock->readers) != 0)
			schedule();

		raw_spin_lock_irqsave(&m->wait_lock, flags);

		raw_spin_lock(&self->pi_lock);
		__set_current_state_no_track(TASK_UNINTERRUPTIBLE);
		raw_spin_unlock(&self->pi_lock);
	}
}

int __write_rt_trylock(struct rt_rw_lock *lock)
{
	struct rt_mutex *m = &lock->rtmutex;
	unsigned long flags;

	if (!__rt_mutex_trylock(m))
		return 0;

	atomic_sub(READER_BIAS, &lock->readers);

	raw_spin_lock_irqsave(&m->wait_lock, flags);
	if (!atomic_read(&lock->readers)) {
		atomic_set(&lock->readers, WRITER_BIAS);
		raw_spin_unlock_irqrestore(&m->wait_lock, flags);
		return 1;
	}
	__write_unlock_common(lock, 0, flags);
	return 0;
}

void __write_rt_unlock(struct rt_rw_lock *lock)
{
	struct rt_mutex *m = &lock->rtmutex;
	unsigned long flags;

	raw_spin_lock_irqsave(&m->wait_lock, flags);
	__write_unlock_common(lock, WRITER_BIAS, flags);
}

/* Map the reader biased implementation */
static inline int do_read_rt_trylock(rwlock_t *rwlock)
{
	return __read_rt_trylock(rwlock);
}

static inline int do_write_rt_trylock(rwlock_t *rwlock)
{
	return __write_rt_trylock(rwlock);
}

static inline void do_read_rt_lock(rwlock_t *rwlock)
{
	__read_rt_lock(rwlock);
}

static inline void do_write_rt_lock(rwlock_t *rwlock)
{
	__write_rt_lock(rwlock);
}

static inline void do_read_rt_unlock(rwlock_t *rwlock)
{
	__read_rt_unlock(rwlock);
}

static inline void do_write_rt_unlock(rwlock_t *rwlock)
{
	__write_rt_unlock(rwlock);
}

static inline void do_rwlock_rt_init(rwlock_t *rwlock, const char *name,
				     struct lock_class_key *key)
{
	__rwlock_biased_rt_init(rwlock, name, key);
}

int __lockfunc rt_read_can_lock(rwlock_t *rwlock)
{
	return  atomic_read(&rwlock->readers) < 0;
}

int __lockfunc rt_write_can_lock(rwlock_t *rwlock)
{
	return atomic_read(&rwlock->readers) == READER_BIAS;
}

/*
 * The common functions which get wrapped into the rwlock API.
 */
int __lockfunc rt_read_trylock(rwlock_t *rwlock)
{
	int ret;

	sleeping_lock_inc();
	migrate_disable();
	ret = do_read_rt_trylock(rwlock);
	if (ret) {
		rwlock_acquire_read(&rwlock->dep_map, 0, 1, _RET_IP_);
	} else {
		migrate_enable();
		sleeping_lock_dec();
	}
	return ret;
}
EXPORT_SYMBOL(rt_read_trylock);

int __lockfunc rt_write_trylock(rwlock_t *rwlock)
{
	int ret;

	sleeping_lock_inc();
	migrate_disable();
	ret = do_write_rt_trylock(rwlock);
	if (ret) {
		rwlock_acquire(&rwlock->dep_map, 0, 1, _RET_IP_);
	} else {
		migrate_enable();
		sleeping_lock_dec();
	}
	return ret;
}
EXPORT_SYMBOL(rt_write_trylock);

void __lockfunc rt_read_lock(rwlock_t *rwlock)
{
	sleeping_lock_inc();
	migrate_disable();
	rwlock_acquire_read(&rwlock->dep_map, 0, 0, _RET_IP_);
	do_read_rt_lock(rwlock);
}
EXPORT_SYMBOL(rt_read_lock);

void __lockfunc rt_write_lock(rwlock_t *rwlock)
{
	sleeping_lock_inc();
	migrate_disable();
	rwlock_acquire(&rwlock->dep_map, 0, 0, _RET_IP_);
	do_write_rt_lock(rwlock);
}
EXPORT_SYMBOL(rt_write_lock);

void __lockfunc rt_read_unlock(rwlock_t *rwlock)
{
	rwlock_release(&rwlock->dep_map, 1, _RET_IP_);
	do_read_rt_unlock(rwlock);
	migrate_enable();
	sleeping_lock_dec();
}
EXPORT_SYMBOL(rt_read_unlock);

void __lockfunc rt_write_unlock(rwlock_t *rwlock)
{
	rwlock_release(&rwlock->dep_map, 1, _RET_IP_);
	do_write_rt_unlock(rwlock);
	migrate_enable();
	sleeping_lock_dec();
}
EXPORT_SYMBOL(rt_write_unlock);

void __rt_rwlock_init(rwlock_t *rwlock, char *name, struct lock_class_key *key)
{
	do_rwlock_rt_init(rwlock, name, key);
}
EXPORT_SYMBOL(__rt_rwlock_init);
