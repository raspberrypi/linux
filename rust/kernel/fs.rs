// SPDX-License-Identifier: GPL-2.0

//! File systems.
//!
//! C headers: [`include/linux/fs.h`](../../../../include/linux/fs.h)

use crate::bindings;
use crate::types::{AlwaysRefCounted, Opaque};
use core::ptr;

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
