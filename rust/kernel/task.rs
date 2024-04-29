// SPDX-License-Identifier: GPL-2.0

//! Tasks (threads and processes).
//!
//! C header: [`include/linux/sched.h`](srctree/include/linux/sched.h).

use crate::{
    bindings,
    types::{NotThreadSafe, Opaque},
};
use core::{
    cmp::{Eq, PartialEq},
    ffi::{c_int, c_long, c_uint},
    ops::Deref,
    ptr,
};

/// A sentinel value used for infinite timeouts.
pub const MAX_SCHEDULE_TIMEOUT: c_long = c_long::MAX;

/// Bitmask for tasks that are sleeping in an interruptible state.
pub const TASK_INTERRUPTIBLE: c_int = bindings::TASK_INTERRUPTIBLE as c_int;
/// Bitmask for tasks that are sleeping in an uninterruptible state.
pub const TASK_UNINTERRUPTIBLE: c_int = bindings::TASK_UNINTERRUPTIBLE as c_int;
/// Bitmask for tasks that are sleeping in a freezable state.
pub const TASK_FREEZABLE: c_int = bindings::TASK_FREEZABLE as c_int;
/// Convenience constant for waking up tasks regardless of whether they are in interruptible or
/// uninterruptible sleep.
pub const TASK_NORMAL: c_uint = bindings::TASK_NORMAL as c_uint;

/// Returns the currently running task.
#[macro_export]
macro_rules! current {
    () => {
        // SAFETY: Deref + addr-of below create a temporary `TaskRef` that cannot outlive the
        // caller.
        unsafe { &*$crate::task::Task::current() }
    };
}

/// Wraps the kernel's `struct task_struct`.
///
/// # Invariants
///
/// All instances are valid tasks created by the C portion of the kernel.
///
/// Instances of this type are always ref-counted, that is, a call to `get_task_struct` ensures
/// that the allocation remains valid at least until the matching call to `put_task_struct`.
///
/// # Examples
///
/// The following is an example of getting the PID of the current thread with zero additional cost
/// when compared to the C version:
///
/// ```
/// let pid = current!().pid();
/// ```
///
/// Getting the PID of the current process, also zero additional cost:
///
/// ```
/// let pid = current!().group_leader().pid();
/// ```
///
/// Getting the current task and storing it in some struct. The reference count is automatically
/// incremented when creating `State` and decremented when it is dropped:
///
/// ```
/// use kernel::{task::Task, types::ARef};
///
/// struct State {
///     creator: ARef<Task>,
///     index: u32,
/// }
///
/// impl State {
///     fn new() -> Self {
///         Self {
///             creator: current!().into(),
///             index: 0,
///         }
///     }
/// }
/// ```
#[repr(transparent)]
pub struct Task(pub(crate) Opaque<bindings::task_struct>);

// SAFETY: By design, the only way to access a `Task` is via the `current` function or via an
// `ARef<Task>` obtained through the `AlwaysRefCounted` impl. This means that the only situation in
// which a `Task` can be accessed mutably is when the refcount drops to zero and the destructor
// runs. It is safe for that to happen on any thread, so it is ok for this type to be `Send`.
unsafe impl Send for Task {}

// SAFETY: It's OK to access `Task` through shared references from other threads because we're
// either accessing properties that don't change (e.g., `pid`, `group_leader`) or that are properly
// synchronised by C code (e.g., `signal_pending`).
unsafe impl Sync for Task {}

/// The type of process identifiers (PIDs).
pub type Pid = bindings::pid_t;

/// The type of user identifiers (UIDs).
#[derive(Copy, Clone)]
pub struct Kuid {
    kuid: bindings::kuid_t,
}

impl Task {
    /// Returns a raw pointer to the current task.
    ///
    /// It is up to the user to use the pointer correctly.
    #[inline]
    pub fn current_raw() -> *mut bindings::task_struct {
        // SAFETY: Getting the current pointer is always safe.
        unsafe { bindings::get_current() }
    }

