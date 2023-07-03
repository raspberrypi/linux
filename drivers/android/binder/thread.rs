// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! This module defines the `Thread` type, which represents a userspace thread that is using
//! binder.
//!
//! The `Process` object stores all of the threads in an rb tree.

use kernel::{
    bindings,
    list::{
        AtomicListArcTracker, HasListLinks, List, ListArc, ListArcSafe, ListItem, ListLinks,
        TryNewListArc,
    },
    prelude::*,
    security,
    sync::{Arc, CondVar, SpinLock},
    types::Either,
    uaccess::{UserSlice, UserSliceWriter},
};

use crate::{
    allocation::Allocation, defs::*, error::BinderResult, process::Process, ptr_align,
    transaction::Transaction, DArc, DLArc, DTRWrap, DeliverCode, DeliverToRead,
};

use core::{
    mem::size_of,
    sync::atomic::{AtomicU32, Ordering},
};

pub(crate) enum PushWorkRes {
    Ok,
    FailedDead(DLArc<dyn DeliverToRead>),
}

impl PushWorkRes {
    fn is_ok(&self) -> bool {
        match self {
            PushWorkRes::Ok => true,
            PushWorkRes::FailedDead(_) => false,
        }
    }
}

/// The fields of `Thread` protected by the spinlock.
struct InnerThread {
    /// Determines the looper state of the thread. It is a bit-wise combination of the constants
    /// prefixed with `LOOPER_`.
    looper_flags: u32,

    /// Determines whether the looper should return.
    looper_need_return: bool,

    /// Determines if thread is dead.
    is_dead: bool,

    /// Work item used to deliver error codes to the current thread. Stored here so that it can be
    /// reused.
    return_work: DArc<ThreadError>,

    /// Determines whether the work list below should be processed. When set to false, `work_list`
    /// is treated as if it were empty.
    process_work_list: bool,
    /// List of work items to deliver to userspace.
    work_list: List<DTRWrap<dyn DeliverToRead>>,

    /// Extended error information for this thread.
    extended_error: ExtendedError,
}

const LOOPER_REGISTERED: u32 = 0x01;
const LOOPER_ENTERED: u32 = 0x02;
const LOOPER_EXITED: u32 = 0x04;
const LOOPER_INVALID: u32 = 0x08;
const LOOPER_WAITING: u32 = 0x10;
const LOOPER_WAITING_PROC: u32 = 0x20;

impl InnerThread {
    fn new() -> Result<Self> {
        fn next_err_id() -> u32 {
            static EE_ID: AtomicU32 = AtomicU32::new(0);
            EE_ID.fetch_add(1, Ordering::Relaxed)
        }

        Ok(Self {
            looper_flags: 0,
            looper_need_return: false,
            is_dead: false,
            process_work_list: false,
            return_work: ThreadError::try_new()?,
            work_list: List::new(),
            extended_error: ExtendedError::new(next_err_id(), BR_OK, 0),
        })
    }

    fn pop_work(&mut self) -> Option<DLArc<dyn DeliverToRead>> {
        if !self.process_work_list {
            return None;
        }

        let ret = self.work_list.pop_front();
        self.process_work_list = !self.work_list.is_empty();
        ret
    }

    fn push_work(&mut self, work: DLArc<dyn DeliverToRead>) -> PushWorkRes {
        if self.is_dead {
            PushWorkRes::FailedDead(work)
        } else {
            self.work_list.push_back(work);
            self.process_work_list = true;
            PushWorkRes::Ok
        }
    }

    fn push_return_work(&mut self, reply: u32) {
        if let Ok(work) = ListArc::try_from_arc(self.return_work.clone()) {
            work.set_error_code(reply);
            self.push_work(work);
        } else {
            pr_warn!("Thread return work is already in use.");
        }
    }

