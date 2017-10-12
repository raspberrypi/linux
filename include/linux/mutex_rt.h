#ifndef __LINUX_MUTEX_RT_H
#define __LINUX_MUTEX_RT_H

#ifndef __LINUX_MUTEX_H
#error "Please include mutex.h"
#endif

#include <linux/rtmutex.h>

/* FIXME: Just for __lockfunc */
#include <linux/spinlock.h>

struct mutex {
	struct rt_mutex		lock;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};

#define __MUTEX_INITIALIZER(mutexname)					\
	{								\
		.lock = __RT_MUTEX_INITIALIZER(mutexname.lock)		\
		__DEP_MAP_MUTEX_INITIALIZER(mutexname)			\
	}

#define DEFINE_MUTEX(mutexname)						\
	struct mutex mutexname = __MUTEX_INITIALIZER(mutexname)

extern void __mutex_do_init(struct mutex *lock, const char *name, struct lock_class_key *key);
extern void __lockfunc _mutex_lock(struct mutex *lock);
extern void __lockfunc _mutex_lock_io(struct mutex *lock);
extern void __lockfunc _mutex_lock_io_nested(struct mutex *lock, int subclass);
extern int __lockfunc _mutex_lock_interruptible(struct mutex *lock);
extern int __lockfunc _mutex_lock_killable(struct mutex *lock);
extern void __lockfunc _mutex_lock_nested(struct mutex *lock, int subclass);
extern void __lockfunc _mutex_lock_nest_lock(struct mutex *lock, struct lockdep_map *nest_lock);
extern int __lockfunc _mutex_lock_interruptible_nested(struct mutex *lock, int subclass);
extern int __lockfunc _mutex_lock_killable_nested(struct mutex *lock, int subclass);
extern int __lockfunc _mutex_trylock(struct mutex *lock);
extern void __lockfunc _mutex_unlock(struct mutex *lock);

#define mutex_is_locked(l)		rt_mutex_is_locked(&(l)->lock)
#define mutex_lock(l)			_mutex_lock(l)
#define mutex_lock_interruptible(l)	_mutex_lock_interruptible(l)
#define mutex_lock_killable(l)		_mutex_lock_killable(l)
#define mutex_trylock(l)		_mutex_trylock(l)
#define mutex_unlock(l)			_mutex_unlock(l)
#define mutex_lock_io(l)		_mutex_lock_io(l);

#define __mutex_owner(l)		((l)->lock.owner)

#ifdef CONFIG_DEBUG_MUTEXES
#define mutex_destroy(l)		rt_mutex_destroy(&(l)->lock)
#else
static inline void mutex_destroy(struct mutex *lock) {}
#endif

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define mutex_lock_nested(l, s)	_mutex_lock_nested(l, s)
# define mutex_lock_interruptible_nested(l, s) \
					_mutex_lock_interruptible_nested(l, s)
# define mutex_lock_killable_nested(l, s) \
					_mutex_lock_killable_nested(l, s)
# define mutex_lock_io_nested(l, s)	_mutex_lock_io_nested(l, s)

# define mutex_lock_nest_lock(lock, nest_lock)				\
do {									\
	typecheck(struct lockdep_map *, &(nest_lock)->dep_map);		\
	_mutex_lock_nest_lock(lock, &(nest_lock)->dep_map);		\
} while (0)

#else
# define mutex_lock_nested(l, s)	_mutex_lock(l)
# define mutex_lock_interruptible_nested(l, s) \
					_mutex_lock_interruptible(l)
# define mutex_lock_killable_nested(l, s) \
					_mutex_lock_killable(l)
# define mutex_lock_nest_lock(lock, nest_lock) mutex_lock(lock)
# define mutex_lock_io_nested(l, s)	_mutex_lock_io(l)
#endif

# define mutex_init(mutex)				\
do {							\
	static struct lock_class_key __key;		\
							\
	rt_mutex_init(&(mutex)->lock);			\
	__mutex_do_init((mutex), #mutex, &__key);	\
} while (0)

# define __mutex_init(mutex, name, key)			\
do {							\
	rt_mutex_init(&(mutex)->lock);			\
	__mutex_do_init((mutex), name, key);		\
} while (0)

/**
 * These values are chosen such that FAIL and SUCCESS match the
 * values of the regular mutex_trylock().
 */
enum mutex_trylock_recursive_enum {
	MUTEX_TRYLOCK_FAILED    = 0,
	MUTEX_TRYLOCK_SUCCESS   = 1,
	MUTEX_TRYLOCK_RECURSIVE,
};
/**
 * mutex_trylock_recursive - trylock variant that allows recursive locking
 * @lock: mutex to be locked
 *
 * This function should not be used, _ever_. It is purely for hysterical GEM
 * raisins, and once those are gone this will be removed.
 *
 * Returns:
 *  MUTEX_TRYLOCK_FAILED    - trylock failed,
 *  MUTEX_TRYLOCK_SUCCESS   - lock acquired,
 *  MUTEX_TRYLOCK_RECURSIVE - we already owned the lock.
 */
int __rt_mutex_owner_current(struct rt_mutex *lock);

static inline /* __deprecated */ __must_check enum mutex_trylock_recursive_enum
mutex_trylock_recursive(struct mutex *lock)
{
	if (unlikely(__rt_mutex_owner_current(&lock->lock)))
		return MUTEX_TRYLOCK_RECURSIVE;

	return mutex_trylock(lock);
}

extern int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock);

#endif
