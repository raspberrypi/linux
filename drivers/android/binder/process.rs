// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! This module defines the `Process` type, which represents a process using a particular binder
//! context.
//!
//! The `Process` object keeps track of all of the resources that this process owns in the binder
//! context.
//!
//! There is one `Process` object for each binder fd that a process has opened, so processes using
//! several binder contexts have several `Process` objects. This ensures that the contexts are
//! fully separated.

use kernel::{
    bindings,
    cred::Credential,
    file::{self, File},
    list::{HasListLinks, ListArc, ListArcSafe, ListItem, ListLinks},
    mm,
    prelude::*,
    rbtree::{self, RBTree},
    sync::poll::PollTable,
    sync::{Arc, ArcBorrow, SpinLock},
    task::Task,
    types::ARef,
    uaccess::{UserSlice, UserSliceReader},
    workqueue::{self, Work},
};

use crate::{context::Context, defs::*, thread::Thread};

use core::mem::take;

const PROC_DEFER_FLUSH: u8 = 1;
const PROC_DEFER_RELEASE: u8 = 2;

/// The fields of `Process` protected by the spinlock.
pub(crate) struct ProcessInner {
    is_dead: bool,
    threads: RBTree<i32, Arc<Thread>>,

    /// The number of requested threads that haven't registered yet.
    requested_thread_count: u32,
    /// The maximum number of threads used by the process thread pool.
    max_threads: u32,
    /// The number of threads the started and registered with the thread pool.
    started_thread_count: u32,

    /// Bitmap of deferred work to do.
    defer_work: u8,
}

impl ProcessInner {
    fn new() -> Self {
        Self {
            is_dead: false,
            threads: RBTree::new(),
            requested_thread_count: 0,
            max_threads: 0,
            started_thread_count: 0,
            defer_work: 0,
        }
    }

    fn register_thread(&mut self) -> bool {
        if self.requested_thread_count == 0 {
            return false;
        }

        self.requested_thread_count -= 1;
        self.started_thread_count += 1;
        true
    }
}

/// A process using binder.
///
/// Strictly speaking, there can be multiple of these per process. There is one for each binder fd
/// that a process has opened, so processes using several binder contexts have several `Process`
/// objects. This ensures that the contexts are fully separated.
#[pin_data]
pub(crate) struct Process {
    pub(crate) ctx: Arc<Context>,

    // The task leader (process).
    pub(crate) task: ARef<Task>,

    // Credential associated with file when `Process` is created.
    pub(crate) cred: ARef<Credential>,

    #[pin]
    pub(crate) inner: SpinLock<ProcessInner>,

    // Work node for deferred work item.
    #[pin]
    defer_work: Work<Process>,

    // Links for process list in Context.
    #[pin]
    links: ListLinks,
}

kernel::impl_has_work! {
    impl HasWork<Process> for Process { self.defer_work }
}

kernel::list::impl_has_list_links! {
    impl HasListLinks<0> for Process { self.links }
}
kernel::list::impl_list_arc_safe! {
    impl ListArcSafe<0> for Process { untracked; }
}
kernel::list::impl_list_item! {
    impl ListItem<0> for Process {
        using ListLinks;
    }
}

impl workqueue::WorkItem for Process {
    type Pointer = Arc<Process>;

    fn run(me: Arc<Self>) {
        let defer;
        {
            let mut inner = me.inner.lock();
            defer = inner.defer_work;
            inner.defer_work = 0;
        }

        if defer & PROC_DEFER_FLUSH != 0 {
            me.deferred_flush();
        }
        if defer & PROC_DEFER_RELEASE != 0 {
            me.deferred_release();
        }
    }
}

impl Process {
    fn new(ctx: Arc<Context>, cred: ARef<Credential>) -> Result<Arc<Self>> {
        let list_process = ListArc::pin_init(pin_init!(Process {
            ctx,
            cred,
            inner <- kernel::new_spinlock!(ProcessInner::new(), "Process::inner"),
            task: kernel::current!().group_leader().into(),
            defer_work <- kernel::new_work!("Process::defer_work"),
            links <- ListLinks::new(),
        }))?;

        let process = list_process.clone_arc();
        process.ctx.register_process(list_process);

        Ok(process)
    }

    fn get_current_thread(self: ArcBorrow<'_, Self>) -> Result<Arc<Thread>> {
        let id = {
            let current = kernel::current!();
            if !core::ptr::eq(current.group_leader(), &*self.task) {
                pr_err!("get_current_thread was called from the wrong process.");
                return Err(EINVAL);
            }
            current.pid()
        };

        {
            let inner = self.inner.lock();
            if let Some(thread) = inner.threads.get(&id) {
                return Ok(thread.clone());
            }
        }

        // Allocate a new `Thread` without holding any locks.
        let reservation = RBTree::try_reserve_node()?;
        let ta: Arc<Thread> = Thread::new(id, self.into())?;

        let mut inner = self.inner.lock();
        match inner.threads.entry(id) {
            rbtree::Entry::Vacant(entry) => {
                entry.insert(ta.clone(), reservation);
                Ok(ta)
            }
            rbtree::Entry::Occupied(_entry) => {
                pr_err!("Cannot create two threads with the same id.");
                Err(EINVAL)
            }
        }
    }

    fn version(&self, data: UserSlice) -> Result {
        data.writer().write(&BinderVersion::current())
    }

