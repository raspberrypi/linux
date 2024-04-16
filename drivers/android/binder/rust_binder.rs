// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Binder -- the Android IPC mechanism.
#![recursion_limit = "256"]

use kernel::{
    bindings::{self, seq_file},
    file::File,
    list::{
        HasListLinks, ListArc, ListArcSafe, ListItem, ListLinks, ListLinksSelfPtr, TryNewListArc,
    },
    page_range::Shrinker,
    prelude::*,
    seq_file::SeqFile,
    seq_print,
    sync::poll::PollTable,
    sync::Arc,
    types::ForeignOwnable,
    uaccess::UserSliceWriter,
};

use crate::{context::Context, process::Process, thread::Thread};

use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

mod allocation;
mod context;
mod defs;
mod error;
mod node;
mod prio;
mod process;
mod range_alloc;
mod thread;
mod transaction;

module! {
    type: BinderModule,
    name: "rust_binder",
    author: "Wedson Almeida Filho, Alice Ryhl",
    description: "Android Binder",
    license: "GPL",
}

fn next_debug_id() -> usize {
    static NEXT_DEBUG_ID: AtomicUsize = AtomicUsize::new(0);

    NEXT_DEBUG_ID.fetch_add(1, Ordering::Relaxed)
}

/// Specifies how a type should be delivered to the read part of a BINDER_WRITE_READ ioctl.
///
/// When a value is pushed to the todo list for a process or thread, it is stored as a trait object
/// with the type `Arc<dyn DeliverToRead>`. Trait objects are a Rust feature that lets you
/// implement dynamic dispatch over many different types. This lets us store many different types
/// in the todo list.
trait DeliverToRead: ListArcSafe + Send + Sync {
    /// Performs work. Returns true if remaining work items in the queue should be processed
    /// immediately, or false if it should return to caller before processing additional work
    /// items.
    fn do_work(self: DArc<Self>, thread: &Thread, writer: &mut UserSliceWriter) -> Result<bool>;

    /// Cancels the given work item. This is called instead of [`DeliverToRead::do_work`] when work
    /// won't be delivered.
    fn cancel(self: DArc<Self>);

    /// Called when a work item is delivered directly to a specific thread, rather than to the
    /// process work list.
    fn on_thread_selected(&self, _thread: &thread::Thread);

    /// Should we use `wake_up_interruptible_sync` or `wake_up_interruptible` when scheduling this
    /// work item?
    ///
    /// Generally only set to true for non-oneway transactions.
    fn should_sync_wakeup(&self) -> bool;

    fn debug_print(&self, m: &mut SeqFile, prefix: &str, transaction_prefix: &str) -> Result<()>;
}

// Wrapper around a `DeliverToRead` with linked list links.
#[pin_data]
struct DTRWrap<T: ?Sized> {
    #[pin]
    links: ListLinksSelfPtr<DTRWrap<dyn DeliverToRead>>,
    #[pin]
    wrapped: T,
}
kernel::list::impl_has_list_links_self_ptr! {
    impl HasSelfPtr<DTRWrap<dyn DeliverToRead>> for DTRWrap<dyn DeliverToRead> { self.links }
}
kernel::list::impl_list_arc_safe! {
    impl{T: ListArcSafe + ?Sized} ListArcSafe<0> for DTRWrap<T> {
        tracked_by wrapped: T;
    }
}
kernel::list::impl_list_item! {
    impl ListItem<0> for DTRWrap<dyn DeliverToRead> {
        using ListLinksSelfPtr;
    }
}

impl<T: ?Sized> core::ops::Deref for DTRWrap<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.wrapped
    }
}

impl<T: ?Sized> core::ops::Receiver for DTRWrap<T> {}

type DArc<T> = kernel::sync::Arc<DTRWrap<T>>;
type DLArc<T> = kernel::list::ListArc<DTRWrap<T>>;

