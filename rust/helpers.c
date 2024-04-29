// SPDX-License-Identifier: GPL-2.0
/*
 * Non-trivial C macros cannot be used in Rust. Similarly, inlined C functions
 * cannot be called either. This file explicitly creates functions ("helpers")
 * that wrap those so that they can be called from Rust.
 *
 * Even though Rust kernel modules should never use directly the bindings, some
 * of these helpers need to be exported because Rust generics and inlined
 * functions may not get their code generated in the crate where they are
 * defined. Other helpers, called from non-inline functions, may not be
 * exported, in principle. However, in general, the Rust compiler does not
 * guarantee codegen will be performed for a non-inline function either.
 * Therefore, this file exports all the helpers. In the future, this may be
 * revisited to reduce the number of exports after the compiler is informed
 * about the places codegen is required.
 *
 * All symbols are exported as GPL-only to guarantee no GPL-only feature is
 * accidentally exposed.
 *
 * Sorted alphabetically.
 */

#include <kunit/test-bug.h>
#include <linux/bug.h>
#include <linux/build_bug.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/errname.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/list_lru.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/sched/signal.h>
#include <linux/security.h>
#include <linux/spinlock.h>
#include <linux/task_work.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

unsigned int rust_shrink_free_page(struct list_head *item,
				   struct list_lru_one *list, spinlock_t *lock,
				   void *cb_arg);

enum lru_status
rust_helper_rust_shrink_free_page_wrap(struct list_head *item,
				       struct list_lru_one *list,
				       spinlock_t *lock, void *cb_arg)
{
	return rust_shrink_free_page(item, list, lock, cb_arg);
}
EXPORT_SYMBOL_GPL(rust_helper_rust_shrink_free_page_wrap);

__noreturn void rust_helper_BUG(void)
{
	BUG();
}
EXPORT_SYMBOL_GPL(rust_helper_BUG);

unsigned long rust_helper_copy_from_user(void *to, const void __user *from,
					 unsigned long n)
{
	return copy_from_user(to, from, n);
}
EXPORT_SYMBOL_GPL(rust_helper_copy_from_user);

unsigned long rust_helper_copy_to_user(void __user *to, const void *from,
				       unsigned long n)
{
	return copy_to_user(to, from, n);
}
EXPORT_SYMBOL_GPL(rust_helper_copy_to_user);

void rust_helper_mutex_lock(struct mutex *lock)
{
	mutex_lock(lock);
}
EXPORT_SYMBOL_GPL(rust_helper_mutex_lock);

void rust_helper___spin_lock_init(spinlock_t *lock, const char *name,
				  struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_SPINLOCK
	__raw_spin_lock_init(spinlock_check(lock), name, key, LD_WAIT_CONFIG);
#else
	spin_lock_init(lock);
#endif
}
EXPORT_SYMBOL_GPL(rust_helper___spin_lock_init);

void rust_helper_spin_lock(spinlock_t *lock)
{
	spin_lock(lock);
}
EXPORT_SYMBOL_GPL(rust_helper_spin_lock);

void rust_helper_spin_unlock(spinlock_t *lock)
{
	spin_unlock(lock);
}
EXPORT_SYMBOL_GPL(rust_helper_spin_unlock);

int rust_helper_spin_trylock(spinlock_t *lock)
{
	return spin_trylock(lock);
}
EXPORT_SYMBOL_GPL(rust_helper_spin_trylock);

void rust_helper_init_wait(struct wait_queue_entry *wq_entry)
{
	init_wait(wq_entry);
}
EXPORT_SYMBOL_GPL(rust_helper_init_wait);

int rust_helper_signal_pending(struct task_struct *t)
{
	return signal_pending(t);
}
EXPORT_SYMBOL_GPL(rust_helper_signal_pending);

struct page *rust_helper_alloc_pages(gfp_t gfp_mask, unsigned int order)
{
       return alloc_pages(gfp_mask, order);
}
EXPORT_SYMBOL_GPL(rust_helper_alloc_pages);

void *rust_helper_kmap_local_page(struct page *page)
{
       return kmap_local_page(page);
}
EXPORT_SYMBOL_GPL(rust_helper_kmap_local_page);

void rust_helper_kunmap_local(const void *addr)
{
       kunmap_local(addr);
}
EXPORT_SYMBOL_GPL(rust_helper_kunmap_local);