    pub(crate) fn register_thread(&self) -> bool {
        self.inner.lock().register_thread()
    }

    fn remove_thread(&self, thread: Arc<Thread>) {
        self.inner.lock().threads.remove(&thread.id);
        thread.release();
    }

    fn set_max_threads(&self, max: u32) {
        self.inner.lock().max_threads = max;
    }

    pub(crate) fn needs_thread(&self) -> bool {
        let mut inner = self.inner.lock();
        let ret =
            inner.requested_thread_count == 0 && inner.started_thread_count < inner.max_threads;
        if ret {
            inner.requested_thread_count += 1
        }
        ret
    }

    fn deferred_flush(&self) {
        // NOOP for now.
    }

    fn deferred_release(self: Arc<Self>) {
        self.inner.lock().is_dead = true;

        self.ctx.deregister_process(&self);

        // Move the threads out of `inner` so that we can iterate over them without holding the
        // lock.
        let mut inner = self.inner.lock();
        let threads = take(&mut inner.threads);
        drop(inner);

        // Release all threads.
        for thread in threads.values() {
            thread.release();
        }
    }

    pub(crate) fn flush(this: ArcBorrow<'_, Process>) -> Result {
        let should_schedule;
        {
            let mut inner = this.inner.lock();
            should_schedule = inner.defer_work == 0;
            inner.defer_work |= PROC_DEFER_FLUSH;
        }

        if should_schedule {
            // Ignore failures to schedule to the workqueue. Those just mean that we're already
            // scheduled for execution.
            let _ = workqueue::system().enqueue(Arc::from(this));
        }
        Ok(())
    }
}

/// The ioctl handler.
impl Process {
    /// Ioctls that are write-only from the perspective of userspace.
    ///
    /// The kernel will only read from the pointer that userspace provided to us.
    fn ioctl_write_only(
        this: ArcBorrow<'_, Process>,
        _file: &File,
        cmd: u32,
        reader: &mut UserSliceReader,
    ) -> Result<i32> {
        let thread = this.get_current_thread()?;
        match cmd {
            bindings::BINDER_SET_MAX_THREADS => this.set_max_threads(reader.read()?),
            bindings::BINDER_THREAD_EXIT => this.remove_thread(thread),
            _ => return Err(EINVAL),
        }
        Ok(0)
    }

    /// Ioctls that are read/write from the perspective of userspace.
    ///
    /// The kernel will both read from and write to the pointer that userspace provided to us.
    fn ioctl_write_read(
        this: ArcBorrow<'_, Process>,
        file: &File,
        cmd: u32,
        data: UserSlice,
    ) -> Result<i32> {
        let thread = this.get_current_thread()?;
        let blocking = (file.flags() & file::flags::O_NONBLOCK) == 0;
        match cmd {
            bindings::BINDER_WRITE_READ => thread.write_read(data, blocking)?,
            bindings::BINDER_VERSION => this.version(data)?,
            bindings::BINDER_GET_EXTENDED_ERROR => thread.get_extended_error(data)?,
            _ => return Err(EINVAL),
        }
        Ok(0)
    }
}

/// The file operations supported by `Process`.
impl Process {
    pub(crate) fn open(ctx: ArcBorrow<'_, Context>, file: &File) -> Result<Arc<Process>> {
        Self::new(ctx.into(), ARef::from(file.cred()))
    }

    pub(crate) fn release(this: Arc<Process>, _file: &File) {
        let should_schedule;
        {
            let mut inner = this.inner.lock();
            should_schedule = inner.defer_work == 0;
            inner.defer_work |= PROC_DEFER_RELEASE;
        }

        if should_schedule {
            // Ignore failures to schedule to the workqueue. Those just mean that we're already
            // scheduled for execution.
            let _ = workqueue::system().enqueue(this);
        }
    }

    pub(crate) fn ioctl(
        this: ArcBorrow<'_, Process>,
        file: &File,
        cmd: u32,
        arg: *mut core::ffi::c_void,
    ) -> Result<i32> {
        use kernel::ioctl::{_IOC_DIR, _IOC_SIZE};
        use kernel::uapi::{_IOC_READ, _IOC_WRITE};

        let user_slice = UserSlice::new(arg, _IOC_SIZE(cmd));

        const _IOC_READ_WRITE: u32 = _IOC_READ | _IOC_WRITE;

        match _IOC_DIR(cmd) {
            _IOC_WRITE => Self::ioctl_write_only(this, file, cmd, &mut user_slice.reader()),
            _IOC_READ_WRITE => Self::ioctl_write_read(this, file, cmd, user_slice),
            _ => Err(EINVAL),
        }
    }

    pub(crate) fn compat_ioctl(
        this: ArcBorrow<'_, Process>,
        file: &File,
        cmd: u32,
        arg: *mut core::ffi::c_void,
    ) -> Result<i32> {
        Self::ioctl(this, file, cmd, arg)
    }

    pub(crate) fn mmap(
        _this: ArcBorrow<'_, Process>,
        _file: &File,
        _vma: &mut mm::virt::Area,
    ) -> Result {
        Err(EINVAL)
    }

    pub(crate) fn poll(
        _this: ArcBorrow<'_, Process>,
        _file: &File,
        _table: &mut PollTable,
    ) -> Result<u32> {
        Err(EINVAL)
    }
}
