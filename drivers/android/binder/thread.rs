// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! This module defines the `Thread` type, which represents a userspace thread that is using
//! binder.
//!
//! The `Process` object stores all of the threads in an rb tree.

use kernel::{
    bindings,
    prelude::*,
    sync::{Arc, SpinLock},
    uaccess::UserSlice,
};

use crate::{defs::*, process::Process};

use core::mem::size_of;

/// The fields of `Thread` protected by the spinlock.
struct InnerThread {
    /// Determines the looper state of the thread. It is a bit-wise combination of the constants
    /// prefixed with `LOOPER_`.
    looper_flags: u32,

    /// Determines if thread is dead.
    is_dead: bool,

    /// Extended error information for this thread.
    extended_error: ExtendedError,
}

const LOOPER_REGISTERED: u32 = 0x01;
const LOOPER_ENTERED: u32 = 0x02;
const LOOPER_EXITED: u32 = 0x04;
const LOOPER_INVALID: u32 = 0x08;

impl InnerThread {
    fn new() -> Self {
        use core::sync::atomic::{AtomicU32, Ordering};

        fn next_err_id() -> u32 {
            static EE_ID: AtomicU32 = AtomicU32::new(0);
            EE_ID.fetch_add(1, Ordering::Relaxed)
        }

        Self {
            looper_flags: 0,
            is_dead: false,
            extended_error: ExtendedError::new(next_err_id(), BR_OK, 0),
        }
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
}

/// This represents a thread that's used with binder.
#[pin_data]
pub(crate) struct Thread {
    pub(crate) id: i32,
    pub(crate) process: Arc<Process>,
    #[pin]
    inner: SpinLock<InnerThread>,
}

impl Thread {
    pub(crate) fn new(id: i32, process: Arc<Process>) -> Result<Arc<Self>> {
        Arc::pin_init(pin_init!(Thread {
            id,
            process,
            inner <- kernel::new_spinlock!(InnerThread::new(), "Thread::inner"),
        }))
    }

    pub(crate) fn get_extended_error(&self, data: UserSlice) -> Result {
        let mut writer = data.writer();
        let ee = self.inner.lock().extended_error;
        writer.write(&ee)?;
        Ok(())
    }

    fn write(self: &Arc<Self>, req: &mut BinderWriteRead) -> Result {
        let write_start = req.write_buffer.wrapping_add(req.write_consumed);
        let write_len = req.write_size - req.write_consumed;
        let mut reader = UserSlice::new(write_start as _, write_len as _).reader();

        while reader.len() >= size_of::<u32>() {
            let before = reader.len();
            let cmd = reader.read::<u32>()?;
            match cmd {
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

    fn read(self: &Arc<Self>, req: &mut BinderWriteRead, _wait: bool) -> Result {
        let read_start = req.read_buffer.wrapping_add(req.read_consumed);
        let read_len = req.read_size - req.read_consumed;
        let mut writer = UserSlice::new(read_start as _, read_len as _).writer();
        let in_pool = self.inner.lock().is_looper();

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
        #[allow(clippy::never_loop)]
        while writer.len() >= size_of::<bindings::binder_transaction_data_secctx>() + 4 {
            // There is enough space in the output buffer to process another work item.
            //
            // However, we have not yet added work items to the driver, so we immediately break
            // from the loop.
            break;
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
        ret
    }

    pub(crate) fn release(self: &Arc<Self>) {
        self.inner.lock().is_dead = true;
    }
}
