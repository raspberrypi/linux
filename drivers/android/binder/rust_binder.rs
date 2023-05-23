// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Binder -- the Android IPC mechanism.

use kernel::{
    bindings::{self, seq_file},
    file::File,
    prelude::*,
    sync::poll::PollTable,
    sync::Arc,
    types::ForeignOwnable,
};

use crate::{context::Context, process::Process};

mod context;
mod defs;
mod error;
mod process;
mod thread;

module! {
    type: BinderModule,
    name: "rust_binder",
    author: "Wedson Almeida Filho, Alice Ryhl",
    description: "Android Binder",
    license: "GPL",
}

struct BinderModule {}

impl kernel::Module for BinderModule {
    fn init(_module: &'static kernel::ThisModule) -> Result<Self> {
        // SAFETY: This is the very first thing that happens in this module, so nothing else has
        // called `init` yet. Furthermore, we cannot move a value in a global, so the `Contexts`
        // will not be moved after this call.
        unsafe { crate::context::CONTEXTS.init() };

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
    unsafe {
        (*file_ptr).private_data = process.into_foreign().cast_mut();
    }
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
unsafe extern "C" fn rust_binder_stats_show(_: *mut seq_file) -> core::ffi::c_int {
    0
}

#[no_mangle]
unsafe extern "C" fn rust_binder_state_show(_: *mut seq_file) -> core::ffi::c_int {
    0
}

#[no_mangle]
unsafe extern "C" fn rust_binder_transactions_show(_: *mut seq_file) -> core::ffi::c_int {
    0
}

#[no_mangle]
unsafe extern "C" fn rust_binder_transaction_log_show(_: *mut seq_file) -> core::ffi::c_int {
    0
}