__force void *rust_helper_ERR_PTR(long err)
{
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(rust_helper_ERR_PTR);

bool rust_helper_IS_ERR(__force const void *ptr)
{
	return IS_ERR(ptr);
}
EXPORT_SYMBOL_GPL(rust_helper_IS_ERR);

long rust_helper_PTR_ERR(__force const void *ptr)
{
	return PTR_ERR(ptr);
}
EXPORT_SYMBOL_GPL(rust_helper_PTR_ERR);

const char *rust_helper_errname(int err)
{
	return errname(err);
}
EXPORT_SYMBOL_GPL(rust_helper_errname);

struct task_struct *rust_helper_get_current(void)
{
	return current;
}
EXPORT_SYMBOL_GPL(rust_helper_get_current);

void rust_helper_get_task_struct(struct task_struct *t)
{
	get_task_struct(t);
}
EXPORT_SYMBOL_GPL(rust_helper_get_task_struct);

void rust_helper_put_task_struct(struct task_struct *t)
{
	put_task_struct(t);
}
EXPORT_SYMBOL_GPL(rust_helper_put_task_struct);

kuid_t rust_helper_task_uid(struct task_struct *task)
{
	return task_uid(task);
}
EXPORT_SYMBOL_GPL(rust_helper_task_uid);

kuid_t rust_helper_task_euid(struct task_struct *task)
{
	return task_euid(task);
}
EXPORT_SYMBOL_GPL(rust_helper_task_euid);

#ifndef CONFIG_USER_NS
uid_t rust_helper_from_kuid(struct user_namespace *to, kuid_t uid)
{
	return from_kuid(to, uid);
}
EXPORT_SYMBOL_GPL(rust_helper_from_kuid);
#endif /* CONFIG_USER_NS */

bool rust_helper_uid_eq(kuid_t left, kuid_t right)
{
	return uid_eq(left, right);
}
EXPORT_SYMBOL_GPL(rust_helper_uid_eq);

kuid_t rust_helper_current_euid(void)
{
	return current_euid();
}
EXPORT_SYMBOL_GPL(rust_helper_current_euid);

struct user_namespace *rust_helper_current_user_ns(void)
{
	return current_user_ns();
}
EXPORT_SYMBOL_GPL(rust_helper_current_user_ns);

pid_t rust_helper_task_tgid_nr_ns(struct task_struct *tsk,
				  struct pid_namespace *ns)
{
	return task_tgid_nr_ns(tsk, ns);
}
EXPORT_SYMBOL_GPL(rust_helper_task_tgid_nr_ns);

struct kunit *rust_helper_kunit_get_current_test(void)
{
	return kunit_get_current_test();
}
EXPORT_SYMBOL_GPL(rust_helper_kunit_get_current_test);

void rust_helper_init_work_with_key(struct work_struct *work, work_func_t func,
				    bool onstack, const char *name,
				    struct lock_class_key *key)
{
	__init_work(work, onstack);
	work->data = (atomic_long_t)WORK_DATA_INIT();
	lockdep_init_map(&work->lockdep_map, name, key, 0);
	INIT_LIST_HEAD(&work->entry);
	work->func = func;
}
EXPORT_SYMBOL_GPL(rust_helper_init_work_with_key);

struct file *rust_helper_get_file(struct file *f)
{
	return get_file(f);
}
EXPORT_SYMBOL_GPL(rust_helper_get_file);

const struct cred *rust_helper_get_cred(const struct cred *cred)
{
	return get_cred(cred);
}
EXPORT_SYMBOL_GPL(rust_helper_get_cred);

void rust_helper_put_cred(const struct cred *cred)
{
	put_cred(cred);
}
EXPORT_SYMBOL_GPL(rust_helper_put_cred);

#ifndef CONFIG_SECURITY
void rust_helper_security_cred_getsecid(const struct cred *c, u32 *secid)
{
	security_cred_getsecid(c, secid);
}
EXPORT_SYMBOL_GPL(rust_helper_security_cred_getsecid);

int rust_helper_security_secid_to_secctx(u32 secid, char **secdata, u32 *seclen)
{
	return security_secid_to_secctx(secid, secdata, seclen);
}
EXPORT_SYMBOL_GPL(rust_helper_security_secid_to_secctx);

void rust_helper_security_release_secctx(char *secdata, u32 seclen)
{
	security_release_secctx(secdata, seclen);
}
EXPORT_SYMBOL_GPL(rust_helper_security_release_secctx);

int rust_helper_security_binder_set_context_mgr(const struct cred *mgr)
{
	return security_binder_set_context_mgr(mgr);
}
EXPORT_SYMBOL_GPL(rust_helper_security_binder_set_context_mgr);

int rust_helper_security_binder_transaction(const struct cred *from,
					    const struct cred *to)
{
	return security_binder_transaction(from, to);
}
EXPORT_SYMBOL_GPL(rust_helper_security_binder_transaction);

int rust_helper_security_binder_transfer_binder(const struct cred *from,
						const struct cred *to)
{
	return security_binder_transfer_binder(from, to);
}
EXPORT_SYMBOL_GPL(rust_helper_security_binder_transfer_binder);

int rust_helper_security_binder_transfer_file(const struct cred *from,
					      const struct cred *to,
					      struct file *file)
{
	return security_binder_transfer_file(from, to, file);
}
EXPORT_SYMBOL_GPL(rust_helper_security_binder_transfer_file);
#endif

void rust_helper_init_task_work(struct callback_head *twork,
				task_work_func_t func)
{
	init_task_work(twork, func);
}
EXPORT_SYMBOL_GPL(rust_helper_init_task_work);

unsigned long rust_helper_task_rlimit(const struct task_struct *task,
				      unsigned int limit)
{
	return task_rlimit(task, limit);
}
EXPORT_SYMBOL_GPL(rust_helper_task_rlimit);

void rust_helper_rb_link_node(struct rb_node *node, struct rb_node *parent,
			      struct rb_node **rb_link)
{
	rb_link_node(node, parent, rb_link);
}
EXPORT_SYMBOL_GPL(rust_helper_rb_link_node);

void rust_helper_mmgrab(struct mm_struct *mm)
{
	mmgrab(mm);
}
EXPORT_SYMBOL_GPL(rust_helper_mmgrab);

void rust_helper_mmdrop(struct mm_struct *mm)
{
	mmdrop(mm);
}
EXPORT_SYMBOL_GPL(rust_helper_mmdrop);

bool rust_helper_mmget_not_zero(struct mm_struct *mm)
{
	return mmget_not_zero(mm);
}
EXPORT_SYMBOL_GPL(rust_helper_mmget_not_zero);

bool rust_helper_mmap_read_trylock(struct mm_struct *mm)
{
	return mmap_read_trylock(mm);
}
EXPORT_SYMBOL_GPL(rust_helper_mmap_read_trylock);

void rust_helper_mmap_read_unlock(struct mm_struct *mm)
{
	mmap_read_unlock(mm);
}
EXPORT_SYMBOL_GPL(rust_helper_mmap_read_unlock);

void rust_helper_mmap_write_lock(struct mm_struct *mm)
{
	mmap_write_lock(mm);
}
EXPORT_SYMBOL_GPL(rust_helper_mmap_write_lock);

void rust_helper_mmap_write_unlock(struct mm_struct *mm)
{
	mmap_write_unlock(mm);
}
EXPORT_SYMBOL_GPL(rust_helper_mmap_write_unlock);

struct vm_area_struct *rust_helper_vma_lookup(struct mm_struct *mm,
					      unsigned long addr)
{
	return vma_lookup(mm, addr);
}
EXPORT_SYMBOL_GPL(rust_helper_vma_lookup);

unsigned long rust_helper_list_lru_count(struct list_lru *lru)
{
	return list_lru_count(lru);
}
EXPORT_SYMBOL_GPL(rust_helper_list_lru_count);

unsigned long rust_helper_list_lru_walk(struct list_lru *lru,
					list_lru_walk_cb isolate, void *cb_arg,
					unsigned long nr_to_walk)
{
	return list_lru_walk(lru, isolate, cb_arg, nr_to_walk);
}
EXPORT_SYMBOL_GPL(rust_helper_list_lru_walk);

/*
 * `bindgen` binds the C `size_t` type as the Rust `usize` type, so we can
 * use it in contexts where Rust expects a `usize` like slice (array) indices.
 * `usize` is defined to be the same as C's `uintptr_t` type (can hold any
 * pointer) but not necessarily the same as `size_t` (can hold the size of any
 * single object). Most modern platforms use the same concrete integer type for
 * both of them, but in case we find ourselves on a platform where
 * that's not true, fail early instead of risking ABI or
 * integer-overflow issues.
 *
 * If your platform fails this assertion, it means that you are in
 * danger of integer-overflow bugs (even if you attempt to add
 * `--no-size_t-is-usize`). It may be easiest to change the kernel ABI on
 * your platform such that `size_t` matches `uintptr_t` (i.e., to increase
 * `size_t`, because `uintptr_t` has to be at least as big as `size_t`).
 */
static_assert(
	sizeof(size_t) == sizeof(uintptr_t) &&
	__alignof__(size_t) == __alignof__(uintptr_t),
	"Rust code expects C `size_t` to match Rust `usize`"
);
