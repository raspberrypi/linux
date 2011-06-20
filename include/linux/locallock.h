#ifndef _LINUX_LOCALLOCK_H
#define _LINUX_LOCALLOCK_H

#include <linux/percpu.h>
#include <linux/spinlock.h>

#ifdef CONFIG_PREEMPT_RT_BASE

#ifdef CONFIG_DEBUG_SPINLOCK
# define LL_WARN(cond)	WARN_ON(cond)
#else
# define LL_WARN(cond)	do { } while (0)
#endif

/*
 * per cpu lock based substitute for local_irq_*()
 */
struct local_irq_lock {
	spinlock_t		lock;
	struct task_struct	*owner;
	int			nestcnt;
	unsigned long		flags;
};

#define DEFINE_LOCAL_IRQ_LOCK(lvar)					\
	DEFINE_PER_CPU(struct local_irq_lock, lvar) = {			\
		.lock = __SPIN_LOCK_UNLOCKED((lvar).lock) }

#define DECLARE_LOCAL_IRQ_LOCK(lvar)					\
	DECLARE_PER_CPU(struct local_irq_lock, lvar)

#define local_irq_lock_init(lvar)					\
	do {								\
		int __cpu;						\
		for_each_possible_cpu(__cpu)				\
			spin_lock_init(&per_cpu(lvar, __cpu).lock);	\
	} while (0)

static inline void __local_lock(struct local_irq_lock *lv)
{
	if (lv->owner != current) {
		spin_lock(&lv->lock);
		LL_WARN(lv->owner);
		LL_WARN(lv->nestcnt);
		lv->owner = current;
	}
	lv->nestcnt++;
}

#define local_lock(lvar)					\
	do { __local_lock(&get_local_var(lvar)); } while (0)

#define local_lock_on(lvar, cpu)				\
	do { __local_lock(&per_cpu(lvar, cpu)); } while (0)

static inline int __local_trylock(struct local_irq_lock *lv)
{
	if (lv->owner != current && spin_trylock(&lv->lock)) {
		LL_WARN(lv->owner);
		LL_WARN(lv->nestcnt);
		lv->owner = current;
		lv->nestcnt = 1;
		return 1;
	} else if (lv->owner == current) {
		lv->nestcnt++;
		return 1;
	}
	return 0;
}

#define local_trylock(lvar)						\
	({								\
		int __locked;						\
		__locked = __local_trylock(&get_local_var(lvar));	\
		if (!__locked)						\
			put_local_var(lvar);				\
		__locked;						\
	})

static inline void __local_unlock(struct local_irq_lock *lv)
{
	LL_WARN(lv->nestcnt == 0);
	LL_WARN(lv->owner != current);
	if (--lv->nestcnt)
		return;

	lv->owner = NULL;
	spin_unlock(&lv->lock);
}

#define local_unlock(lvar)					\
	do {							\
		__local_unlock(this_cpu_ptr(&lvar));		\
		put_local_var(lvar);				\
	} while (0)

#define local_unlock_on(lvar, cpu)                       \
	do { __local_unlock(&per_cpu(lvar, cpu)); } while (0)

static inline void __local_lock_irq(struct local_irq_lock *lv)
{
	spin_lock_irqsave(&lv->lock, lv->flags);
	LL_WARN(lv->owner);
	LL_WARN(lv->nestcnt);
	lv->owner = current;
	lv->nestcnt = 1;
}

#define local_lock_irq(lvar)						\
	do { __local_lock_irq(&get_local_var(lvar)); } while (0)

#define local_lock_irq_on(lvar, cpu)					\
	do { __local_lock_irq(&per_cpu(lvar, cpu)); } while (0)

static inline void __local_unlock_irq(struct local_irq_lock *lv)
{
	LL_WARN(!lv->nestcnt);
	LL_WARN(lv->owner != current);
	lv->owner = NULL;
	lv->nestcnt = 0;
	spin_unlock_irq(&lv->lock);
}

#define local_unlock_irq(lvar)						\
	do {								\
		__local_unlock_irq(this_cpu_ptr(&lvar));		\
		put_local_var(lvar);					\
	} while (0)

#define local_unlock_irq_on(lvar, cpu)					\
	do {								\
		__local_unlock_irq(&per_cpu(lvar, cpu));		\
	} while (0)

static inline int __local_lock_irqsave(struct local_irq_lock *lv)
{
	if (lv->owner != current) {
		__local_lock_irq(lv);
		return 0;
	} else {
		lv->nestcnt++;
		return 1;
	}
}

