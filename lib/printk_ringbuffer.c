// SPDX-License-Identifier: GPL-2.0
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/printk_ringbuffer.h>

#define PRB_SIZE(rb) (1 << rb->size_bits)
#define PRB_SIZE_BITMASK(rb) (PRB_SIZE(rb) - 1)
#define PRB_INDEX(rb, lpos) (lpos & PRB_SIZE_BITMASK(rb))
#define PRB_WRAPS(rb, lpos) (lpos >> rb->size_bits)
#define PRB_WRAP_LPOS(rb, lpos, xtra) \
	((PRB_WRAPS(rb, lpos) + xtra) << rb->size_bits)
#define PRB_DATA_SIZE(e) (e->size - sizeof(struct prb_entry))
#define PRB_DATA_ALIGN sizeof(long)

static bool __prb_trylock(struct prb_cpulock *cpu_lock,
			  unsigned int *cpu_store)
{
	unsigned long *flags;
	unsigned int cpu;

	cpu = get_cpu();

	*cpu_store = atomic_read(&cpu_lock->owner);
	/* memory barrier to ensure the current lock owner is visible */
	smp_rmb();
	if (*cpu_store == -1) {
		flags = per_cpu_ptr(cpu_lock->irqflags, cpu);
		local_irq_save(*flags);
		if (atomic_try_cmpxchg_acquire(&cpu_lock->owner,
					       cpu_store, cpu)) {
			return true;
		}
		local_irq_restore(*flags);
	} else if (*cpu_store == cpu) {
		return true;
	}

	put_cpu();
	return false;
}

/*
 * prb_lock: Perform a processor-reentrant spin lock.
 * @cpu_lock: A pointer to the lock object.
 * @cpu_store: A "flags" pointer to store lock status information.
 *
 * If no processor has the lock, the calling processor takes the lock and
 * becomes the owner. If the calling processor is already the owner of the
 * lock, this function succeeds immediately. If lock is locked by another
 * processor, this function spins until the calling processor becomes the
 * owner.
 *
 * It is safe to call this function from any context and state.
 */
void prb_lock(struct prb_cpulock *cpu_lock, unsigned int *cpu_store)
{
	for (;;) {
		if (__prb_trylock(cpu_lock, cpu_store))
			break;
		cpu_relax();
	}
}

/*
 * prb_unlock: Perform a processor-reentrant spin unlock.
 * @cpu_lock: A pointer to the lock object.
 * @cpu_store: A "flags" object storing lock status information.
 *
 * Release the lock. The calling processor must be the owner of the lock.
 *
 * It is safe to call this function from any context and state.
 */
void prb_unlock(struct prb_cpulock *cpu_lock, unsigned int cpu_store)
{
	unsigned long *flags;
	unsigned int cpu;

	cpu = atomic_read(&cpu_lock->owner);
	atomic_set_release(&cpu_lock->owner, cpu_store);

	if (cpu_store == -1) {
		flags = per_cpu_ptr(cpu_lock->irqflags, cpu);
		local_irq_restore(*flags);
	}

	put_cpu();
}

static struct prb_entry *to_entry(struct printk_ringbuffer *rb,
				  unsigned long lpos)
{
	char *buffer = rb->buffer;
	buffer += PRB_INDEX(rb, lpos);
	return (struct prb_entry *)buffer;
}

static int calc_next(struct printk_ringbuffer *rb, unsigned long tail,
		     unsigned long lpos, int size, unsigned long *calced_next)
{
	unsigned long next_lpos;
	int ret = 0;
again:
	next_lpos = lpos + size;
	if (next_lpos - tail > PRB_SIZE(rb))
		return -1;

	if (PRB_WRAPS(rb, lpos) != PRB_WRAPS(rb, next_lpos)) {
		lpos = PRB_WRAP_LPOS(rb, next_lpos, 0);
		ret |= 1;
		goto again;
	}

	*calced_next = next_lpos;
	return ret;
}

static bool push_tail(struct printk_ringbuffer *rb, unsigned long tail)
{
	unsigned long new_tail;
	struct prb_entry *e;
	unsigned long head;

	if (tail != atomic_long_read(&rb->tail))
		return true;

	e = to_entry(rb, tail);
	if (e->size != -1)
		new_tail = tail + e->size;
	else
		new_tail = PRB_WRAP_LPOS(rb, tail, 1);

	/* make sure the new tail does not overtake the head */
	head = atomic_long_read(&rb->head);
	if (head - new_tail > PRB_SIZE(rb))
		return false;

	atomic_long_cmpxchg(&rb->tail, tail, new_tail);
	return true;
}