    /// Used to push work items that do not need to be processed immediately and can wait until the
    /// thread gets another work item.
    fn push_work_deferred(&mut self, work: DLArc<dyn DeliverToRead>) {
        self.work_list.push_back(work);
    }

    fn looper_enter(&mut self) {
        self.looper_flags |= LOOPER_ENTERED;
        if self.looper_flags & LOOPER_REGISTERED != 0 {
            self.looper_flags |= LOOPER_INVALID;
        }
    }

    fn looper_register(&mut self, valid: bool) {
        self.looper_flags |= LOOPER_REGISTERED;
        if !valid || self.looper_flags & LOOPER_ENTERED != 0 {
            self.looper_flags |= LOOPER_INVALID;
        }
    }

    fn looper_exit(&mut self) {
        self.looper_flags |= LOOPER_EXITED;
    }

    /// Determines whether the thread is part of a pool, i.e., if it is a looper.
    fn is_looper(&self) -> bool {
        self.looper_flags & (LOOPER_ENTERED | LOOPER_REGISTERED) != 0
    }

    /// Determines whether the thread should attempt to fetch work items from the process queue.
    /// This is generally case when the thread is registered as a looper. But if there is local
    /// work, we want to return to userspace before we deliver any remote work.
    ///
    /// In future patches, it will also be required that the thread is not part of a transaction
    /// stack.
    fn should_use_process_work_queue(&self) -> bool {
        !self.process_work_list && self.is_looper()
    }
}

/// This represents a thread that's used with binder.
#[pin_data]
pub(crate) struct Thread {
    pub(crate) id: i32,
    pub(crate) process: Arc<Process>,
    #[pin]
    inner: SpinLock<InnerThread>,
    #[pin]
    work_condvar: CondVar,
    /// Used to insert this thread into the process' `ready_threads` list.
    ///
    /// INVARIANT: May never be used for any other list than the `self.process.ready_threads`.
    #[pin]
    links: ListLinks,
    #[pin]
    links_track: AtomicListArcTracker,
}

kernel::list::impl_has_list_links! {
    impl HasListLinks<0> for Thread { self.links }
}
kernel::list::impl_list_arc_safe! {
    impl ListArcSafe<0> for Thread {
        tracked_by links_track: AtomicListArcTracker;
    }
}
kernel::list::impl_list_item! {
    impl ListItem<0> for Thread {
        using ListLinks;
    }
}

impl Thread {
    pub(crate) fn new(id: i32, process: Arc<Process>) -> Result<Arc<Self>> {
        let inner = InnerThread::new()?;

        Arc::pin_init(pin_init!(Thread {
            id,
            process,
            inner <- kernel::new_spinlock!(inner, "Thread::inner"),
            work_condvar <- kernel::new_condvar!("Thread::work_condvar"),
            links <- ListLinks::new(),
            links_track <- AtomicListArcTracker::new(),
        }))
    }

    pub(crate) fn get_extended_error(&self, data: UserSlice) -> Result {
        let mut writer = data.writer();
        let ee = self.inner.lock().extended_error;
        writer.write(&ee)?;
        Ok(())
    }

    /// Attempts to fetch a work item from the thread-local queue. The behaviour if the queue is
    /// empty depends on `wait`: if it is true, the function waits for some work to be queued (or a
    /// signal); otherwise it returns indicating that none is available.
    fn get_work_local(self: &Arc<Self>, wait: bool) -> Result<Option<DLArc<dyn DeliverToRead>>> {
        {
            let mut inner = self.inner.lock();
            if inner.looper_need_return {
                return Ok(inner.pop_work());
            }
        }

        // Try once if the caller does not want to wait.
        if !wait {
            return self.inner.lock().pop_work().ok_or(EAGAIN).map(Some);
        }

        // Loop waiting only on the local queue (i.e., not registering with the process queue).
        let mut inner = self.inner.lock();
        loop {
            if let Some(work) = inner.pop_work() {
                return Ok(Some(work));
            }

            inner.looper_flags |= LOOPER_WAITING;
            let signal_pending = self.work_condvar.wait_interruptible(&mut inner);
            inner.looper_flags &= !LOOPER_WAITING;

            if signal_pending {
                return Err(EINTR);
            }
            if inner.looper_need_return {
                return Ok(None);
            }
        }
    }