impl<T: ListArcSafe> DTRWrap<T> {
    fn new(val: impl PinInit<T>) -> impl PinInit<Self> {
        pin_init!(Self {
            links <- ListLinksSelfPtr::new(),
            wrapped <- val,
        })
    }

    fn arc_try_new(val: T) -> Result<DLArc<T>, alloc::alloc::AllocError> {
        ListArc::pin_init(pin_init!(Self {
            links <- ListLinksSelfPtr::new(),
            wrapped: val,
        }))
        .map_err(|_| alloc::alloc::AllocError)
    }

    fn arc_pin_init(init: impl PinInit<T>) -> Result<DLArc<T>, kernel::error::Error> {
        ListArc::pin_init(pin_init!(Self {
            links <- ListLinksSelfPtr::new(),
            wrapped <- init,
        }))
    }
}

struct DeliverCode {
    code: u32,
    skip: AtomicBool,
}

kernel::list::impl_list_arc_safe! {
    impl ListArcSafe<0> for DeliverCode { untracked; }
}

impl DeliverCode {
    fn new(code: u32) -> Self {
        Self {
            code,
            skip: AtomicBool::new(false),
        }
    }

    /// Disable this DeliverCode and make it do nothing.
    ///
    /// This is used instead of removing it from the work list, since `LinkedList::remove` is
    /// unsafe, whereas this method is not.
    fn skip(&self) {
        self.skip.store(true, Ordering::Relaxed);
    }
}

impl DeliverToRead for DeliverCode {
    fn do_work(self: DArc<Self>, _thread: &Thread, writer: &mut UserSliceWriter) -> Result<bool> {
        if !self.skip.load(Ordering::Relaxed) {
            writer.write(&self.code)?;
        }
        Ok(true)
    }

    fn cancel(self: DArc<Self>) {}
    fn on_thread_selected(&self, _thread: &thread::Thread) {}

    fn should_sync_wakeup(&self) -> bool {
        false
    }

    fn debug_print(&self, m: &mut SeqFile, prefix: &str, _tprefix: &str) -> Result<()> {
        seq_print!(m, "{}", prefix);
        if self.skip.load(Ordering::Relaxed) {
            seq_print!(m, "(skipped) ");
        }
        if self.code == defs::BR_TRANSACTION_COMPLETE {
            seq_print!(m, "transaction complete\n");
        } else {
            seq_print!(m, "transaction error: {}\n", self.code);
        }
        Ok(())
    }
}

const fn ptr_align(value: usize) -> usize {
    let size = core::mem::size_of::<usize>() - 1;
    (value + size) & !size
}

// SAFETY: We call register in `init`.
static BINDER_SHRINKER: Shrinker = unsafe { Shrinker::new() };

struct BinderModule {}

impl kernel::Module for BinderModule {
    fn init(_module: &'static kernel::ThisModule) -> Result<Self> {
        // SAFETY: This is the very first thing that happens in this module, so nothing else has
        // called `Contexts::init` yet. Furthermore, we cannot move a value in a global, so the
        // `Contexts` will not be moved after this call.
        unsafe { crate::context::CONTEXTS.init() };

        // SAFETY: This just accesses global booleans.
        #[cfg(CONFIG_ANDROID_BINDER_IPC)]
        unsafe {
            extern "C" {
                static mut binder_use_rust: bool;
                static mut binder_driver_initialized: bool;
            }

            if !binder_use_rust {
                return Ok(Self {});
            }
            binder_driver_initialized = true;
        }

        BINDER_SHRINKER.register(kernel::c_str!("android-binder"))?;

        // SAFETY: The module is being loaded, so we can initialize binderfs.
        #[cfg(CONFIG_ANDROID_BINDERFS_RUST)]
        unsafe {
            kernel::error::to_result(bindings::init_rust_binderfs())?;
        }

        Ok(Self {})
    }
}