/*
 * prb_commit: Commit a reserved entry to the ring buffer.
 * @h: An entry handle referencing the data entry to commit.
 *
 * Commit data that has been reserved using prb_reserve(). Once the data
 * block has been committed, it can be invalidated at any time. If a writer
 * is interested in using the data after committing, the writer should make
 * its own copy first or use the prb_iter_ reader functions to access the
 * data in the ring buffer.
 *
 * It is safe to call this function from any context and state.
 */
void prb_commit(struct prb_handle *h)
{
	struct printk_ringbuffer *rb = h->rb;
	bool changed = false;
	struct prb_entry *e;
	unsigned long head;
	unsigned long res;

	for (;;) {
		if (atomic_read(&rb->ctx) != 1) {
			/* the interrupted context will fixup head */
			atomic_dec(&rb->ctx);
			break;
		}
		/* assign sequence numbers before moving head */
		head = atomic_long_read(&rb->head);
		res = atomic_long_read(&rb->reserve);
		while (head != res) {
			e = to_entry(rb, head);
			if (e->size == -1) {
				head = PRB_WRAP_LPOS(rb, head, 1);
				continue;
			}
			while (atomic_long_read(&rb->lost)) {
				atomic_long_dec(&rb->lost);
				rb->seq++;
			}
			e->seq = ++rb->seq;
			head += e->size;
			changed = true;
		}
		atomic_long_set_release(&rb->head, res);

		atomic_dec(&rb->ctx);

		if (atomic_long_read(&rb->reserve) == res)
			break;
		atomic_inc(&rb->ctx);
	}

	prb_unlock(rb->cpulock, h->cpu);

	if (changed) {
		atomic_long_inc(&rb->wq_counter);
		if (wq_has_sleeper(rb->wq)) {
#ifdef CONFIG_IRQ_WORK
			irq_work_queue(rb->wq_work);
#else
			if (!in_nmi())
				wake_up_interruptible_all(rb->wq);
#endif
		}
	}
}

/*
 * prb_reserve: Reserve an entry within a ring buffer.
 * @h: An entry handle to be setup and reference an entry.
 * @rb: A ring buffer to reserve data within.
 * @size: The number of bytes to reserve.
 *
 * Reserve an entry of at least @size bytes to be used by the caller. If
 * successful, the data region of the entry belongs to the caller and cannot
 * be invalidated by any other task/context. For this reason, the caller
 * should call prb_commit() as quickly as possible in order to avoid preventing
 * other tasks/contexts from reserving data in the case that the ring buffer
 * has wrapped.
 *
 * It is safe to call this function from any context and state.
 *
 * Returns a pointer to the reserved entry (and @h is setup to reference that
 * entry) or NULL if it was not possible to reserve data.
 */
char *prb_reserve(struct prb_handle *h, struct printk_ringbuffer *rb,
		  unsigned int size)
{
	unsigned long tail, res1, res2;
	int ret;

	if (size == 0)
		return NULL;
	size += sizeof(struct prb_entry);
	size += PRB_DATA_ALIGN - 1;
	size &= ~(PRB_DATA_ALIGN - 1);
	if (size >= PRB_SIZE(rb))
		return NULL;

	h->rb = rb;
	prb_lock(rb->cpulock, &h->cpu);

	atomic_inc(&rb->ctx);

	do {
		for (;;) {
			tail = atomic_long_read(&rb->tail);
			res1 = atomic_long_read(&rb->reserve);
			ret = calc_next(rb, tail, res1, size, &res2);
			if (ret >= 0)
				break;
			if (!push_tail(rb, tail)) {
				prb_commit(h);
				return NULL;
			}
		}
	} while (!atomic_long_try_cmpxchg_acquire(&rb->reserve, &res1, res2));

	h->entry = to_entry(rb, res1);

	if (ret) {
		/* handle wrap */
		h->entry->size = -1;
		h->entry = to_entry(rb, PRB_WRAP_LPOS(rb, res2, 0));
	}

	h->entry->size = size;

	return &h->entry->data[0];
}

/*
 * prb_iter_copy: Copy an iterator.
 * @dest: The iterator to copy to.
 * @src: The iterator to copy from.
 *
 * Make a deep copy of an iterator. This is particularly useful for making
 * backup copies of an iterator in case a form of rewinding it needed.
 *
 * It is safe to call this function from any context and state. But
 * note that this function is not atomic. Callers should not make copies
 * to/from iterators that can be accessed by other tasks/contexts.
 */
void prb_iter_copy(struct prb_iterator *dest, struct prb_iterator *src)
{
	memcpy(dest, src, sizeof(*dest));
}

