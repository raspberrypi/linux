#ifndef __LINUX_RWLOCK_RT_H
#define __LINUX_RWLOCK_RT_H

#ifndef __LINUX_SPINLOCK_H
#error Do not include directly. Use spinlock.h
#endif

extern void __lockfunc rt_write_lock(rwlock_t *rwlock);
extern void __lockfunc rt_read_lock(rwlock_t *rwlock);
extern int __lockfunc rt_write_trylock(rwlock_t *rwlock);
extern int __lockfunc rt_read_trylock(rwlock_t *rwlock);
extern void __lockfunc rt_write_unlock(rwlock_t *rwlock);
extern void __lockfunc rt_read_unlock(rwlock_t *rwlock);
extern int __lockfunc rt_read_can_lock(rwlock_t *rwlock);
extern int __lockfunc rt_write_can_lock(rwlock_t *rwlock);
extern void __rt_rwlock_init(rwlock_t *rwlock, char *name, struct lock_class_key *key);

#define read_can_lock(rwlock)		rt_read_can_lock(rwlock)
#define write_can_lock(rwlock)		rt_write_can_lock(rwlock)

#define read_trylock(lock)	__cond_lock(lock, rt_read_trylock(lock))
#define write_trylock(lock)	__cond_lock(lock, rt_write_trylock(lock))

static inline int __write_trylock_rt_irqsave(rwlock_t *lock, unsigned long *flags)
{
	/* XXX ARCH_IRQ_ENABLED */
	*flags = 0;
	return rt_write_trylock(lock);
}

#define write_trylock_irqsave(lock, flags)		\
	__cond_lock(lock, __write_trylock_rt_irqsave(lock, &(flags)))

#define read_lock_irqsave(lock, flags)			\
	do {						\
		typecheck(unsigned long, flags);	\
		rt_read_lock(lock);			\
		flags = 0;				\
	} while (0)

#define write_lock_irqsave(lock, flags)			\
	do {						\
		typecheck(unsigned long, flags);	\
		rt_write_lock(lock);			\
		flags = 0;				\
	} while (0)

#define read_lock(lock)		rt_read_lock(lock)

#define read_lock_bh(lock)				\
	do {						\
		local_bh_disable();			\
		rt_read_lock(lock);			\
	} while (0)

#define read_lock_irq(lock)	read_lock(lock)

#define write_lock(lock)	rt_write_lock(lock)

#define write_lock_bh(lock)				\
	do {						\
		local_bh_disable();			\
		rt_write_lock(lock);			\
	} while (0)

#define write_lock_irq(lock)	write_lock(lock)

#define read_unlock(lock)	rt_read_unlock(lock)

#define read_unlock_bh(lock)				\
	do {						\
		rt_read_unlock(lock);			\
		local_bh_enable();			\
	} while (0)

#define read_unlock_irq(lock)	read_unlock(lock)

#define write_unlock(lock)	rt_write_unlock(lock)

#define write_unlock_bh(lock)				\
	do {						\
		rt_write_unlock(lock);			\
		local_bh_enable();			\
	} while (0)

#define write_unlock_irq(lock)	write_unlock(lock)

#define read_unlock_irqrestore(lock, flags)		\
	do {						\
		typecheck(unsigned long, flags);	\
		(void) flags;				\
		rt_read_unlock(lock);			\
	} while (0)

#define write_unlock_irqrestore(lock, flags) \
	do {						\
		typecheck(unsigned long, flags);	\
		(void) flags;				\
		rt_write_unlock(lock);			\
	} while (0)

#define rwlock_init(rwl)				\
do {							\
	static struct lock_class_key __key;		\
							\
	__rt_rwlock_init(rwl, #rwl, &__key);		\
} while (0)

/*
 * Internal functions made global for CPU pinning
 */
void __read_rt_lock(struct rt_rw_lock *lock);
int __read_rt_trylock(struct rt_rw_lock *lock);
void __write_rt_lock(struct rt_rw_lock *lock);
int __write_rt_trylock(struct rt_rw_lock *lock);
void __read_rt_unlock(struct rt_rw_lock *lock);
void __write_rt_unlock(struct rt_rw_lock *lock);

#endif