/// Makes the inner type Sync.
#[repr(transparent)]
pub struct AssertSync<T>(T);
// SAFETY: Used only to insert `file_operations` into a global, which is safe.
unsafe impl<T> Sync for AssertSync<T> {}

/// File operations that rust_binderfs.c can use.
#[no_mangle]
#[used]
pub static rust_binder_fops: AssertSync<kernel::bindings::file_operations> = {
    // SAFETY: All zeroes is safe for the `file_operations` type.
    let zeroed_ops = unsafe { core::mem::MaybeUninit::zeroed().assume_init() };

    let ops = kernel::bindings::file_operations {
        owner: THIS_MODULE.as_ptr(),
        poll: Some(rust_binder_poll),
        unlocked_ioctl: Some(rust_binder_unlocked_ioctl),
        compat_ioctl: Some(rust_binder_compat_ioctl),
        mmap: Some(rust_binder_mmap),
        open: Some(rust_binder_open),
        release: Some(rust_binder_release),
        mmap_supported_flags: 0,
        flush: Some(rust_binder_flush),
        ..zeroed_ops
    };
    AssertSync(ops)
};

#[no_mangle]
unsafe extern "C" fn rust_binder_new_device(
    name: *const core::ffi::c_char,
) -> *mut core::ffi::c_void {
    // SAFETY: The caller will always provide a valid c string here.
    let name = unsafe { kernel::str::CStr::from_char_ptr(name) };
    match Context::new(name) {
        Ok(ctx) => Arc::into_foreign(ctx).cast_mut(),
        Err(_err) => core::ptr::null_mut(),
    }
}

#[no_mangle]
unsafe extern "C" fn rust_binder_remove_device(device: *mut core::ffi::c_void) {
    if !device.is_null() {
        // SAFETY: The caller ensures that the `device` pointer came from a previous call to
        // `rust_binder_new_device`.
        let ctx = unsafe { Arc::<Context>::from_foreign(device) };
        ctx.deregister();
        drop(ctx);
    }
}

unsafe extern "C" fn rust_binder_open(
    inode: *mut bindings::inode,
    file_ptr: *mut bindings::file,
) -> core::ffi::c_int {
    // SAFETY: The `rust_binderfs.c` file ensures that `i_private` is set to the return value of a
    // successful call to `rust_binder_new_device`.
    let ctx = unsafe { Arc::<Context>::borrow((*inode).i_private) };

    // SAFETY: The caller provides a valid file pointer to a new `struct file`.
    let file = unsafe { File::from_ptr(file_ptr) };
    let process = match Process::open(ctx, file) {
        Ok(process) => process,
        Err(err) => return err.to_errno(),
    };

    // SAFETY: This file is associated with Rust binder, so we own the `private_data` field.
    unsafe { (*file_ptr).private_data = process.into_foreign().cast_mut() };
    0
}

unsafe extern "C" fn rust_binder_release(
    _inode: *mut bindings::inode,
    file: *mut bindings::file,
) -> core::ffi::c_int {
    // SAFETY: We previously set `private_data` in `rust_binder_open`.
    let process = unsafe { Arc::<Process>::from_foreign((*file).private_data) };
    // SAFETY: The caller ensures that the file is valid.
    let file = unsafe { File::from_ptr(file) };
    Process::release(process, file);
    0
}

unsafe extern "C" fn rust_binder_compat_ioctl(
    file: *mut bindings::file,
    cmd: core::ffi::c_uint,
    arg: core::ffi::c_ulong,
) -> core::ffi::c_long {
    // SAFETY: We previously set `private_data` in `rust_binder_open`.
    let f = unsafe { Arc::<Process>::borrow((*file).private_data) };
    // SAFETY: The caller ensures that the file is valid.
    match Process::compat_ioctl(f, unsafe { File::from_ptr(file) }, cmd as _, arg as _) {
        Ok(ret) => ret.into(),
        Err(err) => err.to_errno().into(),
    }
}

