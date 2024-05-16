/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM binder
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_BINDER_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_BINDER_H
#include <trace/hooks/vendor_hooks.h>
/*
 * Following tracepoints are not exported in tracefs and provide a
 * mechanism for vendor modules to hook and extend functionality
 */
struct binder_transaction;
struct task_struct;
struct binder_work;
struct binder_buffer;

DECLARE_HOOK(android_vh_binder_transaction_init,
	TP_PROTO(struct binder_transaction *t),
	TP_ARGS(t));
DECLARE_HOOK(android_vh_binder_set_priority,
	TP_PROTO(struct binder_transaction *t, struct task_struct *task),
	TP_ARGS(t, task));
DECLARE_HOOK(android_vh_binder_restore_priority,
	TP_PROTO(struct binder_transaction *t, struct task_struct *task),
	TP_ARGS(t, task));

DECLARE_HOOK(android_vh_binder_alloc_new_buf_locked,
	TP_PROTO(size_t size, size_t *free_async_space, int is_async, bool *should_fail),
	TP_ARGS(size, free_async_space, is_async, should_fail));
struct binder_proc;
struct binder_thread;
DECLARE_HOOK(android_vh_binder_preset,
	TP_PROTO(struct hlist_head *hhead, struct mutex *lock, struct binder_proc *proc),
	TP_ARGS(hhead, lock, proc));
struct binder_transaction_data;
DECLARE_HOOK(android_vh_binder_reply,
	TP_PROTO(struct binder_proc *target_proc, struct binder_proc *proc,
		struct binder_thread *thread, struct binder_transaction_data *tr),
	TP_ARGS(target_proc, proc, thread, tr));
DECLARE_HOOK(android_vh_binder_trans,
	TP_PROTO(struct binder_proc *target_proc, struct binder_proc *proc,
		struct binder_thread *thread, struct binder_transaction_data *tr),
	TP_ARGS(target_proc, proc, thread, tr));
DECLARE_HOOK(android_vh_binder_wait_for_work,
	TP_PROTO(bool do_proc_work, struct binder_thread *tsk, struct binder_proc *proc),
	TP_ARGS(do_proc_work, tsk, proc));
DECLARE_HOOK(android_vh_sync_txn_recvd,
	TP_PROTO(struct task_struct *tsk, struct task_struct *from),
	TP_ARGS(tsk, from));
DECLARE_HOOK(android_vh_binder_proc_transaction_finish,
	TP_PROTO(struct binder_proc *proc, struct binder_transaction *t,
		struct task_struct *binder_th_task, bool pending_async, bool sync),
	TP_ARGS(proc, t, binder_th_task, pending_async, sync));
DECLARE_HOOK(android_vh_binder_select_special_worklist,
	TP_PROTO(struct list_head **list, struct binder_thread *thread, struct binder_proc *proc,
	int wait_for_proc_work, bool *nothing_to_do),
	TP_ARGS(list, thread, proc, wait_for_proc_work, nothing_to_do));
DECLARE_HOOK(android_vh_alloc_oem_binder_struct,
	TP_PROTO(struct binder_transaction_data *tr, struct binder_transaction *t,
		struct binder_proc *proc),
	TP_ARGS(tr, t, proc));
DECLARE_HOOK(android_vh_binder_transaction_received,
	TP_PROTO(struct binder_transaction *t, struct binder_proc *proc,
		struct binder_thread *thread, uint32_t cmd),
	TP_ARGS(t, proc, thread, cmd));
DECLARE_HOOK(android_vh_free_oem_binder_struct,
	TP_PROTO(struct binder_transaction *t),
	TP_ARGS(t));
DECLARE_HOOK(android_vh_binder_special_task,
	TP_PROTO(struct binder_transaction *t, struct binder_proc *proc,
		struct binder_thread *thread, struct binder_work *w,
		struct list_head *head, bool sync, bool *special_task),
	TP_ARGS(t, proc, thread, w, head, sync, special_task));
DECLARE_HOOK(android_vh_binder_buffer_release,
	TP_PROTO(struct binder_proc *proc, struct binder_thread *thread,
		struct binder_buffer *buffer, bool has_transaction),
	TP_ARGS(proc, thread, buffer, has_transaction));

DECLARE_HOOK(android_vh_binder_ioctl_end,
	TP_PROTO(struct task_struct *caller_task,
		unsigned int cmd,
		unsigned long arg,
		struct binder_thread *thread,
		struct binder_proc *proc,
		int *ret),
	TP_ARGS(caller_task, cmd, arg, thread, proc, ret));
DECLARE_HOOK(android_vh_binder_looper_exited,
	TP_PROTO(struct binder_thread *thread, struct binder_proc *proc),
	TP_ARGS(thread, proc));
DECLARE_HOOK(android_vh_binder_spawn_new_thread,
	TP_PROTO(struct binder_thread *thread, struct binder_proc *proc, bool *force_spawn),
	TP_ARGS(thread, proc, force_spawn));
DECLARE_HOOK(android_vh_binder_has_special_work_ilocked,
	TP_PROTO(struct binder_thread *thread, bool do_proc_work, bool *has_work),
	TP_ARGS(thread, do_proc_work, has_work));
DECLARE_HOOK(android_vh_binder_proc_transaction,
	TP_PROTO(struct task_struct *caller_task, struct task_struct *binder_proc_task,
		struct task_struct *binder_th_task, int node_debug_id,
		struct binder_transaction *t, bool pending_async),
	TP_ARGS(caller_task, binder_proc_task, binder_th_task, node_debug_id, t, pending_async));
DECLARE_HOOK(android_vh_binder_new_ref,
	TP_PROTO(struct task_struct *proc, uint32_t ref_desc, int node_debug_id),
	TP_ARGS(proc, ref_desc, node_debug_id));
DECLARE_HOOK(android_vh_binder_del_ref,
	TP_PROTO(struct task_struct *proc, uint32_t ref_desc),
	TP_ARGS(proc, ref_desc));
DECLARE_HOOK(android_vh_binder_list_add_work,
	TP_PROTO(struct binder_work *work, struct list_head *target_list),
	TP_ARGS(work, target_list));
DECLARE_HOOK(android_vh_binder_has_proc_work_ilocked,
	TP_PROTO(struct binder_thread *thread, bool do_proc_work, bool *has_work),
	TP_ARGS(thread, do_proc_work, has_work));
DECLARE_HOOK(android_vh_binder_release_special_work,
	TP_PROTO(struct binder_proc *proc, struct list_head **special_list),
	TP_ARGS(proc, special_list));
DECLARE_HOOK(android_vh_binder_looper_state_registered,
	TP_PROTO(struct binder_thread *thread, struct binder_proc *proc),
	TP_ARGS(thread, proc));
DECLARE_HOOK(android_vh_binder_thread_read,
	TP_PROTO(struct list_head **list, struct binder_proc *proc,
		struct binder_thread *thread),
	TP_ARGS(list, proc, thread));
DECLARE_HOOK(android_vh_binder_free_proc,
	TP_PROTO(struct binder_proc *proc),
	TP_ARGS(proc));
DECLARE_HOOK(android_vh_binder_thread_release,
	TP_PROTO(struct binder_proc *proc, struct binder_thread *thread),
	TP_ARGS(proc, thread));
DECLARE_HOOK(android_vh_binder_read_done,
	TP_PROTO(struct binder_proc *proc, struct binder_thread *thread),
	TP_ARGS(proc, thread));
#endif /* _TRACE_HOOK_BINDER_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