/*
 * prb_iter_init: Initialize an iterator for a ring buffer.
 * @iter: The iterator to initialize.
 * @rb: A ring buffer to that @iter should iterate.
 * @seq: The sequence number of the position preceding the first record.
 *       May be NULL.
 *
 * Initialize an iterator to be used with a specified ring buffer. If @seq
 * is non-NULL, it will be set such that prb_iter_next() will provide a
 * sequence value of "@seq + 1" if no records were missed.
 *
 * It is safe to call this function from any context and state.
 */
void prb_iter_init(struct prb_iterator *iter, struct printk_ringbuffer *rb,
		   u64 *seq)
{
	memset(iter, 0, sizeof(*iter));
	iter->rb = rb;
	iter->lpos = PRB_INIT;

	if (!seq)
		return;

	for (;;) {
		struct prb_iterator tmp_iter;
		int ret;

		prb_iter_copy(&tmp_iter, iter);

		ret = prb_iter_next(&tmp_iter, NULL, 0, seq);
		if (ret < 0)
			continue;

		if (ret == 0)
			*seq = 0;
		else
			(*seq)--;
		break;
	}
}

static bool is_valid(struct printk_ringbuffer *rb, unsigned long lpos)
{
	unsigned long head, tail;

	tail = atomic_long_read(&rb->tail);
	head = atomic_long_read(&rb->head);
	head -= tail;
	lpos -= tail;

	if (lpos >= head)
		return false;
	return true;
}

/*
 * prb_iter_data: Retrieve the record data at the current position.
 * @iter: Iterator tracking the current position.
 * @buf: A buffer to store the data of the record. May be NULL.
 * @size: The size of @buf. (Ignored if @buf is NULL.)
 * @seq: The sequence number of the record. May be NULL.
 *
 * If @iter is at a record, provide the data and/or sequence number of that
 * record (if specified by the caller).
 *
 * It is safe to call this function from any context and state.
 *
 * Returns >=0 if the current record contains valid data (returns 0 if @buf
 * is NULL or returns the size of the data block if @buf is non-NULL) or
 * -EINVAL if @iter is now invalid.
 */
int prb_iter_data(struct prb_iterator *iter, char *buf, int size, u64 *seq)
{
	struct printk_ringbuffer *rb = iter->rb;
	unsigned long lpos = iter->lpos;
	unsigned int datsize = 0;
	struct prb_entry *e;

	if (buf || seq) {
		e = to_entry(rb, lpos);
		if (!is_valid(rb, lpos))
			return -EINVAL;
		/* memory barrier to ensure valid lpos */
		smp_rmb();
		if (buf) {
			datsize = PRB_DATA_SIZE(e);
			/* memory barrier to ensure load of datsize */
			smp_rmb();
			if (!is_valid(rb, lpos))
				return -EINVAL;
			if (PRB_INDEX(rb, lpos) + datsize >
			    PRB_SIZE(rb) - PRB_DATA_ALIGN) {
				return -EINVAL;
			}
			if (size > datsize)
				size = datsize;
			memcpy(buf, &e->data[0], size);
		}
		if (seq)
			*seq = e->seq;
		/* memory barrier to ensure loads of entry data */
		smp_rmb();
	}

	if (!is_valid(rb, lpos))
		return -EINVAL;

	return datsize;
}

/*
 * prb_iter_next: Advance to the next record.
 * @iter: Iterator tracking the current position.
 * @buf: A buffer to store the data of the next record. May be NULL.
 * @size: The size of @buf. (Ignored if @buf is NULL.)
 * @seq: The sequence number of the next record. May be NULL.
 *
 * If a next record is available, @iter is advanced and (if specified)
 * the data and/or sequence number of that record are provided.
 *
 * It is safe to call this function from any context and state.
 *
 * Returns 1 if @iter was advanced, 0 if @iter is at the end of the list, or
 * -EINVAL if @iter is now invalid.
 */
