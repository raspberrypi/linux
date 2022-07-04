// SPDX-License-Identifier: GPL-2.0

//! File systems.
//!
//! C headers: [`include/linux/fs.h`](../../../../include/linux/fs.h)

use crate::error::{to_result, Result};
use crate::types::{AlwaysRefCounted, Opaque};
use crate::{bindings, error::code::*, str::CStr, ThisModule};
use core::{marker::PhantomPinned, pin::Pin, ptr};

/// A file system type.
pub trait Type {
    /// The name of the file system type.
    const NAME: &'static CStr;

    /// The flags of this file system type.
    ///
    /// It is a combination of the flags in the [`flags`] module.
    const FLAGS: i32;
}

/// File system flags.
pub mod flags {
    use crate::bindings;

    /// The file system requires a device.
    pub const REQUIRES_DEV: i32 = bindings::FS_REQUIRES_DEV as _;

    /// The options provided when mounting are in binary form.
    pub const BINARY_MOUNTDATA: i32 = bindings::FS_BINARY_MOUNTDATA as _;

    /// The file system has a subtype. It is extracted from the name and passed in as a parameter.
    pub const HAS_SUBTYPE: i32 = bindings::FS_HAS_SUBTYPE as _;

    /// The file system can be mounted by userns root.
    pub const USERNS_MOUNT: i32 = bindings::FS_USERNS_MOUNT as _;

    /// Disables fanotify permission events.
    pub const DISALLOW_NOTIFY_PERM: i32 = bindings::FS_DISALLOW_NOTIFY_PERM as _;

    /// The file system has been updated to handle vfs idmappings.
    pub const ALLOW_IDMAP: i32 = bindings::FS_ALLOW_IDMAP as _;

    /// The file systen will handle `d_move` during `rename` internally.
    pub const RENAME_DOES_D_MOVE: i32 = bindings::FS_RENAME_DOES_D_MOVE as _;
}

/// A file system registration.
#[derive(Default)]
pub struct Registration {
    is_registered: bool,
    fs: UnsafeCell<bindings::file_system_type>,
    _pin: PhantomPinned,
}

// SAFETY: `Registration` doesn't really provide any `&self` methods, so it is safe to pass
// references to it around.
unsafe impl Sync for Registration {}

// SAFETY: Both registration and unregistration are implemented in C and safe to be performed from
// any thread, so `Registration` is `Send`.
unsafe impl Send for Registration {}

impl Registration {
    /// Creates a new file system registration.
    ///
    /// It is not visible or accessible yet. A successful call to [`Registration::register`] needs
    /// to be made before users can mount it.
    pub fn new() -> Self {
        Self {
            is_registered: false,
            fs: UnsafeCell::new(bindings::file_system_type::default()),
            _pin: PhantomPinned,
        }
    }

    /// Registers a file system so that it can be mounted by users.
    ///
    /// The file system is described by the [`Type`] argument.
    ///
    /// It is automatically unregistered when the registration is dropped.
    pub fn register<T: Type>(self: Pin<&mut Self>, module: &'static ThisModule) -> Result {
        // SAFETY: We never move out of `this`.
        let this = unsafe { self.get_unchecked_mut() };

        if this.is_registered {
            return Err(EINVAL);
        }

        let mut fs = this.fs.get_mut();
        fs.owner = module.0;
        fs.name = T::NAME.as_char_ptr();
        fs.fs_flags = T::FLAGS;
        fs.init_fs_context = Some(Self::init_fs_context_callback::<T>);
        fs.kill_sb = Some(Self::kill_sb_callback::<T>);
        // SAFETY: Pointers stored in `fs` are either static so will live for as long as the
        // registration is active (it is undone in `drop`).
        to_result(unsafe { bindings::register_filesystem(this.fs.get()) })?;
        this.is_registered = true;
        Ok(())
    }

    unsafe extern "C" fn init_fs_context_callback<T: Type>(
        _fc_ptr: *mut bindings::fs_context,
    ) -> core::ffi::c_int {
        EINVAL.to_errno()
    }

    unsafe extern "C" fn kill_sb_callback<T: Type>(_sb_ptr: *mut bindings::super_block) {}
}

impl Drop for Registration {
    fn drop(&mut self) {
        if self.is_registered {
            // SAFETY: When `is_registered` is `true`, a previous call to `register_filesystem` has
            // succeeded, so it is safe to unregister here.
            unsafe { bindings::unregister_filesystem(self.fs.get()) };
        }
    }
}

/// Wraps the kernel's `struct inode`.
///
/// # Invariants
///
/// Instances of this type are always ref-counted, that is, a call to `ihold` ensures that the
/// allocation remains valid at least until the matching call to `iput`.
#[repr(transparent)]
pub struct INode(pub(crate) Opaque<bindings::inode>);

// SAFETY: The type invariants guarantee that `INode` is always ref-counted.
unsafe impl AlwaysRefCounted for INode {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        unsafe { bindings::ihold(self.0.get()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        unsafe { bindings::iput(obj.cast().as_ptr()) }
    }
}

/// Wraps the kernel's `struct dentry`.
///
/// # Invariants
///
/// Instances of this type are always ref-counted, that is, a call to `dget` ensures that the
/// allocation remains valid at least until the matching call to `dput`.
#[repr(transparent)]
pub struct DEntry(pub(crate) Opaque<bindings::dentry>);

// SAFETY: The type invariants guarantee that `DEntry` is always ref-counted.
unsafe impl AlwaysRefCounted for DEntry {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        unsafe { bindings::dget(self.0.get()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        unsafe { bindings::dput(obj.cast().as_ptr()) }
    }
}