    /// Returns a task reference for the currently executing task/thread.
    ///
    /// The recommended way to get the current task/thread is to use the
    /// [`current`] macro because it is safe.
    ///
    /// # Safety
    ///
    /// Callers must ensure that the returned object doesn't outlive the current task/thread.
    pub unsafe fn current() -> impl Deref<Target = Task> {
        struct TaskRef<'a> {
            task: &'a Task,
            _not_send: NotThreadSafe,
        }

        impl Deref for TaskRef<'_> {
            type Target = Task;

            fn deref(&self) -> &Self::Target {
                self.task
            }
        }

        let current = Task::current_raw();
        TaskRef {
            // SAFETY: If the current thread is still running, the current task is valid. Given
            // that `TaskRef` is not `Send`, we know it cannot be transferred to another thread
            // (where it could potentially outlive the caller).
            task: unsafe { &*current.cast() },
            _not_send: NotThreadSafe,
        }
    }

    /// Returns a raw pointer to the task.
    #[inline]
    pub fn as_raw(&self) -> *mut bindings::task_struct {
        self.0.get()
    }

    /// Returns the group leader of the given task.
    pub fn group_leader(&self) -> &Task {
        // SAFETY: By the type invariant, we know that `self.0` is a valid task. Valid tasks always
        // have a valid group_leader.
        let ptr = unsafe { *ptr::addr_of!((*self.0.get()).group_leader) };

        // SAFETY: The lifetime of the returned task reference is tied to the lifetime of `self`,
        // and given that a task has a reference to its group leader, we know it must be valid for
        // the lifetime of the returned task reference.
        unsafe { &*ptr.cast() }
    }

    /// Returns the PID of the given task.
    pub fn pid(&self) -> Pid {
        // SAFETY: By the type invariant, we know that `self.0` is a valid task. Valid tasks always
        // have a valid pid.
        unsafe { *ptr::addr_of!((*self.0.get()).pid) }
    }

    /// Returns the UID of the given task.
    pub fn uid(&self) -> Kuid {
        // SAFETY: By the type invariant, we know that `self.0` is valid.
        Kuid::from_raw(unsafe { bindings::task_uid(self.0.get()) })
    }

    /// Returns the effective UID of the given task.
    pub fn euid(&self) -> Kuid {
        // SAFETY: By the type invariant, we know that `self.0` is valid.
        Kuid::from_raw(unsafe { bindings::task_euid(self.0.get()) })
    }

    /// Determines whether the given task has pending signals.
    pub fn signal_pending(&self) -> bool {
        // SAFETY: By the type invariant, we know that `self.0` is valid.
        unsafe { bindings::signal_pending(self.0.get()) != 0 }
    }

    /// Returns the given task's pid in the current pid namespace.
    pub fn pid_in_current_ns(&self) -> Pid {
        // SAFETY: We know that `self.0.get()` is valid by the type invariant, and passing a null
        // pointer as the namespace is correct for using the current namespace.
        unsafe { bindings::task_tgid_nr_ns(self.0.get(), ptr::null_mut()) }
    }

    /// Returns whether this task corresponds to a kernel thread.
    pub fn is_kthread(&self) -> bool {
        // SAFETY: By the type invariant, we know that `self.0.get()` is non-null and valid. There
        // are no further requirements to read the task's flags.
        let flags = unsafe { (*self.0.get()).flags };
        (flags & bindings::PF_KTHREAD) != 0
    }

    /// Wakes up the task.
    pub fn wake_up(&self) {
        // SAFETY: By the type invariant, we know that `self.0.get()` is non-null and valid.
        // And `wake_up_process` is safe to be called for any valid task, even if the task is
        // running.
        unsafe { bindings::wake_up_process(self.0.get()) };
    }

    /// Check if the task has the given capability without logging to the audit log.
    pub fn has_capability_noaudit(&self, capability: i32) -> bool {
        // SAFETY: By the type invariant, we know that `self.0.get()` is valid.
        unsafe { bindings::has_capability_noaudit(self.0.get(), capability) }
    }

    /// Returns the current scheduling policy.
    pub fn policy(&self) -> u32 {
        // SAFETY: The file is valid because the shared reference guarantees a nonzero refcount.
        //
        // This uses a volatile read because C code may be modifying this field in parallel using
        // non-atomic unsynchronized writes. This corresponds to how the C macro READ_ONCE is
        // implemented.
        unsafe { core::ptr::addr_of!((*self.0.get()).policy).read_volatile() }
    }

    /// Returns the current normal priority.
    pub fn normal_prio(&self) -> i32 {
        // SAFETY: The file is valid because the shared reference guarantees a nonzero refcount.
        //
        // This uses a volatile read because C code may be modifying this field in parallel using
        // non-atomic unsynchronized writes. This corresponds to how the C macro READ_ONCE is
        // implemented.
        unsafe { core::ptr::addr_of!((*self.0.get()).normal_prio).read_volatile() }
    }

    /// Get the rlimit value for RTPRIO.
    pub fn rlimit_rtprio(&self) -> i32 {
        // SAFETY: By the type invariant, we know that `self.0.get()` is valid, and RLIMIT_RTPRIO
        // is a valid limit type.
        unsafe { bindings::task_rlimit(self.0.get(), bindings::RLIMIT_RTPRIO) as i32 }
    }

    /// Get the rlimit value for NICE, converted to a nice value.
    pub fn rlimit_nice(&self) -> i32 {
        // SAFETY: By the type invariant, we know that `self.0.get()` is valid, and RLIMIT_NICE
        // is a valid limit type.
        let prio = unsafe { bindings::task_rlimit(self.0.get(), bindings::RLIMIT_NICE) as i32 };
        // Convert rlimit style value [1,40] to nice value [-20, 19].
        bindings::MAX_NICE as i32 - prio + 1
    }

    /// Set the scheduling properties for this task without checking whether the task is allowed to
    /// set them.
    pub fn sched_setscheduler_nocheck(
        &self,
        policy: i32,
        sched_priority: i32,
        reset_on_fork: bool,
    ) {
        let params = bindings::sched_param { sched_priority };

        let mut policy = policy;
        if reset_on_fork {
            policy |= bindings::SCHED_RESET_ON_FORK as i32;
        }
        unsafe { bindings::sched_setscheduler_nocheck(self.0.get(), policy, &params) };
    }

    /// Set the nice value of this task.
    pub fn set_user_nice(&self, nice: i32) {
        unsafe { bindings::set_user_nice(self.0.get(), nice as _) };
    }
}