    /// Attempts to fetch a work item from the thread-local queue, falling back to the process-wide
    /// queue if none is available locally.
    ///
    /// This must only be called when the thread is not participating in a transaction chain. If it
    /// is, the local version (`get_work_local`) should be used instead.
    fn get_work(self: &Arc<Self>, wait: bool) -> Result<Option<DLArc<dyn DeliverToRead>>> {
        // Try to get work from the thread's work queue, using only a local lock.
        {
            let mut inner = self.inner.lock();
            if let Some(work) = inner.pop_work() {
                return Ok(Some(work));
            }
            if inner.looper_need_return {
                drop(inner);
                return Ok(self.process.get_work());
            }
        }

        // If the caller doesn't want to wait, try to grab work from the process queue.
        //
        // We know nothing will have been queued directly to the thread queue because it is not in
        // a transaction and it is not in the process' ready list.
        if !wait {
            return self.process.get_work().ok_or(EAGAIN).map(Some);
        }

        // Get work from the process queue. If none is available, atomically register as ready.
        let reg = match self.process.get_work_or_register(self) {
            Either::Left(work) => return Ok(Some(work)),
            Either::Right(reg) => reg,
        };

        let mut inner = self.inner.lock();
        loop {
            if let Some(work) = inner.pop_work() {
                return Ok(Some(work));
            }

            inner.looper_flags |= LOOPER_WAITING | LOOPER_WAITING_PROC;
            let signal_pending = self.work_condvar.wait_interruptible(&mut inner);
            inner.looper_flags &= !(LOOPER_WAITING | LOOPER_WAITING_PROC);

            if signal_pending || inner.looper_need_return {
                // We need to return now. We need to pull the thread off the list of ready threads
                // (by dropping `reg`), then check the state again after it's off the list to
                // ensure that something was not queued in the meantime. If something has been
                // queued, we just return it (instead of the error).
                drop(inner);
                drop(reg);

                let res = match self.inner.lock().pop_work() {
                    Some(work) => Ok(Some(work)),
                    None if signal_pending => Err(EINTR),
                    None => Ok(None),
                };
                return res;
            }
        }
    }

    /// Push the provided work item to be delivered to user space via this thread.
    ///
    /// Returns whether the item was successfully pushed. This can only fail if the thread is dead.
    pub(crate) fn push_work(&self, work: DLArc<dyn DeliverToRead>) -> PushWorkRes {
        let sync = work.should_sync_wakeup();

        let res = self.inner.lock().push_work(work);

        if res.is_ok() {
            if sync {
                self.work_condvar.notify_sync();
            } else {
                self.work_condvar.notify_one();
            }
        }

        res
    }

    pub(crate) fn push_work_deferred(&self, work: DLArc<dyn DeliverToRead>) {
        self.inner.lock().push_work_deferred(work);
    }