unsafe extern "C" fn rust_binder_unlocked_ioctl(
    file: *mut bindings::file,
    cmd: core::ffi::c_uint,
    arg: core::ffi::c_ulong,
) -> core::ffi::c_long {
    // SAFETY: We previously set `private_data` in `rust_binder_open`.
    let f = unsafe { Arc::<Process>::borrow((*file).private_data) };
    // SAFETY: The caller ensures that the file is valid.
    match Process::ioctl(f, unsafe { File::from_ptr(file) }, cmd as _, arg as _) {
        Ok(ret) => ret.into(),
        Err(err) => err.to_errno().into(),
    }
}

unsafe extern "C" fn rust_binder_mmap(
    file: *mut bindings::file,
    vma: *mut bindings::vm_area_struct,
) -> core::ffi::c_int {
    // SAFETY: We previously set `private_data` in `rust_binder_open`.
    let f = unsafe { Arc::<Process>::borrow((*file).private_data) };
    // SAFETY: The caller ensures that the vma is valid.
    let area = unsafe { kernel::mm::virt::Area::from_ptr_mut(vma) };
    // SAFETY: The caller ensures that the file is valid.
    match Process::mmap(f, unsafe { File::from_ptr(file) }, area) {
        Ok(()) => 0,
        Err(err) => err.to_errno(),
    }
}

unsafe extern "C" fn rust_binder_poll(
    file: *mut bindings::file,
    wait: *mut bindings::poll_table_struct,
) -> bindings::__poll_t {
    // SAFETY: We previously set `private_data` in `rust_binder_open`.
    let f = unsafe { Arc::<Process>::borrow((*file).private_data) };
    // SAFETY: The caller ensures that the file is valid.
    let fileref = unsafe { File::from_ptr(file) };
    // SAFETY: The caller ensures that the `PollTable` is valid.
    match Process::poll(f, fileref, unsafe { PollTable::from_ptr(wait) }) {
        Ok(v) => v,
        Err(_) => bindings::POLLERR,
    }
}

unsafe extern "C" fn rust_binder_flush(
    file: *mut bindings::file,
    _id: bindings::fl_owner_t,
) -> core::ffi::c_int {
    // SAFETY: We previously set `private_data` in `rust_binder_open`.
    let f = unsafe { Arc::<Process>::borrow((*file).private_data) };
    match Process::flush(f) {
        Ok(()) => 0,
        Err(err) => err.to_errno(),
    }
}

#[no_mangle]
unsafe extern "C" fn rust_binder_stats_show(
    _: *mut seq_file,
    _: *mut core::ffi::c_void,
) -> core::ffi::c_int {
    0
}

#[no_mangle]
unsafe extern "C" fn rust_binder_state_show(
    ptr: *mut seq_file,
    _: *mut core::ffi::c_void,
) -> core::ffi::c_int {
    // SAFETY: The caller ensures that the pointer is valid and exclusive for the duration in which
    // this method is called.
    let m = unsafe { SeqFile::from_raw(ptr) };
    if let Err(err) = rust_binder_state_show_impl(m) {
        seq_print!(m, "failed to generate state: {:?}\n", err);
    }
    0
}

#[no_mangle]
unsafe extern "C" fn rust_binder_transactions_show(
    _: *mut seq_file,
    _: *mut core::ffi::c_void,
) -> core::ffi::c_int {
    0
}

#[no_mangle]
unsafe extern "C" fn rust_binder_transaction_log_show(
    _: *mut seq_file,
    _: *mut core::ffi::c_void,
) -> core::ffi::c_int {
    0
}

fn rust_binder_state_show_impl(m: &mut SeqFile) -> Result<()> {
    seq_print!(m, "binder state:\n");
    let contexts = context::get_all_contexts()?;
    for ctx in contexts {
        let procs = ctx.get_all_procs()?;
        for proc in procs {
            proc.debug_print(m, &ctx)?;
            seq_print!(m, "\n");
        }
    }
    Ok(())
}