int prb_iter_next(struct prb_iterator *iter, char *buf, int size, u64 *seq)
{
	struct printk_ringbuffer *rb = iter->rb;
	unsigned long next_lpos;
	struct prb_entry *e;
	unsigned int esize;

	if (iter->lpos == PRB_INIT) {
		next_lpos = atomic_long_read(&rb->tail);
	} else {
		if (!is_valid(rb, iter->lpos))
			return -EINVAL;
		/* memory barrier to ensure valid lpos */
		smp_rmb();
		e = to_entry(rb, iter->lpos);
		esize = e->size;
		/* memory barrier to ensure load of size */
		smp_rmb();
		if (!is_valid(rb, iter->lpos))
			return -EINVAL;
		next_lpos = iter->lpos + esize;
	}
	if (next_lpos == atomic_long_read(&rb->head))
		return 0;
	if (!is_valid(rb, next_lpos))
		return -EINVAL;
	/* memory barrier to ensure valid lpos */
	smp_rmb();

	iter->lpos = next_lpos;
	e = to_entry(rb, iter->lpos);
	esize = e->size;
	/* memory barrier to ensure load of size */
	smp_rmb();
	if (!is_valid(rb, iter->lpos))
		return -EINVAL;
	if (esize == -1)
		iter->lpos = PRB_WRAP_LPOS(rb, iter->lpos, 1);

	if (prb_iter_data(iter, buf, size, seq) < 0)
		return -EINVAL;

	return 1;
}

/*
 * prb_iter_wait_next: Advance to the next record, blocking if none available.
 * @iter: Iterator tracking the current position.
 * @buf: A buffer to store the data of the next record. May be NULL.
 * @size: The size of @buf. (Ignored if @buf is NULL.)
 * @seq: The sequence number of the next record. May be NULL.
 *
 * If a next record is already available, this function works like
 * prb_iter_next(). Otherwise block interruptible until a next record is
 * available.
 *
 * When a next record is available, @iter is advanced and (if specified)
 * the data and/or sequence number of that record are provided.
 *
 * This function might sleep.
 *
 * Returns 1 if @iter was advanced, -EINVAL if @iter is now invalid, or
 * -ERESTARTSYS if interrupted by a signal.
 */
int prb_iter_wait_next(struct prb_iterator *iter, char *buf, int size, u64 *seq)
{
	unsigned long last_seen;
	int ret;

	for (;;) {
		last_seen = atomic_long_read(&iter->rb->wq_counter);

		ret = prb_iter_next(iter, buf, size, seq);
		if (ret != 0)
			break;

		ret = wait_event_interruptible(*iter->rb->wq,
			last_seen != atomic_long_read(&iter->rb->wq_counter));
		if (ret < 0)
			break;
	}

	return ret;
}

/*
 * prb_iter_seek: Seek forward to a specific record.
 * @iter: Iterator to advance.
 * @seq: Record number to advance to.
 *
 * Advance @iter such that a following call to prb_iter_data() will provide
 * the contents of the specified record. If a record is specified that does
 * not yet exist, advance @iter to the end of the record list.
 *
 * Note that iterators cannot be rewound. So if a record is requested that
 * exists but is previous to @iter in position, @iter is considered invalid.
 *
 * It is safe to call this function from any context and state.
 *
 * Returns 1 on succces, 0 if specified record does not yet exist (@iter is
 * now at the end of the list), or -EINVAL if @iter is now invalid.
 */
int prb_iter_seek(struct prb_iterator *iter, u64 seq)
{
	u64 cur_seq;
	int ret;

	/* first check if the iterator is already at the wanted seq */
	if (seq == 0) {
		if (iter->lpos == PRB_INIT)
			return 1;
		else
			return -EINVAL;
	}
	if (iter->lpos != PRB_INIT) {
		if (prb_iter_data(iter, NULL, 0, &cur_seq) >= 0) {
			if (cur_seq == seq)
				return 1;
			if (cur_seq > seq)
				return -EINVAL;
		}
	}

	/* iterate to find the wanted seq */
	for (;;) {
		ret = prb_iter_next(iter, NULL, 0, &cur_seq);
		if (ret <= 0)
			break;

		if (cur_seq == seq)
			break;

		if (cur_seq > seq) {
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

/*
 * prb_buffer_size: Get the size of the ring buffer.
 * @rb: The ring buffer to get the size of.
 *
 * Return the number of bytes used for the ring buffer entry storage area.
 * Note that this area stores both entry header and entry data. Therefore
 * this represents an upper bound to the amount of data that can be stored
 * in the ring buffer.
 *
 * It is safe to call this function from any context and state.
 *
 * Returns the size in bytes of the entry storage area.
 */
int prb_buffer_size(struct printk_ringbuffer *rb)
{
	return PRB_SIZE(rb);
}

/*
 * prb_inc_lost: Increment the seq counter to signal a lost record.
 * @rb: The ring buffer to increment the seq of.
 *
 * Increment the seq counter so that a seq number is intentially missing
 * for the readers. This allows readers to identify that a record is
 * missing. A writer will typically use this function if prb_reserve()
 * fails.
 *
 * It is safe to call this function from any context and state.
 */
void prb_inc_lost(struct printk_ringbuffer *rb)
{
	atomic_long_inc(&rb->lost);
}
