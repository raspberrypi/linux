#ifndef _LINUX_RWSEM_RT_H
#define _LINUX_RWSEM_RT_H

#ifndef _LINUX_RWSEM_H
#error "Include rwsem.h"
#endif

#include <linux/rtmutex.h>
#include <linux/swait.h>

#define READER_BIAS		(1U << 31)
#define WRITER_BIAS		(1U << 30)

struct rw_semaphore {
	atomic_t		readers;
	struct rt_mutex		rtmutex;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};

#define __RWSEM_INITIALIZER(name)				\
{								\
	.readers = ATOMIC_INIT(READER_BIAS),			\
	.rtmutex = __RT_MUTEX_INITIALIZER(name.rtmutex),	\
	RW_DEP_MAP_INIT(name)					\
}

#define DECLARE_RWSEM(lockname) \
	struct rw_semaphore lockname = __RWSEM_INITIALIZER(lockname)

extern void  __rwsem_init(struct rw_semaphore *rwsem, const char *name,
			  struct lock_class_key *key);

#define __init_rwsem(sem, name, key)			\
do {							\
		rt_mutex_init(&(sem)->rtmutex);		\
		__rwsem_init((sem), (name), (key));	\
} while (0)

#define init_rwsem(sem)					\
do {							\
	static struct lock_class_key __key;		\
							\
	__init_rwsem((sem), #sem, &__key);		\
} while (0)

static inline int rwsem_is_locked(struct rw_semaphore *sem)
{
	return atomic_read(&sem->readers) != READER_BIAS;
}

static inline int rwsem_is_contended(struct rw_semaphore *sem)
{
	return atomic_read(&sem->readers) > 0;
}

extern void __down_read(struct rw_semaphore *sem);
extern int __down_read_killable(struct rw_semaphore *sem);
extern int __down_read_trylock(struct rw_semaphore *sem);
extern void __down_write(struct rw_semaphore *sem);
extern int __must_check __down_write_killable(struct rw_semaphore *sem);
extern int __down_write_trylock(struct rw_semaphore *sem);
extern void __up_read(struct rw_semaphore *sem);
extern void __up_write(struct rw_semaphore *sem);
extern void __downgrade_write(struct rw_semaphore *sem);

#endif
