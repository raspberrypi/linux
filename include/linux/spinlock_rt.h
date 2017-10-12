#ifndef __LINUX_SPINLOCK_RT_H
#define __LINUX_SPINLOCK_RT_H

#ifndef __LINUX_SPINLOCK_H
#error Do not include directly. Use spinlock.h
#endif

#include <linux/bug.h>

extern void
__rt_spin_lock_init(spinlock_t *lock, const char *name, struct lock_class_key *key);

#define spin_lock_init(slock)				\
do {							\
	static struct lock_class_key __key;		\
							\
	rt_mutex_init(&(slock)->lock);			\
	__rt_spin_lock_init(slock, #slock, &__key);	\
} while (0)

extern void __lockfunc rt_spin_lock(spinlock_t *lock);
extern unsigned long __lockfunc rt_spin_lock_trace_flags(spinlock_t *lock);
extern void __lockfunc rt_spin_lock_nested(spinlock_t *lock, int subclass);
extern void __lockfunc rt_spin_unlock(spinlock_t *lock);
extern void __lockfunc rt_spin_unlock_wait(spinlock_t *lock);
extern int __lockfunc rt_spin_trylock_irqsave(spinlock_t *lock, unsigned long *flags);
extern int __lockfunc rt_spin_trylock_bh(spinlock_t *lock);
extern int __lockfunc rt_spin_trylock(spinlock_t *lock);
extern int atomic_dec_and_spin_lock(atomic_t *atomic, spinlock_t *lock);

/*
 * lockdep-less calls, for derived types like rwlock:
 * (for trylock they can use rt_mutex_trylock() directly.
 * Migrate disable handling must be done at the call site.
 */
extern void __lockfunc __rt_spin_lock(struct rt_mutex *lock);
extern void __lockfunc __rt_spin_trylock(struct rt_mutex *lock);
extern void __lockfunc __rt_spin_unlock(struct rt_mutex *lock);

#define spin_lock(lock)			rt_spin_lock(lock)

#define spin_lock_bh(lock)			\
	do {					\
		local_bh_disable();		\
		rt_spin_lock(lock);		\
	} while (0)

#define spin_lock_irq(lock)		spin_lock(lock)

#define spin_do_trylock(lock)		__cond_lock(lock, rt_spin_trylock(lock))

#define spin_trylock(lock)			\
({						\
	int __locked;				\
	__locked = spin_do_trylock(lock);	\
	__locked;				\
})

#ifdef CONFIG_LOCKDEP
# define spin_lock_nested(lock, subclass)		\
	do {						\
		rt_spin_lock_nested(lock, subclass);	\
	} while (0)

#define spin_lock_bh_nested(lock, subclass)		\
	do {						\
		local_bh_disable();			\
		rt_spin_lock_nested(lock, subclass);	\
	} while (0)

# define spin_lock_irqsave_nested(lock, flags, subclass) \
	do {						 \
		typecheck(unsigned long, flags);	 \
		flags = 0;				 \
		rt_spin_lock_nested(lock, subclass);	 \
	} while (0)
#else
# define spin_lock_nested(lock, subclass)	spin_lock(lock)
# define spin_lock_bh_nested(lock, subclass)	spin_lock_bh(lock)

# define spin_lock_irqsave_nested(lock, flags, subclass) \
	do {						 \
		typecheck(unsigned long, flags);	 \
		flags = 0;				 \
		spin_lock(lock);			 \
	} while (0)
#endif

#define spin_lock_irqsave(lock, flags)			 \
	do {						 \
		typecheck(unsigned long, flags);	 \
		flags = 0;				 \
		spin_lock(lock);			 \
	} while (0)

static inline unsigned long spin_lock_trace_flags(spinlock_t *lock)
{
	unsigned long flags = 0;
#ifdef CONFIG_TRACE_IRQFLAGS
	flags = rt_spin_lock_trace_flags(lock);
#else
	spin_lock(lock); /* lock_local */
#endif
	return flags;
}

/* FIXME: we need rt_spin_lock_nest_lock */
#define spin_lock_nest_lock(lock, nest_lock) spin_lock_nested(lock, 0)

#define spin_unlock(lock)			rt_spin_unlock(lock)

#define spin_unlock_bh(lock)				\
	do {						\
		rt_spin_unlock(lock);			\
		local_bh_enable();			\
	} while (0)

#define spin_unlock_irq(lock)		spin_unlock(lock)

#define spin_unlock_irqrestore(lock, flags)		\
	do {						\
		typecheck(unsigned long, flags);	\
		(void) flags;				\
		spin_unlock(lock);			\
	} while (0)

#define spin_trylock_bh(lock)	__cond_lock(lock, rt_spin_trylock_bh(lock))
#define spin_trylock_irq(lock)	spin_trylock(lock)

#define spin_trylock_irqsave(lock, flags)	\
	rt_spin_trylock_irqsave(lock, &(flags))

#define spin_unlock_wait(lock)		rt_spin_unlock_wait(lock)

#ifdef CONFIG_GENERIC_LOCKBREAK
# define spin_is_contended(lock)	((lock)->break_lock)
#else
# define spin_is_contended(lock)	(((void)(lock), 0))
#endif

static inline int spin_can_lock(spinlock_t *lock)
{
	return !rt_mutex_is_locked(&lock->lock);
}

static inline int spin_is_locked(spinlock_t *lock)
{
	return rt_mutex_is_locked(&lock->lock);
}

static inline void assert_spin_locked(spinlock_t *lock)
{
	BUG_ON(!spin_is_locked(lock));
}

#define atomic_dec_and_lock(atomic, lock) \
	atomic_dec_and_spin_lock(atomic, lock)

#endif