#define local_lock_irqsave(lvar, _flags)				\
	do {								\
		if (__local_lock_irqsave(&get_local_var(lvar)))		\
			put_local_var(lvar);				\
		_flags = __this_cpu_read(lvar.flags);			\
	} while (0)

#define local_lock_irqsave_on(lvar, _flags, cpu)			\
	do {								\
		__local_lock_irqsave(&per_cpu(lvar, cpu));		\
		_flags = per_cpu(lvar, cpu).flags;			\
	} while (0)

static inline int __local_unlock_irqrestore(struct local_irq_lock *lv,
					    unsigned long flags)
{
	LL_WARN(!lv->nestcnt);
	LL_WARN(lv->owner != current);
	if (--lv->nestcnt)
		return 0;

	lv->owner = NULL;
	spin_unlock_irqrestore(&lv->lock, lv->flags);
	return 1;
}

#define local_unlock_irqrestore(lvar, flags)				\
	do {								\
		if (__local_unlock_irqrestore(this_cpu_ptr(&lvar), flags)) \
			put_local_var(lvar);				\
	} while (0)

#define local_unlock_irqrestore_on(lvar, flags, cpu)			\
	do {								\
		__local_unlock_irqrestore(&per_cpu(lvar, cpu), flags);	\
	} while (0)

#define local_spin_trylock_irq(lvar, lock)				\
	({								\
		int __locked;						\
		local_lock_irq(lvar);					\
		__locked = spin_trylock(lock);				\
		if (!__locked)						\
			local_unlock_irq(lvar);				\
		__locked;						\
	})

#define local_spin_lock_irq(lvar, lock)					\
	do {								\
		local_lock_irq(lvar);					\
		spin_lock(lock);					\
	} while (0)

#define local_spin_unlock_irq(lvar, lock)				\
	do {								\
		spin_unlock(lock);					\
		local_unlock_irq(lvar);					\
	} while (0)

#define local_spin_lock_irqsave(lvar, lock, flags)			\
	do {								\
		local_lock_irqsave(lvar, flags);			\
		spin_lock(lock);					\
	} while (0)

#define local_spin_unlock_irqrestore(lvar, lock, flags)			\
	do {								\
		spin_unlock(lock);					\
		local_unlock_irqrestore(lvar, flags);			\
	} while (0)

#define get_locked_var(lvar, var)					\
	(*({								\
		local_lock(lvar);					\
		this_cpu_ptr(&var);					\
	}))

#define put_locked_var(lvar, var)	local_unlock(lvar);

#define local_lock_cpu(lvar)						\
	({								\
		local_lock(lvar);					\
		smp_processor_id();					\
	})

#define local_unlock_cpu(lvar)			local_unlock(lvar)

#else /* PREEMPT_RT_BASE */

#define DEFINE_LOCAL_IRQ_LOCK(lvar)		__typeof__(const int) lvar
#define DECLARE_LOCAL_IRQ_LOCK(lvar)		extern __typeof__(const int) lvar

static inline void local_irq_lock_init(int lvar) { }

#define local_trylock(lvar)					\
	({							\
		preempt_disable();				\
		1;						\
	})

#define local_lock(lvar)			preempt_disable()
#define local_unlock(lvar)			preempt_enable()
#define local_lock_irq(lvar)			local_irq_disable()
#define local_lock_irq_on(lvar, cpu)		local_irq_disable()
#define local_unlock_irq(lvar)			local_irq_enable()
#define local_unlock_irq_on(lvar, cpu)		local_irq_enable()
#define local_lock_irqsave(lvar, flags)		local_irq_save(flags)
#define local_unlock_irqrestore(lvar, flags)	local_irq_restore(flags)

#define local_spin_trylock_irq(lvar, lock)	spin_trylock_irq(lock)
#define local_spin_lock_irq(lvar, lock)		spin_lock_irq(lock)
#define local_spin_unlock_irq(lvar, lock)	spin_unlock_irq(lock)
#define local_spin_lock_irqsave(lvar, lock, flags)	\
	spin_lock_irqsave(lock, flags)
#define local_spin_unlock_irqrestore(lvar, lock, flags)	\
	spin_unlock_irqrestore(lock, flags)

#define get_locked_var(lvar, var)		get_cpu_var(var)
#define put_locked_var(lvar, var)		put_cpu_var(var)

#define local_lock_cpu(lvar)			get_cpu()
#define local_unlock_cpu(lvar)			put_cpu()

#endif

#endif
