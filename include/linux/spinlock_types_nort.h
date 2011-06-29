#ifndef __LINUX_SPINLOCK_TYPES_NORT_H
#define __LINUX_SPINLOCK_TYPES_NORT_H

#ifndef __LINUX_SPINLOCK_TYPES_H
#error "Do not include directly. Include spinlock_types.h instead"
#endif

/*
 * The non RT version maps spinlocks to raw_spinlocks
 */
typedef struct spinlock {
	union {
		struct raw_spinlock rlock;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define LOCK_PADSIZE (offsetof(struct raw_spinlock, dep_map))
		struct {
			u8 __padding[LOCK_PADSIZE];
			struct lockdep_map dep_map;
		};
#endif
	};
} spinlock_t;

#define __SPIN_LOCK_INITIALIZER(lockname) \
	{ { .rlock = __RAW_SPIN_LOCK_INITIALIZER(lockname) } }

#define __SPIN_LOCK_UNLOCKED(lockname) \
	(spinlock_t ) __SPIN_LOCK_INITIALIZER(lockname)

#define DEFINE_SPINLOCK(x)	spinlock_t x = __SPIN_LOCK_UNLOCKED(x)

#endif