impl Kuid {
    /// Get the current euid.
    #[inline]
    pub fn current_euid() -> Kuid {
        // SAFETY: Just an FFI call.
        Self::from_raw(unsafe { bindings::current_euid() })
    }

    /// Create a `Kuid` given the raw C type.
    #[inline]
    pub fn from_raw(kuid: bindings::kuid_t) -> Self {
        Self { kuid }
    }

    /// Turn this kuid into the raw C type.
    #[inline]
    pub fn into_raw(self) -> bindings::kuid_t {
        self.kuid
    }

    /// Converts this kernel UID into a userspace UID.
    ///
    /// Uses the namespace of the current task.
    #[inline]
    pub fn into_uid_in_current_ns(self) -> bindings::uid_t {
        // SAFETY: Just an FFI call.
        unsafe { bindings::from_kuid(bindings::current_user_ns(), self.kuid) }
    }
}

impl PartialEq for Kuid {
    #[inline]
    fn eq(&self, other: &Kuid) -> bool {
        // SAFETY: Just an FFI call.
        unsafe { bindings::uid_eq(self.kuid, other.kuid) }
    }
}

impl Eq for Kuid {}

// SAFETY: The type invariants guarantee that `Task` is always ref-counted.
unsafe impl crate::types::AlwaysRefCounted for Task {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        unsafe { bindings::get_task_struct(self.0.get()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        unsafe { bindings::put_task_struct(obj.cast().as_ptr()) }
    }
}