    /// This method copies the payload of a transaction into the target process.
    ///
    /// The resulting payload will have several different components, which will be stored next to
    /// each other in the allocation. Furthermore, various objects can be embedded in the payload,
    /// and those objects have to be translated so that they make sense to the target transaction.
    pub(crate) fn copy_transaction_data(
        &self,
        to_process: Arc<Process>,
        tr: &BinderTransactionDataSg,
        txn_security_ctx_offset: Option<&mut usize>,
    ) -> BinderResult<Allocation> {
        let trd = &tr.transaction_data;
        let is_oneway = trd.flags & TF_ONE_WAY != 0;
        let mut secctx = if let Some(offset) = txn_security_ctx_offset {
            let secid = self.process.cred.get_secid();
            let ctx = match security::SecurityCtx::from_secid(secid) {
                Ok(ctx) => ctx,
                Err(err) => {
                    pr_warn!("Failed to get security ctx for id {}: {:?}", secid, err);
                    return Err(err.into());
                }
            };
            Some((offset, ctx))
        } else {
            None
        };

        let data_size = trd.data_size.try_into().map_err(|_| EINVAL)?;
        let aligned_data_size = ptr_align(data_size);
        let aligned_secctx_size = secctx
            .as_ref()
            .map(|(_, ctx)| ptr_align(ctx.len()))
            .unwrap_or(0);

        // This guarantees that at least `sizeof(usize)` bytes will be allocated.
        let len = usize::max(
            aligned_data_size
                .checked_add(aligned_secctx_size)
                .ok_or(ENOMEM)?,
            size_of::<usize>(),
        );
        let secctx_off = aligned_data_size;
        let alloc = match to_process.buffer_alloc(len, is_oneway) {
            Ok(alloc) => alloc,
            Err(err) => {
                pr_warn!(
                    "Failed to allocate buffer. len:{}, is_oneway:{}",
                    len,
                    is_oneway
                );
                return Err(err);
            }
        };

        // SAFETY: This is unsafe as a speed-bump to make TOCTOU bugs hard, but it's not actually
        // unsafe to call. UserSlice need to be fixed so that this isn't unsafe...
        let mut buffer_reader =
            unsafe { UserSlice::new(trd.data.ptr.buffer as _, data_size) }.reader();

        alloc.copy_into(&mut buffer_reader, 0, data_size)?;

        if let Some((off_out, secctx)) = secctx.as_mut() {
            if let Err(err) = alloc.write(secctx_off, secctx.as_bytes()) {
                pr_warn!("Failed to write security context: {:?}", err);
                return Err(err.into());
            }
            **off_out = secctx_off;
        }
        Ok(alloc)
    }

    fn transaction<T>(self: &Arc<Self>, tr: &BinderTransactionDataSg, inner: T)
    where
        T: FnOnce(&Arc<Self>, &BinderTransactionDataSg) -> BinderResult,
    {
        if let Err(err) = inner(self, tr) {
            if err.reply != BR_TRANSACTION_COMPLETE {
                let mut ee = self.inner.lock().extended_error;
                ee.command = err.reply;
                ee.param = err.as_errno();
                pr_warn!(
                    "Transaction failed: {:?} my_pid:{}",
                    err,
                    self.process.task.pid_in_current_ns()
                );
            }

            self.inner.lock().push_return_work(err.reply);
        }
    }

    fn oneway_transaction_inner(self: &Arc<Self>, tr: &BinderTransactionDataSg) -> BinderResult {
        // SAFETY: The `handle` field is valid for all possible byte values, so reading from the
        // union is okay.
        let handle = unsafe { tr.transaction_data.target.handle };
        let node_ref = self.process.get_transaction_node(handle)?;
        security::binder_transaction(&self.process.cred, &node_ref.node.owner.cred)?;
        let list_completion = DTRWrap::arc_try_new(DeliverCode::new(BR_TRANSACTION_COMPLETE))?;
        let transaction = Transaction::new(node_ref, self, tr)?;
        let completion = list_completion.clone_arc();
        self.inner.lock().push_work(list_completion);
        match transaction.submit() {
            Ok(()) => Ok(()),
            Err(err) => {
                completion.skip();
                Err(err)
            }
        }
    }

