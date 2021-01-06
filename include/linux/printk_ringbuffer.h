/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PRINTK_RINGBUFFER_H
#define _LINUX_PRINTK_RINGBUFFER_H

#include <linux/irq_work.h>
#include <linux/atomic.h>
#include <linux/percpu.h>
#include <linux/wait.h>

struct prb_cpulock {
	atomic_t owner;
	unsigned long __percpu *irqflags;
};

struct printk_ringbuffer {
	void *buffer;
	unsigned int size_bits;

	u64 seq;
	atomic_long_t lost;

	atomic_long_t tail;
	atomic_long_t head;
	atomic_long_t reserve;

	struct prb_cpulock *cpulock;
	atomic_t ctx;

	struct wait_queue_head *wq;
	atomic_long_t wq_counter;
	struct irq_work *wq_work;
};

struct prb_entry {
	unsigned int size;
	u64 seq;
	char data[0];
};

struct prb_handle {
	struct printk_ringbuffer *rb;
	unsigned int cpu;
	struct prb_entry *entry;
};

#define DECLARE_STATIC_PRINTKRB_CPULOCK(name)				\
static DEFINE_PER_CPU(unsigned long, _##name##_percpu_irqflags);	\
static struct prb_cpulock name = {					\
	.owner = ATOMIC_INIT(-1),					\
	.irqflags = &_##name##_percpu_irqflags,				\
}

#define PRB_INIT ((unsigned long)-1)

#define DECLARE_STATIC_PRINTKRB_ITER(name, rbaddr)			\
static struct prb_iterator name = {					\
	.rb = rbaddr,							\
	.lpos = PRB_INIT,						\
}

struct prb_iterator {
	struct printk_ringbuffer *rb;
	unsigned long lpos;
};

#define DECLARE_STATIC_PRINTKRB(name, szbits, cpulockptr)		\
static char _##name##_buffer[1 << (szbits)]				\
	__aligned(__alignof__(long));					\
static DECLARE_WAIT_QUEUE_HEAD(_##name##_wait);				\
static void _##name##_wake_work_func(struct irq_work *irq_work)		\
{									\
	wake_up_interruptible_all(&_##name##_wait);			\
}									\
static struct irq_work _##name##_wake_work = {				\
	.func = _##name##_wake_work_func,				\
	.flags = IRQ_WORK_LAZY,						\
};									\
static struct printk_ringbuffer name = {				\
	.buffer = &_##name##_buffer[0],					\
	.size_bits = szbits,						\
	.seq = 0,							\
	.lost = ATOMIC_LONG_INIT(0),					\
	.tail = ATOMIC_LONG_INIT(-111 * sizeof(long)),			\
	.head = ATOMIC_LONG_INIT(-111 * sizeof(long)),			\
	.reserve = ATOMIC_LONG_INIT(-111 * sizeof(long)),		\
	.cpulock = cpulockptr,						\
	.ctx = ATOMIC_INIT(0),						\
	.wq = &_##name##_wait,						\
	.wq_counter = ATOMIC_LONG_INIT(0),				\
	.wq_work = &_##name##_wake_work,				\
}

/* writer interface */
char *prb_reserve(struct prb_handle *h, struct printk_ringbuffer *rb,
		  unsigned int size);
void prb_commit(struct prb_handle *h);

/* reader interface */
void prb_iter_init(struct prb_iterator *iter, struct printk_ringbuffer *rb,
		   u64 *seq);
void prb_iter_copy(struct prb_iterator *dest, struct prb_iterator *src);
int prb_iter_next(struct prb_iterator *iter, char *buf, int size, u64 *seq);
int prb_iter_wait_next(struct prb_iterator *iter, char *buf, int size,
		       u64 *seq);
int prb_iter_seek(struct prb_iterator *iter, u64 seq);
int prb_iter_data(struct prb_iterator *iter, char *buf, int size, u64 *seq);

/* utility functions */
int prb_buffer_size(struct printk_ringbuffer *rb);
void prb_inc_lost(struct printk_ringbuffer *rb);
void prb_lock(struct prb_cpulock *cpu_lock, unsigned int *cpu_store);
void prb_unlock(struct prb_cpulock *cpu_lock, unsigned int cpu_store);

#endif /*_LINUX_PRINTK_RINGBUFFER_H */
