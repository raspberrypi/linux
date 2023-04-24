// SPDX-License-Identifier: GPL-2.0

//! Credentials management.
//!
//! C header: [`include/linux/cred.h`](../../../../include/linux/cred.h)
//!
//! Reference: <https://www.kernel.org/doc/html/latest/security/credentials.html>

use crate::{bindings, types::AlwaysRefCounted};
use core::cell::UnsafeCell;

/// Wraps the kernel's `struct cred`.
///
/// # Invariants
///
/// Instances of this type are always ref-counted, that is, a call to `get_cred` ensures that the
/// allocation remains valid at least until the matching call to `put_cred`.
#[repr(transparent)]
pub struct Credential(pub(crate) UnsafeCell<bindings::cred>);

impl Credential {
    /// Creates a reference to a [`Credential`] from a valid pointer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `ptr` is valid and remains valid for the lifetime of the
    /// returned [`Credential`] reference.
    pub(crate) unsafe fn from_ptr<'a>(ptr: *const bindings::cred) -> &'a Self {
        // SAFETY: The safety requirements guarantee the validity of the dereference, while the
        // `Credential` type being transparent makes the cast ok.
        unsafe { &*ptr.cast() }
    }
}

// SAFETY: The type invariants guarantee that `Credential` is always ref-counted.
unsafe impl AlwaysRefCounted for Credential {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        unsafe { bindings::get_cred(self.0.get()) };
    }

    unsafe fn dec_ref(obj: core::ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        unsafe { bindings::put_cred(obj.cast().as_ptr()) };
    }
}