    fn write(self: &Arc<Self>, req: &mut BinderWriteRead) -> Result {
        let write_start = req.write_buffer.wrapping_add(req.write_consumed);
        let write_len = req.write_size - req.write_consumed;
        let mut reader = UserSlice::new(write_start as _, write_len as _).reader();

        while reader.len() >= size_of::<u32>() && self.inner.lock().return_work.is_unused() {
            let before = reader.len();
            let cmd = reader.read::<u32>()?;
            match cmd {
                BC_TRANSACTION => {
                    let tr = reader.read::<BinderTransactionData>()?.with_buffers_size(0);
                    if tr.transaction_data.flags & TF_ONE_WAY != 0 {
                        // TODO: Allow sending oneway transactions when they are serialized.
                        //self.transaction(&tr, Self::oneway_transaction_inner);
                        return Err(EINVAL);
                    } else {
                        return Err(EINVAL);
                    }
                }
                BC_TRANSACTION_SG => {
                    let tr = reader.read::<BinderTransactionDataSg>()?;
                    if tr.transaction_data.flags & TF_ONE_WAY != 0 {
                        // TODO: Allow sending oneway transactions when they are serialized.
                        //self.transaction(&tr, Self::oneway_transaction_inner);
                        return Err(EINVAL);
                    } else {
                        return Err(EINVAL);
                    }
                }
                BC_FREE_BUFFER => drop(self.process.buffer_get(reader.read()?)),
                BC_INCREFS => {
                    self.process
                        .as_arc_borrow()
                        .update_ref(reader.read()?, true, false)?
                }
                BC_ACQUIRE => {
                    self.process
                        .as_arc_borrow()
                        .update_ref(reader.read()?, true, true)?
                }
                BC_RELEASE => {
                    self.process
                        .as_arc_borrow()
                        .update_ref(reader.read()?, false, true)?
                }
                BC_DECREFS => {
                    self.process
                        .as_arc_borrow()
                        .update_ref(reader.read()?, false, false)?
                }
                BC_INCREFS_DONE => self.process.inc_ref_done(&mut reader, false)?,
                BC_ACQUIRE_DONE => self.process.inc_ref_done(&mut reader, true)?,
                BC_REGISTER_LOOPER => {
                    let valid = self.process.register_thread();
                    self.inner.lock().looper_register(valid);
                }
                BC_ENTER_LOOPER => self.inner.lock().looper_enter(),
                BC_EXIT_LOOPER => self.inner.lock().looper_exit(),

                // Fail if given an unknown error code.
                // BC_ATTEMPT_ACQUIRE and BC_ACQUIRE_RESULT are no longer supported.
                _ => return Err(EINVAL),
            }
            // Update the number of write bytes consumed.
            req.write_consumed += (before - reader.len()) as u64;
        }

        Ok(())
    }

    fn read(self: &Arc<Self>, req: &mut BinderWriteRead, wait: bool) -> Result {
        let read_start = req.read_buffer.wrapping_add(req.read_consumed);
        let read_len = req.read_size - req.read_consumed;
        let mut writer = UserSlice::new(read_start as _, read_len as _).writer();
        let (in_pool, use_proc_queue) = {
            let inner = self.inner.lock();
            (inner.is_looper(), inner.should_use_process_work_queue())
        };
        let getter = if use_proc_queue {
            Self::get_work
        } else {
            Self::get_work_local
        };

        // Reserve some room at the beginning of the read buffer so that we can send a
        // BR_SPAWN_LOOPER if we need to.
        let mut has_noop_placeholder = false;
        if req.read_consumed == 0 {
            if let Err(err) = writer.write(&BR_NOOP) {
                pr_warn!("Failure when writing BR_NOOP at beginning of buffer.");
                return Err(err);
            }
            has_noop_placeholder = true;
        }

        // Loop doing work while there is room in the buffer.
        let initial_len = writer.len();
        while writer.len() >= size_of::<bindings::binder_transaction_data_secctx>() + 4 {
            match getter(self, wait && initial_len == writer.len()) {
                Ok(Some(work)) => {
                    let work_ty = work.debug_name();
                    match work.into_arc().do_work(self, &mut writer) {
                        Ok(true) => {}
                        Ok(false) => break,
                        Err(err) => {
                            pr_warn!("Failure inside do_work of type {}.", work_ty);
                            return Err(err);
                        }
                    }
                }
                Ok(None) => {
                    break;
                }
                Err(err) => {
                    // Propagate the error if we haven't written anything else.
                    if err != EINTR && err != EAGAIN {
                        pr_warn!("Failure in work getter: {:?}", err);
                    }
                    if initial_len == writer.len() {
                        return Err(err);
                    } else {
                        break;
                    }
                }
            }
        }

        req.read_consumed += read_len - writer.len() as u64;

        // Write BR_SPAWN_LOOPER if the process needs more threads for its pool.
        if has_noop_placeholder && in_pool && self.process.needs_thread() {
            let mut writer = UserSlice::new(req.read_buffer as _, req.read_size as _).writer();
            writer.write(&BR_SPAWN_LOOPER)?;
        }
        Ok(())
    }

    pub(crate) fn write_read(self: &Arc<Self>, data: UserSlice, wait: bool) -> Result {
        let (mut reader, mut writer) = data.reader_writer();
        let mut req = reader.read::<BinderWriteRead>()?;

        // Go through the write buffer.
        if req.write_size > 0 {
            if let Err(err) = self.write(&mut req) {
                pr_warn!(
                    "Write failure {:?} in pid:{}",
                    err,
                    self.process.task.pid_in_current_ns()
                );
                req.read_consumed = 0;
                writer.write(&req)?;
                self.inner.lock().looper_need_return = false;
                return Err(err);
            }
        }

        // Go through the work queue.
        let mut ret = Ok(());
        if req.read_size > 0 {
            ret = self.read(&mut req, wait);
            if ret.is_err() && ret != Err(EINTR) {
                pr_warn!(
                    "Read failure {:?} in pid:{}",
                    ret,
                    self.process.task.pid_in_current_ns()
                );
            }
        }

        // Write the request back so that the consumed fields are visible to the caller.
        writer.write(&req)?;

        self.inner.lock().looper_need_return = false;

        ret
    }

    /// Make the call to `get_work` or `get_work_local` return immediately, if any.
    pub(crate) fn exit_looper(&self) {
        let mut inner = self.inner.lock();
        let should_notify = inner.looper_flags & LOOPER_WAITING != 0;
        if should_notify {
            inner.looper_need_return = true;
        }
        drop(inner);

        if should_notify {
            self.work_condvar.notify_one();
        }
    }

    pub(crate) fn release(self: &Arc<Self>) {
        self.inner.lock().is_dead = true;

        // Cancel all pending work items.
        while let Ok(Some(work)) = self.get_work_local(false) {
            work.into_arc().cancel();
        }
    }
}

#[pin_data]
struct ThreadError {
    error_code: AtomicU32,
    #[pin]
    links_track: AtomicListArcTracker,
}

impl ThreadError {
    fn try_new() -> Result<DArc<Self>> {
        DTRWrap::arc_pin_init(pin_init!(Self {
            error_code: AtomicU32::new(BR_OK),
            links_track <- AtomicListArcTracker::new(),
        }))
        .map(ListArc::into_arc)
    }

    fn set_error_code(&self, code: u32) {
        self.error_code.store(code, Ordering::Relaxed);
    }

    fn is_unused(&self) -> bool {
        self.error_code.load(Ordering::Relaxed) == BR_OK
    }
}

impl DeliverToRead for ThreadError {
    fn do_work(self: DArc<Self>, _thread: &Thread, writer: &mut UserSliceWriter) -> Result<bool> {
        let code = self.error_code.load(Ordering::Relaxed);
        self.error_code.store(BR_OK, Ordering::Relaxed);
        writer.write(&code)?;
        Ok(true)
    }

    fn should_sync_wakeup(&self) -> bool {
        false
    }
}

kernel::list::impl_list_arc_safe! {
    impl ListArcSafe<0> for ThreadError {
        tracked_by links_track: AtomicListArcTracker;
    }
}
